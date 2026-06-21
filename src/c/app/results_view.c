#include "results_view.h"
#include "molkky.h"
#include "standings.h"
#include "c/lib/ui/view.h"

// One results screen exists at a time, and it's built from a deep call stack
// (throw -> end -> push), so the block array is static rather than on the stack
// (at 16 players an on-stack array faults the app — see the old game.c note).
#define RV_MAX_STATS 5
static Block s_blocks[1 + MK_MAX_PLAYERS + 2 + RV_MAX_STATS];

// "12 min", "1 h 5 min", "2 h"; sub-minute games read "< 1 min".
static void fmt_duration(uint16_t mins, char *buf, size_t n) {
  if (mins == 0)        snprintf(buf, n, "< 1 min");
  else if (mins < 60)   snprintf(buf, n, "%d min", mins);
  else if (mins % 60)   snprintf(buf, n, "%d h %d min", mins / 60, mins % 60);
  else                  snprintf(buf, n, "%d h", mins / 60);
}

// The rules a game was played under, e.g. "Lose from 3 · Final round".
static void fmt_rules(uint8_t settings, char *buf, size_t n) {
  const char *lose  = (settings & MK_SET_LOSE3) ? "Lose from 3" : NULL;
  const char *final = (settings & MK_SET_FINAL) ? "Final round" : NULL;
  if (lose && final) snprintf(buf, n, "%s · %s", lose, final);
  else if (lose)     snprintf(buf, n, "%s", lose);
  else if (final)    snprintf(buf, n, "%s", final);
  else               snprintf(buf, n, "Standard");
}

void results_view_push(const char *title, const ResultRow *rows, int count,
                       uint16_t duration, uint8_t settings, void (*on_select)(void)) {
  int n = 0;
  s_blocks[n++] = block_section(title);
  for (int i = 0; i < count && i < MK_MAX_PLAYERS; i++) {
    ListItem item = list_item_empty();
    standings_fill_row(&item, rows[i].name, rows[i].place, rows[i].score, rows[i].out);
    s_blocks[n++] = block_item(item);
  }

  s_blocks[n++] = block_gap(GAP_MD);
  s_blocks[n++] = block_section("Stats");
  char val[32];

  // Most misses — only a single clear leader who actually missed.
  int mm = 0;
  for (int i = 0; i < count; i++) if (rows[i].misses > mm) mm = rows[i].misses;
  int mm_n = 0, mm_i = -1;
  for (int i = 0; i < count; i++) if (rows[i].misses == mm) { mm_n++; mm_i = i; }
  if (mm > 0 && mm_n == 1) {
    snprintf(val, sizeof val, "%s (%d)", rows[mm_i].name, mm);
    s_blocks[n++] = block_field("Most misses", val);
  }

  // Most accurate — only if exactly one player missed the least of everyone.
  if (count > 1) {
    int lo = rows[0].misses;
    for (int i = 1; i < count; i++) if (rows[i].misses < lo) lo = rows[i].misses;
    int lo_n = 0, lo_i = -1;
    for (int i = 0; i < count; i++) if (rows[i].misses == lo) { lo_n++; lo_i = i; }
    if (lo_n == 1) {
      snprintf(val, sizeof val, "%s (%d miss%s)", rows[lo_i].name, lo, lo == 1 ? "" : "es");
      s_blocks[n++] = block_field("Most accurate", val);
    }
  }

  // Average points per turn across the whole game (a miss is a 0-point turn).
  int pts = 0, turns = 0;
  for (int i = 0; i < count; i++) { pts += rows[i].points; turns += rows[i].throws; }
  if (turns > 0) {
    int tenths = (pts * 10 + turns / 2) / turns;     // one decimal, rounded
    snprintf(val, sizeof val, "%d.%d pts", tenths / 10, tenths % 10);
    s_blocks[n++] = block_field("Avg per turn", val);
  }

  fmt_duration(duration, val, sizeof val);
  s_blocks[n++] = block_field("Duration", val);
  fmt_rules(settings, val, sizeof val);
  s_blocks[n++] = block_field("Rules", val);

  view_push(s_blocks, n, (ViewOpts){ .size = UI_SIZE_SM, .on_select = on_select });
}
