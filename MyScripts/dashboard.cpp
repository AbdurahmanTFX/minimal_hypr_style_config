// dashboard.cpp — Hyprland dashboard TUI, на C++ / FTXUI
// Переписано с dashboard3.c (ncurses) без потери функциональности:
// вкладки SYS / AUDIO / NOTIF / WIN / NOTES, редактор быстрых заметок,
// интеграция с hyprctl/wpctl/pactl/dunstctl/bluetoothctl/nmcli/brightnessctl.
//
// Все внешние команды (включая специфичные для этого конфига Hyprland
// вызовы вида hl.dsp.*(...) и внешний скрипт ~/.config/hypr/audio-parse.py)
// сохранены дословно — рерайт меняет только UI-слой (ncurses → FTXUI) и
// модель исполнения (блокирующий цикл → асинхронные фоновые потоки).
//
// Сборка: см. CMakeLists.txt / Makefile.

#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include "layout_keys.hpp"
#include "net_status.hpp"
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace ftxui;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kTabCount = 5;
enum Tab { TAB_SYS = 0, TAB_AUDIO, TAB_NOTIF, TAB_WIN, TAB_NOTES };
enum NotifMode { NOTIF_NORMAL = 0, NOTIF_SILENT, NOTIF_HIDE };

std::string HomeDir() {
  const char* home = getenv("HOME");
  return home ? home : "";
}

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

constexpr int kSpawnTimeoutMs = 3000;

// Ждёт завершения процесса, но не дольше timeout_ms. Если процесс не
// завершился к дедлайну — убивает его и пожинает в отдельном потоке, но
// САМ вызывающий поток при этом никогда не блокируется дольше timeout_ms,
// что бы ни случилось с дочерним процессом. Обычный blocking waitpid()
// без таймаута — именно то место, где раньше мог зависнуть весь фоновый
// поток-тикер, даже после того как читающий цикл уже получил EOF.
int WaitPidBounded(pid_t pid, int timeout_ms) {
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (r < 0)
      return -1;
    if (Clock::now() >= deadline) {
      kill(pid, SIGKILL);
      std::thread([pid] {
        int st = 0;
        waitpid(pid, &st, 0);
      }).detach();
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Запускает `/bin/sh -c cmd` через posix_spawn и захватывает его stdout.
//
// Раньше здесь использовался popen()/system() (то есть fork()). В этой
// программе есть фоновый поток-тикер, который регулярно запускает внешние
// команды, ПОКА главный поток непрерывно рендерит UI (то есть непрерывно
// аллоцирует память). fork() копирует только вызывающий поток — если в
// момент fork() другой поток удерживал внутренний лок аллокатора glibc,
// дочерний процесс наследует его уже необратимо залоченным и зависает на
// первом же malloc() (классическая ловушка «fork в многопоточной
// программе»). На практике это воспроизводилось стабильно на 2–3-м тике.
// posix_spawn() не дублирует память процесса и этой гонке не подвержен —
// заменяем везде, где раньше был popen()/system().

std::pair<int, std::string> RunSpawn(const std::string& cmd) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return {-1, ""};

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  // Отвязываем дочерний процесс от управляющего терминала (наш pty).
  // Иначе /bin/sh, унаследовав stdin от терминала, может по своей
  // инициативе выполнить tcsetpgrp() как часть включения job-control —
  // а поскольку процесс запускается в ПОСТОЯННО занятом терминале,
  // такой вызов перехватывает "foreground process group" терминала у
  // самой программы. После этого клавиатурный ввод у FTXUI перестаёт
  // доходить до процесса вообще (ядро трактует его как фоновый). На
  // практике это воспроизводилось стабильно после нескольких тиков
  // фонового потока. У /dev/null никакого job-control не бывает.
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                    O_RDONLY, 0);

  char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"),
                   const_cast<char*>(cmd.c_str()), nullptr};
  pid_t pid = -1;
  int rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);
  if (rc != 0) {
    close(pipefd[0]);
    return {-1, ""};
  }

  std::string out;
  char buf[512];
  auto deadline = Clock::now() + std::chrono::milliseconds(kSpawnTimeoutMs);
  bool timed_out = false;
  while (true) {
    auto remaining = deadline - Clock::now();
    int ms_left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                      remaining)
                      .count();
    if (ms_left <= 0) {
      timed_out = true;
      break;
    }
    struct pollfd pfd = {pipefd[0], POLLIN, 0};
    int pr = poll(&pfd, 1, ms_left);
    if (pr == 0) {
      timed_out = true;
      break;
    }
    if (pr < 0)
      break;  // прервано сигналом или ошибка poll — останавливаемся
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n <= 0)
      break;  // EOF или ошибка чтения — команда завершилась
    out.append(buf, static_cast<size_t>(n));
  }
  close(pipefd[0]);

  if (timed_out) {
    // Таймаут — убиваем сам процесс (обычно этого достаточно: у него
    // нет своего процесса-группы, поэтому лишний раз не трогаем job
    // control терминала и здесь).
    kill(pid, SIGKILL);
    std::thread([pid] {
      int status = 0;
      waitpid(pid, &status, 0);
    }).detach();
    return {-1, out};
  }

  int code = WaitPidBounded(pid, kSpawnTimeoutMs);
  return {code, out};
}

std::string RunCapture(const std::string& cmd) {
  return TrimNewline(RunSpawn(cmd).second);
}

// Полный (многострочный) вывод, без обрезки по первой строке.
std::string RunCaptureFull(const std::string& cmd) {
  return RunSpawn(cmd).second;
}

// Фоновый вызов "выстрелил и забыл" — как system(cmd) в оригинале, но через
// posix_spawn (см. комментарий у RunSpawn), с тем же защитным таймаутом и
// той же отвязкой от управляющего терминала.
void FireAndForget(const std::string& cmd) {
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                    O_RDONLY, 0);

  char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"),
                   const_cast<char*>(cmd.c_str()), nullptr};
  pid_t pid = -1;
  int rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0)
    return;
  WaitPidBounded(pid, kSpawnTimeoutMs);
}

// Строка network во вкладке SYS: по кабелю или по Wi-Fi идёт интернет.
std::string NetworkLine(const net::Status& s) {
  auto name = [](const net::Link& l) {
    return l.wired() ? std::string("Кабель") : "Wi-Fi «" + l.connection + "»";
  };
  if (s.links.empty())
    return "  Отключён";
  if (!s.online)
    return "  " + name(s.links[0]) + " — без интернета";
  std::string out = "  " + name(s.primary);
  if (const net::Link* w = s.idle_wifi())
    out += "  (Wi-Fi «" + w->connection + "» в запасе)";
  return out;
}

// ---------------------------------------------------------------- модель --

struct AudioApp {
  std::string id;
  std::string name;
  int vol = 100;
  bool muted = false;
};

struct WinItem {
  std::string addr;
  int ws = 0;
  std::string cls;
  std::string title;
};

struct BgProc {
  std::string name;
  int pid = 0;
  bool running = false;
};

