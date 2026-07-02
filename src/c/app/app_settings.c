#include "app_settings.h"
#include "molkky.h"
#include "strings.h"
#include "pins_art.h"
#include "c/lib/ui/menu.h"

// App settings menu.

static Menu *s_menu;
static Menu *s_lang_menu;

// ---- language picker ----
static uint16_t lang_count(void *c) { return locale_count(); }
static void lang_item(void *c, uint16_t i, ListItem *out) {
  snprintf(out->title, sizeof out->title, "%s", locale_autonym(i));
  if ((int)i == mk_lang())
    out->trailing = (Accessory){ .kind = ACC_CHECK };
}
static void lang_select(void *c, uint16_t i) {
  mk_set_lang(i);
  if (s_menu) menu_reload(s_menu);     // re-translate the Settings list underneath
  // Back to the now-retranslated Settings screen.
  if (s_lang_menu) window_stack_remove(menu_window(s_lang_menu), false);
}
static void lang_push(void) {
  s_lang_menu = menu_push(t(STR_LANGUAGE), (MenuConfig) {
    .get_count = lang_count, .get_item = lang_item, .on_select = lang_select,
  });
}

// Rows: "Other" gains a Language row after Pin artwork (flat index 4).
enum { ST_LOSE3, ST_FINAL, ST_HEADER, ST_PINS, ST_LANG, ST_N };

static uint16_t st_count(void *c) { return ST_N; }
static uint16_t st_sections(void *c) { return 2; }
static uint16_t st_section_count(void *c, uint16_t s) { return s == 0 ? 2 : 3; }
static const char *st_section_title(void *c, uint16_t s) { return s == 0 ? t(STR_SEC_GAMEPLAY) : t(STR_SEC_OTHER); }
static void st_item(void *c, uint16_t i, ListItem *out) {
  if (i == ST_LOSE3) {
    snprintf(out->title, sizeof out->title, "%s", t(STR_LOSE_3));
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_lose_on_3() };
  } else if (i == ST_FINAL) {
    snprintf(out->title, sizeof out->title, "%s", t(STR_FINISH_ROUND));
    snprintf(out->subtitle, sizeof out->subtitle, "%s", t(STR_TIES_SHARED));
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_final_round() };
  } else if (i == ST_HEADER) {
    snprintf(out->title, sizeof out->title, "%s", t(STR_PAGE_HEADER));
    snprintf(out->subtitle, sizeof out->subtitle, "%s", t(STR_TITLE_CLOCK));
    out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = mk_show_header() };
  } else if (i == ST_PINS) {
    snprintf(out->title, sizeof out->title, "%s", t(STR_PIN_ARTWORK));
    snprintf(out->subtitle, sizeof out->subtitle, "%s", t(STR_PIN_FORMATION));
  } else {
    snprintf(out->title, sizeof out->title, "%s", t(STR_LANGUAGE));
    snprintf(out->subtitle, sizeof out->subtitle, "%s", locale_autonym(mk_lang()));
    out->trailing = (Accessory){ .kind = ACC_CHEVRON };
  }
}
static void st_select(void *c, uint16_t i) {
  if (i == ST_LOSE3)       { mk_set_lose_on_3(!mk_lose_on_3());     menu_reload(s_menu); }
  else if (i == ST_FINAL)  { mk_set_final_round(!mk_final_round()); menu_reload(s_menu); }
  else if (i == ST_HEADER) { mk_set_show_header(!mk_show_header()); menu_reload(s_menu); }
  else if (i == ST_PINS)   pins_art_push();
  else                     lang_push();
}

void app_settings_push(void) {
  s_menu = menu_push(t(STR_SETTINGS), (MenuConfig) {
    .get_count = st_count, .get_item = st_item, .on_select = st_select,
    .get_sections = st_sections, .get_section_count = st_section_count,
    .section_title = st_section_title,
  });
}
