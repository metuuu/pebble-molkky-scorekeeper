#include "view.h"
#include "ui_theme.h"
#include "ui_section.h"

#define VIEW_ICON_CACHE 8
#define VIEW_BUILD_CAP  32       // block buffer for dynamic (build) views
#define LPAD  6                  // horizontal padding, matching list_item
#define BAR_W 2                  // scrollbar thumb width
#define BAR_MIN 16               // shortest the thumb gets

struct View {
  Window      *window;
  ScrollLayer *scroll;
  Layer       *content;
  Layer       *bar;             // fixed scrollbar overlay (over the scroll layer)
  Footer      *ftr;
  Block       *blocks;
  int          n;
  int          width;
  int          viewport_h;      // scroll height = window height - footer
  int          content_h;
  UiSize       size;
  ScrollSpeed  speed;
  bool         hide_scrollbar;
  bool         armed;           // footer action focused
  // config callbacks
  void       (*on_select)(void);
  void       (*on_back)(void);
  int        (*build)(Block *out, int cap, int avail_h);
  bool         footer;
  void       (*get_footer)(FooterModel *out);
  bool       (*can_arm)(void);
  void       (*on_action)(void);
  struct { uint32_t res; GBitmap *bmp; } icons[VIEW_ICON_CACHE];
  uint8_t      icon_n;
};

// --- block constructors ------------------------------------------------------
Block block_section(const char *title) {
  Block b = { .kind = BLOCK_SECTION };
  snprintf(b.section, sizeof(b.section), "%s", title ? title : "");
  return b;
}
Block block_item(ListItem item) {
  Block b = { .kind = BLOCK_ITEM }; b.item = item; return b;
}
static Block field_block(const char *label, const char *value, bool stacked) {
  Block b = { .kind = BLOCK_FIELD };
  b.field.stacked = stacked;
  snprintf(b.field.label, sizeof(b.field.label), "%s", label ? label : "");
  snprintf(b.field.value, sizeof(b.field.value), "%s", value ? value : "");
  return b;
}
Block block_field(const char *label, const char *value)        { return field_block(label, value, true); }
Block block_field_inline(const char *label, const char *value) { return field_block(label, value, false); }
Block block_gap(int16_t pixels) {
  Block b = { .kind = BLOCK_GAP }; b.gap = pixels; return b;
}
Block block_custom(int16_t height, BlockDraw draw, void *data) {
  Block b = { .kind = BLOCK_CUSTOM };
  b.custom.h = height; b.custom.draw = draw; b.custom.data = data;
  return b;
}

// --- icon cache (for ITEM blocks with ACC_ICON) ------------------------------
static GBitmap *view_icon(void *ctx, uint32_t res) {
  View *v = ctx;
  if (!res) return NULL;
  for (int i = 0; i < v->icon_n; i++) if (v->icons[i].res == res) return v->icons[i].bmp;
  if (v->icon_n >= VIEW_ICON_CACHE) return NULL;
  GBitmap *bmp = gbitmap_create_with_resource(res);
  v->icons[v->icon_n].res = res;
  v->icons[v->icon_n].bmp = bmp;
  v->icon_n++;
  return bmp;
}

// --- per-block geometry ------------------------------------------------------
static int16_t section_h(void)        { return ui_section_height(false); }  // view spaces sections itself
static int16_t field_h(const Block *b) {
  if (b->field.stacked)
    return 4 + ui_font_cap(UI_FONT_TITLE) - 2 + ui_font_cap(UI_FONT_BODY_LARGE) + 4;
  return 3 + ui_font_cap(UI_FONT_TITLE) + 3;
}
static int16_t block_h(View *v, const Block *b) {
  switch (b->kind) {
    case BLOCK_SECTION: return section_h();
    case BLOCK_ITEM:    return list_item_height(&b->item, v->size);
    case BLOCK_FIELD:   return field_h(b);
    case BLOCK_GAP:     return b->gap;
    case BLOCK_CUSTOM:  return b->custom.h;
  }
  return 0;
}

