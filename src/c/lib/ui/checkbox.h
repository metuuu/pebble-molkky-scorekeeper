#pragma once
#include <pebble.h>

// A checkbox: outlined square, with a checkmark when `checked`. `selected`
// flips the stroke to white for drawing over a highlighted (inverted) row.
void ui_draw_checkbox(GContext *ctx, GRect box, bool checked, bool selected);