struct NoteFile {
  std::string name;
};

}  // namespace

// ============================================================================

class Dashboard {
 public:
  explicit Dashboard(ScreenInteractive* screen) : screen_(screen) {}

  Component Build() {
    Component main = Renderer([this] { return Render(); });
    main = CatchEvent(main, [this](const Event& e) { return HandleKey(e); });
    return main;
  }

  // Начальная синхронная загрузка (как в оригинале — до входа в цикл).
  void Start() {
    UpdateTime();
    UpdateSys();
    UpdateWorkspace();
    UpdateLayout();
    UpdateBrightness();
    UpdateAudio();
    UpdateBgProcs();
    UpdateNotifMode();
    LoadNotifSound();
    LoadNotes();
    UpdateQuicknote();

    // Просим терминал сообщать о фокусе (ESC[I / ESC[O): скрытому dashboard
    // незачем обновляться, см. TickerLoop.
    fputs("\x1b[?1004h", stdout);
    fflush(stdout);

    ticker_ = std::thread([this] { TickerLoop(); });
    ticker_.detach();
  }

  // Синхронная очистка при выходе (Q, SIGINT, SIGTERM) — выполняется на
  // главном потоке ПОСЛЕ выхода из screen.Loop(), поэтому безопасна
  // (в отличие от вызова файлового I/O прямо в обработчике сигнала).
  void Shutdown() {
    fputs("\x1b[?1004l", stdout);
    fflush(stdout);
    SaveNotes();
    FireAndForget("hyprctl dispatch 'hl.dsp.submap(\"reset\")' >/dev/null 2>&1");
  }

  // Вызывается из обработчика сигнала (SIGINT/SIGTERM). Только просит
  // цикл событий завершиться — вся небезопасная для сигнала работа
  // (файловый I/O и т.п.) выполняется отдельно, в Shutdown(), уже на
  // основном потоке после выхода из screen.Loop().
  void RequestExit() { screen_->ExitLoopClosure()(); }

 private:
  // ------------------------------------------------------------ утилиты --

  void Wake() { screen_->PostEvent(Event::Custom); }

  void OpenPopup(const std::string& cmd) {
    std::string buf =
        "kitty --class dashboard-popup "
        "--override remember_window_size=no "
        "--override initial_window_width=800 "
        "--override initial_window_height=500 "
        "sh -c " +
        ShellQuote(cmd) + " &";
    FireAndForget(buf);
  }

  void Hyprctl(const std::string& args) {
    FireAndForget("hyprctl " + args + " >/dev/null 2>&1 &");
  }

  // -------------------------------------------------------- обновление ---

