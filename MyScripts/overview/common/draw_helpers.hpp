// draw_helpers.hpp — общие примитивы отрисовки (перенесены из
// profile-overview.c / ws-overview.c без изменений в математике).
#pragma once
#include <cairo/cairo.h>
#include <cmath>

inline void set_color(cairo_t *cr, unsigned int hex, double alpha) {
    cairo_set_source_rgba(cr,
        ((hex >> 16) & 0xff) / 255.0,
        ((hex >> 8)  & 0xff) / 255.0,
        (hex & 0xff) / 255.0,
        alpha);
}

inline void rounded_rect(cairo_t *cr, double x, double y,
                          double w, double h, double r) {
    if (r <= 0 || w < 2 * r || h < 2 * r) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_move_to(cr, x + r, y);
    cairo_line_to(cr, x + w - r, y);       cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_line_to(cr, x + w, y + h - r);   cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_line_to(cr, x + r, y + h);       cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2, M_PI);
    cairo_line_to(cr, x, y + r);           cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// Новое: отдельная рамочная подсветка статуса ACT/TAB — рисуется поверх
// обычной рамки выделения курсора (rounded_rect + stroke в основном коде),
// вынесена чуть дальше наружу (offset), так что курсор-рамка (синяя) и
// статусная рамка (зелёная ACT / жёлтая TAB) видны одновременно и не
// перекрывают друг друга.
inline void status_frame(cairo_t *cr, double cx, double cy, double cw, double ch,
                          double corner_r, unsigned int color, double offset,
                          double line_width, double alpha) {
    rounded_rect(cr, cx - offset, cy - offset, cw + 2 * offset, ch + 2 * offset,
                 corner_r + offset);
    set_color(cr, color, alpha);
    cairo_set_line_width(cr, line_width);
    cairo_stroke(cr);
}
