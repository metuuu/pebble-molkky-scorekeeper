#include "list_core.h"
#include "ui_theme.h"

// Distinct icons a single list can show. Each screen uses only a handful (the
// main menu's four is the most), so this is comfortably oversized.
#define LC_ICON_CACHE 8

struct ListCore {
  Window    *window;
  MenuLayer *menu;
  char       title[32];
  ListCoreConfig cfg;
  // Icons load lazily on first draw and are cached for the list's lifetime
  // (keyed by resource id), then recolored per-draw to the row's text color.
  struct { uint32_t res; GBitmap *bmp; } icons[LC_ICON_CACHE];
  uint8_t    icon_n;
};

// Icon resolver handed to list_item_draw: load-or-fetch by resource id. Returns
// NULL for res 0, a load failure, or a full cache.
static GBitmap *lc_icon(void *ctx, uint32_t res) {
  ListCore *c = ctx;
  if (!res) return NULL;
  for (int i = 0; i < c->icon_n; i++) if (c->icons[i].res == res) return c->icons[i].bmp;
  if (c->icon_n >= LC_ICON_CACHE) return NULL;
  GBitmap *bmp = gbitmap_create_with_resource(res);
  c->icons[c->icon_n].res = res;
  c->icons[c->icon_n].bmp = bmp;
  c->icon_n++;
  return bmp;
}

static uint16_t lc_sections(MenuLayer *ml, void *ctx) { return 1; }
static uint16_t lc_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  ListCore *c = ctx; return c->cfg.get_count(c->cfg.ctx);
}
static int16_t lc_cell_h(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  ListCore *c = ctx;
  ListItem item = list_item_empty();
  c->cfg.get_item(c->cfg.ctx, idx->row, &item);
  return list_item_height(&item, c->cfg.size);
}
static void lc_draw(GContext *g, const Layer *cl, MenuIndex *idx, void *ctx) {
  ListCore *c = ctx;
  ListItem item = list_item_empty();
  c->cfg.get_item(c->cfg.ctx, idx->row, &item);
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
  ListItem item = list_item_empty();
  c->cfg.get_item(c->cfg.ctx, idx->row, &item);
  if (item.disabled) return;                         // focusable but inert
  if (c->cfg.on_select) c->cfg.on_select(c->cfg.ctx, idx->row);
}

// If the selected row is disabled, move the highlight to the first enabled row
// so the list never opens with a dead button focused. Interactive lists only.
static void lc_skip_disabled(ListCore *c) {
  if (!c->cfg.interactive) return;
  uint16_t n = c->cfg.get_count(c->cfg.ctx);
  uint16_t cur = menu_layer_get_selected_index(c->menu).row;
  ListItem item = list_item_empty();
  if (cur < n) { c->cfg.get_item(c->cfg.ctx, cur, &item); if (!item.disabled) return; }
  for (uint16_t i = 0; i < n; i++) {
    item = list_item_empty();
    c->cfg.get_item(c->cfg.ctx, i, &item);
    if (!item.disabled) {
      menu_layer_set_selected_index(c->menu, MenuIndex(0, i), MenuRowAlignCenter, false);
      return;
    }
  }
}

static void lc_appear(Window *w) {
  ListCore *c = window_get_user_data(w);
  menu_layer_reload_data(c->menu);
  lc_skip_disabled(c);
}
static void lc_unload(Window *w) {
  ListCore *c = window_get_user_data(w);
  if (c->cfg.on_unload) c->cfg.on_unload(c->cfg.ctx);   // let the host drop its pointer to us
  for (int i = 0; i < c->icon_n; i++) if (c->icons[i].bmp) gbitmap_destroy(c->icons[i].bmp);
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
    .draw_row = lc_draw,
    .select_click = lc_select,
  });
  menu_layer_set_normal_colors(c->menu, ui_background(), ui_text());
  if (c->cfg.interactive)
    menu_layer_set_highlight_colors(c->menu, ui_accent(), ui_accent_text());
  else
    menu_layer_set_highlight_colors(c->menu, ui_background(), ui_text());   // no cursor on a display list
  menu_layer_set_click_config_onto_window(c->menu, w);   // Up/Down scroll, Select, Back pops
  layer_add_child(root, menu_layer_get_layer(c->menu));
  lc_skip_disabled(c);   // don't render the first frame with a disabled row focused
}

ListCore *list_core_push(const char *title, ListCoreConfig cfg) {
  ListCore *c = malloc(sizeof(ListCore));
  if (!c) return NULL;
  c->cfg = cfg;
  c->icon_n = 0;
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
void    list_core_reload(ListCore *c) { if (c && c->menu) menu_layer_reload_data(c->menu); }
Window *list_core_window(ListCore *c) { return c ? c->window : NULL; }
