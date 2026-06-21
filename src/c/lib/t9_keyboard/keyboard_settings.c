#include "keyboard_settings.h"

// Persistent storage keys.
#define PKEY_WAIT       1
#define PKEY_AUTOCAPS   2
#define PKEY_DBLSPACE   3
#define PKEY_EXT        4   // bitmask of enabled extended characters
#define PKEY_THEME      5
#define PKEY_HAPTICS    6
#define PKEY_DELMODE    7
#define PKEY_DELRPT_CH  8
#define PKEY_DELRPT_WD  9
#define PKEY_HAPTIC_MS  10
#define PKEY_RESPECT_APP 11   // bool: use the app's theme over the global pick

static const int WAIT_PRESETS[] = { 300, 400, 500, 600, 800, 1000, 1200 };
#define NUM_WAIT (int)(sizeof(WAIT_PRESETS) / sizeof(WAIT_PRESETS[0]))

static const int DEL_REPEAT_PRESETS[] = { 60, 100, 150, 250, 400 };
#define NUM_DEL (int)(sizeof(DEL_REPEAT_PRESETS) / sizeof(DEL_REPEAT_PRESETS[0]))

static const int HAPTIC_PRESETS[] = { 10, 20, 40, 80 };
#define NUM_HAPTIC (int)(sizeof(HAPTIC_PRESETS) / sizeof(HAPTIC_PRESETS[0]))

static T9Keyboard *s_kb;
static T9Settings  s_settings;
static Window     *s_window;
static MenuLayer  *s_menu;

// Theme is split in two: `s_global_theme` is the user's pick among the built-in
// themes (the "global selected one"), and `s_respect_app` decides whether a
// host-registered app theme overrides it. Keeping them separate means toggling
// the app theme off restores whatever built-in the user last chose.
static int  s_global_theme;
static bool s_respect_app;

static Window    *s_char_window;
static MenuLayer *s_char_menu;
static void char_window_push(void);
static void help_window_push(void);

// ---- Persistence ------------------------------------------------------------

static void settings_load(T9Settings *s) {
  s->commit_timeout_ms = persist_exists(PKEY_WAIT)
                         ? persist_read_int(PKEY_WAIT) : T9_DEFAULT_WAIT_MS;
  s->auto_caps = persist_exists(PKEY_AUTOCAPS)
                 ? persist_read_bool(PKEY_AUTOCAPS) : true;
  s->two_space_period = persist_exists(PKEY_DBLSPACE)
                        ? persist_read_bool(PKEY_DBLSPACE) : false;
  s->haptics = persist_exists(PKEY_HAPTICS)
               ? persist_read_bool(PKEY_HAPTICS) : true;
  s->delete_mode = persist_exists(PKEY_DELMODE) ? persist_read_int(PKEY_DELMODE) : 0;
  s->del_repeat_chars_ms = persist_exists(PKEY_DELRPT_CH)
                           ? persist_read_int(PKEY_DELRPT_CH) : 100;
  s->del_repeat_words_ms = persist_exists(PKEY_DELRPT_WD)
                           ? persist_read_int(PKEY_DELRPT_WD) : 250;
  s->haptic_ms = persist_exists(PKEY_HAPTIC_MS)
                 ? persist_read_int(PKEY_HAPTIC_MS) : 20;
}

static void settings_save(const T9Settings *s) {
  persist_write_int(PKEY_WAIT, s->commit_timeout_ms);
  persist_write_bool(PKEY_AUTOCAPS, s->auto_caps);
  persist_write_bool(PKEY_DBLSPACE, s->two_space_period);
  persist_write_bool(PKEY_HAPTICS, s->haptics);
  persist_write_int(PKEY_DELMODE, s->delete_mode);
  persist_write_int(PKEY_DELRPT_CH, s->del_repeat_chars_ms);
  persist_write_int(PKEY_DELRPT_WD, s->del_repeat_words_ms);
  persist_write_int(PKEY_HAPTIC_MS, s->haptic_ms);
}

static void ext_save(T9Keyboard *kb) {
  int mask = 0, n = t9_keyboard_ext_count();
  for (int i = 0; i < n; i++)
    if (t9_keyboard_ext_enabled(kb, i)) mask |= (1 << i);
  persist_write_int(PKEY_EXT, mask);
}