// --- drawing -----------------------------------------------------------------
static void draw_section(GContext *ctx, const char *title, GRect r) {
  ui_section_draw(ctx, r, title, false);
}
static void draw_field(GContext *ctx, const Block *b, GRect r) {
  int x = r.origin.x + LPAD, w = r.size.w - 2 * LPAD;
  if (b->field.stacked) {
    int lc = ui_font_cap(UI_FONT_TITLE);
    graphics_context_set_text_color(ctx, ui_text());
    ui_text_draw(ctx, b->field.label, UI_FONT_TITLE,
                 GRect(x, r.origin.y + 4, w, lc),
                 GTextAlignmentLeft, false, GTextOverflowModeTrailingEllipsis);
    graphics_context_set_text_color(ctx, ui_text_muted());
    ui_text_draw(ctx, b->field.value, UI_FONT_BODY_LARGE,
                 GRect(x, r.origin.y + 4 + lc - 2, w, ui_font_cap(UI_FONT_BODY_LARGE)),
                 GTextAlignmentLeft, false, GTextOverflowModeTrailingEllipsis);
  } else {                                            // inline: label left, value right
    graphics_context_set_text_color(ctx, ui_text());
    ui_text_draw(ctx, b->field.label, UI_FONT_BODY_LARGE, GRect(x, r.origin.y, w, r.size.h),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
    graphics_context_set_text_color(ctx, ui_text());
    ui_text_draw(ctx, b->field.value, UI_FONT_TITLE, GRect(x, r.origin.y, w, r.size.h),
                 GTextAlignmentRight, true, GTextOverflowModeTrailingEllipsis);
  }
}
static void view_update(Layer *layer, GContext *ctx) {
  View *v = *(View **)layer_get_data(layer);
  int w = layer_get_bounds(layer).size.w;
  int y = 0;
  for (int i = 0; i < v->n; i++) {
    Block *b = &v->blocks[i];
    int16_t h = block_h(v, b);
    GRect r = GRect(0, y, w, h);
    switch (b->kind) {
      case BLOCK_SECTION: draw_section(ctx, b->section, r); break;
      case BLOCK_ITEM: {
        RowState st = { .highlighted = false, .interactive = false, .disabled = b->item.disabled };
        list_item_draw(ctx, r, &b->item, st, v->size, view_icon, v);
        break;
      }
      case BLOCK_FIELD:  draw_field(ctx, b, r); break;
      case BLOCK_CUSTOM: if (b->custom.draw) b->custom.draw(ctx, r, b->custom.data); break;
      case BLOCK_GAP:    break;                          // empty space
    }
    y += h;
  }
}

// --- scrollbar ---------------------------------------------------------------
static void bar_update(Layer *layer, GContext *ctx) {
  View *v = *(View **)layer_get_data(layer);
  if (v->hide_scrollbar || v->content_h <= v->viewport_h) return;   // nothing to scroll
  int vp = v->viewport_h, ch = v->content_h, maxo = ch - vp;
  int off = -scroll_layer_get_content_offset(v->scroll).y;          // 0..maxo
  if (off < 0) off = 0; else if (off > maxo) off = maxo;
  int th = vp * vp / ch; if (th < BAR_MIN) th = BAR_MIN;
  int ty = maxo > 0 ? (off * (vp - th)) / maxo : 0;
  graphics_context_set_fill_color(ctx, ui_text_muted());
  graphics_fill_rect(ctx, GRect(v->width - BAR_W, ty, BAR_W, th), 1, GCornersAll);
}

// --- layout / rebuild --------------------------------------------------------
static void relayout(View *v) {
  int total = 0;
  for (int i = 0; i < v->n; i++) total += block_h(v, &v->blocks[i]);
  v->content_h = total;
  int ch = total < v->viewport_h ? v->viewport_h : total;
  if (v->content) layer_set_frame(v->content, GRect(0, 0, v->width, ch));
  if (v->scroll) {
    scroll_layer_set_content_size(v->scroll, GSize(v->width, total));
    int maxo = total - v->viewport_h; if (maxo < 0) maxo = 0;
    if (-scroll_layer_get_content_offset(v->scroll).y > maxo)
      scroll_layer_set_content_offset(v->scroll, GPoint(0, -maxo), false);
  }
}
static void rebuild(View *v) {
  if (!v->build) return;
  v->n = v->build(v->blocks, VIEW_BUILD_CAP, v->viewport_h);
}

static bool armable(View *v) {
  return v->footer && v->on_action && (v->can_arm ? v->can_arm() : true);
}
static void refresh_footer(View *v) {
  if (!v->footer || !v->ftr) return;
  FooterModel m = (FooterModel){0};
  if (v->get_footer) v->get_footer(&m);
  m.action_enabled = armable(v);
  m.action_focused = v->armed && m.action_enabled;
  footer_set_model(v->ftr, m);
}

void view_reload(View *v) {
  if (!v) return;
  rebuild(v);
  relayout(v);
  if (v->armed && !armable(v)) v->armed = false;
  refresh_footer(v);
  if (v->content) layer_mark_dirty(v->content);
  if (v->bar)     layer_mark_dirty(v->bar);
}

// --- input -------------------------------------------------------------------
static int scroll_step(View *v) {
  switch (v->speed) {
    case SCROLL_SLOW: return 26;
    case SCROLL_FAST: return v->viewport_h > 24 ? v->viewport_h - 16 : v->viewport_h;
    default:          return 50;                       // SCROLL_MED
  }
}
static void scroll_by(View *v, int dy) {               // dy<0 scrolls down (toward content end)
  int maxo = v->content_h - v->viewport_h; if (maxo < 0) maxo = 0;
  int noff = scroll_layer_get_content_offset(v->scroll).y + dy;
  if (noff > 0) noff = 0; else if (noff < -maxo) noff = -maxo;
  scroll_layer_set_content_offset(v->scroll, GPoint(0, noff), false);
  layer_mark_dirty(v->bar);
}
static void up_click(ClickRecognizerRef ref, void *context) {
  View *v = context;
  if (v->armed) {                                                  // leave the action
    v->armed = false; refresh_footer(v); layer_mark_dirty(v->content);
    return;
  }
  scroll_by(v, scroll_step(v));
}
static void down_click(ClickRecognizerRef ref, void *context) {
  View *v = context;
  if (v->armed) return;
  int maxo = v->content_h - v->viewport_h; if (maxo < 0) maxo = 0;
  if (-scroll_layer_get_content_offset(v->scroll).y >= maxo) {     // already at the bottom
    if (armable(v)) {                                              // arm the footer action
      v->armed = true; refresh_footer(v); layer_mark_dirty(v->content);
    }
    return;
  }
  scroll_by(v, -scroll_step(v));
}
static void select_click(ClickRecognizerRef ref, void *context) {
  View *v = context;
  if (v->armed && v->on_action) { v->on_action(); return; }
  if (v->on_select) v->on_select();
}
static void back_click(ClickRecognizerRef ref, void *context) {
  View *v = context;
  if (v->on_back) { v->on_back(); return; }                        // app-level Back action
  window_stack_pop(true);
}
static void view_ccp(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// --- window / scroll lifecycle ----------------------------------------------
static void view_load(Window *w) {
  View *v = window_get_user_data(w);
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  v->width = b.size.w;
  v->viewport_h = b.size.h - (v->footer ? FOOTER_H : 0);

  rebuild(v);                                          // initial blocks (dynamic views)

  v->scroll = scroll_layer_create(GRect(0, 0, b.size.w, v->viewport_h));
  v->content = layer_create_with_data(GRect(0, 0, b.size.w, v->viewport_h), sizeof(View *));
  *(View **)layer_get_data(v->content) = v;
  layer_set_update_proc(v->content, view_update);
  scroll_layer_add_child(v->scroll, v->content);
  layer_add_child(root, scroll_layer_get_layer(v->scroll));

  if (!v->hide_scrollbar) {                            // fixed overlay, over the scroll layer
    v->bar = layer_create_with_data(GRect(0, 0, b.size.w, v->viewport_h), sizeof(View *));
    *(View **)layer_get_data(v->bar) = v;
    layer_set_update_proc(v->bar, bar_update);
    layer_add_child(root, v->bar);
  }
  if (v->footer) {
    v->ftr = footer_create(root);
    refresh_footer(v);
  }
  relayout(v);

  window_set_click_config_provider_with_context(w, view_ccp, v);
}
static void view_appear(Window *w) {
  View *v = window_get_user_data(w);
  if (v->build) view_reload(v);                        // refresh on return (e.g. from a turn)
}
static void view_unload(Window *w) {
  View *v = window_get_user_data(w);
  for (int i = 0; i < v->icon_n; i++) if (v->icons[i].bmp) gbitmap_destroy(v->icons[i].bmp);
  if (v->ftr) footer_destroy(v->ftr);
  if (v->bar) layer_destroy(v->bar);
  layer_destroy(v->content);
  scroll_layer_destroy(v->scroll);
  window_destroy(v->window);
  free(v->blocks);
  free(v);
}

View *view_push(const Block *blocks, int n, ViewOpts opts) {
  View *v = malloc(sizeof(View));
  if (!v) return NULL;
  *v = (View){0};
  v->size       = opts.size;
  v->speed      = opts.speed;
  v->hide_scrollbar = opts.hide_scrollbar;
  v->on_select  = opts.on_select;
  v->on_back    = opts.on_back;
  v->build      = opts.build;
  v->footer     = opts.footer;
  v->get_footer = opts.get_footer;
  v->can_arm    = opts.can_arm;
  v->on_action  = opts.on_action;

  if (v->build) {
    v->blocks = malloc(sizeof(Block) * VIEW_BUILD_CAP);
    v->n = 0;                                          // filled by rebuild() in load
  } else {
    v->blocks = malloc(sizeof(Block) * (n > 0 ? n : 1));
    if (v->blocks && n > 0) memcpy(v->blocks, blocks, sizeof(Block) * n);
    v->n = n;
  }
  if (!v->blocks) { free(v); return NULL; }

  v->window = window_create();
  window_set_background_color(v->window, ui_background());
  window_set_user_data(v->window, v);
  window_set_window_handlers(v->window, (WindowHandlers){
    .load = view_load, .appear = view_appear, .unload = view_unload,
  });
  window_stack_push(v->window, true);
  return v;
}
Window *view_window(View *v) { return v ? v->window : NULL; }
bool view_action_focused(View *v) { return v && v->armed; }
