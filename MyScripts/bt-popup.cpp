// bt-popup.cpp — консольный менеджер Bluetooth (через bluetoothctl), C++ / FTXUI
// Переписано с bt-popup.c (ncurses) без потери функциональности.
//
// Сборка: см. CMakeLists.txt (рекомендуется) или Makefile.

#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
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
#include <vector>

using namespace ftxui;

namespace {

constexpr int kMaxDevices = 64;

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

struct BtDevice {
  std::string mac;
  std::string name;
  bool connected = false;
  bool paired = false;
  bool trusted = false;
};

}  // namespace

// ============================================================================

class BtApp {
 public:
  explicit BtApp(ScreenInteractive* screen) : screen_(screen) {}

  Component Build() {
    Component main = Renderer([this] { return RenderMain(); });
    main = CatchEvent(main, [this](const Event& e) { return HandleKey(e); });
    return main;
  }

  void Start() { Reload(); }

 private:
  // ------------------------------------------------------------ данные ---

  bool LoadPowered() {
    std::string out = RunCapture(
        "bluetoothctl show 2>/dev/null | grep -m1 'Powered:' | awk "
        "'{print $2}'");
    out = TrimNewline(out);
    return out == "yes";
  }

  std::vector<BtDevice> ScanDevices() {
    std::vector<BtDevice> devs;

    if (!LoadPowered()) {
      std::lock_guard<std::mutex> lk(mu_);
      powered_ = false;
      return devs;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      powered_ = true;
    }

    std::string list = RunCapture("bluetoothctl devices 2>/dev/null");
    std::istringstream iss(list);
    std::string line;
    while (std::getline(iss, line) && (int)devs.size() < kMaxDevices) {
      line = TrimNewline(line);
      // Формат: "Device XX:XX:XX:XX:XX:XX Name..."
      if (line.rfind("Device ", 0) != 0)
        continue;
      std::string rest = line.substr(7);
      if (rest.size() < 17)
        continue;
      BtDevice d;
      d.mac = rest.substr(0, 17);
      d.name = (rest.size() > 18) ? rest.substr(18) : d.mac;
      d.paired = true;
      devs.push_back(std::move(d));
    }

    // Определяем connected/trusted через `bluetoothctl info`.
    std::string info = RunCapture(
        "bluetoothctl info 2>/dev/null | grep -E 'Device|Connected|Trusted'");
    std::istringstream iss2(info);
    std::string cur_mac;
    while (std::getline(iss2, line)) {
      line = TrimNewline(line);
      if (line.rfind("Device ", 0) == 0) {
        cur_mac = line.substr(7, 17);
      } else if (line.find("Connected: yes") != std::string::npos &&
                 !cur_mac.empty()) {
        for (auto& d : devs)
          if (d.mac == cur_mac) d.connected = true;
      } else if (line.find("Trusted: yes") != std::string::npos &&
                 !cur_mac.empty()) {
        for (auto& d : devs)
          if (d.mac == cur_mac) d.trusted = true;
      }
    }
    return devs;
  }

