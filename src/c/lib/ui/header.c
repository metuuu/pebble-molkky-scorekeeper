#include "header.h"
#include "ui_theme.h"
#include "ui_text.h"
#include "ui_tick.h"

// DEBUG: the left icon. Off for now — the only icons we have are 24px PNGs and
// the bar is 24px tall, so they're cramped, and Pebble can't runtime-scale a
// raster bitmap (graphics_draw_bitmap_in_rect clips, it doesn't resample). To
// show it again, generate a smaller PNG variant (icons.json `size`) and flip
// this to 1; the title shifts back to the left edge when it's 0.
#define HEADER_SHOW_ICON 0

#define LPAD     6
#define RPAD     6
#define ICON_GAP 5      // gap between the left icon and the title
#define DIV_GAP  6      // breathing room between the title and the clock divider
#define TEXT_DY  1      // nudge text down: the font reads optically high in this short bar

struct Header {
  Layer   *layer;
  char     title[24];
  GBitmap *icon;         // left icon (owned), or NULL
  Header  *next;         // intrusive list of live headers, for the minute tick
};

// App-level defaults, shared by every container that draws a header. Off / no
// icon until the app opts in (e.g. from a persisted user setting + branding).
static bool     s_enabled;
static uint32_t s_icon_res;

// Live headers, newest first. Menus stack, so several headers can be alive at
// once; the (context-less) minute tick walks the list and marks each dirty.
// Hidden headers sit under the top window and redraw cheaply.
static Header *s_headers;

void     header_set_enabled(bool e)   { s_enabled = e; }
bool     header_enabled(void)         { return s_enabled; }
void     header_set_icon(uint32_t r)  { s_icon_res = r; }
uint32_t header_icon(void)            { return s_icon_res; }

static void header_update(Layer *layer, GContext *ctx) {
  Header *h = *(Header **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);

  // A dark bar carrying the title, with a white "clock pane" cut into the right
  // end. The fill boundary between the two divides title from clock, so no
  // separate divider line is needed.
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Clock geometry: right-aligned in its own white pane.
  char t[16]; clock_copy_time_string(t, sizeof t);
  GSize tz = graphics_text_layout_get_content_size(t, ui_font(UI_FONT_BODY_BOLD),
                 GRect(0, 0, b.size.w, b.size.h), GTextOverflowModeFill, GTextAlignmentLeft);
  int clock_x = b.origin.x + b.size.w - RPAD - tz.w;
  int pane_x  = clock_x - DIV_GAP;                         // left edge of the white pane

  int pane_w = b.origin.x + b.size.w - pane_x;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(pane_x, b.origin.y, pane_w, b.size.h), 0, GCornerNone);
  // A 3px gray frame on all four sides of the white clock pane.
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(pane_x, b.origin.y, pane_w, 3), 0, GCornerNone);                       // top
  graphics_fill_rect(ctx, GRect(pane_x, b.origin.y, 3, b.size.h), 0, GCornerNone);                     // left
  graphics_fill_rect(ctx, GRect(b.origin.x + b.size.w - 3, b.origin.y, 3, b.size.h), 0, GCornerNone);  // right
  graphics_fill_rect(ctx, GRect(pane_x, b.origin.y + b.size.h - 3, pane_w, 3), 0, GCornerNone);        // bottom
  graphics_context_set_text_color(ctx, GColorBlack);       // clock: dark ink on the white pane
  ui_text_draw(ctx, t, UI_FONT_BODY_BOLD,
               GRect(clock_x, b.origin.y + TEXT_DY - 1, tz.w, b.size.h),
               GTextAlignmentLeft, true, GTextOverflowModeFill);

  graphics_context_set_text_color(ctx, GColorWhite);       // title: light ink on the dark bar
  int title_right = pane_x - DIV_GAP;

  // Left: an optional brand icon, then the title fills the rest. The icon keeps
  // its authored colors (drawn with GCompOpSet) and is vertically centered.
  int text_left = b.origin.x + LPAD;
#if HEADER_SHOW_ICON
  if (h->icon) {
    GSize is = gbitmap_get_bounds(h->icon).size;
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, h->icon,
        GRect(text_left, b.origin.y + (b.size.h - is.h) / 2, is.w, is.h));
    text_left += is.w + ICON_GAP;
  }
#endif

  if (h->title[0]) {
    ui_text_draw(ctx, h->title, UI_FONT_BODY_BOLD,
                 GRect(text_left, b.origin.y + TEXT_DY, title_right - text_left, b.size.h),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
  }
}

static void header_minute(void) {
  for (Header *h = s_headers; h; h = h->next) layer_mark_dirty(h->layer);
}

Header *header_create(Layer *parent, const char *title, uint32_t icon_res) {
  Header *h = malloc(sizeof(Header));
  if (!h) return NULL;
  strncpy(h->title, title ? title : "", sizeof(h->title) - 1);
  h->title[sizeof(h->title) - 1] = '\0';
  h->icon = (HEADER_SHOW_ICON && icon_res) ? gbitmap_create_with_resource(icon_res) : NULL;
  GRect pb = layer_get_bounds(parent);
  h->layer = layer_create_with_data(
      GRect(pb.origin.x, pb.origin.y, pb.size.w, HEADER_H), sizeof(Header *));
  *(Header **)layer_get_data(h->layer) = h;
  layer_set_update_proc(h->layer, header_update);
  layer_add_child(parent, h->layer);

  if (!s_headers) ui_tick_register(header_minute);   // shared tick (see ui_tick.h)
  h->next = s_headers;
  s_headers = h;
  return h;
}

void header_set_title(Header *h, const char *title) {
  if (!h) return;
  strncpy(h->title, title ? title : "", sizeof(h->title) - 1);
  h->title[sizeof(h->title) - 1] = '\0';
  layer_mark_dirty(h->layer);
}

void header_destroy(Header *h) {
  if (!h) return;
  // Unlink from the live list; drop the tick once the last one is gone.
  for (Header **pp = &s_headers; *pp; pp = &(*pp)->next) {
    if (*pp == h) { *pp = h->next; break; }
  }
  if (!s_headers) ui_tick_unregister(header_minute);
  if (h->icon) gbitmap_destroy(h->icon);
  layer_destroy(h->layer);
  free(h);
}
