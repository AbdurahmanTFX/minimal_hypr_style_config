-- hyprland.lua — переведено с hyprland.conf на Lua-конфиг Hyprland 0.55+
-- Все формы hl.* сверены с официальным example/hyprland.lua из репозитория
-- hyprwm/Hyprland и вики wiki.hypr.land.

hl.config({
    quirks = {
        skip_non_kms_dmabuf_formats = false,
    },
})

------------------
---- MONITORS ----
------------------
hl.monitor({
    output   = "",
    mode     = "preferred",
    position = "auto",
    scale    = "auto",
})

---------------------
---- MY PROGRAMS ----
---------------------
local terminal     = "kitty"
local fileManager  = "kitty -e yazi"
local browser      = "firefox"
local pdfViewer    = "zathura"
local menu         = "rofi -show drun"
local mainMod      = "SUPER"
local navigate     = "~/.config/hypr/MyScripts/ws-navigate.sh"
local profile      = "~/.config/hypr/MyScripts/profile-switch.sh"

-------------------
---- AUTOSTART ----
-------------------
hl.on("hyprland.start", function()
    hl.exec_cmd("/usr/lib/xdg-desktop-portal-hyprland")
    hl.exec_cmd("/usr/lib/xdg-desktop-portal-gtk")
    hl.exec_cmd("awww-daemon")
    hl.exec_cmd("~/.config/hypr/MyScripts/wallpaper-switch.sh static")
    hl.exec_cmd("dunst")
    hl.exec_cmd("~/.config/hypr/MyScripts/battery-guard.sh")
    hl.exec_cmd("~/.config/hypr/MyScripts/layout-notify.sh")
    hl.exec_cmd("foot --server --socket /tmp/foot-pov.sock")
end)

-------------------------------
---- ENVIRONMENT VARIABLES ----
-------------------------------
hl.env("XCURSOR_SIZE", "24")
hl.env("HYPRCURSOR_SIZE", "24")
hl.env("GTK_THEME", "Adwaita:dark")

-----------------------
---- LOOK AND FEEL ----
-----------------------
hl.config({
    general = {
        gaps_in  = 4,
        gaps_out = 7,
        border_size = 2,
        col = {
            active_border   = "rgba(808080ff)",
            inactive_border = "rgba(0d0d0dff)",
        },
        resize_on_border = false,
        allow_tearing    = false,
        layout           = "dwindle",
    },
    decoration = {
        rounding       = 10,
        rounding_power = 2,
        inactive_opacity = 1.0,
        active_opacity   = 1.0,
        blur = {
            enabled            = false,
            size               = 2,
            passes             = 2,
            new_optimizations  = true,
            ignore_opacity     = true,
            noise              = 0,
            brightness         = 0.50,
        },
    },
    animations = {
        enabled = true,
    },
})

-- Bezier-кривые
hl.curve("easeOutQuint",   { type = "bezier", points = { {0.23, 1},    {0.32, 1}    } })
hl.curve("easeInOutCubic", { type = "bezier", points = { {0.65, 0.05}, {0.36, 1}    } })
hl.curve("linear",         { type = "bezier", points = { {0, 0},       {1, 1}       } })
hl.curve("almostLinear",   { type = "bezier", points = { {0.5, 0.5},   {0.75, 1}    } })
hl.curve("quick",          { type = "bezier", points = { {0.15, 0},    {0.1, 1}     } })
hl.curve("profovFade",     { type = "bezier", points = { {0.25, 1},    {0.5, 1}     } })

-- Анимации
hl.animation({ leaf = "global",        enabled = true, speed = 10,   bezier = "default" })
hl.animation({ leaf = "border",        enabled = true, speed = 5.39, bezier = "easeOutQuint" })
hl.animation({ leaf = "windows",       enabled = true, speed = 4.79, bezier = "easeOutQuint" })
hl.animation({ leaf = "windowsIn",     enabled = true, speed = 4.1,  bezier = "easeOutQuint", style = "popin 87%" })
hl.animation({ leaf = "windowsOut",    enabled = true, speed = 1.49, bezier = "linear",       style = "popin 87%" })
hl.animation({ leaf = "fadeIn",        enabled = true, speed = 1.73, bezier = "almostLinear" })
hl.animation({ leaf = "fadeOut",       enabled = true, speed = 1.46, bezier = "almostLinear" })
hl.animation({ leaf = "fade",          enabled = true, speed = 3.03, bezier = "quick" })
hl.animation({ leaf = "layers",        enabled = true, speed = 3.81, bezier = "easeOutQuint" })
hl.animation({ leaf = "layersIn",      enabled = true, speed = 4,    bezier = "easeOutQuint", style = "fade" })
hl.animation({ leaf = "layersOut",     enabled = true, speed = 1.5,  bezier = "linear",       style = "fade" })
hl.animation({ leaf = "fadeLayersIn",  enabled = true, speed = 1.79, bezier = "almostLinear" })
hl.animation({ leaf = "fadeLayersOut", enabled = true, speed = 1.39, bezier = "almostLinear" })
hl.animation({ leaf = "workspaces",    enabled = true, speed = 1.94, bezier = "almostLinear", style = "fade" })
hl.animation({ leaf = "workspacesIn",  enabled = true, speed = 1.21, bezier = "almostLinear", style = "fade" })
hl.animation({ leaf = "workspacesOut", enabled = true, speed = 1.94, bezier = "almostLinear", style = "fade" })
hl.animation({ leaf = "zoomFactor",    enabled = true, speed = 7,    bezier = "quick" })

