#include "pins_art.h"
#include <pebble.h>
#include "c/lib/ui/ui_theme.h"

// =============================================================================
// Pins art — the classic 12-pin Mölkky formation drawn as fake-3D pins, seen
// head-on: each pin is a green outline (no fill) cylinder with a tall top oval
// and its number inside that oval, like a real mölkky pin branded on top. The
// rows stack back-to-front so nearer pins overlap and occlude the ones behind. A
// preview of the store-icon artwork; the bottom-right throwing log is left out on
// purpose (just the numbered pin bundle). Pure presentation, no model access.
//
// "No fill" is honoured by painting each pin's silhouette in the background
// colour before stroking it — the interior reads as empty, and the fill is what
// lets a front pin cleanly cover the back of the one behind it.
// =============================================================================

// --- pin geometry (tuned for the emery content area) ------------------------
#define PIN_R    17   // half width of a pin
#define TOP_RX   18   // top oval half width — 1px wider than the body on each side, a lip
#define TOP_RY   20   // half height of the (tall) top oval — the number lives here
#define BOT_RY    9   // half height of the bottom rim oval (rounder = taller)
#define PIN_BODY 48   // body height: top-oval centre → bottom-oval centre
#define PIN_GAP   6   // horizontal gap between neighbouring pins in a row
#define ROW_DY   30   // how much lower each row sits than the one behind it
#define STROKE    2

#define PITCH    (2 * PIN_R + PIN_GAP)   // centre-to-centre spacing in a row
#define HPITCH   (PITCH / 2)             // hex rows nest by half a pitch

// Full vertical reach of the bundle, from the back row's top oval to the front
// row's bottom rim — used to centre the whole thing in the window.
#define BUNDLE_H (3 * ROW_DY + PIN_BODY + TOP_RY + BOT_RY)

// "MÖLKKY" title band across the top; the bundle centres in the space below it.
#define TITLE_TOP     8
#define TITLE_H      36
#define TITLE_BOTTOM (TITLE_TOP + TITLE_H)

// Hand-drawn umlaut over the O (the font's own Ö glyph renders wrong). Two dots,
// each UMLAUT_R radius, UMLAUT_DX either side of the O's centre, at UMLAUT_DY
// from the top — nudge these a pixel if the dots sit high/low on device.
#define UMLAUT_R   3
#define UMLAUT_DX  6
#define UMLAUT_DY  4

// The standard formation, back row first. `off` is the horizontal offset from
// centre in half-pitch units; rows nest hexagonally (odd-width rows shifted).
typedef struct { int n; int v[4]; int off[4]; } PinRow;
static const PinRow ROWS[4] = {
  { 3, { 7,  9,  8    }, { -2,  0,  2     } },
  { 4, { 5, 11, 12, 6 }, { -3, -1,  1,  3 } },
  { 3, { 3, 10,  4    }, { -2,  0,  2     } },
  { 2, { 1,  2        }, { -1,  1         } },
};

// PebbleOS has no ellipse primitive — graphics_draw_arc / graphics_fill_radial
// inscribe a *circle* (the smaller dimension), which would collapse our ovals to
// dots. So ellipses are built by hand: filled by horizontal scanlines, outlined
// by stepping points with the integer trig tables (no floats).

static int isqrt(int v) {
  if (v <= 0) return 0;
  int x = v, y = (x + 1) / 2;
  while (y < x) { x = y; y = (x + v / x) / 2; }
  return x;
}

// Solid ellipse centred at (cx, cy), x-radius rx, y-radius ry.
static void fill_ellipse(GContext *ctx, int cx, int cy, int rx, int ry, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  for (int dy = -ry; dy <= ry; dy++) {
    int w = rx * isqrt(ry * ry - dy * dy) / ry;
    graphics_draw_line(ctx, GPoint(cx - w, cy + dy), GPoint(cx + w, cy + dy));
  }
}

// Ellipse outline arc from a0 to a1 (Pebble angle units; 0 = right, a quarter-
// turn = straight down on screen), drawn as short line segments.
static void stroke_ellipse(GContext *ctx, int cx, int cy, int rx, int ry,
                           int32_t a0, int32_t a1) {
  int32_t step = TRIG_MAX_ANGLE / 48;
  GPoint prev = GPoint(cx + rx * cos_lookup(a0) / TRIG_MAX_RATIO,
                       cy + ry * sin_lookup(a0) / TRIG_MAX_RATIO);
  for (int32_t a = a0 + step; a < a1; a += step) {
    GPoint p = GPoint(cx + rx * cos_lookup(a) / TRIG_MAX_RATIO,
                      cy + ry * sin_lookup(a) / TRIG_MAX_RATIO);
    graphics_draw_line(ctx, prev, p);
    prev = p;
  }
  graphics_draw_line(ctx, prev, GPoint(cx + rx * cos_lookup(a1) / TRIG_MAX_RATIO,
                                       cy + ry * sin_lookup(a1) / TRIG_MAX_RATIO));
}

