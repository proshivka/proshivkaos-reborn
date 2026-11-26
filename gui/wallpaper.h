/* gui/wallpaper.h — обои рабочего стола и синхронизация палитры тем. */
#ifndef GUI_WALLPAPER_H
#define GUI_WALLPAPER_H

void gui_draw_wallpaper(int screen_w, int screen_h);
void gui_draw_wallpaper_themed(int screen_w, int screen_h, int theme);
void gui_theme_sync_palette(int theme);

#endif
