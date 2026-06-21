#include "menu.h"
#include "list_core.h"

Menu *menu_push(const char *title, MenuConfig cfg) {
  return list_core_push(title, (ListCoreConfig) {
    .ctx = cfg.ctx,
    .size = cfg.size,
    .interactive = true,
    .get_count = cfg.get_count,
    .get_item = cfg.get_item,
    .on_select = cfg.on_select,
    .on_unload = cfg.on_unload,
  });
}
void    menu_reload(Menu *m) { list_core_reload(m); }
Window *menu_window(Menu *m) { return list_core_window(m); }
