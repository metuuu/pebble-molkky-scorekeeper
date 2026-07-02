#include "paged_list.h"
#include "ui_theme.h"
#include "ui_text.h"
#include "button.h"
#include "header.h"

// MenuLayer plus a fixed pager bar. Up/Down can move focus between list and pager.

#define PAGER_H   30
#define BTN_W     30          // each pager button's width
#define NBTN      4
#define LPAD      6
#define ICON_CACHE 8

struct PagedList {
  Window         *window;
  MenuLayer      *menu;
  Layer          *pager;
  Header         *header;          // top title+clock bar, or NULL when not shown
  char            title[32];
  PagedListConfig cfg;
  bool            focus_pager;     // focus is in the pager bar (else in the list)
  int             btn;             // focused pager button (PAGER_FIRST..PAGER_LAST)
  bool            pager_shown;     // pager bar currently visible (only when there's something to page)
  bool            pager_laid_out;  // has visibility been applied at least once?
  PagerModel      pm;              // refreshed before each input / draw
  struct { uint32_t res; GBitmap *bmp; } icons[ICON_CACHE];
  uint8_t         icon_n;
};

// ---- icon cache (ACC_ICON rows + pager glyphs), same shape as list_core ----
static GBitmap *pl_icon(void *ctx, uint32_t res) {
  PagedList *p = ctx;
  if (!res) return NULL;
  for (int i = 0; i < p->icon_n; i++) if (p->icons[i].res == res) return p->icons[i].bmp;
  if (p->icon_n >= ICON_CACHE) return NULL;
  GBitmap *bmp = gbitmap_create_with_resource(res & ~LIST_ICON_RAW_BIT);
  p->icons[p->icon_n].res = res; p->icons[p->icon_n].bmp = bmp; p->icon_n++;
  return bmp;
}

// ---- pager model + button helpers ----
static void refresh_pm(PagedList *p) {
  p->pm = (PagerModel){0};
  p->pm.total_pages = 1;
  if (p->cfg.get_pager) p->cfg.get_pager(p->cfg.ctx, &p->pm);
}
static bool btn_enabled(PagedList *p, int b) {
  if (p->pm.busy) return false;
  return (b == PAGER_FIRST || b == PAGER_PREV) ? p->pm.has_prev : p->pm.has_next;
}
static int first_enabled(PagedList *p) {
  for (int b = 0; b < NBTN; b++) if (btn_enabled(p, b)) return b;
  return -1;
}
static int next_enabled(PagedList *p, int from) {
  for (int b = from + 1; b < NBTN; b++) if (btn_enabled(p, b)) return b;
  return -1;
}
static int prev_enabled(PagedList *p, int from) {
  for (int b = from - 1; b >= 0; b--) if (btn_enabled(p, b)) return b;
  return -1;
}

// When focus is on the pager, suppress the list's selection highlight so only
// one focus ring shows at a time.
static void apply_menu_focus(PagedList *p) {
  if (p->focus_pager)
    menu_layer_set_highlight_colors(p->menu, ui_background(), ui_text());
  else
    menu_layer_set_highlight_colors(p->menu, ui_accent(), ui_accent_text());
  layer_mark_dirty(menu_layer_get_layer(p->menu));
  if (p->pager) layer_mark_dirty(p->pager);
}

// Hide the pager when there is no navigation or status to show.
static bool pager_wanted(PagedList *p) {
  if (!p->cfg.get_pager) return false;
  refresh_pm(p);
  return p->pm.has_prev || p->pm.has_next || p->pm.busy || p->pm.status[0];
}
// Space reserved at the top for the header bar (0 when there's no header).
static int pl_top(PagedList *p) { return p->header ? HEADER_H : 0; }

static void apply_pager_visibility(PagedList *p) {
  if (!p->pager) return;
  bool show = pager_wanted(p);
  if (p->pager_laid_out && show == p->pager_shown) return;     // no change
  p->pager_laid_out = true;
  p->pager_shown = show;
  GRect rb = layer_get_bounds(window_get_root_layer(p->window));
  int top = pl_top(p);
  layer_set_frame(menu_layer_get_layer(p->menu),
                  GRect(0, top, rb.size.w, rb.size.h - top - (show ? PAGER_H : 0)));
  layer_set_hidden(p->pager, !show);
  if (!show && p->focus_pager) { p->focus_pager = false; apply_menu_focus(p); }
}

