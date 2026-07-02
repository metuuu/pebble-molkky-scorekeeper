#include "list_core.h"
#include "ui_theme.h"
#include "ui_text.h"
#include "ui_section.h"
#include "header.h"

// Distinct icons a single list can show. Each screen uses only a handful (the
// main menu's four is the most), so this is comfortably oversized.
#define LC_ICON_CACHE 8

struct ListCore {
  Window    *window;
  MenuLayer *menu;
  Header    *header;     // top title+clock bar, or NULL when not shown
  char       title[32];
  ListCoreConfig cfg;
  // Icons load lazily on first draw and are cached for the list's lifetime
  // (keyed by resource id), then recolored per-draw to the row's text color.
  struct { uint32_t res; GBitmap *bmp; } icons[LC_ICON_CACHE];
  uint8_t    icon_n;
};

// Icon resolver handed to list_item_draw: load-or-fetch by resource id. Keyed
// on the full lookup value, so tinted and raw uses of one resource get separate
// bitmaps (see LIST_ICON_RAW_BIT). Returns NULL for res 0, a load failure, or a
// full cache.
static GBitmap *lc_icon(void *ctx, uint32_t res) {
  ListCore *c = ctx;
  if (!res) return NULL;
  for (int i = 0; i < c->icon_n; i++) if (c->icons[i].res == res) return c->icons[i].bmp;
  if (c->icon_n >= LC_ICON_CACHE) return NULL;
  GBitmap *bmp = gbitmap_create_with_resource(res & ~LIST_ICON_RAW_BIT);
  c->icons[c->icon_n].res = res;
  c->icons[c->icon_n].bmp = bmp;
  c->icon_n++;
  return bmp;
}

// Map a (section, row) cell to the host's flat row index — the rows of every
// earlier section, plus this row. Without a section map it's just idx->row.
static uint16_t lc_flat(ListCore *c, const MenuIndex *idx) {
  if (!c->cfg.get_sections) return idx->row;
  uint16_t flat = idx->row;
  for (uint16_t s = 0; s < idx->section; s++) flat += c->cfg.get_section_count(c->cfg.ctx, s);
  return flat;
}
static void lc_get(ListCore *c, const MenuIndex *idx, ListItem *out) {
  *out = list_item_empty();
  c->cfg.get_item(c->cfg.ctx, lc_flat(c, idx), out);
}

static uint16_t lc_sections(MenuLayer *ml, void *ctx) {
  ListCore *c = ctx; return c->cfg.get_sections ? c->cfg.get_sections(c->cfg.ctx) : 1;
}
static uint16_t lc_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  ListCore *c = ctx;
  return c->cfg.get_sections ? c->cfg.get_section_count(c->cfg.ctx, s)
                             : c->cfg.get_count(c->cfg.ctx);
}
static int16_t lc_header_h(MenuLayer *ml, uint16_t s, void *ctx) {
  ListCore *c = ctx;
  if (!c->cfg.section_title) return 0;
  const char *t = c->cfg.section_title(c->cfg.ctx, s);
  if (!t || !t[0]) return 0;
  return ui_section_height(s > 0);   // sections after the first carry a blank gap above
}
static void lc_draw_header(GContext *g, const Layer *cl, uint16_t s, void *ctx) {
  ListCore *c = ctx;
  const char *t = c->cfg.section_title ? c->cfg.section_title(c->cfg.ctx, s) : NULL;
  ui_section_draw(g, layer_get_bounds(cl), t, s > 0);
}
static int16_t lc_cell_h(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  ListCore *c = ctx;
  ListItem item;
  lc_get(c, idx, &item);
  return list_item_height(&item, c->cfg.size);
}
static void lc_draw(GContext *g, const Layer *cl, MenuIndex *idx, void *ctx) {
  ListCore *c = ctx;
  ListItem item;
  lc_get(c, idx, &item);
  // Use the cell's animated highlight (not the selected index) so text color
  // flips in lockstep with the sliding bar; keying off the index flashes the
  // in/out cells, since the index jumps instantly while the highlight animates.
  RowState st = {
    .highlighted = menu_cell_layer_is_highlighted(cl),
    .interactive = c->cfg.interactive,
    .disabled    = item.disabled,
  };
  list_item_draw(g, layer_get_bounds(cl), &item, st, c->cfg.size, lc_icon, c);
}
static void lc_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  ListCore *c = ctx;
  ListItem item;
  lc_get(c, idx, &item);
  if (item.disabled) return;                         // focusable but inert
  if (c->cfg.on_select) c->cfg.on_select(c->cfg.ctx, lc_flat(c, idx));
}

