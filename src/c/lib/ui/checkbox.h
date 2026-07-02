#pragma once
#include <pebble.h>

// A checkbox: outlined square, with a checkmark when `checked`. `selected`
// flips the stroke to white for drawing over a highlighted (inverted) row.
void ui_draw_checkbox(GContext *ctx, GRect box, bool checked, bool selected);

// Just the checkmark tick (no box), sized to `box`. `selected` flips the
// stroke to the on-accent ink for drawing over a highlighted row.
void ui_draw_check(GContext *ctx, GRect box, bool selected);