// Draw one fake-3D pin, head-on: the tall top oval (with the number inside it),
// two straight sides and the front bottom rim — outlined in `ink` over a `bg`
// silhouette so it covers whatever is behind it. `cyT` is the top-oval centre.
static void draw_pin(GContext *ctx, int cx, int cyT, int value, GColor ink, GColor bg) {
  int baseY = cyT + PIN_BODY;

  // Silhouette in the background colour: body rect + the two end ovals (the top
  // oval is a touch wider than the body, giving a 1px lip on each side).
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(cx - PIN_R, cyT, 2 * PIN_R, PIN_BODY), 0, GCornerNone);
  fill_ellipse(ctx, cx, cyT, TOP_RX, TOP_RY, bg);
  fill_ellipse(ctx, cx, baseY, PIN_R, BOT_RY, bg);

  // Outline: full top oval, the two sides, and the front (lower) half of the
  // bottom rim — angles 0 (right) through a half-turn (left) bulge downward.
  graphics_context_set_stroke_color(ctx, ink);
  graphics_context_set_stroke_width(ctx, STROKE);
  stroke_ellipse(ctx, cx, cyT, TOP_RX, TOP_RY, 0, TRIG_MAX_ANGLE);
  graphics_draw_line(ctx, GPoint(cx - PIN_R, cyT), GPoint(cx - PIN_R, baseY));
  graphics_draw_line(ctx, GPoint(cx + PIN_R, cyT), GPoint(cx + PIN_R, baseY));
  stroke_ellipse(ctx, cx, baseY, PIN_R, BOT_RY, 0, TRIG_MAX_ANGLE / 2);
  graphics_context_set_stroke_width(ctx, 1);

  // Number inside the top oval. Sitting high on the pin, it also stays clear of
  // the row in front, so every number reads in the bundle.
  char s[3];
  snprintf(s, sizeof s, "%d", value);
  graphics_context_set_text_color(ctx, ink);
  graphics_draw_text(ctx, s, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(cx - PIN_R, cyT - 17, 2 * PIN_R, 29),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static int text_w(const char *s, GFont f) {
  return graphics_text_layout_get_content_size(
      s, f, GRect(0, 0, 200, 40), GTextOverflowModeFill, GTextAlignmentLeft).w;
}

static void art_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int mid = b.origin.x + b.size.w / 2;

  // Title, big and bold across the top. The font's own "Ö" glyph is broken, so
  // we draw "MOLKKY" and place the umlaut over the O by hand.
  GFont tf = fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);
  int tw = text_w("MOLKKY", tf);
  int tx = mid - tw / 2;
  graphics_context_set_text_color(ctx, ui_accent());
  graphics_draw_text(ctx, "MOLKKY", tf,
                     GRect(tx, b.origin.y + TITLE_TOP, b.size.w, TITLE_H),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  int o_cx = tx + text_w("M", tf) + text_w("O", tf) / 2;
  int dot_y = b.origin.y + TITLE_TOP + UMLAUT_DY;
  graphics_context_set_fill_color(ctx, ui_accent());
  graphics_fill_circle(ctx, GPoint(o_cx - UMLAUT_DX, dot_y), UMLAUT_R);
  graphics_fill_circle(ctx, GPoint(o_cx + UMLAUT_DX, dot_y), UMLAUT_R);

  // Back row's top-oval centre, so the bundle centres in the space below the title.
  int area_h = b.size.h - TITLE_BOTTOM;
  int cyT0 = b.origin.y + TITLE_BOTTOM + (area_h - BUNDLE_H) / 2 + TOP_RY;

  // Back to front: a later (nearer) row is drawn over the one behind it.
  for (int r = 0; r < 4; r++) {
    int cyT = cyT0 + r * ROW_DY;
    for (int p = 0; p < ROWS[r].n; p++) {
      int cx = mid + ROWS[r].off[p] * HPITCH;
      draw_pin(ctx, cx, cyT, ROWS[r].v[p], ui_accent(), ui_background());
    }
  }
}

static Window *s_window;
static Layer  *s_layer;

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, art_update);
  layer_add_child(root, s_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_layer);
  window_destroy(window);
  s_window = NULL;
}

void pins_art_push(void) {
  if (s_window) return;
  s_window = window_create();
  window_set_background_color(s_window, ui_background());
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