hl.config({
    dwindle = {
        preserve_split = true,
    },
})

hl.config({
    master = {
        new_status = "master",
    },
})

hl.config({
    misc = {
        force_default_wallpaper = 0,
        disable_hyprland_logo   = true,
    },
})

---------------
---- INPUT ----
---------------
hl.config({
    input = {
        kb_layout    = "us,ru",
        kb_options   = "grp:alt_shift_toggle",
        follow_mouse = 1,
        mouse_refocus = true, -- фикс известного бага: без явного true фокус
                               -- залипает на окне special-воркспейса и не
                               -- возвращается остальным окнам ни кликом,
                               -- ни хоткеями (see hyprwm/Hyprland#11449)
        sensitivity  = 0,
        touchpad = {
            natural_scroll = true,
        },
    },
})

hl.gesture({
    fingers   = 3,
    direction = "horizontal",
    action    = "workspace",
})

hl.device({
    name        = "epic-mouse-v1",
    sensitivity = -0.5,
})

---------------------
---- KEYBINDINGS ----
---------------------

hl.bind(mainMod .. " + T", hl.dsp.exec_cmd(terminal))
hl.bind(mainMod .. " + Y", hl.dsp.exec_cmd(fileManager))
hl.bind(mainMod .. " + F", hl.dsp.exec_cmd(browser))
hl.bind(mainMod .. " + R", hl.dsp.exec_cmd(pdfViewer))
hl.bind("CTRL + ALT + S", hl.dsp.exec_cmd("rofi -show drun"))

-- Закрыть активное окно (аналог killactive: hl.dsp.window.close() без
-- аргументов = закрыть активное окно; подтверждено официальным примером)
hl.bind("ALT + " .. mainMod .. " + X",  hl.dsp.window.close())
hl.bind("ALT + " .. mainMod .. " + F4", hl.dsp.window.close())

-- Fullscreen (старое "fullscreen, 0" — обычный toggle fullscreen).
-- ВНИМАНИЕ: точное имя режима у fullscreen() менялось между патч-релизами
-- 0.55.x — если после reload это не переключает fullscreen, скажите,
-- подберём точное значение mode под вашу версию.
hl.bind("ALT + " .. mainMod .. " + F", hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" }))

-- Скрытые окна ТЕКУЩЕГО ПРОФИЛЯ (см. hidden-apps.sh). Раньше было одно
-- общее special:hidden на все профили сразу — теперь у каждого
-- (рабочий стол, профиль) свой список.
--   Win+Alt+C — спрятать активное окно в список текущего профиля
--   Win+Alt+S — вернуть последнее спрятанное окно текущего профиля
--   Win+Alt+V — открыть панель со списком скрытых окон профиля
hl.bind("ALT + " .. mainMod .. " + C", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/hidden-apps.sh hide"))
hl.bind("ALT + " .. mainMod .. " + S", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/hidden-apps.sh restore-last"))
hl.bind("ALT + " .. mainMod .. " + V", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
hl.bind("CTRL + ALT + D", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/show-desktop.sh"))

-- Фокус между окнами
hl.bind("ALT + " .. mainMod .. " + H", hl.dsp.focus({ direction = "left" }))
hl.bind("ALT + " .. mainMod .. " + J", hl.dsp.focus({ direction = "down" }))
hl.bind("ALT + " .. mainMod .. " + K", hl.dsp.focus({ direction = "up" }))
hl.bind("ALT + " .. mainMod .. " + L", hl.dsp.focus({ direction = "right" }))

-- Перемещение окна в тайлинге
hl.bind("CTRL + " .. mainMod .. " + H", hl.dsp.window.move({ direction = "left" }))
hl.bind("CTRL + " .. mainMod .. " + J", hl.dsp.window.move({ direction = "down" }))
hl.bind("CTRL + " .. mainMod .. " + K", hl.dsp.window.move({ direction = "up" }))
hl.bind("CTRL + " .. mainMod .. " + L", hl.dsp.window.move({ direction = "right" }))

-- Изменение размера активного окна
hl.bind("SHIFT + " .. mainMod .. " + right", hl.dsp.window.resize({ x =  50, y =   0 }))
hl.bind("SHIFT + " .. mainMod .. " + left",  hl.dsp.window.resize({ x = -50, y =   0 }))
hl.bind("SHIFT + " .. mainMod .. " + up",    hl.dsp.window.resize({ x =   0, y = -50 }))
hl.bind("SHIFT + " .. mainMod .. " + down",  hl.dsp.window.resize({ x =   0, y =  50 }))

-- Навигация по воркспейсам (9-профильная система, см. ws-navigate.sh)
hl.bind(mainMod .. " + SHIFT + H", hl.dsp.exec_cmd(navigate .. " move-left"))
hl.bind(mainMod .. " + SHIFT + L", hl.dsp.exec_cmd(navigate .. " move-right"))
hl.bind(mainMod .. " + SHIFT + K", hl.dsp.exec_cmd(navigate .. " move-up"))
hl.bind(mainMod .. " + SHIFT + J", hl.dsp.exec_cmd(navigate .. " move-down"))

hl.bind("CTRL + ALT + H", hl.dsp.exec_cmd(navigate .. " left"))
hl.bind("CTRL + ALT + L", hl.dsp.exec_cmd(navigate .. " right"))
hl.bind("CTRL + ALT + K", hl.dsp.exec_cmd(navigate .. " up"))
hl.bind("CTRL + ALT + J", hl.dsp.exec_cmd(navigate .. " down"))

-- Профили (Ctrl+Alt+1-9)
for i = 1, 9 do
    hl.bind("CTRL + ALT + " .. i, hl.dsp.exec_cmd(profile .. " goto " .. i))
end

-- Alt+Tab: P1 ↔ alt-tab партнёр
hl.bind("ALT + Tab", hl.dsp.exec_cmd(profile .. " altab"))

-- Win+Shift+1-9: переместить окно в профиль N
for i = 1, 9 do
    hl.bind(mainMod .. " + SHIFT + " .. i, hl.dsp.exec_cmd(profile .. " move-win " .. i))
end

hl.bind("CTRL + ALT + T", hl.dsp.exec_cmd(
    "notify-send -t 3000 \"🕐 $(date '+%H:%M')\" \"🔋 $(cat /sys/class/power_supply/BAT0/capacity 2>/dev/null || echo 'N/A')%\""
))

hl.bind("CTRL + ALT + I", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-dashboard.sh"))
hl.bind("CTRL + ALT + E", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-overview.sh"))
hl.bind("CTRL + ALT + W", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/wallpaper-switch.sh toggle"))
hl.bind("CTRL + ALT + N", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/wallpaper-switch.sh next"))
hl.bind("CTRL + ALT + B", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/wallpaper-pick.sh"))
hl.bind("CTRL + ALT + O", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-touchpad.sh"))
hl.bind("CTRL + ALT + P", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-profile-overview.sh"))

-- Скриншоты
hl.bind("Print",               hl.dsp.exec_cmd("hyprshot -m output -o ~/Pictures/Screenshots"))
hl.bind(mainMod .. " + Print", hl.dsp.exec_cmd("hyprshot -m window -o ~/Pictures/Screenshots"))
hl.bind("SHIFT + Print",       hl.dsp.exec_cmd("hyprshot -m region -o ~/Pictures/Screenshots"))
hl.bind("CTRL + Print",        hl.dsp.exec_cmd("hyprshot -m region --freeze -o ~/Pictures/Screenshots | swappy -f -"))

-- Медиаклавиши (locked+repeating = аналог bindel, locked = аналог bindl)
hl.bind("XF86AudioRaiseVolume",  hl.dsp.exec_cmd("wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+"), { locked = true, repeating = true })
hl.bind("XF86AudioLowerVolume",  hl.dsp.exec_cmd("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"),      { locked = true, repeating = true })
hl.bind("XF86AudioMute",         hl.dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"),     { locked = true, repeating = true })
hl.bind("XF86AudioMicMute",      hl.dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"),   { locked = true, repeating = true })
hl.bind("XF86MonBrightnessUp",   hl.dsp.exec_cmd("brightnessctl -e4 -n2 set 5%+"),                  { locked = true, repeating = true })
hl.bind("XF86MonBrightnessDown", hl.dsp.exec_cmd("brightnessctl -e4 -n2 set 5%-"),                  { locked = true, repeating = true })
hl.bind("XF86AudioNext",  hl.dsp.exec_cmd("playerctl next"),       { locked = true })
hl.bind("XF86AudioPause", hl.dsp.exec_cmd("playerctl play-pause"), { locked = true })
hl.bind("XF86AudioPlay",  hl.dsp.exec_cmd("playerctl play-pause"), { locked = true })
hl.bind("XF86AudioPrev",  hl.dsp.exec_cmd("playerctl previous"),   { locked = true })

-- Мышь: перетаскивание/ресайз mainMod + ЛКМ/ПКМ
hl.bind(mainMod .. " + mouse:272", hl.dsp.window.drag(),   { mouse = true })
hl.bind(mainMod .. " + mouse:273", hl.dsp.window.resize(), { mouse = true })

--------------------------------
---- WINDOWS AND WORKSPACES ----
--------------------------------

-- Smart gaps — окно занимает весь экран когда оно одно
hl.workspace_rule({ workspace = "w[tv1]", gaps_out = 0, gaps_in = 0 })
hl.workspace_rule({ workspace = "f[1]",   gaps_out = 0, gaps_in = 0 })

hl.window_rule({
    name  = "no-gaps-wtv1",
    match = { float = false, workspace = "w[tv1]" },
    border_size = 0,
    rounding    = 0,
})
hl.window_rule({
    name  = "no-gaps-f1",
    match = { float = false, workspace = "f[1]" },
    border_size = 0,
    rounding    = 0,
})

hl.window_rule({
    name  = "suppress-maximize-events",
    match = { class = ".*" },
    suppress_event = "maximize",
})

hl.window_rule({
    name  = "fix-xwayland-drags",
    match = {
        class      = "^$",
        title      = "^$",
        xwayland   = true,
        float      = true,
        fullscreen = false,
        pin        = false,
    },
    no_focus = true,
})

hl.window_rule({
    name  = "move-hyprland-run",
    match = { class = "hyprland-run" },
    move  = "20 monitor_h-120",
    float = true,
})

-- Dashboard TUI
hl.window_rule({
    name  = "dashboard-window",
    match = { class = "dashboard" },
    float       = true,
    move        = "0 0",
    size        = "100% 100%",
    border_size = 0,
    animation   = "off",
    fullscreen  = 1,
})
hl.window_rule({
    name  = "dashboard-hide",
    match = { class = "dashboard" },
    workspace = "special:dashboard silent",
})

-- Workspace overview
hl.window_rule({
    name  = "ws-overview",
    match = { title = "ws-overview" },
    float       = true,
    move        = "0 0",
    size        = "100% 100%",
    border_size = 0,
    fullscreen  = 1,
})
hl.window_rule({
    name  = "ws-overview-hide",
    match = { title = "ws-overview" },
    workspace = "special:overview silent",
})

-- Profile overview window rules
hl.window_rule({
    name  = "profile-overview",
    match = { title = "profile-overview" },
    float       = true,
    move        = "0 0",
    size        = "100% 100%",
    border_size = 0,
    fullscreen  = 1,
})
hl.window_rule({
    name  = "profile-overview-hide",
    match = { title = "profile-overview" },
    workspace = "special:profiles silent",
})

-- Hidden apps view (список скрытых окон профиля, Win+Alt+V)
hl.window_rule({
    name  = "hidden-apps-view",
    match = { title = "hidden-apps-view" },
    float       = true,
    move        = "0 0",
    size        = "100% 100%",
    border_size = 0,
    fullscreen  = 1,
})
hl.window_rule({
    name  = "hidden-apps-view-hide",
    match = { title = "hidden-apps-view" },
    workspace = "special:hiddenview silent",
})

-- Wallpaper picker
hl.window_rule({
    name  = "wallpaper-picker",
    match = { class = "wallpaper-picker" },
    float = true,
    size  = "1200 700",
    move  = "center center",
})

-- Popup окна из дашборда (nmtui, bluetoothctl и т.д.)
hl.window_rule({
    name  = "dashboard-popup",
    match = { class = "dashboard-popup" },
    float       = true,
    size        = "800 500",
    move        = "center center",
    border_size = 1,
})

-- Reaper и плагины — floating
hl.window_rule({
    name  = "reaper-float",
    match = { class = "reaper" },
    float = true,
})
hl.window_rule({
    name  = "reaper-plugins",
    match = { title = "(FX|Noise|Plugin|VST|LV2)" },
    float = true,
    size  = "800 600",
    move  = "center center",
})

--------------------
---- SUBMAPS ----
--------------------

-- Dashboard submap
-- ВНИМАНИЕ: было два hl.bind на одну и ту же комбинацию — второй
-- ("reset") перебивал первый, и Ctrl+Alt+I внутри дашборда просто
-- выходил из сабмапа, не пряча окно (toggle-dashboard.sh сам вызывает
-- submap("reset") в ветке visible), см. остальные сабмапы ниже.
hl.define_submap("dashboard", function()
    hl.bind("CTRL + ALT + I", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-dashboard.sh"))
    hl.bind("ALT + " .. mainMod .. " + V", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
end)

-- Overview submap — блокирует все hotkeys пока открыт ws-overview
hl.define_submap("overview", function()
    hl.bind("CTRL + ALT + E", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-overview.sh"))
    hl.bind("CTRL + ALT + I", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-dashboard.sh"))
    -- Страховка на случай потери keyboard focus у ws-overview (что бы
    -- её ни вызвало): без этого Escape, пока фокус не на ws-overview,
    -- не долетает ни клиенту (нет фокуса), ни сюда.
    -- ВАЖНО: раньше здесь стоял голый submap("reset") — он разблокирует
    -- обычные хоткеи, но САМ ОВЕРЛЕЙ никуда не прячет (в отличие от
    -- toggle-overview.sh, который в ветке visible) делает и window.move,
    -- и submap("reset") вместе). Из-за этого Escape выглядел так, будто
    -- вообще ничего не происходит — окно оставалось видимым.
    -- Дёргаем сам toggle-скрипт: он видит "visible" и прячет+резетит как надо.
    --
    -- ВАЖНО #2: Return сюда НЕ биндим. Хоткеи сабмапа перехватываются
    -- компоситором ДО доставки клиенту — если забиндить Return здесь,
    -- ws_overview.cpp никогда не увидит Enter и не сможет перейти на
    -- выбранный workspace (goto_selected()), Enter будет только закрывать
    -- панель. Escape ок, т.к. в обоих местах действие одно и то же
    -- (закрыть); для Return действия разные — конфликт.
    hl.bind("Escape", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-overview.sh"))
    hl.bind("ALT + " .. mainMod .. " + V", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
end)

-- Profile overview submap
hl.define_submap("profile-overview", function()
    hl.bind("CTRL + ALT + P", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-profile-overview.sh"))
    hl.bind("CTRL + ALT + I", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-dashboard.sh"))
    hl.bind("CTRL + ALT + E", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-overview.sh"))
    hl.bind("ALT + Tab", hl.dsp.exec_cmd("true"))
    -- См. комментарий в сабмапе "overview" — та же причина, тот же фикс:
    -- Return сюда НЕ биндим, иначе goto_prof_action() (в т.ч. переход на
    -- ячейку TAB со свапом ACT/TAB) никогда не увидит Enter — компоситор
    -- будет перехватывать её и просто закрывать панель. Клавиша "a"
    -- работает и без этого (не забинжена в сабмапе, доходит до клиента).
    hl.bind("Escape", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-profile-overview.sh"))
    hl.bind("ALT + " .. mainMod .. " + V", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
    -- Клавиша "s" (список скрытых окон ВЫБРАННОЙ ячейки) обрабатывается
    -- самим profile-overview (у окна фокус ввода, см. layer_window.cpp) —
    -- здесь дополнительный биндинг не нужен.
end)

-- Hidden apps view submap (Win+Alt+V) — блокирует остальные хоткеи,
-- пока открыт список скрытых окон профиля.
hl.define_submap("hidden-apps-view", function()
    hl.bind("ALT + " .. mainMod .. " + V", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
    hl.bind("CTRL + ALT + I", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-dashboard.sh"))
    hl.bind("CTRL + ALT + E", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-overview.sh"))
    hl.bind("CTRL + ALT + P", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-profile-overview.sh"))
    -- См. комментарий в сабмапе "overview" — та же причина, тот же фикс:
    -- Return сюда НЕ биндим, иначе restore_selected() никогда не увидит
    -- Enter — компоситор перехватит её и просто закроет панель, ничего
    -- не восстановив.
    hl.bind("Escape", hl.dsp.exec_cmd("~/.config/hypr/MyScripts/toggle-hidden-apps.sh"))
end)
