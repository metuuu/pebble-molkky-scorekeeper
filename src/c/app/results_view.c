#include "results_view.h"
#include "molkky.h"
#include "standings.h"
#include "c/lib/ui/view.h"
#include "c/lib/ui/ui_theme.h"

// One results screen exists at a time, and it's built from a deep call stack
// (throw -> end -> push), so the block array is static rather than on the stack
// (at 16 players an on-stack array faults the app — see the old game.c note).
#define RV_MAX_STATS 5
static Block s_blocks[2 + MK_MAX_PLAYERS + 2 + RV_MAX_STATS];

// The game's date sits above the first section as a plain title; the buffer is
// static because the block only holds a pointer to it.
static char s_date[24];
static void draw_date(GContext *ctx, GRect r, void *data) {
  graphics_context_set_text_color(ctx, ui_text());
  ui_text_draw(ctx, s_date, UI_FONT_BODY_BOLD, GRect(r.origin.x + 6, r.origin.y, r.size.w - 12, r.size.h),
               GTextAlignmentLeft, true, GTextOverflowModeFill);
}

// "12 min", "1 h 5 min", "2 h"; sub-minute games read "< 1 min".
static void fmt_duration(uint16_t mins, char *buf, size_t n) {
  if (mins == 0)        snprintf(buf, n, "< 1 min");
  else if (mins < 60)   snprintf(buf, n, "%d min", mins);
  else if (mins % 60)   snprintf(buf, n, "%d h %d min", mins / 60, mins % 60);
  else                  snprintf(buf, n, "%d h", mins / 60);
}

View *results_view_push(const char *title, const ResultRow *rows, int count,
                        uint16_t duration, uint8_t settings, void (*on_select)(void)) {
  (void)settings;                                    // rules row dropped; kept for API compatibility
  int n = 0;
  snprintf(s_date, sizeof s_date, "%s", title ? title : "");
  s_blocks[n++] = block_custom(4 + ui_font_cap(UI_FONT_BODY_BOLD) + 4, draw_date, NULL);
  s_blocks[n++] = block_section("Results");
  for (int i = 0; i < count && i < MK_MAX_PLAYERS; i++) {
    ListItem item = list_item_empty();
    standings_fill_row(&item, rows[i].name, rows[i].place, rows[i].score, rows[i].out);
    s_blocks[n++] = block_item(item);
  }

  s_blocks[n++] = block_gap(GAP_MD);
  s_blocks[n++] = block_section("Stats");
  char val[32];

  // Accuracy extremes — a player's hits / throws as a percentage. Show the best,
  // and the worst when it's a different player. Players who never threw are
  // skipped (no throws → no meaningful accuracy).
  int hi_i = -1, lo_i = -1, hi_acc = -1, lo_acc = 101;
  for (int i = 0; i < count; i++) {
    if (rows[i].throws == 0) continue;
    int acc = (rows[i].throws - rows[i].misses) * 100 / rows[i].throws;
    if (acc > hi_acc) { hi_acc = acc; hi_i = i; }
    if (acc < lo_acc) { lo_acc = acc; lo_i = i; }
  }
  if (hi_i >= 0) {
    int m = rows[hi_i].misses;
    snprintf(val, sizeof val, "%s %d%% (%d miss%s)", rows[hi_i].name, hi_acc, m, m == 1 ? "" : "es");
    s_blocks[n++] = block_field("Highest accuracy", val);
  }
  if (lo_i >= 0 && lo_i != hi_i) {
    int m = rows[lo_i].misses;
    snprintf(val, sizeof val, "%s %d%% (%d miss%s)", rows[lo_i].name, lo_acc, m, m == 1 ? "" : "es");
    s_blocks[n++] = block_field("Lowest accuracy", val);
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

  return view_push(s_blocks, n, (ViewOpts){ .size = UI_SIZE_MD, .on_select = on_select });
}
