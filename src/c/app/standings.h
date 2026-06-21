#pragma once
#include <pebble.h>
#include "c/lib/ui/list_item.h"

// =============================================================================
// standings — game-specific presentation helpers for the placement and history
// screens. These know about Mölkky concepts (place, score, out, won), so they
// live with the app rather than in the reusable UI library. Medal colors are
// semantic and stay here, never in the theme.
// =============================================================================

// A crown badge with `number` inside it, centered in `box`. Drawn directly by
// the live board and placement screens. (`selected` is accepted for call-site
// symmetry; the medal color is decided by `number`.)
void ui_draw_crown(GContext *ctx, GRect box, int number, bool selected);

// list_item ACC_CUSTOM leading accessory: a podium crown for places 1-3.
// `data` is (void *)(intptr_t)place.
void standings_crown_acc(GContext *ctx, GRect box, RowColors colors, void *data);

// Fill `item` as one standings row: a crown badge for the podium (places 1-3),
// otherwise the place number; the name as the title; the score (or "out") on
// the right. An eliminated, unplaced player (out && place 0) renders muted.
void standings_fill_row(ListItem *item, const char *name, int place, int score, bool out);
