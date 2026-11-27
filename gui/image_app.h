/* gui/image_app.h — просмотрщик изображений (PNG и GIF) для оконного режима. */
#ifndef GUI_IMAGE_APP_H
#define GUI_IMAGE_APP_H

#include "window.h"
#include "ramfs.h"

typedef struct {
    const gui_window_t *win;
    int dark;
    int scale;
    int mode;       /* 0 = список, 1 = просмотр */
    int scroll;
    int selected;
} image_app_t;

void image_app_init(image_app_t *app, const gui_window_t *win);
void image_app_set_theme(image_app_t *app, int dark, int scale);
void image_app_open(image_app_t *app, const char *path);
void image_app_render(image_app_t *app);
int  image_app_handle_click(image_app_t *app, int mx, int my);
int  image_app_handle_key(image_app_t *app, int key);
int  image_app_tick(image_app_t *app);

#endif