  void UpdateTime() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    std::lock_guard<std::mutex> lk(mu_);
    time_str_ = buf;
  }

  void UpdateWorkspace() {
    std::string out = RunCapture(
        "hyprctl activeworkspace -j 2>/dev/null | jq -r '.id // 1'");
    int ws_raw = out.empty() ? 1 : atoi(out.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    ws_id_ = ((ws_raw - 1) % 6) + 1;
    int prof = ((ws_raw - 1) / 6) + 1;
    profile_ = "P" + std::to_string(prof);
  }

  // Раскладка клавиатуры, которой пользовались последней ("main" в hyprctl).
  void UpdateLayout() {
    std::string out = RunCapture(
        "hyprctl devices -j 2>/dev/null | jq -r '"
        "first(.keyboards[] | select(.main)) | "
        "\"\\((.layout | split(\",\"))[.active_layout_index])  "
        "\\(.active_keymap)  [\\(.layout)]\"' 2>/dev/null");
    std::lock_guard<std::mutex> lk(mu_);
    layout_ = out.empty() ? "N/A" : out;
  }

  void UpdateBrightness() {
    static const char* cur_paths[] = {
        "/sys/class/backlight/intel_backlight/brightness",
        "/sys/class/backlight/amdgpu_bl1/brightness",
        "/sys/class/backlight/amdgpu_bl0/brightness",
        "/sys/class/backlight/acpi_video0/brightness", nullptr};
    static const char* max_paths[] = {
        "/sys/class/backlight/intel_backlight/max_brightness",
        "/sys/class/backlight/amdgpu_bl1/max_brightness",
        "/sys/class/backlight/amdgpu_bl0/max_brightness",
        "/sys/class/backlight/acpi_video0/max_brightness", nullptr};
    int cur = -1, max = -1;
    for (int i = 0; cur_paths[i]; ++i) {
      FILE* f = fopen(cur_paths[i], "r");
      if (f) {
        char buf[16] = {0};
        if (fgets(buf, sizeof(buf), f)) cur = atoi(buf);
        fclose(f);
        break;
      }
    }
    for (int i = 0; max_paths[i]; ++i) {
      FILE* f = fopen(max_paths[i], "r");
      if (f) {
        char buf[16] = {0};
        if (fgets(buf, sizeof(buf), f)) max = atoi(buf);
        fclose(f);
        break;
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    brightness_ = (cur >= 0 && max > 0) ? (cur * 100 / max) : -1;
  }

  void UpdateSys() {
    time_t now = time(nullptr);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (now - last_full_refresh_ < 5)
        return;
      last_full_refresh_ = now;
    }

    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%A, %d %B %Y", &tm_buf);

    std::string kernel = RunCapture("uname -r");
    std::string uptime = RunCapture("uptime -p | sed 's/up //'");

    std::string cap = RunCapture(
        "cat /sys/class/power_supply/BAT0/capacity 2>/dev/null || echo N/A");
    std::string status =
        RunCapture("cat /sys/class/power_supply/BAT0/status 2>/dev/null");
    std::string battery;
    if (status == "Charging")
      battery = "⚡ " + cap + "%";
    else if (cap != "N/A" && atoi(cap.c_str()) < 20)
      battery = "🪫 " + cap + "%";
    else
      battery = "🔋 " + cap + "%";

    std::string network = NetworkLine(net::ParseStatus(
        RunCapture(net::kDevicesCmd), RunCapture(net::kRoutesCmd)));

    std::string powered = RunCapture(
        "bluetoothctl show 2>/dev/null | grep 'Powered:' | awk '{print "
        "$2}'");
    std::string device = RunCapture(
        "bluetoothctl info 2>/dev/null | grep 'Name:' | head -1 | awk -F': "
        "' '{print $2}'");
    std::string bluetooth;
    if (powered == "yes" && !device.empty())
      bluetooth = "  " + device;
    else if (powered == "yes")
      bluetooth = "  Включён";
    else
      bluetooth = "  Выключен";

    std::lock_guard<std::mutex> lk(mu_);
    date_str_ = date_buf;
    kernel_ = kernel;
    uptime_ = uptime;
    battery_ = battery;
    network_ = network;
    bluetooth_ = bluetooth;
  }

  void UpdateAudio() {
    std::string vol_raw = RunCapture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    bool muted = vol_raw.find("MUTED") != std::string::npos;
    double v = 0;
    sscanf(vol_raw.c_str(), "Volume: %lf", &v);
    int vol = (int)(v * 100);

    std::string script = "python3 " + HomeDir() + "/.config/hypr/audio-parse.py";
    std::string buf = RunCaptureFull(script);

    std::vector<AudioApp> apps;
    // Простой парсер JSON-подобных объектов {..} — как в оригинале.
    size_t p = 0;
    while ((p = buf.find('{', p)) != std::string::npos) {
      size_t end = buf.find('}', p);
      if (end == std::string::npos)
        break;
      std::string obj = buf.substr(p, end - p + 1);
      AudioApp a;
      a.vol = 100;
      a.muted = false;

      auto extract_str = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\":";
        size_t f = obj.find(needle);
        if (f == std::string::npos)
          return "";
        f += needle.size();
        while (f < obj.size() && (obj[f] == ' ' || obj[f] == '"'))
          ++f;
        size_t e = obj.find_first_of("\",}", f);
        if (e == std::string::npos)
          e = obj.size();
        return obj.substr(f, e - f);
      };

      a.id = extract_str("id");
      a.name = extract_str("name");
      std::string vol_s = extract_str("vol");
      if (!vol_s.empty())
        a.vol = atoi(vol_s.c_str());
      std::string muted_s = extract_str("muted");
      a.muted = (muted_s == "true");

      if (!a.id.empty())
        apps.push_back(std::move(a));
      p = end + 1;
    }

    std::lock_guard<std::mutex> lk(mu_);
    master_vol_ = vol;
    master_muted_ = muted;
    // Выделение идёт за приложением, а не за номером строки: потоки появляются
    // и закрываются. Если выбранное закрылось, остаёмся на том же номере или
    // встаём на последнюю строку (без приложений это notif, её индекс как раз -1).
    if (audio_sel_ >= 0) {
      std::string sel_id =
          audio_sel_ < (int)apps_.size() ? apps_[audio_sel_].id : "";
      auto it = std::find_if(apps.begin(), apps.end(), [&](const AudioApp& a) {
        return a.id == sel_id;
      });
      if (it != apps.end())
        audio_sel_ = (int)(it - apps.begin());
      else if (audio_sel_ >= (int)apps.size())
        audio_sel_ = (int)apps.size() - 1;
    }
    apps_ = std::move(apps);
  }

  void UpdateNotifs() {
    static const char* script =
        "dunstctl history 2>/dev/null | python3 -c \""
        "import sys,json;"
        "data=json.load(sys.stdin);"
        "items=data.get('data',[[]])[0][:24];"
        "[print(('['+i.get('appname',{}).get('data','')+'] ' if "
        "i.get('appname',{}).get('data') else '')"
        "+i.get('summary',{}).get('data',''))"
        " for i in items]\"";
    std::string out = RunCaptureFull(script);
    std::vector<std::string> notifs;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
      line = TrimNewline(line);
      if (!line.empty())
        notifs.push_back(line);
    }
    std::lock_guard<std::mutex> lk(mu_);
    notifs_ = std::move(notifs);
  }

  // Режим хранится в dunst (правила silent_mode и hide_mode в dunstrc), а здесь
  // только читается: dunstctl reload и перезапуск dunst сбрасывают его, и
  // вкладка должна показывать то, что действует на самом деле.
  void UpdateNotifMode() {
    std::string on = RunCapture(
        "dunstctl rules --json 2>/dev/null | python3 -c \""
        "import sys,json;"
        "print(' '.join(r['name']['data'] for r in json.load(sys.stdin)['data'][0]"
        " if r['enabled']['data'] and r['name']['data'] in ('silent_mode','hide_mode')))"
        "\" 2>/dev/null");
    NotifMode mode = NOTIF_NORMAL;
    if (on.find("silent_mode") != std::string::npos)
      mode = NOTIF_SILENT;
    else if (on.find("hide_mode") != std::string::npos)
      mode = NOTIF_HIDE;
    std::lock_guard<std::mutex> lk(mu_);
    notif_mode_ = mode;
  }

  // Special-воркспейсы отсеиваются по имени: в Hyprland 0.57 у них в
  // hyprctl clients -j нет поля id, и прежняя проверка id>=0 их пропускала.
  // Панели скрыты и по классу: hidden-apps-view показывается на обычном
  // рабочем столе.
  void UpdateWindows() {
    static const char* script =
        "hyprctl clients -j 2>/dev/null | python3 -c \""
        "import sys,json;"
        "skip={'dashboard','dashboard-term','normal-term','dashboard-popup',"
        "'profile-overview','ws-overview','hidden-apps-view'};"
        "clients=json.load(sys.stdin);"
        "[print(c.get('address','')+'|'+"
        "str(c.get('workspace',{}).get('id',0))+'|'+"
        "c.get('class','')[:30]+'|'+"
        "(c.get('title','') or c.get('class',''))[:50])"
        " for c in clients"
        " if not c.get('workspace',{}).get('name','').startswith('special')"
        " and c.get('class','') not in skip]\"";
    std::string out = RunCaptureFull(script);
    std::vector<WinItem> wins;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
      line = TrimNewline(line);
      if (line.empty())
        continue;
      WinItem w;
      size_t p1 = line.find('|');
      if (p1 == std::string::npos) continue;
      w.addr = line.substr(0, p1);
      size_t p2 = line.find('|', p1 + 1);
      if (p2 == std::string::npos) continue;
      w.ws = atoi(line.substr(p1 + 1, p2 - p1 - 1).c_str());
      size_t p3 = line.find('|', p2 + 1);
      if (p3 == std::string::npos) continue;
      w.cls = line.substr(p2 + 1, p3 - p2 - 1);
      w.title = line.substr(p3 + 1);
      wins.push_back(std::move(w));
    }
    std::lock_guard<std::mutex> lk(mu_);
    wins_ = std::move(wins);
    if (win_sel_ >= (int)wins_.size() && !wins_.empty())
      win_sel_ = (int)wins_.size() - 1;
  }

  void UpdateBgProcs() {
    static const char* known[] = {"nextcloud",     "syncthing", "pipewire",
                                   "wireplumber",   "nm-applet", "blueman-applet",
                                   "dunst",         "awww-daemon", nullptr};
    std::vector<BgProc> procs;
    for (int i = 0; known[i]; ++i) {
      std::string out =
          RunCapture(std::string("pgrep -x ") + ShellQuote(known[i]) +
                     " 2>/dev/null | head -1");
      BgProc p;
      p.name = known[i];
      p.pid = out.empty() ? 0 : atoi(out.c_str());
      p.running = p.pid > 0;
      procs.push_back(std::move(p));
    }
    std::lock_guard<std::mutex> lk(mu_);
    bg_procs_ = std::move(procs);
  }

  // ---------------------------------------------------------- заметки ---

  std::string GetNoteFullPath(const std::string& filename) {
    if (!filename.empty() && filename[0] == '/')
      return filename;
    return HomeDir() + "/Documents/Notes/dashboard/" + filename;
  }

  void NotesInitPath() {
    if (notes_path_.empty())
      notes_path_ = HomeDir() + "/.config/hypr/dashboard_notes.dat";
  }

  void SaveNotes() {
    std::lock_guard<std::mutex> lk(mu_);
    SaveNotesLocked();
  }

  void LoadNotes() {
    NotesInitPath();
    std::vector<NoteFile> files;
    FILE* f = fopen(notes_path_.c_str(), "r");
    if (f) {
      char raw[512];
      while (fgets(raw, sizeof(raw), f)) {
        std::string line = TrimNewline(std::string(raw));
        if (line.rfind("F:", 0) == 0)
          files.push_back({line.substr(2)});
      }
      fclose(f);
    }
    std::lock_guard<std::mutex> lk(mu_);
    note_files_ = std::move(files);
  }

  void OpenNoteInNvim(const std::string& filename) {
    OpenPopup("mkdir -p " + ShellQuote(HomeDir() + "/Documents/Notes/dashboard") +
              " && nvim " + ShellQuote(GetNoteFullPath(filename)));
  }

  // Быструю заметку редактирует nvim, панель только показывает файл, поэтому
  // перечитываем его и видим правки сразу после закрытия редактора.
  void UpdateQuicknote() {
    std::vector<std::string> qn;
    FILE* f = fopen(GetNoteFullPath(kQuicknoteFile).c_str(), "r");
    if (f) {
      char raw[512];
      while (fgets(raw, sizeof(raw), f)) {
        std::string line = TrimNewline(std::string(raw));
        std::replace(line.begin(), line.end(), '\t', ' ');
        qn.push_back(std::move(line));
      }
      fclose(f);
    }
    std::lock_guard<std::mutex> lk(mu_);
    quicknote_ = std::move(qn);
  }

  // ------------------------------------------------- звук уведомлений ---
  // Файл читает notif-sound.sh, который запускает dunst.

  std::string NotifSoundPath() {
    return HomeDir() + "/.config/hypr/notif_sound.dat";
  }

  void LoadNotifSound() {
    int vol = notif_vol_;
    int muted = notif_muted_ ? 1 : 0;
    FILE* f = fopen(NotifSoundPath().c_str(), "r");
    if (f) {
      char line[64];
      while (fgets(line, sizeof(line), f)) {
        sscanf(line, "volume=%d", &vol);
        sscanf(line, "muted=%d", &muted);
      }
      fclose(f);
    }
    std::lock_guard<std::mutex> lk(mu_);
    notif_vol_ = std::clamp(vol, 0, 100);
    notif_muted_ = muted != 0;
  }

  void SaveNotifSoundLocked() {
    FILE* f = fopen(NotifSoundPath().c_str(), "w");
    if (!f)
      return;
    fprintf(f, "volume=%d\nmuted=%d\n", notif_vol_, notif_muted_ ? 1 : 0);
    fclose(f);
  }

  void ChangeNotifVolume(int delta) {
    bool preview;
    {
      std::lock_guard<std::mutex> lk(mu_);
      notif_vol_ = std::clamp(notif_vol_ + delta, 0, 100);
      SaveNotifSoundLocked();
      preview = (notif_mode_ != NOTIF_SILENT);
    }
    if (preview)
      FireAndForget(HomeDir() +
                    "/.config/hypr/MyScripts/notif-sound.sh >/dev/null 2>&1 &");
  }

  void ToggleNotifMute() {
    std::lock_guard<std::mutex> lk(mu_);
    notif_muted_ = !notif_muted_;
    SaveNotifSoundLocked();
  }

  // -------------------------------------------------------- фоновый тик --

  void TickerLoop() {
    int counter = 0;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      // Окна не видно (или поверх него открыт nvim): не тратим батарею.
      if (!focused_)
        continue;
      ++counter;

      UpdateTime();
      UpdateWorkspace();

      Tab tab;
      {
        std::lock_guard<std::mutex> lk(mu_);
        tab = cur_tab_;
      }

      switch (tab) {
        case TAB_AUDIO:
          UpdateAudio();
          if (counter % 3 == 0) UpdateNotifMode();
          break;
        case TAB_NOTES:
          UpdateQuicknote();
          break;
        case TAB_NOTIF:
          if (counter % 3 == 0) { UpdateNotifs(); UpdateNotifMode(); }
          break;
        case TAB_WIN:
          if (counter % 2 == 0) { UpdateWindows(); UpdateBgProcs(); }
          break;
        case TAB_SYS:
          UpdateLayout();
          if (counter % 2 == 0) UpdateBrightness();
          if (counter % 8 == 0) UpdateSys();
          break;
        default:
          break;
      }
      if (tab != TAB_SYS && counter % 10 == 0)
        UpdateSys();

      if (counter >= 120)
        counter = 0;
      Wake();
    }
  }

  // Пока окна не было видно, данные не обновлялись: показываем свежие сразу.
  void RefreshAfterFocus() {
    UpdateTime();
    UpdateWorkspace();
    UpdateLayout();
    UpdateNotifMode();
    Tab tab;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tab = cur_tab_;
    }
    switch (tab) {
      case TAB_SYS:   UpdateBrightness(); UpdateSys();            break;
      case TAB_AUDIO: UpdateAudio();                              break;
      case TAB_NOTIF: UpdateNotifs();                             break;
      case TAB_WIN:   UpdateWindows(); UpdateBgProcs();           break;
      case TAB_NOTES: UpdateQuicknote();                          break;
    }
    Wake();
  }

  // -------------------------------------------------------- действия ----

  void SwitchTab(Tab t) {
    std::lock_guard<std::mutex> lk(mu_);
    cur_tab_ = t;
    audio_off_ = 0;
    notif_off_ = 0;
  }

  void HandleEnter() {
    Tab tab;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tab = cur_tab_;
    }
    switch (tab) {
      case TAB_SYS: {
        int sel;
        {
          std::lock_guard<std::mutex> lk(mu_);
          sel = sys_sel_;
        }
        if (sel == 0) OpenPopup("~/.config/hypr/wifi-popup");
        else if (sel == 1) OpenPopup("~/.config/hypr/bt-popup");
        break;
      }
      case TAB_WIN: {
        WinItem w;
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (win_sel_ >= (int)wins_.size())
            return;
          w = wins_[win_sel_];
        }
        FireAndForget(
            "hyprctl dispatch 'hl.dsp.submap(\"reset\")' >/dev/null 2>&1");
        FireAndForget(
            "hyprctl dispatch 'hl.dsp.window.move({ workspace = "
            "\"special:dashboard\", window = \"class:dashboard\", follow = "
            "false })' >/dev/null 2>&1");
        std::string cmd = "sleep 0.1 && hyprctl dispatch 'hl.dsp.focus({ "
                           "workspace = " +
                           std::to_string(w.ws) +
                           " })' && hyprctl dispatch 'hl.dsp.focus({ window "
                           "= \"address:" +
                           w.addr + "\" })'";
        FireAndForget(cmd);
        break;
      }
      case TAB_NOTES: {
        std::string fname;
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (note_sel_ < (int)note_files_.size())
            fname = note_files_[note_sel_].name;
        }
        if (!fname.empty())
          OpenNoteInNvim(fname);
        break;
      }
      default:
        break;
    }
  }

  // Повторное нажатие той же клавиши возвращает обычный режим.
  void ToggleNotifMode(NotifMode m) {
    NotifMode next;
    {
      std::lock_guard<std::mutex> lk(mu_);
      next = (notif_mode_ == m) ? NOTIF_NORMAL : m;
      notif_mode_ = next;
    }
    FireAndForget(std::string("dunstctl rule silent_mode ") +
                  (next == NOTIF_SILENT ? "enable" : "disable") +
                  " >/dev/null 2>&1; dunstctl rule hide_mode " +
                  (next == NOTIF_HIDE ? "enable" : "disable") +
                  " >/dev/null 2>&1");
  }

  void HandleDelete() {
    Tab tab;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tab = cur_tab_;
    }
    switch (tab) {
      case TAB_NOTIF:
        FireAndForget("dunstctl history-clear");
        std::thread([this] { UpdateNotifs(); Wake(); }).detach();
        break;
      case TAB_WIN: {
        std::string addr;
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (win_sel_ >= (int)wins_.size())
            return;
          addr = wins_[win_sel_].addr;
        }
        FireAndForget("hyprctl dispatch 'hl.dsp.window.close({ window = "
                       "\"address:" +
                       addr + "\" })'");
        std::thread([this] {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          UpdateWindows();
          Wake();
        }).detach();
        break;
      }
      case TAB_NOTES: {
        std::lock_guard<std::mutex> lk(mu_);
        if (note_files_.empty())
          return;
        note_files_.erase(note_files_.begin() + note_sel_);
        if (note_sel_ >= (int)note_files_.size() && note_sel_ > 0)
          note_sel_--;
        SaveNotesLocked();
        break;
      }
      default:
        break;
    }
  }

  // Сохраняет заметки, когда мьютекс УЖЕ захвачен вызывающим (в отличие от
  // SaveNotes(), которая захватывает его сама).
  void SaveNotesLocked() {
    NotesInitPath();
    FILE* f = fopen(notes_path_.c_str(), "w");
    if (!f)
      return;
    for (auto& nf : note_files_)
      fprintf(f, "F:%s\n", nf.name.c_str());
    fclose(f);
  }

  void NavDown() {
    std::lock_guard<std::mutex> lk(mu_);
    switch (cur_tab_) {
      case TAB_SYS: sys_sel_ = (sys_sel_ + 1) % 3; break;
      case TAB_AUDIO:
        if (audio_sel_ < (int)apps_.size() - 1) audio_sel_++;
        break;
      case TAB_NOTIF: notif_off_++; break;
      case TAB_WIN:
        if (win_sel_ < (int)wins_.size() - 1) win_sel_++;
        break;
      case TAB_NOTES:
        if (note_sel_ < (int)note_files_.size() - 1) note_sel_++;
        break;
    }
  }

  void NavUp() {
    std::lock_guard<std::mutex> lk(mu_);
    switch (cur_tab_) {
      case TAB_SYS: sys_sel_ = (sys_sel_ + 2) % 3; break;
      case TAB_AUDIO:
        if (audio_sel_ > kAudioMaster) audio_sel_--;
        break;
      case TAB_NOTIF: if (notif_off_ > 0) notif_off_--; break;
      case TAB_WIN: if (win_sel_ > 0) win_sel_--; break;
      case TAB_NOTES:
        if (note_sel_ > 0) note_sel_--;
        break;
    }
  }

  void ActionLeft() {
    std::string audio_id;
    bool is_master = false;
    bool is_notif = false;
    bool did_something = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (cur_tab_ == TAB_SYS) {
        did_something = true;
      } else if (cur_tab_ == TAB_AUDIO) {
        is_master = (audio_sel_ == kAudioMaster);
        is_notif = (audio_sel_ == kAudioNotifSound);
        if (is_master || is_notif || audio_sel_ < (int)apps_.size()) {
          if (!is_master && !is_notif) audio_id = apps_[audio_sel_].id;
          did_something = true;
        }
      }
    }
    if (!did_something) return;
    if (is_notif) { ChangeNotifVolume(-5); return; }
    if (is_master)
      FireAndForget("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- >/dev/null 2>&1");
    else if (!audio_id.empty())
      FireAndForget("pactl set-sink-input-volume " + ShellQuote(audio_id) +
                     " -5% >/dev/null 2>&1");
    else
      FireAndForget("brightnessctl -e4 -n2 set 5%- >/dev/null 2>&1");
    UpdateAudio();
    UpdateBrightness();
  }

  void ActionRight() {
    std::string audio_id;
    bool is_master = false;
    bool is_notif = false;
    bool did_something = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (cur_tab_ == TAB_SYS) {
        did_something = true;
      } else if (cur_tab_ == TAB_AUDIO) {
        is_master = (audio_sel_ == kAudioMaster);
        is_notif = (audio_sel_ == kAudioNotifSound);
        if (is_master || is_notif || audio_sel_ < (int)apps_.size()) {
          if (!is_master && !is_notif) audio_id = apps_[audio_sel_].id;
          did_something = true;
        }
      }
    }
    if (!did_something) return;
    if (is_notif) { ChangeNotifVolume(5); return; }
    if (is_master)
      FireAndForget(
          "wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+ >/dev/null 2>&1");
    else if (!audio_id.empty())
      FireAndForget("pactl set-sink-input-volume " + ShellQuote(audio_id) +
                     " +5% >/dev/null 2>&1");
    else
      FireAndForget("brightnessctl -e4 -n2 set 5%+ >/dev/null 2>&1");
    UpdateAudio();
    UpdateBrightness();
  }

  void ActionMute() {
    std::string audio_id;
    bool is_master = false;
    bool is_notif = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (cur_tab_ == TAB_NOTIF) {
        is_notif = true;
      } else if (cur_tab_ != TAB_AUDIO) {
        return;
      } else {
        is_master = (audio_sel_ == kAudioMaster);
        is_notif = (audio_sel_ == kAudioNotifSound);
        if (!is_master && !is_notif) {
          if (audio_sel_ >= (int)apps_.size()) return;
          audio_id = apps_[audio_sel_].id;
        }
      }
    }
    if (is_notif) { ToggleNotifMute(); return; }
    if (is_master)
      FireAndForget("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle >/dev/null 2>&1");
    else
      FireAndForget("pactl set-sink-input-mute " + ShellQuote(audio_id) +
                     " toggle >/dev/null 2>&1");
    UpdateAudio();
  }

  // ------------------------------------------------ модалка "имя файла" --

  void OpenAddFileModal() {
    std::lock_guard<std::mutex> lk(mu_);
    add_file_modal_ = true;
    add_file_buffer_.clear();
  }

  void SubmitAddFile() {
    std::string fname;
    {
      std::lock_guard<std::mutex> lk(mu_);
      add_file_modal_ = false;
      fname = add_file_buffer_;
      add_file_buffer_.clear();
    }
    if (fname.empty())
      return;

    std::string home = HomeDir();
    FireAndForget("mkdir -p " + ShellQuote(home + "/Documents/Notes/dashboard"));
    std::string full_path = GetNoteFullPath(fname);
    FILE* f = fopen(full_path.c_str(), "w");
    if (!f)
      return;
    fclose(f);

    std::lock_guard<std::mutex> lk(mu_);
    if ((int)note_files_.size() < kMaxNoteFiles) {
      note_files_.push_back({fname});
      note_sel_ = (int)note_files_.size() - 1;
      SaveNotesLocked();
    }
  }

  void CancelAddFile() {
    std::lock_guard<std::mutex> lk(mu_);
    add_file_modal_ = false;
    add_file_buffer_.clear();
  }

  // -------------------------------------------------------- клавиши -----

  bool HandleKey(const Event& event) {
    // Потеря фокуса: FTXUI превращает ESC[O в тот же код, что и у клавиши F3.
    if (event == Event::F3) {
      focused_ = false;
      return true;
    }
    if (event.input() == "\x1b[I") {
      focused_ = true;
      std::thread([this] { RefreshAfterFocus(); }).detach();
      return true;
    }
    // Пришло нажатие, значит фокус есть: настоящий F3 не должен заморозить экран.
    if (!focused_ && event != Event::Custom)
      focused_ = true;

    bool add_modal;
    {
      std::lock_guard<std::mutex> lk(mu_);
      add_modal = add_file_modal_;
    }

    // -- модалка "имя файла" поглощает всё --
    if (add_modal) {
      if (event == Event::Escape) { CancelAddFile(); return true; }
      if (event == Event::Return) { SubmitAddFile(); return true; }
      if (event == Event::Backspace) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!add_file_buffer_.empty()) add_file_buffer_.pop_back();
        return true;
      }
      if (event.is_character()) {
        std::lock_guard<std::mutex> lk(mu_);
        add_file_buffer_ += event.character();
        return true;
      }
      return true;
    }

    return HandleCommand(LatinKey(event));
  }

  // Клавиши вне текстового ввода, русская раскладка уже переведена в латиницу.
  bool HandleCommand(const Event& event) {
    // // -- отложенный ESC: ESC+h / ESC+l = переключение вкладки (Alt+h/l в
    // //    большинстве терминалов приходит как два байта ESC, 'h'/'l') --
    // if (pending_escape_) {
    //   pending_escape_ = false;
    //   bool too_late = Clock::now() - escape_time_ > std::chrono::milliseconds(120);
    //   if (!too_late && event == Event::Character('h')) {
    //     Tab t;
    //     { std::lock_guard<std::mutex> lk(mu_); t = (Tab)((cur_tab_ + kTabCount - 1) % kTabCount); }
    //     SwitchTab(t);
    //     return true;
    //   }
    //   if (!too_late && event == Event::Character('l')) {
    //     Tab t;
    //     { std::lock_guard<std::mutex> lk(mu_); t = (Tab)((cur_tab_ + 1) % kTabCount); }
    //     SwitchTab(t);
    //     return true;
    //   }
    //   // Не пара ESC+h/l — обычный самостоятельный Escape уже обработан
    //   // ниже (он лишь выходил из режима редактирования заметок, если тот
    //   // был активен — это уже проверено выше). Продолжаем обработку
    //   // ТЕКУЩЕГО события как обычной клавиши.
    // }
    //
    // if (event == Event::Escape) {
    //   pending_escape_ = true;
    //   escape_time_ = Clock::now();
    //   return true;
    // }

    // -- Переключение вкладок через Alt+h / Alt+l --
    std::string raw = event.input();

    if (raw == "\x1bh") {
      Tab t;
      { std::lock_guard<std::mutex> lk(mu_); t = (Tab)((cur_tab_ + kTabCount - 1) % kTabCount); }
      SwitchTab(t);
      return true;
    }

    if (raw == "\x1bl") {
      Tab t;
      { std::lock_guard<std::mutex> lk(mu_); t = (Tab)((cur_tab_ + 1) % kTabCount); }
      SwitchTab(t);
      return true;
    }

    if (event == Event::Character('q') || event == Event::Character('Q')) {
      screen_->ExitLoopClosure()();
      return true;
    }
    if (event == Event::Character('1')) { SwitchTab(TAB_SYS);   return true; }
    if (event == Event::Character('2')) { SwitchTab(TAB_AUDIO); return true; }
    if (event == Event::Character('3')) { SwitchTab(TAB_NOTIF); return true; }
    if (event == Event::Character('4')) { SwitchTab(TAB_WIN);   return true; }
    if (event == Event::Character('5')) { SwitchTab(TAB_NOTES); return true; }

    // Только в NOTIF: S — тихий режим, C — режим с короткой всплывашкой,
    // H/L и стрелки — громкость звука уведомлений.
    bool in_notif;
    {
      std::lock_guard<std::mutex> lk(mu_);
      in_notif = (cur_tab_ == TAB_NOTIF);
    }
    if (in_notif && (event == Event::Character('s') || event == Event::Character('S'))) {
      ToggleNotifMode(NOTIF_SILENT);
      return true;
    }
    if (in_notif && (event == Event::Character('c') || event == Event::Character('C'))) {
      ToggleNotifMode(NOTIF_HIDE);
      return true;
    }
    if (in_notif && (event == Event::Character('h') || event == Event::Character('H') ||
                     event == Event::ArrowLeft)) {
      ChangeNotifVolume(-5);
      return true;
    }
    if (in_notif && (event == Event::Character('l') || event == Event::Character('L') ||
                     event == Event::ArrowRight)) {
      ChangeNotifVolume(5);
      return true;
    }

    if (event == Event::Character('j') || event == Event::ArrowDown) { NavDown(); return true; }
    if (event == Event::Character('k') || event == Event::ArrowUp)   { NavUp();   return true; }
    if (event == Event::Character('h') || event == Event::ArrowLeft)  { ActionLeft();  return true; }
    if (event == Event::Character('l') || event == Event::ArrowRight) { ActionRight(); return true; }
    if (event == Event::Character('m') || event == Event::Character('M')) { ActionMute(); return true; }
    if (event == Event::Character('d') || event == Event::Character('D')) { HandleDelete(); return true; }
    if (event == Event::Return) { HandleEnter(); return true; }

    // Только в NOTES: A — добавить файл, Tab — быстрая заметка в nvim.
    bool in_notes;
    {
      std::lock_guard<std::mutex> lk(mu_);
      in_notes = (cur_tab_ == TAB_NOTES);
    }
    if (in_notes && (event == Event::Character('a') || event == Event::Character('A'))) {
      OpenAddFileModal();
      return true;
    }
    if (in_notes && event == Event::Tab) {
      OpenNoteInNvim(kQuicknoteFile);
      return true;
    }

    return false;
  }

  // ---------------------------------------------------------- отрисовка -

  Element KV(const std::string& key, const std::string& val, Color val_color) {
    return hbox({
        text(" " + key) | size(WIDTH, EQUAL, 15) | color(Color::Cyan),
        text("= ") | color(Color::Cyan),
        text(val) | color(val_color),
    });
  }

  Element RenderTabBar() {
    static const char* names[] = {"SYS", "AUDIO", "NOTIF", "WIN", "NOTES"};
    Elements tabs;
    for (int i = 0; i < kTabCount; ++i) {
      std::string label = std::to_string(i + 1) + ":" + names[i];
      bool active = (i == cur_tab_);
      Element el = text(" " + label + " ") |
                   (active ? (bgcolor(Color::Blue) | color(Color::White) | bold)
                           : dim);
      tabs.push_back(el);
      if (i < kTabCount - 1) tabs.push_back(text("│") | dim);
    }
    std::string info = " " + time_str_ + "  ws_" + std::to_string(ws_id_) +
                        "·" + profile_ + " ";
    return hbox({
        hbox(std::move(tabs)),
        filler(),
        text(info) | dim,
    });
  }

  Element RenderSys() {
    Elements rows;
    rows.push_back(text(time_str_) | color(Color::Yellow) | bold);
    rows.push_back(text(date_str_) | color(Color::Green));
    rows.push_back(text("kernel: " + kernel_ + "  •  up: " + uptime_) | dim);
    rows.push_back(separator());
    rows.push_back(KV("battery", battery_, Color::Green));
    rows.push_back(separator());

    const char* labels[] = {"network", "bluetooth", "brightness"};
    std::string bri = (brightness_ >= 0) ? std::to_string(brightness_) + "%" : "N/A";
    std::string vals[] = {network_, bluetooth_, bri};
    for (int i = 0; i < 3; ++i) {
      bool sel = (i == sys_sel_);
      rows.push_back(hbox({
          text(sel ? "▶ " : "  ") | (sel ? color(Color::Cyan) | bold : nothing),
          text(labels[i]) | size(WIDTH, EQUAL, 15) |
              (sel ? color(Color::Cyan) | bold : color(Color::White)),
          text("= ") | color(Color::Cyan),
          text(vals[i]) | color(Color::White),
      }));
    }
    rows.push_back(separator());
    rows.push_back(KV("workspace", "ws_" + std::to_string(ws_id_), Color::White));
    rows.push_back(KV("profile", "\"" + profile_ + "\"", Color::White));
    rows.push_back(KV("layout", layout_, Color::White));

    rows.push_back(filler());
    rows.push_back(text("J/K nav  H/L brightness  Enter open  1-5 tab  Q quit") | dim);
    return vbox(std::move(rows));
  }

  // "[|||||     ] 50%" и пометка [M]: общие для строк AUDIO и настройки в NOTIF.
  Elements VolumeCells(int vol, bool muted, bool sel) {
    const int bar_len = 20;
    int filled = std::clamp(vol * bar_len / 100, 0, bar_len);
    std::string bar(filled, '|');
    bar += std::string(bar_len - filled, ' ');
    Elements cells = {
        text("[" + bar + "]") | color(sel ? Color::Cyan : Color::Green),
        text(" " + std::to_string(vol) + "%") | color(Color::White),
    };
    if (muted) cells.push_back(text(" [M]") | color(Color::Red));
    return cells;
  }

  Elements NotifSoundCells(bool sel) {
    Elements cells = VolumeCells(notif_vol_, notif_muted_, sel);
    if (notif_mode_ == NOTIF_SILENT)
      cells.push_back(text(" [silent]") | color(Color::Red));
    return cells;
  }

  Element RenderAudio() {
    Elements rows;
    bool is_master = (audio_sel_ == kAudioMaster);
    bool is_notif = (audio_sel_ == kAudioNotifSound);
    Elements master_row = {
        text(is_master ? "▶ " : "  ") | (is_master ? color(Color::Cyan) | bold : nothing),
        text("master") | size(WIDTH, EQUAL, 15) |
            (is_master ? color(Color::Cyan) | bold : color(Color::White)),
        text("= ") | color(Color::Cyan),
        text(std::to_string(master_vol_) + "%") | color(Color::White),
    };
    if (master_muted_) master_row.push_back(text("  [MUTED]") | color(Color::Red));
    rows.push_back(hbox(std::move(master_row)));
    rows.push_back(hbox({
        text(is_notif ? "▶ " : "  ") | (is_notif ? color(Color::Cyan) | bold : nothing),
        text("notif") | size(WIDTH, EQUAL, 15) |
            (is_notif ? color(Color::Cyan) | bold : color(Color::White)),
        text("= ") | color(Color::Cyan),
        hbox(NotifSoundCells(is_notif)),
    }));
    rows.push_back(separator());

    if (!apps_.empty()) rows.push_back(text("// apps") | dim);

    if (apps_.empty()) {
      rows.push_back(text("• нет активных потоков") | dim);
    } else {
      for (int i = 0; i < (int)apps_.size(); ++i) {
        const AudioApp& a = apps_[i];
        bool sel = (audio_sel_ == i);

        std::string name_t = a.name;
        if (name_t.size() > 18) name_t.resize(18);

        rows.push_back(hbox({
            text(sel ? "▶ " : "  ") | (sel ? color(Color::Cyan) | bold : nothing),
            text(name_t) | size(WIDTH, EQUAL, 19) |
                (sel ? color(Color::Cyan) | bold : color(Color::White)),
            text("= ") | color(Color::Cyan),
            hbox(VolumeCells(a.vol, a.muted, sel)),
        }));
      }
    }
    rows.push_back(filler());
    rows.push_back(text("J/K nav  H/L volume  M mute  1-5 tab  Q quit") | dim);
    return vbox(std::move(rows));
  }

  Element RenderNotif() {
    Elements rows;
    switch (notif_mode_) {
      case NOTIF_NORMAL: rows.push_back(KV("mode", "normal", Color::White));  break;
      case NOTIF_SILENT: rows.push_back(KV("mode", "silent", Color::Red));    break;
      case NOTIF_HIDE:   rows.push_back(KV("mode", "hide",   Color::Yellow)); break;
    }
    rows.push_back(hbox({
        text(" sound") | size(WIDTH, EQUAL, 15) | color(Color::Cyan),
        text("= ") | color(Color::Cyan),
        hbox(NotifSoundCells(false)),
    }));
    rows.push_back(separator());
    if (notifs_.empty()) {
      rows.push_back(text("• нет уведомлений") | dim);
    } else {
      int off = notif_off_;
      if (off > (int)notifs_.size() - 1) off = (int)notifs_.size() - 1;
      if (off < 0) off = 0;
      for (int i = off; i < (int)notifs_.size(); ++i) {
        rows.push_back(hbox({
            text("• ") | color(Color::Cyan),
            text(notifs_[i]) | dim,
        }));
      }
    }
    rows.push_back(filler());
    rows.push_back(text("J/K scroll  H/L sound  M mute  S silent  C hide  D clear  1-5 tab  Q quit") | dim);
    return vbox(std::move(rows)) | vscroll_indicator | frame;
  }

  Element RenderWindows() {
    Elements rows;
    if (wins_.empty()) {
      rows.push_back(text("нет окон") | dim);
    } else {
      for (int i = 0; i < (int)wins_.size(); ++i) {
        const WinItem& w = wins_[i];
        bool sel = (i == win_sel_);
        rows.push_back(hbox({
            text(sel ? "▶ " : "  ") | (sel ? color(Color::Cyan) | bold : nothing),
            text("ws" + std::to_string(w.ws)) | size(WIDTH, EQUAL, 6) | color(Color::Cyan),
            text(" › ") | dim,
            text(w.title) | flex | (sel ? color(Color::Cyan) | bold : color(Color::White)),
            text("[" + w.cls + "]") | dim,
        }));
      }
    }
    rows.push_back(separator());
    rows.push_back(text("// background") | dim);
    for (auto& p : bg_procs_) {
      if (p.running) {
        rows.push_back(hbox({
            text("◉ ") | color(Color::Cyan),
            text(p.name) | size(WIDTH, EQUAL, 20) | color(Color::White),
            text("pid:" + std::to_string(p.pid)) | dim,
        }));
      } else {
        rows.push_back(text("○ " + p.name) | dim);
      }
    }
    rows.push_back(filler());
    rows.push_back(text("J/K nav  Enter focus  D close  1-5 tab  Q quit") | dim);
    return vbox(std::move(rows)) | vscroll_indicator | frame;
  }

  Element RenderNotes() {
    Elements left;
    if (note_files_.empty()) {
      left.push_back(text("* пусто  (A — добавить)") | dim);
    } else {
      for (int i = 0; i < (int)note_files_.size(); ++i) {
        bool sel = (i == note_sel_);
        left.push_back(text((sel ? "> " : "  ") + note_files_[i].name) |
                        (sel ? color(Color::Cyan) | bold : color(Color::White)));
      }
    }

    Elements right;
    bool qn_empty = quicknote_.empty() ||
                    (quicknote_.size() == 1 && quicknote_[0].empty());
    if (qn_empty) {
      right.push_back(text("Tab — открыть в nvim") | dim);
    } else {
      for (const std::string& line : quicknote_)
        right.push_back(text(line) | color(Color::White));
    }

    return vbox({
               hbox({
                   vbox({text("// files") | dim, separatorEmpty(),
                         vbox(std::move(left))}) |
                       size(WIDTH, EQUAL, kNotesFilesWidth),
                   separator(),
                   vbox({text("// quick note") | dim, separatorEmpty(),
                         vbox(std::move(right))}) |
                       flex,
               }) | flex,
               filler(),
               text("J/K nav  A add  D del  Enter open  Tab quick note  1-5 tab  Q quit") | dim,
           });
  }

  // Вызывается ТОЛЬКО изнутри Render(), которая уже держит mu_ — поэтому
  // здесь захватывать его повторно нельзя (std::mutex не реентрантный,
  // повторный lock на том же потоке — это гарантированный self-deadlock;
  // именно так раньше намертво вис главный поток сразу после открытия
  // этого диалога).
  Element RenderModal() {
    const std::string& buf = add_file_buffer_;
    return vbox({
               text(" Имя файла заметки ") | bold,
               separator(),
               hbox({text(buf) | color(Color::White), text("▏") | blink}),
               separatorEmpty(),
               text("Enter — создать   Esc — отмена") | dim,
           }) |
           border | bgcolor(Color::Black) | color(Color::White) |
           size(WIDTH, EQUAL, 44);
  }

  Element Render() {
    std::lock_guard<std::mutex> lk(mu_);
    Element body;
    switch (cur_tab_) {
      case TAB_SYS:   body = RenderSys();     break;
      case TAB_AUDIO: body = RenderAudio();   break;
      case TAB_NOTIF: body = RenderNotif();   break;
      case TAB_WIN:   body = RenderWindows(); break;
      case TAB_NOTES: body = RenderNotes();   break;
    }
    Element base = vbox({
                       RenderTabBar(),
                       separator(),
                       body | flex,
                   }) |
                   size(WIDTH, GREATER_THAN, 70) | size(HEIGHT, GREATER_THAN, 20);
    if (!add_file_modal_)
      return base;
    return dbox({base, RenderModal() | clear_under | center});
  }

  static constexpr int kMaxNoteFiles = 256;
  static constexpr int kNotesFilesWidth = 26;
  static constexpr const char* kQuicknoteFile = "quicknote.txt";

  // Строки вкладки AUDIO над списком приложений; приложения идут с индекса 0.
  static constexpr int kAudioMaster = -2;
  static constexpr int kAudioNotifSound = -1;

  ScreenInteractive* screen_;
  std::mutex mu_;
  // Считаем окно видимым, пока терминал не скажет обратное: если сообщений о
  // фокусе не будет, dashboard просто работает как раньше.
  std::atomic<bool> focused_{true};
  std::thread ticker_;

  Tab cur_tab_ = TAB_SYS;
  int sys_sel_ = 0;
  int audio_sel_ = kAudioMaster;
  int win_sel_ = 0;
  int notif_off_ = 0;
  int audio_off_ = 0;
  int note_sel_ = 0;

  time_t last_full_refresh_ = 0;
  std::string time_str_, date_str_, kernel_, uptime_, battery_, network_, bluetooth_;
  std::string layout_ = "N/A";
  int brightness_ = -1;

  int master_vol_ = 0;
  bool master_muted_ = false;
  std::vector<AudioApp> apps_;

  std::vector<std::string> notifs_;
  NotifMode notif_mode_ = NOTIF_NORMAL;
  int notif_vol_ = 50;
  bool notif_muted_ = false;
  std::vector<WinItem> wins_;
  std::vector<BgProc> bg_procs_;

  int ws_id_ = 1;
  std::string profile_ = "P1";

  std::vector<NoteFile> note_files_;
  std::vector<std::string> quicknote_;
  std::string notes_path_;

  bool add_file_modal_ = false;
  std::string add_file_buffer_;

  bool pending_escape_ = false;
  Clock::time_point escape_time_;
};

namespace {
Dashboard* g_app = nullptr;
void SignalHandler(int) {
  if (g_app)
    g_app->RequestExit();
}
}  // namespace

int main() {
  auto screen = ScreenInteractive::Fullscreen();
  Dashboard app(&screen);
  g_app = &app;
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  signal(SIGHUP, SIG_IGN);

  Component root = app.Build();
  app.Start();
  screen.Loop(root);

  app.Shutdown();
  return 0;
}
