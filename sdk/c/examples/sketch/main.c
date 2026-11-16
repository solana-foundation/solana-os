// Sketch: images and a canvas from C. The d-pad draws lines on an 80 x 54
// canvas shown at 4x; SELECT stamps art/gem.png (a file bundled with the app,
// loaded once with ui_load) and changes the pen's colour. The pen is pixel
// art (ui_image_rows) on top of the canvas.
#include <stdio.h>

#include "badge.h"

#define W 80
#define H 54
#define SCALE 4

static const uint32_t colours[] = {COL_GREEN, COL_PURPLE, COL_AQUA, COL_PINK, COL_YELLOW, 0xFFFFFF};
static ui_obj canvas, pen, info;
static ui_img gem;
static int x = W / 2, y = H / 2, colour;

static void update_info(void)
{
    char text[48];
    int32_t under = ui_canvas_get(canvas, x, y);
    if (under < 0) {
        snprintf(text, sizeof text, "%d,%d  empty", x, y);
    } else {
        snprintf(text, sizeof text, "%d,%d  #%06lX", x, y, (unsigned long)under);
    }
    ui_label_text(info, text);
    ui_obj_pos(pen, x * SCALE - 4, y * SCALE - 4);
}

void on_start(void)
{
    static const char *const pen_art[] = {
        "..w..",
        "..w..",
        "ww.ww",
        "..w..",
        "..w..",
    };
    canvas = ui_canvas(0, 0, W, H, SCALE, COL_PANEL);
    gem = ui_load("art/gem.png");
    if (!gem.ref) {
        ui_toast(badge_error());
    }
    ui_image_of(UI_W - 36, 2, gem, 4); // views share the pixels: no copy
    pen = ui_image_rows(0, 0, pen_art, 5, NULL, 0, 2);
    info = ui_label(6, 2, "", &(ui_label_opts){.size = 12, .color = COL_TEXT});
    ui_hint("D-pad draw   SELECT stamp   CANCEL exit");
    ui_canvas_rect(canvas, 0, H - 1, W, 1, COL_BORDER);
    update_info();
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    int nx = x, ny = y;
    switch (key) {
    case KEY_UP: ny--; break;
    case KEY_DOWN: ny++; break;
    case KEY_LEFT: nx--; break;
    case KEY_RIGHT: nx++; break;
    case KEY_SELECT:
        ui_canvas_image(canvas, gem, x - 4, y - 4); // see-through pixels leave the canvas as it was
        colour = (colour + 1) % (int)(sizeof colours / sizeof colours[0]);
        update_info();
        return 1;
    default:
        return 0; // CANCEL exits
    }
    if (nx < 0 || nx >= W || ny < 0 || ny >= H - 1) {
        return 1;
    }
    ui_canvas_line(canvas, x, y, nx, ny, colours[colour]);
    x = nx;
    y = ny;
    update_info();
    return 1;
}

void on_exit(void)
{
    badge_obj_free(&gem);
}
