// net_status.hpp — через что сейчас идёт интернет: по кабелю или по Wi-Fi.
//
// Кабель и Wi-Fi могут быть подключены одновременно, а трафик идёт только через
// один из них: через тот, у чьего маршрута по умолчанию меньше метрика
// (NetworkManager ставит кабелю 100, Wi-Fi 600). Поэтому одного nmcli мало,
// нужен ещё ip route.
//
// Команды запускает вызывающий код (у dashboard и wifi-popup свои RunCapture),
// здесь только разбор их вывода.
#pragma once

#include <climits>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace net {

inline const char* const kDevicesCmd =
    "nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev status 2>/dev/null";
inline const char* const kRoutesCmd =
    "ip route show default 2>/dev/null; ip -6 route show default 2>/dev/null";

struct Link {
  std::string device;      // enp2s0, wlan1
  std::string type;        // "ethernet" или "wifi"
  std::string connection;  // профиль NetworkManager; у Wi-Fi обычно это SSID
  bool wired() const { return type == "ethernet"; }
  bool wifi() const { return type == "wifi"; }
};

struct Status {
  std::vector<Link> links;  // все подключённые кабели и Wi-Fi
  bool online = false;      // у одного из links есть маршрут по умолчанию
  Link primary;             // через него идёт интернет, если online

  // Подключённый Wi-Fi, через который интернет сейчас не идёт (запасной).
  const Link* idle_wifi() const {
    for (auto& l : links)
      if (l.wifi() && !(online && l.device == primary.device)) return &l;
    return nullptr;
  }
};

// Строка nmcli -t: поля через ':', а ':' и '\' внутри значений экранированы '\'.
inline std::vector<std::string> SplitNmcli(const std::string& line) {
  std::vector<std::string> fields(1);
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size())
      fields.back() += line[++i];
    else if (line[i] == ':')
      fields.emplace_back();
    else
      fields.back() += line[i];
  }
  return fields;
}

inline Status ParseStatus(const std::string& devices, const std::string& routes) {
  Status s;
  std::istringstream dev_in(devices);
  std::string line;
  while (std::getline(dev_in, line)) {
    auto f = SplitNmcli(line);
    // "connected (externally)" у lo и tailscale0 сюда не попадает: нужен
    // именно физический канал.
    if (f.size() < 4 || f[2] != "connected") continue;
    if (f[1] != "ethernet" && f[1] != "wifi") continue;
    s.links.push_back({f[0], f[1], f[3]});
  }

  long best = LONG_MAX;
  std::istringstream route_in(routes);
  while (std::getline(route_in, line)) {
    std::istringstream words(line);
    std::string w, dev;
    long metric = 0;  // без "metric" в строке метрика равна 0
    while (words >> w) {
      if (w == "dev") words >> dev;
      else if (w == "metric" && words >> w) metric = strtol(w.c_str(), nullptr, 10);
    }
    if (metric >= best) continue;
    for (auto& l : s.links) {
      if (l.device == dev) {
        best = metric;
        s.online = true;
        s.primary = l;
      }
    }
  }
  return s;
}

}  // namespace net
