/*
 * config.h — Центральная конфигурация C-программ
 * Подключай: #include "config.h"
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Раскладка воркспейсов ──────────────────────────────── */
#define WS_COLS         3
#define WS_ROWS         2
#define WS_COUNT        6
#define PROFILE_COUNT   9

/* Формулы (0-based profile индекс: 0..8) */
#define BASE_WS(id)     (((id)-1) % WS_COUNT + 1)
#define PROFILE_IDX(id) (((id)-1) / WS_COUNT)        /* 0-based */
#define PROFILE_N(id)   (((id)-1) / WS_COUNT + 1)    /* 1-based */
#define HYPR_ID(base, prof) ((base) + ((prof)-1) * WS_COUNT)

/* ── Монитор: запасной размер, обычно берётся настоящий ─── */
#define MON_W           1366
#define MON_H           768

/* ── ws-overview геометрия ──────────────────────────────── */
#define OV_GAP          14
#define OV_LABEL_H      24
#define OV_ICON_SIZE    24
#define OV_CORNER_R     7.0

/* ── Цвета ws-overview (0xRRGGBB) ──────────────────────── */
#define C_BG            0x080b0f
#define C_CELL_BG       0x0e1117
#define C_SEL           0x61afef
#define C_CUR           0x264f78
#define C_DIM           0x2c313a
#define C_TEXT          0xabb2bf
#define C_TEXT_DIM      0x5c6370
#define C_WIN_BG        0x181e25
#define C_WIN_ACT       0x1e2530
#define C_BORDER_SEL    0x61afef
#define C_BORDER_CUR    0x3a5f82
#define C_BORDER_DIM    0x1e2530

/* ── Цвета профилей (индекс 0=неиспользуется, 1-9) ──────── */
static const unsigned int PROFILE_COLORS[10] = {
    0x000000,  /* [0] не используется */
    0x98c379,  /* [1] P1  зелёный   */
    0x61afef,  /* [2] P2  синий     */
    0xe5c07b,  /* [3] P3  жёлтый   */
    0xc678dd,  /* [4] P4  фиолет.  */
    0xe06c75,  /* [5] P5  красный  */
    0x56b6c2,  /* [6] P6  циан     */
    0xd19a66,  /* [7] P7  оранжев. */
    0xabb2bf,  /* [8] P8  серый    */
    0xbe5046,  /* [9] P9  т.красн. */
};

/* ── Пути хранения состояния ────────────────────────────── */
#define PROFILE_STORAGE     "/tmp/hypr-profiles"
#define ALTAB_STORAGE       "/tmp/hypr-altab"
#define WS_CACHE_DIR        "/tmp/ws-cache"
#define OV_RELOAD_FLAG      "/tmp/ws-overview-reload"
#define POV_RELOAD_FLAG     "/tmp/profile-overview-reload"

/* Скрытые окна (Win+Alt+C) — список свой у каждого (base_ws, профиль),
 * файлы HIDDEN_APPS_DIR/ws_<base>_p<prof>, формат см. config.sh. */
#define HIDDEN_APPS_DIR     "/tmp/hypr-hidden-apps"
#define HAV_RELOAD_FLAG     "/tmp/hidden-apps-view-reload"
#define HAV_TARGET_FILE     "/tmp/hidden-apps-view.target"

/* set_color определяется в ws-overview.c, не здесь */

/* ── Хелпер: чтение профиля из файла ───────────────────── */
static inline int read_ws_profile(int base_ws) {
    char path[64];
    snprintf(path, sizeof(path), "%s/ws_%d.profile", WS_CACHE_DIR, base_ws);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char buf[8] = {0};
    if (fgets(buf, sizeof(buf), f)) {}
    fclose(f);
    int p = atoi(buf);
    return (p >= 1 && p <= PROFILE_COUNT) ? p : 1;
}

static inline void write_ws_profile(int base_ws, int prof) {
    char path[64];
    snprintf(path, sizeof(path), "%s/ws_%d.profile", WS_CACHE_DIR, base_ws);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", prof);
    fclose(f);
}