// ---- MenuLayer callbacks ----
static uint16_t mc_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  PagedList *p = ctx; return p->cfg.get_count(p->cfg.ctx);
}
static int16_t mc_cell_h(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  PagedList *p = ctx;
  ListItem item = list_item_empty();
  p->cfg.get_item(p->cfg.ctx, idx->row, &item);
  return list_item_height(&item, p->cfg.size);
}
static void mc_draw(GContext *g, const Layer *cl, MenuIndex *idx, void *ctx) {
  PagedList *p = ctx;
  ListItem item = list_item_empty();
  p->cfg.get_item(p->cfg.ctx, idx->row, &item);
  RowState st = {
    .highlighted = menu_cell_layer_is_highlighted(cl) && !p->focus_pager,
    .interactive = true,
    .disabled    = item.disabled,
  };
  list_item_draw(g, layer_get_bounds(cl), &item, st, p->cfg.size, pl_icon, p);
}

// ---- pager bar drawing ----
// Nav button icons, indexed by PagerNav.
static const uint32_t PAGER_ICONS[NBTN] = {
  RESOURCE_ID_IMAGE_PAGER_FIRST, RESOURCE_ID_IMAGE_PAGER_PREV,
  RESOURCE_ID_IMAGE_PAGER_NEXT,  RESOURCE_ID_IMAGE_PAGER_LAST,
};
static void pager_update(Layer *layer, GContext *ctx) {
  PagedList *p = *(PagedList **)layer_get_data(layer);
  refresh_pm(p);
  GRect b = layer_get_bounds(layer);

  // Hairline divider above the bar.
  graphics_context_set_stroke_color(ctx, ui_text_muted());
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(b.origin.x, b.origin.y), GPoint(b.origin.x + b.size.w - 1, b.origin.y));

  // Left label: a busy/status string, or the page indicator.
  char label[32];
  if (p->pm.status[0]) snprintf(label, sizeof label, "%s", p->pm.status);
  else if (p->pm.total_pages > 0) snprintf(label, sizeof label, "%d / %d", p->pm.page + 1, p->pm.total_pages);
  else snprintf(label, sizeof label, "%d / ?", p->pm.page + 1);
  graphics_context_set_text_color(ctx, ui_text());
  ui_text_draw(ctx, label, UI_FONT_BODY_BOLD,
               GRect(b.origin.x + LPAD, b.origin.y + 2, b.size.w - NBTN * BTN_W - LPAD, b.size.h),
               GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);

  // Right: the four buttons. Muted when disabled; accent pill when focused.
  int x0 = b.origin.x + b.size.w - NBTN * BTN_W;
  for (int i = 0; i < NBTN; i++) {
    GRect r = GRect(x0 + i * BTN_W, b.origin.y + 1, BTN_W, b.size.h - 1);
    bool enabled = btn_enabled(p, i);
    bool focused = p->focus_pager && p->btn == i;
    // Focused button gets an accent pill; at rest only the glyph is drawn.
    UiButtonSpec spec = {
      .style    = focused ? UI_BTN_SOLID   : UI_BTN_GHOST,
      .scheme   = focused ? UI_BTN_PRIMARY : UI_BTN_NEUTRAL,
      .disabled = !enabled,
      .radius   = 4,
      .icon     = pl_icon(p, PAGER_ICONS[i]),
    };
    ui_button_draw(ctx, GRect(r.origin.x + 2, r.origin.y + 2, r.size.w - 4, r.size.h - 4), &spec);
  }
}

// ---- input (our own window click config) ----
static void pl_up(ClickRecognizerRef ref, void *context) {
  PagedList *p = context;
  refresh_pm(p);
  if (p->focus_pager) {
    int prev = prev_enabled(p, p->btn);
    if (prev >= 0) { p->btn = prev; layer_mark_dirty(p->pager); }
    else {                                                    // exit pager → back to the list
      if (click_recognizer_is_repeating(ref)) return;         // held: stop at the boundary
      p->focus_pager = false; apply_menu_focus(p);
    }
  } else {
    MenuIndex idx = menu_layer_get_selected_index(p->menu);
    if (idx.row > 0) menu_layer_set_selected_index(p->menu, MenuIndex(0, idx.row - 1), MenuRowAlignCenter, true);
  }
}
static void pl_down(ClickRecognizerRef ref, void *context) {
  PagedList *p = context;
  refresh_pm(p);
  if (p->focus_pager) {
    int next = next_enabled(p, p->btn);
    if (next >= 0) { p->btn = next; layer_mark_dirty(p->pager); }
  } else {
    uint16_t n = p->cfg.get_count(p->cfg.ctx);
    MenuIndex idx = menu_layer_get_selected_index(p->menu);
    if (idx.row + 1 < n) {
      menu_layer_set_selected_index(p->menu, MenuIndex(0, idx.row + 1), MenuRowAlignCenter, true);
    } else {                                                  // at the last row → enter the pager
      if (click_recognizer_is_repeating(ref)) return;         // held: stop at the boundary — a
                                                              // long scroll must not run into it
      int first = first_enabled(p);
      if (first >= 0) { p->focus_pager = true; p->btn = first; apply_menu_focus(p); }
    }
  }
}
static void pl_select(ClickRecognizerRef ref, void *context) {
  PagedList *p = context;
  refresh_pm(p);
  if (p->focus_pager) {
    if (btn_enabled(p, p->btn) && p->cfg.on_nav) p->cfg.on_nav(p->cfg.ctx, (PagerNav)p->btn);
  } else if (p->cfg.get_count(p->cfg.ctx) > 0 && p->cfg.on_select) {
    p->cfg.on_select(p->cfg.ctx, menu_layer_get_selected_index(p->menu).row);
  }
}
static void pl_back(ClickRecognizerRef ref, void *context) { window_stack_pop(true); }
static void pl_ccp(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 140, pl_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 140, pl_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, pl_select);
  window_single_click_subscribe(BUTTON_ID_BACK, pl_back);
}

