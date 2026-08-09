#pragma once
#include <pebble.h>
#include "c/lib/ui/view.h"

// Finished-game results screen shared by post-game and History detail views.

typedef struct {
  const char *name;     // resolved display name (valid for the duration of the call)
  uint8_t  place;
  uint8_t  score;
  bool     out;
  uint8_t  misses;      // total misses this game
  uint8_t  throws;      // turns taken
  uint16_t points;      // raw pins knocked (a miss is 0)
} ResultRow;

// `rows` are already sorted by place. `start`/`end` are the game's unix stamps;
// the view derives the duration itself so no caller has to pick a unit or a
// width for it. Returns the View handle.
View *results_view_push(const char *title, const ResultRow *rows, int n,
                        int32_t start, int32_t end, uint8_t settings, void (*on_select)(void));
