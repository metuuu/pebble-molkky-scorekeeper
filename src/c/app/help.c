#include "help.h"
#include <pebble.h>
#include "c/lib/ui/ui_theme.h"

// =============================================================================
// Help — a single scrollable window: the 12-pin Mölkky starting formation drawn
// as numbered circles (matching the standard setup), then the rules as text.
// Button-driven: Up/Down scroll, Back pops. Pure presentation, no model access.
// =============================================================================

static Window      *s_window;
static ScrollLayer *s_scroll;
static Layer       *s_diagram;   // the numbered-pin formation, custom-drawn
static TextLayer   *s_rules;

// Circle radius and the row pitch of the formation.
#define PIN_R   15
#define ROW_DY  27
#define DIAG_H  160   // height reserved for heading + the four pin rows

// The standard Mölkky formation, top to bottom. x is the horizontal offset from
// the diagram's center; rows nest hexagonally (odd rows shifted by one radius).
typedef struct { int dx, value; } Pin;
static const Pin PIN_ROWS[4][4] = {
  { {-32, 7}, {  0, 9}, { 32, 8}, {99, 0} },             // 99 = unused slot
  { {-48, 5}, {-16,11}, { 16,12}, {48, 6} },
  { {-32, 3}, {  0,10}, { 32, 4}, {99, 0} },
  { {-16, 1}, { 16, 2}, { 99, 0}, {99, 0} },
};

static void diagram_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;

  graphics_context_set_text_color(ctx, ui_text());
  graphics_draw_text(ctx, "Pin setup", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(0, 6, b.size.w, 22), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  int top = 52;   // center y of the first pin row (leaves a gap below the heading)
  for (int r = 0; r < 4; r++) {
    int cy = top + r * ROW_DY;
    for (int p = 0; p < 4; p++) {
      const Pin *pin = &PIN_ROWS[r][p];
      if (pin->value == 0) continue;
      GPoint c = GPoint(cx + pin->dx, cy);

      graphics_context_set_fill_color(ctx, ui_background());
      graphics_fill_circle(ctx, c, PIN_R);
      graphics_context_set_stroke_color(ctx, ui_accent());
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, c, PIN_R);

      char n[3]; snprintf(n, sizeof(n), "%d", pin->value);
      graphics_context_set_text_color(ctx, ui_text());
      graphics_draw_text(ctx, n, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                         GRect(c.x - PIN_R, c.y - 13, PIN_R * 2, 22),
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    }
  }
  graphics_context_set_stroke_width(ctx, 1);
}

static const char *RULES_TEXT =
  "RULES\n"
  "Be first to score exactly 50 points.\n\n"
  "THROWING\n"
  "Take turns throwing the mölkky underarm to knock pins down. Stand fallen "
  "pins back up where they land.\n\n"
  "SCORING\n"
  "One pin down scores that pin's number. Two or more down score the number of "
  "pins felled (not their values).\n\n"
  "OVERSHOOT\n"
  "Going over 50 drops you back to 25.\n\n"
  "MISSES\n"
  "With \"Lose from 3 misses\" on, three misses in a row knocks you out.\n\n"
  "WINNING\n"
  "First player to hit exactly 50 wins.";

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll, window);

  s_diagram = layer_create(GRect(0, 0, bounds.size.w, DIAG_H));
  layer_set_update_proc(s_diagram, diagram_update);
  scroll_layer_add_child(s_scroll, s_diagram);

  s_rules = text_layer_create(GRect(4, DIAG_H, bounds.size.w - 8, 2000));
  text_layer_set_text(s_rules, RULES_TEXT);
  text_layer_set_font(s_rules, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_rules, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_rules, GColorClear);
  text_layer_set_text_color(s_rules, ui_text());

  GSize used = text_layer_get_content_size(s_rules);
  text_layer_set_size(s_rules, GSize(bounds.size.w - 8, used.h + 8));
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_rules));

  scroll_layer_set_content_size(s_scroll, GSize(bounds.size.w, DIAG_H + used.h + 16));
  layer_add_child(root, scroll_layer_get_layer(s_scroll));
}

static void window_unload(Window *window) {
  text_layer_destroy(s_rules);
  layer_destroy(s_diagram);
  scroll_layer_destroy(s_scroll);
  window_destroy(s_window);
  s_window = NULL; s_scroll = NULL; s_diagram = NULL; s_rules = NULL;
}

void help_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, ui_background());
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