static void ext_load_apply(T9Keyboard *kb) {
  int n = t9_keyboard_ext_count();
  int mask = persist_exists(PKEY_EXT) ? persist_read_int(PKEY_EXT)
                                      : ((1 << n) - 1);   // all on by default
  for (int i = 0; i < n; i++)
    t9_keyboard_ext_set_enabled(kb, i, (mask >> i) & 1);
}

// Load the theme prefs into the statics and clamp the global pick into range.
static void theme_load(void) {
  s_global_theme = persist_exists(PKEY_THEME) ? persist_read_int(PKEY_THEME) : 0;
  if (s_global_theme < 0 || s_global_theme >= t9_keyboard_theme_count()) s_global_theme = 0;
  s_respect_app = persist_exists(PKEY_RESPECT_APP) ? persist_read_bool(PKEY_RESPECT_APP) : true;
}

// The app theme wins only when registered and respected; otherwise the user's
// global built-in pick applies.
static void theme_apply(T9Keyboard *kb) {
  int idx = (s_respect_app && t9_keyboard_has_app_theme())
            ? t9_keyboard_app_theme_index() : s_global_theme;
  t9_keyboard_set_theme(kb, idx);
}

void settings_load_and_apply(T9Keyboard *kb) {
  T9Settings s;
  settings_load(&s);
  t9_keyboard_set_settings(kb, &s);
  ext_load_apply(kb);
  theme_load();
  theme_apply(kb);
}

// ---- Value helpers ----------------------------------------------------------

static void wait_cycle_next(T9Settings *s) {
  int idx = 0, best = 0x7fffffff;
  for (int i = 0; i < NUM_WAIT; i++) {                 // find closest preset
    int d = s->commit_timeout_ms - WAIT_PRESETS[i];
    if (d < 0) d = -d;
    if (d < best) { best = d; idx = i; }
  }
  s->commit_timeout_ms = WAIT_PRESETS[(idx + 1) % NUM_WAIT];
}

// Cycle the repeat speed belonging to the CURRENT delete mode.
static void del_repeat_cycle_next(T9Settings *s) {
  int *val = (s->delete_mode == 1) ? &s->del_repeat_words_ms : &s->del_repeat_chars_ms;
  int idx = 0, best = 0x7fffffff;
  for (int i = 0; i < NUM_DEL; i++) {
    int d = *val - DEL_REPEAT_PRESETS[i];
    if (d < 0) d = -d;
    if (d < best) { best = d; idx = i; }
  }
  *val = DEL_REPEAT_PRESETS[(idx + 1) % NUM_DEL];
}

static void haptic_cycle_next(T9Settings *s) {
  int idx = 0, best = 0x7fffffff;
  for (int i = 0; i < NUM_HAPTIC; i++) {              // find closest preset
    int d = s->haptic_ms - HAPTIC_PRESETS[i];
    if (d < 0) d = -d;
    if (d < best) { best = d; idx = i; }
  }
  s->haptic_ms = HAPTIC_PRESETS[(idx + 1) % NUM_HAPTIC];
}

// ---- MenuLayer callbacks ----------------------------------------------------

// Logical rows, in display order. The "App theme" row only appears when the
// host registered a custom theme, so srow() maps a visible index to its logical
// row by skipping R_APPTHEME when there is none — every callback switches on the
// logical row, so adding/reordering rows stays a one-line change here.
typedef enum {
  R_WAIT = 0, R_AUTOCAP, R_DBLSPACE, R_SPECIAL, R_THEME, R_APPTHEME,
  R_HAPTICS, R_HAPTIC_MS, R_DELETE, R_DELSPEED, R_HELP, R_MAX
} SRow;

