// wifi-popup.cpp — консольный менеджер WiFi (через nmcli), на C++ / FTXUI
// Переписано с wifi-popup.c (ncurses) без потери функциональности.
// Новое: подключение к ранее неизвестным (не сохранённым) защищённым сетям
//        с вводом пароля прямо в интерфейсе.
//
// Сборка: см. CMakeLists.txt (рекомендуется) или Makefile.

#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include "layout_keys.hpp"
#include "net_status.hpp"
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace ftxui;

namespace {

constexpr int kMaxNets = 64;

// ---------------------------------------------------------------- shell ---

// Оборачивает строку в одинарные кавычки для безопасной подстановки в shell,
// экранируя одинарные кавычки внутри (ssid/пароль могут их содержать).
std::string ShellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::string TrimNewline(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

// Выполняет команду и возвращает её stdout целиком.
std::string RunCapture(const std::string& cmd) {
  std::string result;
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp)
    return result;
  char buf[512];
  while (fgets(buf, sizeof(buf), fp))
    result += buf;
  pclose(fp);
  return result;
}

// Выполняет команду, возвращает (exit_code, объединённый stdout+stderr).
std::pair<int, std::string> RunStatus(const std::string& cmd) {
  std::string full = cmd + " 2>&1";
  std::string out;
  FILE* fp = popen(full.c_str(), "r");
  if (!fp)
    return {-1, "popen failed"};
  char buf[512];
  while (fgets(buf, sizeof(buf), fp))
    out += buf;
  int rc = pclose(fp);
  int code = (rc == -1) ? -1 : WEXITSTATUS(rc);
  return {code, TrimNewline(out)};
}

// Разбивает строку nmcli -t на поля по ':' с учётом экранирования '\:'.
std::vector<std::string> SplitEscaped(const std::string& line, size_t nfields) {
  std::vector<std::string> fields;
  std::string cur;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == ':') {
      cur += ':';
      ++i;
    } else if (line[i] == ':') {
      fields.push_back(cur);
      cur.clear();
    } else {
      cur += line[i];
    }
  }
  fields.push_back(cur);
  while (fields.size() < nfields)
    fields.push_back("");
  return fields;
}

// ------------------------------------------------------------ модель ------

struct WifiNet {
  std::string ssid;
  std::string bssid;
  std::string sec;  // WPA2, WPA3, "--"
  int signal = 0;   // 0-100
  bool active = false;
  bool known = false;  // есть сохранённый профиль подключения
};

const char* SigBar(int sig) {
  if (sig >= 75) return "▂▄▆█";
  if (sig >= 50) return "▂▄▆ ";
  if (sig >= 25) return "▂▄  ";
  return "▂   ";
}

Color SigColor(int sig) {
  if (sig >= 70) return Color::Green;
  if (sig >= 40) return Color::Yellow;
  return Color::Red;
}

}  // namespace

// ============================================================================

class WifiApp {
 public:
  explicit WifiApp(ScreenInteractive* screen) : screen_(screen) {}

  // Модальное окно ввода пароля реализовано вручную (dbox + clear_under),
  // а не через ftxui::Modal(): последний оборачивает содержимое в
  // фокус-зависимый Container::Tab, и события не доходят до компонента,
  // пока в дереве нет ни одного "фокусируемого" виджета — наш глобальный
  // обработчик клавиш (Renderer+CatchEvent, без Input/Menu/Button) таким
  // не является. Ручная реализация даёт полный и предсказуемый контроль
  // над вводом пароля через один и тот же обработчик клавиш.
  Component Build() {
    Component main = Renderer([this] { return Render(); });
    main = CatchEvent(main, [this](const Event& e) { return HandleKey(e); });
    return main;
  }

  // Первая загрузка данных (в фоне, чтобы окно сразу отрисовалось).
  void Start() { Reload(/*rescan=*/false); }

 private:
  // ------------------------------------------------------------ данные ---