// If the selected row is disabled, move the highlight to the first enabled row
// so the list never opens with a dead button focused. Interactive lists only.
static void lc_skip_disabled(ListCore *c) {
  if (!c->cfg.interactive) return;
  MenuIndex cur = menu_layer_get_selected_index(c->menu);
  uint16_t nsec = lc_sections(NULL, c);
  ListItem item;
  if (cur.section < nsec && cur.row < lc_rows(NULL, cur.section, c)) {
    lc_get(c, &cur, &item);
    if (!item.disabled) return;
  }
  for (uint16_t s = 0; s < nsec; s++) {
    uint16_t n = lc_rows(NULL, s, c);
    for (uint16_t i = 0; i < n; i++) {
      MenuIndex mi = MenuIndex(s, i);
      lc_get(c, &mi, &item);
      if (!item.disabled) {
        menu_layer_set_selected_index(c->menu, mi, MenuRowAlignCenter, false);
        return;
      }
    }
  }
}

// --- interactive navigation with edge wrap -----------------------------------
// Pebble's MenuLayer stops dead at the first/last row. For interactive menus we
// install our own click config so a distinct Up press on the first row wraps to
// the last and a distinct Down press on the last wraps to the first; every step
// in between moves normally (and still animates/scrolls, since we drive it
// through menu_layer_set_selected_*). Holding the button, however, only repeats
// while there's somewhere to go — it stops dead at the edge rather than looping,
// so scrolling through a long list never runs off into a wrap-around.
#define LC_REPEAT_MS 100   // held Up/Down repeat interval

static MenuIndex lc_first_index(ListCore *c) {
  uint16_t nsec = lc_sections(NULL, c);
  for (uint16_t s = 0; s < nsec; s++)
    if (lc_rows(NULL, s, c) > 0) return MenuIndex(s, 0);
  return MenuIndex(0, 0);
}
static MenuIndex lc_last_index(ListCore *c) {
  uint16_t nsec = lc_sections(NULL, c);
  for (int s = (int)nsec - 1; s >= 0; s--) {
    uint16_t n = lc_rows(NULL, (uint16_t)s, c);
    if (n > 0) return MenuIndex((uint16_t)s, n - 1);
  }
  return MenuIndex(0, 0);
}
static bool lc_index_eq(MenuIndex a, MenuIndex b) {
  return a.section == b.section && a.row == b.row;
}

static void lc_click_up(ClickRecognizerRef ref, void *ctx) {
  ListCore *c = ctx;
  MenuIndex cur = menu_layer_get_selected_index(c->menu);
  if (lc_index_eq(cur, lc_first_index(c))) {              // at the top
    if (click_recognizer_is_repeating(ref)) return;       // held: stop at the edge, don't loop
    menu_layer_set_selected_index(c->menu, lc_last_index(c), MenuRowAlignCenter, true);  // distinct press: wrap to the bottom
  } else {
    menu_layer_set_selected_next(c->menu, true, MenuRowAlignCenter, true);
  }
}
static void lc_click_down(ClickRecognizerRef ref, void *ctx) {
  ListCore *c = ctx;
  MenuIndex cur = menu_layer_get_selected_index(c->menu);
  if (lc_index_eq(cur, lc_last_index(c))) {               // at the bottom
    if (click_recognizer_is_repeating(ref)) return;       // held: stop at the edge, don't loop
    menu_layer_set_selected_index(c->menu, lc_first_index(c), MenuRowAlignCenter, true);  // distinct press: wrap to the top
  } else {
    menu_layer_set_selected_next(c->menu, false, MenuRowAlignCenter, true);
  }
}
static void lc_click_select(ClickRecognizerRef ref, void *ctx) {
  ListCore *c = ctx;
  // MenuLayer reports index (0,0) even with zero rows — don't ask the host for
  // an item (or fire on_select) that doesn't exist.
  uint16_t nsec = lc_sections(NULL, c);
  bool any = false;
  for (uint16_t s = 0; s < nsec && !any; s++) any = lc_rows(NULL, s, c) > 0;
  if (!any) return;
  MenuIndex idx = menu_layer_get_selected_index(c->menu);
  lc_select(c->menu, &idx, c);
}
static void lc_click_back(ClickRecognizerRef ref, void *ctx) { window_stack_pop(true); }
static void lc_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, LC_REPEAT_MS, lc_click_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, LC_REPEAT_MS, lc_click_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, lc_click_select);
  window_single_click_subscribe(BUTTON_ID_BACK, lc_click_back);
}

