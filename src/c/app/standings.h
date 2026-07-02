#pragma once
#include <pebble.h>
#include "c/lib/ui/list_item.h"

// Game-specific standings drawing helpers.

// Crown badge centered in `box`.
void ui_draw_crown(GContext *ctx, GRect box, int number, bool selected);

// list_item ACC_CUSTOM leading accessory: a podium crown for places 1-3.
// `data` is (void *)(intptr_t)place.
void standings_crown_acc(GContext *ctx, GRect box, RowColors colors, void *data);

// Fill one standings row.
void standings_fill_row(ListItem *item, const char *name, int place, int score, bool out);
