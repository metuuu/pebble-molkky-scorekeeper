#include "footer.h"
#include "ui_theme.h"
#include "ui_text.h"
#include "ui_tick.h"
#include "button.h"

#define LPAD      6
#define RPAD      6
#define PILL_PAD  2      // horizontal inset of the focus pill around the icon
#define DIV_GAP   6      // breathing room on each side of the clock/label divider
#define TEXT_DY   2      // nudge text down: the font's optical correction reads high in this short bar

struct Footer {
  Layer      *layer;
  FooterModel model;
  uint32_t    icon_res;  // resource currently loaded into `icon`
  GBitmap    *icon;
};

// Live footer used by the minute tick to refresh the clock.
static Footer *s_tick_footer;

// Load-or-fetch the action icon, reloading if the model's resource changed.
static GBitmap *footer_icon(Footer *f) {
  if (!f->model.action_icon) return NULL;
  if (f->icon && f->icon_res == f->model.action_icon) return f->icon;
  if (f->icon) gbitmap_destroy(f->icon);
  f->icon = gbitmap_create_with_resource(f->model.action_icon);
  f->icon_res = f->model.action_icon;
  return f->icon;
}

static void footer_update(Layer *layer, GContext *ctx) {
  Footer *f = *(Footer **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);

  // A hairline divider separates the footer from the scrolling content above.
  graphics_context_set_stroke_color(ctx, ui_text_muted());
  graphics_draw_line(ctx, GPoint(b.origin.x, b.origin.y),
                     GPoint(b.origin.x + b.size.w - 1, b.origin.y));

  // Right action: hidden when disabled, ghost at rest, accent pill when focused.
  GBitmap *icon = (f->model.action_icon && f->model.action_enabled) ? footer_icon(f) : NULL;
  int right = b.origin.x + b.size.w - RPAD;          // right edge available to text
  if (icon) {
    bool focused = f->model.action_focused;
    GSize is = gbitmap_get_bounds(icon).size;
    int ix = b.origin.x + b.size.w - RPAD - is.w;
    GRect pill = GRect(ix - PILL_PAD, b.origin.y + 2, is.w + 2 * PILL_PAD, b.size.h - 3);
    ui_button_draw(ctx, pill, &(UiButtonSpec){
      .style    = focused ? UI_BTN_SOLID   : UI_BTN_GHOST,
      .scheme   = focused ? UI_BTN_PRIMARY : UI_BTN_NEUTRAL,
      .icon = icon, .radius = 4,
    });
    right = pill.origin.x - RPAD;
  }

  // Left clock, optional divider, then centered label.
  int text_left = b.origin.x + LPAD;
  graphics_context_set_text_color(ctx, ui_text());
  if (f->model.show_clock) {
    char t[16]; clock_copy_time_string(t, sizeof t);
    GSize tz = graphics_text_layout_get_content_size(t, ui_font(UI_FONT_BODY_BOLD),
                   GRect(0, 0, b.size.w, b.size.h), GTextOverflowModeFill, GTextAlignmentLeft);
    ui_text_draw(ctx, t, UI_FONT_BODY_BOLD,
                 GRect(text_left, b.origin.y + TEXT_DY, tz.w, b.size.h),
                 GTextAlignmentLeft, true, GTextOverflowModeFill);
    if (f->model.left[0]) {
      int div_x = text_left + tz.w + DIV_GAP;
      graphics_context_set_stroke_color(ctx, ui_text());   // black divider
      graphics_draw_line(ctx, GPoint(div_x, b.origin.y + 5),
                         GPoint(div_x, b.origin.y + b.size.h - 4));
      text_left = div_x + DIV_GAP;
    }
  }
  if (f->model.left[0]) {
    ui_text_draw(ctx, f->model.left, UI_FONT_BODY_BOLD,
                 GRect(text_left, b.origin.y + TEXT_DY, right - text_left, b.size.h),
                 GTextAlignmentCenter, true, GTextOverflowModeTrailingEllipsis);
  }
}

static void footer_minute(void) {
  if (s_tick_footer) layer_mark_dirty(s_tick_footer->layer);
}

Footer *footer_create(Layer *parent) {
  Footer *f = malloc(sizeof(Footer));
  if (!f) return NULL;
  f->model = (FooterModel){0};
  f->icon_res = 0;
  f->icon = NULL;
  GRect pb = layer_get_bounds(parent);
  f->layer = layer_create_with_data(
      GRect(pb.origin.x, pb.origin.y + pb.size.h - FOOTER_H, pb.size.w, FOOTER_H),
      sizeof(Footer *));
  *(Footer **)layer_get_data(f->layer) = f;
  layer_set_update_proc(f->layer, footer_update);
  layer_add_child(parent, f->layer);
  s_tick_footer = f;
  ui_tick_register(footer_minute);   // shared tick — never steals the headers' (see ui_tick.h)
  return f;
}

void footer_set_model(Footer *f, FooterModel model) {
  if (!f) return;
  f->model = model;
  layer_mark_dirty(f->layer);
}

void footer_destroy(Footer *f) {
  if (!f) return;
  if (s_tick_footer == f) { ui_tick_unregister(footer_minute); s_tick_footer = NULL; }
  if (f->icon) gbitmap_destroy(f->icon);
  layer_destroy(f->layer);
  free(f);
}
