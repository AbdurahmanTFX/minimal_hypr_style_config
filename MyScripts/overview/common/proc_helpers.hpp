// proc_helpers.hpp — общий небольшой хелпер для "выстрелил и забыл"
// запуска внешних команд (hyprctl и т.п.), без блокировки вызывающего
// потока. Раньше в profile_overview.cpp / ws_overview.cpp для этого
// использовался system() — он блокирует до завершения ВСЕЙ команды
// (обычно 10–50+ мс: fork шелла, коннект к Hyprland IPC, dispatch,
// выход), и всё это время LayerWindow::run() полностью заморожен —
// не может обработать ни Wayland-события, ни таймеры. Если хоткей
// toggle нажимался именно в это окно, ответ на него откладывался до
// разморозки event loop, что на практике ощущалось как "нужно
// нажать хоткей дважды".
#pragma once

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char **environ;

// id активного воркспейса или 0, если узнать не удалось. Читаем JSON, а не
// текстовый вывод: Hyprland убрал слово "ID" из строки "workspace ID N", и
// разбор текста молча перестал находить воркспейс — обзоры застревали на том
// столе, где были запущены.
inline int active_workspace_id()
{
    FILE *fp = popen("hyprctl activeworkspace -j 2>/dev/null", "r");
    if (!fp)
        return 0;

    int id = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        const char *p = strstr(buf, "\"id\":");
        if (p) {
            id = atoi(p + 5);
            break;
        }
    }
    pclose(fp);
    return id;
}

// Логический размер активного монитора (с учётом scale). При сбое w и h не меняются.
inline void active_monitor_size(int &w, int &h)
{
    FILE *fp = popen(
        "hyprctl monitors -j 2>/dev/null | jq -r 'first(.[] | select(.focused)) | "
        "\"\\(.width / .scale) \\(.height / .scale)\"' 2>/dev/null",
        "r");
    if (!fp)
        return;

    double mw = 0;
    double mh = 0;
    const int read = fscanf(fp, "%lf %lf", &mw, &mh);
    pclose(fp);
    if (read == 2 && mw >= 1 && mh >= 1) {
        w = static_cast<int>(mw);
        h = static_cast<int>(mh);
    }
}

// Запускает `/bin/sh -c cmd` и сразу возвращает управление, не дожидаясь
// завершения. Требует, чтобы где-то один раз было выставлено
// signal(SIGCHLD, SIG_IGN) (см. main() в profile_overview.cpp /
// ws_overview.cpp) — тогда ядро само пожинает завершившиеся дочерние
// процессы и зомби не накапливаются, без дополнительного потока/waitpid.
inline void spawn_detached(const char *cmd)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Отвязываем от нашего stdin (в частности — от Wayland-совместимого
    // управляющего терминала, если он есть): дочернему процессу это не
    // нужно, а лишний открытый fd на что-то общее — источник сюрпризов.
    posix_spawn_file_actions_addopen(
        &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    char *argv[] = {
        const_cast<char *>("/bin/sh"),
        const_cast<char *>("-c"),
        const_cast<char *>(cmd),
        nullptr};

    pid_t pid = -1;

    posix_spawn(
        &pid,
        "/bin/sh",
        &actions,
        nullptr,
        argv,
        environ);

    posix_spawn_file_actions_destroy(&actions);
}
