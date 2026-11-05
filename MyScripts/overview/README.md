# profile-overview / ws-overview — C++ / Cairo / wlr-layer-shell

Полная переработка `profile-overview.c` и `ws-overview.c`: тот же функционал,
но без GTK4. Причина отказа от изначальной идеи «C++ + FTXUI» — FTXUI это
терминальный (TUI) фреймворк, он не умеет создавать Wayland-оверлеи и не
рисует растровые PNG-превью; после обсуждения решили, что для «максимально
быстро и отзывчиво» лучше вариант — оставить растровый рендеринг (Cairo, как
и было), но убрать GTK4 и рисовать напрямую через протокол
`wlr-layer-shell-unstable-v1` (тот же протокол, которым рисуются waybar,
wofi, mako и другие быстрые Wayland-оверлеи). **Это по‑прежнему обычное
Wayland-приложение, не терминальное** — запускается и биндится в Hyprland
точно так же, как раньше (никакого терминального эмулятора не требуется).

## Что изменилось

- **Убран GTK4.** Вместо `GtkApplication`/`GtkDrawingArea`/сигналов — прямой
  Wayland-клиент (`wl_compositor`, `wl_shm`, `wl_seat`, `zwlr_layer_shell_v1`)
  и Cairo поверх `wl_shm`-буфера. GTK4 на старте поднимает GObject type
  system, тему/CSS, accessibility и т.д. — здесь этого нет, старт почти
  мгновенный, ввод обрабатывается напрямую через `wl_keyboard`/`wl_pointer`
  без слоя GTK event controllers.
- **Двойная буферизация** (`common/layer_window.cpp`, `acquire_buffer`) —
  рендер не блокируется ожиданием компоузера.
- **Событийный `poll()`-луп** вместо GLib main loop — раз-два системных
  вызова на итерацию, без GLib-обвязки.
- **Автообновление раз в секунду**, пока окно открыто (`add_timer(1000, …)`)
  — подхватывает новые/закрытые окна и свежие превью даже без внешнего
  reload-флага. Быстрый поллинг reload-флага (100мс/200мс, как в оригинале)
  оставлен для мгновенной реакции на явное переключение.
- **Рамочное выделение ACT/TAB** (`status_frame()` в `common/draw_helpers.hpp`):
  раньше активный/tab-профиль были помечены только мелким текстом «ACT»/«TAB»
  в подписи под ячейкой. Теперь у ячейки активного профиля/раб.стола
  дополнительно рисуется **отдельная зелёная рамка** (цвет `C_SEL`), а у
  ячейки TAB-профиля — **жёлтая рамка** (`0xe5c07b`), вынесенные чуть дальше
  наружу относительно обычной рамки курсора-выбора — так ACT/TAB видно с
  любого места сетки, а не только по мелкой надписи. В `ws-overview`
  аналогично подсвечивается активный рабочий стол.
- Вся остальная логика (чтение/запись `hypr-profiles`, `hypr-altab`,
  `hyprctl`/`hl.dsp.*` вызовы, python-фильтры списка окон, геометрия сетки,
  vim-навигация, двойной клик) перенесена **без изменений в алгоритме** —
  только синтаксис C → C++ (структуры, `nullptr`, `std::string` там, где
  раньше был `char[]` для имени неймспейса surface).

## Структура

```
hypr-overview/
  ../config.h                  — ваш конфиг, общий с dashboard
  common/
    layer_window.hpp/.cpp      — обвязка Wayland layer-shell + Cairo + событийный луп
    draw_helpers.hpp           — set_color/rounded_rect/status_frame
  src/
    profile_overview.cpp       — порт profile-overview.c
    ws_overview.cpp             — порт ws-overview.c
  Makefile
```

## Сборка

Нужны пакеты (заголовки):

- **Arch/Hyprland-системы:** `pacman -S wayland wayland-protocols wlr-protocols libxkbcommon cairo`
- **Debian/Ubuntu:** `apt install libwayland-dev wayland-protocols libxkbcommon-dev libcairo2-dev`
  — `wlr-protocols` в Debian/Ubuntu отдельным пакетом обычно не идёт;
  тогда положите `wlr-layer-shell-unstable-v1.xml` (из репозитория
  [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) —
  он есть и в исходниках Hyprland/wlroots) в `hypr-overview/protocols/` —
  Makefile подхватит его автоматически.

```sh
cd hypr-overview
make
```

Соберутся два бинарника: `profile-overview` и `ws-overview`. Замените ими
старые в ваших Hyprland-биндах/submap-конфиге — командная строка запуска не
меняется (`profile-overview [номер_воркспейса]`, `ws-overview` без аргументов).

## ⚠️ Важно — код не собирался и не тестировался в этой сессии

В песочнице, где я работаю, нет доступа к сети и не установлены dev-пакеты
`libwayland-dev`/`wayland-protocols`/`libxkbcommon-dev` (попытка `apt-get
install` вернула `403 Forbidden` — сеть отключена), поэтому **я не смог
скомпилировать и прогнать код здесь**. Логику и Wayland-протокольные вызовы
(`zwlr_layer_surface_v1_*`, порядок полей в listener-структурах и т.д.) я
проверил вручную по спецификации — но перед тем как класть в продакшен,
соберите и проверьте на месте:

```sh
make 2>&1 | tee build.log
```

Если будут ошибки компиляции — пришлите мне `build.log`, поправлю.

## Известные ограничения (те же допущения, что и в оригинале)

- Масштабирование превью по-прежнему ориентируется на фиксированные
  `MON_W`/`MON_H` из `config.h`, а не на реальный `wl_output` scale/geometry
  — как и в исходном GTK-варианте, HiDPI/дробный scale не учитывается.
  Если правился масштаб — то же самое допущение, что и раньше.
- Полноэкранное позиционирование layer-surface без явного `wl_output` —
  компоузер сам выбирает вывод (обычно текущий сфокусированный), как и
  `gtk_window_fullscreen()` раньше.