  void Reload() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
      busy_ = true;
    }
    Wake();
    std::thread([this] {
      auto devs = ScanDevices();
      std::lock_guard<std::mutex> lk(mu_);
      devs_ = std::move(devs);
      if (devs_.empty())
        sel_ = 0;
      else if (sel_ >= (int)devs_.size())
        sel_ = (int)devs_.size() - 1;
      busy_ = false;
      Wake();
    }).detach();
  }

  // ------------------------------------------------------------ действия --

  void ConnectSelected() {
    std::string mac;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_ || sel_ < 0 || sel_ >= (int)devs_.size())
        return;
      mac = devs_[sel_].mac;
      busy_ = true;
      SetStatus("Подключение…", false);
    }
    Wake();
    std::thread([this, mac] {
      auto [code, out] = RunStatus("bluetoothctl connect " + ShellQuote(mac));
      auto devs = ScanDevices();
      std::lock_guard<std::mutex> lk(mu_);
      devs_ = std::move(devs);
      if (code == 0)
        SetStatus("Подключено", false);
      else
        SetStatus("Ошибка подключения: " + (out.empty() ? "н/д" : out), true);
      busy_ = false;
      Wake();
    }).detach();
  }

  void DisconnectSelected() {
    std::string mac;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_ || sel_ < 0 || sel_ >= (int)devs_.size())
        return;
      mac = devs_[sel_].mac;
      busy_ = true;
      SetStatus("Отключение…", false);
    }
    Wake();
    std::thread([this, mac] {
      RunStatus("bluetoothctl disconnect " + ShellQuote(mac));
      auto devs = ScanDevices();
      std::lock_guard<std::mutex> lk(mu_);
      devs_ = std::move(devs);
      SetStatus("Отключено", false);
      busy_ = false;
      Wake();
    }).detach();
  }

  void ToggleScan() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
      busy_ = true;
      scanning_ = true;
    }
    Wake();
    std::thread([this] {
      int rc = system("bluetoothctl scan on >/dev/null 2>&1 &");
      (void)rc;  // фоновый запуск сканирования, статус не важен
      std::this_thread::sleep_for(std::chrono::seconds(5));
      RunStatus("bluetoothctl scan off");
      auto devs = ScanDevices();
      std::lock_guard<std::mutex> lk(mu_);
      devs_ = std::move(devs);
      scanning_ = false;
      busy_ = false;
      Wake();
    }).detach();
  }

  void TogglePower() {
    bool cur;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (busy_)
        return;
      cur = powered_;
      busy_ = true;
    }
    Wake();
    std::thread([this, cur] {
      RunStatus(cur ? "bluetoothctl power off" : "bluetoothctl power on");
      auto devs = ScanDevices();
      std::lock_guard<std::mutex> lk(mu_);
      devs_ = std::move(devs);
      busy_ = false;
      Wake();
    }).detach();
  }

  void MoveSel(int delta) {
    std::lock_guard<std::mutex> lk(mu_);
    if (devs_.empty())
      return;
    sel_ += delta;
    if (sel_ < 0) sel_ = 0;
    if (sel_ >= (int)devs_.size()) sel_ = (int)devs_.size() - 1;
  }

  bool HandleKey(const Event& event) {
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
      DisconnectSelected();
      return true;
    }
    if (event == Event::Character('s') || event == Event::Character('S')) {
      ToggleScan();
      return true;
    }
    if (event == Event::Character('p') || event == Event::Character('P')) {
      TogglePower();
      return true;
    }
    if (event == Event::Character('r') || event == Event::Character('R')) {
      Reload();
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

  void Wake() { screen_->PostEvent(Event::Custom); }

  Element RenderMain() {
    std::lock_guard<std::mutex> lk(mu_);

    Element title = hbox({
                        filler(),
                        text(" BLUETOOTH ") | bold | color(Color::Black),
                        filler(),
                    }) |
                    bgcolor(Color::Blue);

    Elements adapter_row = {text(" adapter  ") | color(Color::Cyan)};
    if (powered_)
      adapter_row.push_back(text("● ON") | color(Color::Green) | bold);
    else
      adapter_row.push_back(text("○ OFF") | color(Color::Red));
    if (scanning_) {
      adapter_row.push_back(text("  [scanning...]") | color(Color::Magenta));
    }
    adapter_row.push_back(filler());
    Element adapter_line = hbox(adapter_row);

    Elements rows;
    if (!powered_) {
      rows.push_back(text("  Bluetooth выключен. [P] — включить") | dim);
    } else if (devs_.empty()) {
      rows.push_back(text("  Нет устройств. [S] — сканировать") | dim);
    } else {
      for (int i = 0; i < (int)devs_.size(); ++i) {
        const BtDevice& d = devs_[i];
        bool is_sel = (i == sel_);

        Element cursor = text(is_sel ? " ▶ " : "   ") |
                         (is_sel ? color(Color::Cyan) | bold : dim);

        Element status_icon;
        if (d.connected)
          status_icon = text("● ") | color(Color::Green) | bold;
        else if (d.trusted)
          status_icon = text("◎ ") | color(Color::Yellow);
        else
          status_icon = text("○ ") | dim;

        std::string name_t = d.name;
        if (name_t.size() > 40)
          name_t.resize(40);
        Element name_el = text(name_t) | size(WIDTH, EQUAL, 42) |
                           (is_sel ? color(Color::White) | bold : dim);

        Element mac_el = text(d.mac) | color(Color::GrayLight) | dim;

        Element conn_tag =
            d.connected ? (text(" [C]") | color(Color::Green)) : text("");

        rows.push_back(
            hbox({cursor, status_icon, name_el, mac_el, conn_tag}));
      }
    }
    Element list = vbox(std::move(rows)) | vscroll_indicator | frame | flex;

    Elements bottom;
    if (!status_.empty()) {
      bottom.push_back(
          text(" " + status_) |
          color(status_is_error_ ? Color::Red : Color::Green));
    }

    struct Hint { const char* key; const char* desc; };
    static const Hint hints[] = {
        {"jk", "nav"},   {"Enter", "connect"}, {"d", "disconnect"},
        {"S", "scan"},   {"P", "power"},       {"R", "refresh"},
        {"q", "quit"},
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
               adapter_line,
               separator(),
               list,
               separator(),
               vbox(std::move(bottom)),
           }) |
           size(WIDTH, GREATER_THAN, 60) | size(HEIGHT, GREATER_THAN, 14);
  }

  ScreenInteractive* screen_;

  std::mutex mu_;
  std::vector<BtDevice> devs_;
  int sel_ = 0;
  bool powered_ = false;
  bool scanning_ = false;
  bool busy_ = false;
  std::string status_;
  bool status_is_error_ = false;
};

int main() {
  auto screen = ScreenInteractive::Fullscreen();
  BtApp app(&screen);
  Component root = app.Build();
  app.Start();
  screen.Loop(root);
  return 0;
}