static SRow srow(uint16_t visible) {
  if (!t9_keyboard_has_app_theme() && visible >= R_APPTHEME) visible++;
  return (SRow)visible;
}

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return t9_keyboard_has_app_theme() ? R_MAX : R_MAX - 1;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  char sub[24];
  const char *title = "";
  switch (srow(idx->row)) {
    case R_WAIT:
      title = "Key wait time";
      snprintf(sub, sizeof(sub), "%d ms", s_settings.commit_timeout_ms);
      break;
    case R_AUTOCAP:
      title = "Auto-capitalize";
      snprintf(sub, sizeof(sub), "%s", s_settings.auto_caps ? "On" : "Off");
      break;
    case R_DBLSPACE:
      title = "Double-space period";
      snprintf(sub, sizeof(sub), "%s", s_settings.two_space_period ? "On" : "Off");
      break;
    case R_SPECIAL: {
      title = "Special characters";
      int n = t9_keyboard_ext_count(), on = 0;
      for (int i = 0; i < n; i++) if (t9_keyboard_ext_enabled(s_kb, i)) on++;
      snprintf(sub, sizeof(sub), "%s", (on == n) ? "On" : (on == 0) ? "Off" : "Mixed");
      break;
    }
    case R_THEME:
      title = "Theme";
      snprintf(sub, sizeof(sub), "%s", t9_keyboard_theme_name(s_global_theme));
      break;
    case R_APPTHEME:   // on -> show the app's theme name; off -> the global pick wins
      title = "App theme";
      snprintf(sub, sizeof(sub), "%s",
               s_respect_app ? t9_keyboard_theme_name(t9_keyboard_app_theme_index())
                             : "Off");
      break;
    case R_HAPTICS:
      title = "Haptics";
      snprintf(sub, sizeof(sub), "%s", s_settings.haptics ? "On" : "Off");
      break;
    case R_HAPTIC_MS:
      title = "Haptic strength";
      snprintf(sub, sizeof(sub), "%d ms", s_settings.haptic_ms);
      break;
    case R_DELETE:
      title = "Delete";
      snprintf(sub, sizeof(sub), "%s", s_settings.delete_mode == 1 ? "Words" : "Characters");
      break;
    case R_DELSPEED: {
      title = "Delete speed";
      int v = (s_settings.delete_mode == 1) ? s_settings.del_repeat_words_ms
                                            : s_settings.del_repeat_chars_ms;
      snprintf(sub, sizeof(sub), "%d ms", v);
      break;
    }
    case R_HELP:
      title = "Help";
      snprintf(sub, sizeof(sub), "%s", "How to type");
      break;
    default: sub[0] = '\0';
  }
  menu_cell_basic_draw(ctx, cell, title, sub, NULL);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  switch (srow(idx->row)) {
    case R_WAIT: wait_cycle_next(&s_settings); break;
    case R_AUTOCAP: s_settings.auto_caps = !s_settings.auto_caps; break;
    case R_DBLSPACE: s_settings.two_space_period = !s_settings.two_space_period; break;
    case R_HAPTICS: s_settings.haptics = !s_settings.haptics; break;
    case R_HAPTIC_MS: haptic_cycle_next(&s_settings); break;  // pulse length
    case R_DELETE: s_settings.delete_mode ^= 1; break;        // characters <-> words
    case R_DELSPEED: del_repeat_cycle_next(&s_settings); break;
    case R_SPECIAL: char_window_push(); return;   // open the character sub-menu
    case R_HELP: help_window_push(); return;      // open the help page
    case R_THEME:                                 // cycle the global (built-in) pick
      s_global_theme = (s_global_theme + 1) % t9_keyboard_theme_count();
      persist_write_int(PKEY_THEME, s_global_theme);
      theme_apply(s_kb);   // no visible change while the app theme is respected
      menu_layer_reload_data(s_menu);
      return;
    case R_APPTHEME:                              // use app theme vs. global pick
      s_respect_app = !s_respect_app;
      persist_write_bool(PKEY_RESPECT_APP, s_respect_app);
      theme_apply(s_kb);
      menu_layer_reload_data(s_menu);
      return;
    default: break;
  }
  t9_keyboard_set_settings(s_kb, &s_settings);
  settings_save(&s_settings);
  menu_layer_reload_data(s_menu);
}

// ---- Window lifecycle -------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_num_rows,
    .draw_row     = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void prv_window_unload(Window *window) {
  menu_layer_destroy(s_menu);
  window_destroy(s_window);
  s_window = NULL;
  s_menu = NULL;
}

// ---- Character sub-menu (Toggle all + one row per extended character) --------

static bool prv_all_ext_on(void) {
  int n = t9_keyboard_ext_count();
  for (int i = 0; i < n; i++) if (!t9_keyboard_ext_enabled(s_kb, i)) return false;
  return true;
}

static uint16_t prv_char_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return t9_keyboard_ext_count() + 1;   // +1 for the "Toggle all" row
}