// ---- window lifecycle ----
static void pl_load(Window *w) {
  PagedList *p = window_get_user_data(w);
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);

  // Top header bar (title + clock + brand icon), when the app has it enabled.
  if (header_enabled() && p->title[0])
    p->header = header_create(root, p->title, header_icon());
  int top = pl_top(p);

  p->menu = menu_layer_create(GRect(0, top, b.size.w, b.size.h - top));  // trimmed further if the pager shows
  menu_layer_set_callbacks(p->menu, p, (MenuLayerCallbacks){
    .get_num_rows = mc_rows,
    .get_cell_height = mc_cell_h,
    .draw_row = mc_draw,
  });
  menu_layer_set_normal_colors(p->menu, ui_background(), ui_text());
  layer_add_child(root, menu_layer_get_layer(p->menu));

  if (p->cfg.get_pager) {                    // create the bar; shown only when it has pages/status
    p->pager = layer_create_with_data(GRect(0, b.size.h - PAGER_H, b.size.w, PAGER_H), sizeof(PagedList *));
    *(PagedList **)layer_get_data(p->pager) = p;
    layer_set_update_proc(p->pager, pager_update);
    layer_add_child(root, p->pager);
  }
  apply_pager_visibility(p);                 // size the list to whether the pager is present
  apply_menu_focus(p);                       // list focused initially
  window_set_click_config_provider_with_context(w, pl_ccp, p);
}
static void pl_unload(Window *w) {
  PagedList *p = window_get_user_data(w);
  if (p->cfg.on_unload) p->cfg.on_unload(p->cfg.ctx);
  for (int i = 0; i < p->icon_n; i++) if (p->icons[i].bmp) gbitmap_destroy(p->icons[i].bmp);
  if (p->header) header_destroy(p->header);
  if (p->pager) layer_destroy(p->pager);
  menu_layer_destroy(p->menu);
  window_destroy(p->window);
  free(p);
}

PagedList *paged_list_push(const char *title, PagedListConfig cfg) {
  PagedList *p = malloc(sizeof(PagedList));
  if (!p) return NULL;
  *p = (PagedList){0};
  p->cfg = cfg;
  strncpy(p->title, title ? title : "", sizeof(p->title) - 1);
  p->window = window_create();
  window_set_background_color(p->window, ui_background());
  window_set_user_data(p->window, p);
  window_set_window_handlers(p->window, (WindowHandlers){ .load = pl_load, .unload = pl_unload });
  window_stack_push(p->window, true);
  return p;
}

void paged_list_reload(PagedList *p) {
  if (!p || !p->menu) return;
  menu_layer_reload_data(p->menu);
  refresh_pm(p);
  apply_pager_visibility(p);                              // pages/status may have appeared or vanished
  if (p->focus_pager && !btn_enabled(p, p->btn)) {        // focused button went away (e.g. busy)
    int f = first_enabled(p);
    if (f >= 0) p->btn = f; else p->focus_pager = false;
    apply_menu_focus(p);
  }
  layer_mark_dirty(menu_layer_get_layer(p->menu));
  if (p->pager) layer_mark_dirty(p->pager);
}
void paged_list_top(PagedList *p) {
  if (!p || !p->menu) return;
  p->focus_pager = false;
  menu_layer_set_selected_index(p->menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  apply_menu_focus(p);
  paged_list_reload(p);
}
Window *paged_list_window(PagedList *p) { return p ? p->window : NULL; }