  void LoadKnownProfiles(std::unordered_set<std::string>* out) {
    std::string text =
        RunCapture("nmcli -t -f TYPE,NAME connection show 2>/dev/null");
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
      line = TrimNewline(line);
      auto sep = line.find(':');
      if (sep == std::string::npos)
        continue;
      std::string type = line.substr(0, sep);
      std::string name = line.substr(sep + 1);
      if (type.find("wireless") != std::string::npos || type == "wifi")
        out->insert(name);
    }
  }

  std::vector<WifiNet> ScanNetworks() {
    std::unordered_set<std::string> known;
    LoadKnownProfiles(&known);

    std::string text = RunCapture(
        "nmcli -t -f ACTIVE,SSID,BSSID,SIGNAL,SECURITY dev wifi list "
        "2>/dev/null");

    std::vector<WifiNet> nets;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line) && (int)nets.size() < kMaxNets) {
      line = TrimNewline(line);
      if (line.empty())
        continue;
      auto f = SplitEscaped(line, 5);
      if (f[1].empty())
        continue;  // скрытые/пустые SSID пропускаем, как в оригинале

      WifiNet n;
      n.active = (f[0] == "yes");
      n.ssid = f[1];
      n.bssid = f[2];
      n.signal = atoi(f[3].c_str());
      n.sec = f[4].empty() ? "--" : f[4];
      n.known = known.count(n.ssid) > 0;
      nets.push_back(std::move(n));
    }

    // Активная сеть — наверх (сохраняем поведение оригинала).
    for (size_t i = 1; i < nets.size(); ++i) {
      if (nets[i].active) {
        std::swap(nets[0], nets[i]);
        break;
      }
    }
    return nets;
  }

  // Через что сейчас идёт интернет: Wi-Fi может быть подключён, а трафик
  // при этом идти по кабелю.
  net::Status ReadUplink() {
    return net::ParseStatus(RunCapture(net::kDevicesCmd),
                            RunCapture(net::kRoutesCmd));
  }

  // Запускает перезагрузку списка сетей в фоне, чтобы не блокировать отрисовку.
  // rescan=true — сначала попросить nmcli пересканировать эфир.
  void Reload(bool rescan) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
      busy_ = true;
      scanning_ = rescan;
    }
    Wake();

    std::thread([this, rescan] {
      if (rescan) {
        RunStatus("nmcli dev wifi rescan");
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      auto nets = ScanNetworks();
      auto uplink = ReadUplink();

      std::lock_guard<std::mutex> lk(mu_);
      nets_ = std::move(nets);
      uplink_ = std::move(uplink);
      if (nets_.empty())
        sel_ = 0;
      else if (sel_ >= (int)nets_.size())
        sel_ = (int)nets_.size() - 1;
      busy_ = false;
      scanning_ = false;
      Wake();
    }).detach();
  }

  // ------------------------------------------------------------ действия --

  void ConnectSelected() {
    WifiNet target;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_ || sel_ < 0 || sel_ >= (int)nets_.size())
        return;
      target = nets_[sel_];
    }

    bool needs_password = (target.sec != "--") && !target.known;
    if (needs_password) {
      std::lock_guard<std::mutex> lk(mu_);
      pending_ssid_ = target.ssid;
      password_buffer_.clear();
      show_modal_ = true;
      SetStatus("Введите пароль для «" + target.ssid + "»", false);
      Wake();
      return;
    }

    StartConnect(target.ssid, /*password=*/"", /*has_password=*/false);
  }

  void SubmitPassword() {
    std::string ssid, password;
    {
      std::lock_guard<std::mutex> lk(mu_);
      ssid = pending_ssid_;
      password = password_buffer_;
      show_modal_ = false;
      password_buffer_.clear();
    }
    StartConnect(ssid, password, /*has_password=*/true);
  }

  void CancelPassword() {
    std::lock_guard<std::mutex> lk(mu_);
    show_modal_ = false;
    password_buffer_.clear();
    SetStatus("Отменено", false);
  }

  void StartConnect(const std::string& ssid,
                     const std::string& password,
                     bool has_password) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
      busy_ = true;
      SetStatus("Подключение к «" + ssid + "»…", false);
    }
    Wake();

    std::thread([this, ssid, password, has_password] {
      std::string cmd = "nmcli dev wifi connect " + ShellQuote(ssid);
      if (has_password)
        cmd += " password " + ShellQuote(password);
      auto [code, out] = RunStatus(cmd);

      auto nets = ScanNetworks();
      auto uplink = ReadUplink();

      std::lock_guard<std::mutex> lk(mu_);
      nets_ = std::move(nets);
      uplink_ = std::move(uplink);
      for (size_t i = 0; i < nets_.size(); ++i) {
        if (nets_[i].ssid == ssid) {
          sel_ = (int)i;
          break;
        }
      }
      if (code == 0) {
        SetStatusLocked("Подключено к «" + ssid + "»", false);
      } else {
        std::string reason = out.empty() ? "неизвестная ошибка" : out;
        SetStatusLocked("Ошибка подключения: " + reason, true);
      }
      busy_ = false;
      Wake();
    }).detach();
  }

  void DisconnectActive() {
    std::string device;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
    }
    // Определяем активное wifi-устройство динамически (без хардкода
    // интерфейса, в отличие от оригинала).
    std::string dev_list =
        RunCapture("nmcli -t -f DEVICE,TYPE,STATE dev status 2>/dev/null");
    std::istringstream iss(dev_list);
    std::string line;
    while (std::getline(iss, line)) {
      line = TrimNewline(line);
      auto f = SplitEscaped(line, 3);
      if (f[1] == "wifi" && f[2] == "connected") {
        device = f[0];
        break;
      }
    }
    if (device.empty()) {
      std::lock_guard<std::mutex> lk(mu_);
      SetStatusLocked("Нет активного WiFi-соединения", true);
      Wake();
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      busy_ = true;
      SetStatusLocked("Отключение…", false);
    }
    Wake();

    std::thread([this, device] {
      RunStatus("nmcli dev disconnect " + ShellQuote(device));
      auto nets = ScanNetworks();
      auto uplink = ReadUplink();
      std::lock_guard<std::mutex> lk(mu_);
      nets_ = std::move(nets);
      uplink_ = std::move(uplink);
      busy_ = false;
      SetStatusLocked("Отключено", false);
      Wake();
    }).detach();
  }

  void MoveSel(int delta) {
    std::lock_guard<std::mutex> lk(mu_);
    if (nets_.empty())
      return;
    sel_ += delta;
    if (sel_ < 0) sel_ = 0;
    if (sel_ >= (int)nets_.size()) sel_ = (int)nets_.size() - 1;
  }

  // Добавляет один введённый символ к паролю (может быть многобайтовым —
  // один Event::Character обычно соответствует одному нажатию клавиши).
  void PasswordAppend(const std::string& ch) {
    std::lock_guard<std::mutex> lk(mu_);
    password_buffer_ += ch;
  }

  // Удаляет последний введённый символ (корректно для многобайтового UTF-8).
  void PasswordBackspace() {
    std::lock_guard<std::mutex> lk(mu_);
    if (password_buffer_.empty())
      return;
    password_buffer_.pop_back();
    while (!password_buffer_.empty() &&
           (static_cast<unsigned char>(password_buffer_.back()) & 0xC0) ==
               0x80) {
      password_buffer_.pop_back();
    }
  }

  bool HandleKey(const Event& event) {
    // Пока открыт диалог ввода пароля — все клавиши идут в него,
    // обычные горячие клавиши временно не действуют.
    bool modal_open;
    {
      std::lock_guard<std::mutex> lk(mu_);
      modal_open = show_modal_;
    }
    if (modal_open) {
      if (event == Event::Escape) {
        CancelPassword();
        return true;
      }
      if (event == Event::Return) {
        SubmitPassword();
        return true;
      }
      if (event == Event::Backspace) {
        PasswordBackspace();
        return true;
      }
      if (event.is_character()) {
        PasswordAppend(event.character());
        return true;
      }
      return true;  // поглощаем остальные события, пока диалог открыт
    }

    return HandleCommand(LatinKey(event));
  }

  // Клавиши вне диалога пароля, русская раскладка уже переведена в латиницу.
  bool HandleCommand(const Event& event) {
    if (event == Event::Character('j') || event == Event::ArrowDown) {
      MoveSel(1);
      return true;
    }
    if (event == Event::Character('k') || event == Event::ArrowUp) {
      MoveSel(-1);
      return true;
    }
    if (event == Event::Return) {
      ConnectSelected();
      return true;
    }
    if (event == Event::Character('d') || event == Event::Character('D')) {
      DisconnectActive();
      return true;
    }
    if (event == Event::Character('s') || event == Event::Character('S')) {
      Reload(/*rescan=*/true);
      return true;
    }
    if (event == Event::Character('r') || event == Event::Character('R')) {
      Reload(/*rescan=*/false);
      return true;
    }
    if (event == Event::Character('q') || event == Event::Escape) {
      screen_->ExitLoopClosure()();
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------- отрисовка --

  void SetStatus(const std::string& msg, bool is_error) {
    status_ = msg;
    status_is_error_ = is_error;
  }
  // То же самое, но явно подчёркивает, что мьютекс уже захвачен вызывающим.
  void SetStatusLocked(const std::string& msg, bool is_error) {
    status_ = msg;
    status_is_error_ = is_error;
  }

  void Wake() { screen_->PostEvent(Event::Custom); }

  // Верхнеуровневая отрисовка: базовый экран, и поверх него — диалог
  // ввода пароля, если он открыт (ручной аналог ftxui::Modal, см.
  // комментарий в Build()).
  Element Render() {
    bool modal_open;
    {
      std::lock_guard<std::mutex> lk(mu_);
      modal_open = show_modal_;
    }
    Element base = RenderBase();
    if (!modal_open)
      return base;
    return dbox({base, RenderModal() | clear_under | center});
  }

  Element RenderBase() {
    std::lock_guard<std::mutex> lk(mu_);

    Element title = hbox({
                        filler(),
                        text(" WiFi ") | bold | color(Color::Black),
                        filler(),
                    }) |
                    bgcolor(Color::Blue);

    // Строка "активная сеть".
    Elements active_row = {text(" active   ") | color(Color::Cyan)};
    bool found_active = false;
    for (auto& n : nets_) {
      if (n.active) {
        active_row.push_back(text("● ") | color(Color::Green) | bold);
        active_row.push_back(text(n.ssid) | color(Color::White));
        active_row.push_back(text("  " + std::to_string(n.signal) + "%") |
                              dim);
        found_active = true;
        break;
      }
    }
    if (!found_active)
      active_row.push_back(text("○ не подключено") | dim);
    if (uplink_.online && uplink_.primary.wired())
      active_row.push_back(text("   󰈀 интернет идёт по кабелю") |
                           color(Color::Cyan));
    active_row.push_back(filler());
    if (scanning_)
      active_row.push_back(text("[сканирование...] ") | color(Color::Yellow));
    Element active_line = hbox(active_row);

    // Заголовок колонок.
    Element col_header =
        text("    SSID                             SECURITY  BSSID"
             "                SIG") |
        dim;

    // Список сетей.
    Elements rows;
    for (int i = 0; i < (int)nets_.size(); ++i) {
      const WifiNet& n = nets_[i];
      bool is_sel = (i == sel_);

      Element cursor = text(is_sel ? " ▶ " : "   ") |
                       (is_sel ? color(Color::Cyan) | bold : dim);
      Element star = text(n.active ? "*" : " ") |
                     (n.active ? color(Color::Green) | bold : nothing);

      std::string ssid_t = n.ssid;
      if (ssid_t.size() > 31)
        ssid_t.resize(31);
      Element ssid_el = text(" " + ssid_t) | size(WIDTH, EQUAL, 33) |
                         (is_sel ? color(Color::White) | bold : dim);

      std::string sec_disp = (n.sec == "--") ? "open" : n.sec;
      Element sec_el =
          text(sec_disp) | size(WIDTH, EQUAL, 9) | color(Color::Magenta);

      Element bssid_el = text(n.bssid) | size(WIDTH, EQUAL, 19) | dim;

      Element sig_el = text(std::string(SigBar(n.signal)) + " " +
                             std::to_string(n.signal) + "%") |
                        color(SigColor(n.signal));

      Element tag_el = text("");
      if (n.sec != "--" && !n.known)
        tag_el = text(" ‹new›") | color(Color::Cyan);

      rows.push_back(
          hbox({cursor, star, ssid_el, sec_el, bssid_el, sig_el, tag_el}));
    }
    if (rows.empty()) {
      rows.push_back(text("  сети не найдены — [S] пересканировать") | dim);
    }
    Element list = vbox(std::move(rows)) | vscroll_indicator | frame | flex;

    // Строка статуса.
    Elements bottom;
    if (!status_.empty()) {
      bottom.push_back(
          text(" " + status_) |
          color(status_is_error_ ? Color::Red : Color::Green));
    }

    // Подсказки.
    struct Hint { const char* key; const char* desc; };
    static const Hint hints[] = {
        {"jk", "nav"},      {"Enter", "connect"}, {"d", "disconnect"},
        {"S", "rescan"},    {"R", "refresh"},     {"q", "quit"},
    };
    Elements hint_row;
    hint_row.push_back(text(" "));
    for (auto& h : hints) {
      hint_row.push_back(text(h.key) | color(Color::Cyan) | bold);
      hint_row.push_back(text(std::string(":") + h.desc + "  ") | dim);
    }
    bottom.push_back(hbox(std::move(hint_row)));

    return vbox({
               title,
               separatorEmpty(),
               active_line,
               separator(),
               col_header,
               list,
               separator(),
               vbox(std::move(bottom)),
           }) |
           size(WIDTH, GREATER_THAN, 70) | size(HEIGHT, GREATER_THAN, 16);
  }

  Element RenderModal() {
    std::string ssid_copy;
    size_t pw_len;
    {
      std::lock_guard<std::mutex> lk(mu_);
      ssid_copy = pending_ssid_;
      pw_len = password_buffer_.size();
    }
    std::string masked(pw_len, '*');
    return vbox({
               text(" Требуется пароль ") | bold,
               separator(),
               hbox({text("Сеть:  "), text(ssid_copy) | color(Color::Cyan) |
                                           bold}),
               text("Пароль: "),
               hbox({text(masked) | color(Color::White), text("▏") | blink}) |
                   size(WIDTH, EQUAL, 28),
               separatorEmpty(),
               text("Enter — подключиться   Esc — отмена") | dim,
           }) |
           border | bgcolor(Color::Black) | color(Color::White) |
           size(WIDTH, EQUAL, 44);
  }

  ScreenInteractive* screen_;

  std::mutex mu_;
  std::vector<WifiNet> nets_;
  net::Status uplink_;
  int sel_ = 0;
  bool scanning_ = false;
  bool busy_ = false;
  std::string status_;
  bool status_is_error_ = false;

  bool show_modal_ = false;
  std::string pending_ssid_;
  std::string password_buffer_;
};

int main() {
  auto screen = ScreenInteractive::Fullscreen();
  WifiApp app(&screen);
  Component root = app.Build();
  app.Start();
  screen.Loop(root);
  return 0;
}