static void prv_char_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  if (idx->row == 0) {
    int n = t9_keyboard_ext_count(), on = 0;
    for (int i = 0; i < n; i++) if (t9_keyboard_ext_enabled(s_kb, i)) on++;
    const char *sub = (on == n) ? "All on" : (on == 0) ? "All off" : "Mixed";
    menu_cell_basic_draw(ctx, cell, "Toggle all", sub, NULL);
  } else {
    int i = idx->row - 1;
    menu_cell_basic_draw(ctx, cell, t9_keyboard_ext_glyph(i),
                         t9_keyboard_ext_enabled(s_kb, i) ? "On" : "Off", NULL);
  }
}

static void prv_char_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (idx->row == 0) {
    bool target = !prv_all_ext_on();          // all-on -> off, otherwise -> on
    int n = t9_keyboard_ext_count();
    for (int i = 0; i < n; i++) t9_keyboard_ext_set_enabled(s_kb, i, target);
  } else {
    int i = idx->row - 1;
    t9_keyboard_ext_set_enabled(s_kb, i, !t9_keyboard_ext_enabled(s_kb, i));
  }
  ext_save(s_kb);
  menu_layer_reload_data(s_char_menu);
}

static void prv_char_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_char_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_char_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_char_num_rows,
    .draw_row     = prv_char_draw_row,
    .select_click = prv_char_select,
  });
  menu_layer_set_click_config_onto_window(s_char_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_char_menu));
}

static void prv_char_window_unload(Window *window) {
  menu_layer_destroy(s_char_menu);
  window_destroy(s_char_window);
  s_char_window = NULL;
  s_char_menu = NULL;
  // Reflect the new on/off count back in the parent menu.
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void char_window_push(void) {
  if (s_char_window) return;
  s_char_window = window_create();
  window_set_window_handlers(s_char_window, (WindowHandlers) {
    .load = prv_char_window_load,
    .unload = prv_char_window_unload,
  });
  window_stack_push(s_char_window, true);
}

// ---- Help page (scrollable text) --------------------------------------------

static Window     *s_help_window;
static ScrollLayer *s_help_scroll;
static TextLayer  *s_help_text;

static const char *HELP_TEXT =
  "TYPING\n"
  "Tap a key, then tap again to cycle its letters. Pause, or tap another key, "
  "to keep the letter.\n\n"
  "NUMBERS\n"
  "Hold any key for its number. On the 123 page, hold 0 for a space.\n\n"
  "SHIFT (bottom-left)\n"
  "Tap once for a capital, again for CAPS LOCK. Hold Shift to switch the "
  "layout. On the 123 and #+= pages, tapping it switches the layout.\n\n"
  "SPECIAL LETTERS\n"
  "Letters like a and o cycle to a A a A a. Turn them on or off in Settings.\n\n"
  "BUTTONS\n"
  "Up: switch layout (hold for Settings)\n"
  "OK: enter (hold for a new line)\n"
  "Down: backspace, hold to repeat (delete by letter or word in Settings)\n"
  "Back: go back";

static void prv_help_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_help_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_help_scroll, window);

  s_help_text = text_layer_create(GRect(4, 0, bounds.size.w - 8, 2000));
  text_layer_set_text(s_help_text, HELP_TEXT);
  text_layer_set_font(s_help_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_help_text, GTextOverflowModeWordWrap);

  GSize used = text_layer_get_content_size(s_help_text);
  text_layer_set_size(s_help_text, GSize(bounds.size.w - 8, used.h + 8));
  scroll_layer_set_content_size(s_help_scroll, GSize(bounds.size.w, used.h + 16));

  scroll_layer_add_child(s_help_scroll, text_layer_get_layer(s_help_text));
  layer_add_child(root, scroll_layer_get_layer(s_help_scroll));
}

static void prv_help_window_unload(Window *window) {
  text_layer_destroy(s_help_text);
  scroll_layer_destroy(s_help_scroll);
  window_destroy(s_help_window);
  s_help_window = NULL;
  s_help_scroll = NULL;
  s_help_text = NULL;
}

static void help_window_push(void) {
  if (s_help_window) return;
  s_help_window = window_create();
  window_set_window_handlers(s_help_window, (WindowHandlers) {
    .load = prv_help_window_load,
    .unload = prv_help_window_unload,
  });
  window_stack_push(s_help_window, true);
}

void settings_window_push(T9Keyboard *kb) {
  if (s_window) return;                 // already open
  s_kb = kb;
  t9_keyboard_get_settings(kb, &s_settings);
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
