#include "app_settings.h"
#include "molkky.h"
#include "c/lib/ui/menu.h"

// =============================================================================
// App settings. Two toggles: "Lose from 3 misses" and "Finish round".
// (Named app_settings to avoid clashing with the keyboard's settings_window.)
// =============================================================================

static Menu *s_menu;

static uint16_t st_count(void *c) { return 2; }
static void st_item(void *c, uint16_t i, ListItem *out) {
  if (i == 0) {
    snprintf(out->title, sizeof out->title, "Lose from 3 misses");
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_lose_on_3() };
  } else {
    snprintf(out->title, sizeof out->title, "Finish round");
    snprintf(out->subtitle, sizeof out->subtitle, "Ties shared places");
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_final_round() };
  }
}
static void st_select(void *c, uint16_t i) {
  if (i == 0) mk_set_lose_on_3(!mk_lose_on_3());
  else        mk_set_final_round(!mk_final_round());
  menu_reload(s_menu);
}

void app_settings_push(void) {
  s_menu = menu_push("Settings", (MenuConfig) {
    .get_count = st_count, .get_item = st_item, .on_select = st_select,
  });
}
