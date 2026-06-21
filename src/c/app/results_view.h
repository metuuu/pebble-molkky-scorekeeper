#pragma once
#include <pebble.h>

// =============================================================================
// results_view — a finished-game results screen: a standings section (rows in
// the given order) followed by a Stats section. Shared by the end-of-game result
// screen (game.c) and the History detail view (history.c) so a game looks the
// same whether it just finished or is browsed later.
//
// The caller passes a neutral row array, so the live game (≤16 players) and a
// stored history record (≤MK_MAX_HIST_PLAYERS) feed the same renderer.
// =============================================================================

typedef struct {
  const char *name;     // resolved display name (valid for the duration of the call)
  uint8_t  place;
  uint8_t  score;
  bool     out;
  uint8_t  misses;      // total misses this game
  uint8_t  throws;      // turns taken
  uint16_t points;      // raw pins knocked (a miss is 0)
} ResultRow;

// `rows` are listed in display order (already sorted by place). `duration` is in
// minutes and `settings` carries the MK_SET_* rule bits; both drive Stats rows.
// `on_select` fires on Select (NULL = no action; Back still pops the screen).
void results_view_push(const char *title, const ResultRow *rows, int n,
                       uint16_t duration, uint8_t settings, void (*on_select)(void));
