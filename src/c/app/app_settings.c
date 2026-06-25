#include "app_settings.h"
#include "molkky.h"
#include "pins_art.h"
#include "c/lib/ui/menu.h"

// =============================================================================
// App settings, in two sections: "Gameplay" (lose-on-3, finish-round) and
// "Other" (page header, pin artwork). Rows stay a flat list; the section map
// just groups them.
// (Named app_settings to avoid clashing with the keyboard's settings_window.)
// =============================================================================

static Menu *s_menu;

static uint16_t st_count(void *c) { return 4; }
static uint16_t st_sections(void *c) { return 2; }
static uint16_t st_section_count(void *c, uint16_t s) { return 2; }
static const char *st_section_title(void *c, uint16_t s) { return s == 0 ? "Gameplay" : "Other"; }
static void st_item(void *c, uint16_t i, ListItem *out) {
  if (i == 0) {
    snprintf(out->title, sizeof out->title, "Lose from 3 misses");
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_lose_on_3() };
  } else if (i == 1) {
    snprintf(out->title, sizeof out->title, "Finish round");
    snprintf(out->subtitle, sizeof out->subtitle, "Ties shared places");
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_final_round() };
  } else if (i == 2) {
    snprintf(out->title, sizeof out->title, "Page header");
    snprintf(out->subtitle, sizeof out->subtitle, "Title and clock bar");
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_show_header() };
  } else {
    snprintf(out->title, sizeof out->title, "Pin artwork");
    snprintf(out->subtitle, sizeof out->subtitle, "The 12-pin formation");
  }
}
static void st_select(void *c, uint16_t i) {
  if (i == 0)      { mk_set_lose_on_3(!mk_lose_on_3());   menu_reload(s_menu); }
  else if (i == 1) { mk_set_final_round(!mk_final_round()); menu_reload(s_menu); }
  else if (i == 2) { mk_set_show_header(!mk_show_header()); menu_reload(s_menu); }
  else             pins_art_push();
}

void app_settings_push(void) {
  s_menu = menu_push("Settings", (MenuConfig) {
    .get_count = st_count, .get_item = st_item, .on_select = st_select,
    .get_sections = st_sections, .get_section_count = st_section_count,
    .section_title = st_section_title,
  });
}