// Reconcile header visibility and resize the menu.
static void lc_layout(ListCore *c) {
  Layer *root = window_get_root_layer(c->window);
  GRect rb = layer_get_bounds(root);
  bool want = header_enabled() && c->title[0];
  if (want && !c->header) {
    c->header = header_create(root, c->title, header_icon());
  } else if (!want && c->header) {
    header_destroy(c->header);
    c->header = NULL;
  }
  int top = c->header ? HEADER_H : 0;
  layer_set_frame(menu_layer_get_layer(c->menu),
                  GRect(rb.origin.x, rb.origin.y + top, rb.size.w, rb.size.h - top));
}

static void lc_appear(Window *w) {
  ListCore *c = window_get_user_data(w);
  lc_layout(c);          // reconcile the header: the setting may have toggled in a sub-window
  menu_layer_reload_data(c->menu);
  lc_skip_disabled(c);
}
static void lc_unload(Window *w) {
  ListCore *c = window_get_user_data(w);
  if (c->cfg.on_unload) c->cfg.on_unload(c->cfg.ctx);   // let the host drop its pointer to us
  for (int i = 0; i < c->icon_n; i++) if (c->icons[i].bmp) gbitmap_destroy(c->icons[i].bmp);
  if (c->header) header_destroy(c->header);
  menu_layer_destroy(c->menu);
  window_destroy(c->window);
  free(c);
}
static void lc_load(Window *w) {
  ListCore *c = window_get_user_data(w);
  Layer *root = window_get_root_layer(w);
  c->menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(c->menu, c, (MenuLayerCallbacks) {
    .get_num_sections = lc_sections,
    .get_num_rows = lc_rows,
    .get_cell_height = lc_cell_h,
    .get_header_height = lc_header_h,
    .draw_header = lc_draw_header,
    .draw_row = lc_draw,
    .select_click = lc_select,
  });
  menu_layer_set_normal_colors(c->menu, ui_background(), ui_text());
  if (c->cfg.interactive)
    menu_layer_set_highlight_colors(c->menu, ui_accent(), ui_accent_text());
  else
    menu_layer_set_highlight_colors(c->menu, ui_background(), ui_text());   // no cursor on a display list
  if (c->cfg.interactive)
    // Our own config: Up/Down step with edge wrap, Select fires, Back pops.
    window_set_click_config_provider_with_context(w, lc_click_config, c);
  else
    menu_layer_set_click_config_onto_window(c->menu, w);   // display list: plain Up/Down scroll
  layer_add_child(root, menu_layer_get_layer(c->menu));
  lc_layout(c);          // add the header (if enabled) and size the menu below it
  lc_skip_disabled(c);   // don't render the first frame with a disabled row focused
}

ListCore *list_core_push(const char *title, ListCoreConfig cfg) {
  ListCore *c = malloc(sizeof(ListCore));
  if (!c) return NULL;
  c->cfg = cfg;
  c->icon_n = 0;
  c->header = NULL;
  strncpy(c->title, title ? title : "", sizeof(c->title) - 1);
  c->title[sizeof(c->title) - 1] = '\0';
  c->window = window_create();
  window_set_background_color(c->window, ui_background());
  window_set_user_data(c->window, c);
  window_set_window_handlers(c->window, (WindowHandlers) {
    .load = lc_load, .appear = lc_appear, .unload = lc_unload,
  });
  window_stack_push(c->window, true);
  return c;
}
void    list_core_reload(ListCore *c) {
  if (c && c->menu) { lc_layout(c); menu_layer_reload_data(c->menu); }
}
Window *list_core_window(ListCore *c) { return c ? c->window : NULL; }

void list_core_set_header_enabled(bool enabled) { header_set_enabled(enabled); }
bool list_core_header_enabled(void) { return header_enabled(); }
