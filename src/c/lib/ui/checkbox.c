#include "checkbox.h"
#include "ui_theme.h"

void ui_draw_check(GContext *ctx, GRect box, bool selected) {
  // Use on-accent ink when drawn over a highlighted row.
  int x = box.origin.x, y = box.origin.y, w = box.size.w, h = box.size.h;
  graphics_context_set_stroke_color(ctx, selected ? ui_accent_text() : ui_accent());
  graphics_context_set_stroke_width(ctx, 2);   // checkmark: down-stroke then up-stroke
  graphics_draw_line(ctx, GPoint(x + w/4, y + h/2),   GPoint(x + w*2/5, y + h*3/4));
  graphics_draw_line(ctx, GPoint(x + w*2/5, y + h*3/4), GPoint(x + w*3/4, y + h/4));
  graphics_context_set_stroke_width(ctx, 1);
}

void ui_draw_checkbox(GContext *ctx, GRect box, bool checked, bool selected) {
  // Match highlighted rows; otherwise use normal text outline and accent tick.
  GColor outline = selected ? ui_accent_text() : ui_text();
  graphics_context_set_stroke_color(ctx, outline);
  graphics_draw_round_rect(ctx, box, 3);
  if (checked) ui_draw_check(ctx, box, selected);
}
