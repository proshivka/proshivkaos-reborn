/* gui/files_app.h — проводник файлов поверх RamFS для оконного режима. */
#ifndef GUI_FILES_APP_H
#define GUI_FILES_APP_H

#include "window.h"
#include "gconsole.h"
#include "ramfs.h"

typedef void (*files_open_image_fn)(const char *path);

typedef struct {
    const gui_window_t *win;
    gconsole_t viewer;
    char cwd[RAMFS_MAX_PATH_LEN];
    char view_name[RAMFS_MAX_PATH_LEN];
    int dark;
    int scale;
    int mode;       /* 0 = список, 1 = просмотр текста */
    int scroll;
    int selected;
    files_open_image_fn on_open_image;
} files_app_t;

void files_app_init(files_app_t *app, const gui_window_t *win, files_open_image_fn cb);
void files_app_set_theme(files_app_t *app, int dark, int scale, uint8_t fg, uint8_t bg);
void files_app_render(files_app_t *app);
int  files_app_handle_click(files_app_t *app, int mx, int my);
int  files_app_handle_key(files_app_t *app, int key);

#endif
