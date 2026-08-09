#include "results_view.h"
#include "molkky.h"
#include "strings.h"
#include "standings.h"
#include "c/lib/ui/view.h"
#include "c/lib/ui/ui_theme.h"

#define RV_MAX_STATS 5
#define RV_MAX_BLOCKS (2 + MK_MAX_PLAYERS + 2 + RV_MAX_STATS)

// The game's date sits above the first section as a plain title; the buffer is
// static because the block only holds a pointer to it.
static char s_date[24];
static void draw_date(GContext *ctx, GRect r, void *data) {
  graphics_context_set_text_color(ctx, ui_text());
  ui_text_draw(ctx, s_date, UI_FONT_BODY_BOLD, GRect(r.origin.x + 6, r.origin.y, r.size.w - 12, r.size.h),
               GTextAlignmentLeft, true, GTextOverflowModeFill);
}

// "12 min", "1 h 5 min", "2 h", "3 d 4 h"; sub-minute games read "< 1 min".
// Takes the two unix stamps rather than a precomputed count: the subtraction
// stays at the width the stamps are stored in, so no span can wrap into a
// wrong-but-plausible number. Both stamps come from the device clock, so the
// pathological cases — a record with no start, a clock that moved backwards —
// read "< 1 min". Past a day the minutes are dropped: a game only spans days by
// being left open across sessions, where the odd minute means nothing.
static void fmt_duration(int32_t start, int32_t end, char *buf, size_t n) {
  int32_t mins = (start > 0 && end > start) ? (end - start) / 60 : 0;
  if (mins <= 0) {
    snprintf(buf, n, "%s", t(STR_DUR_LT_MIN));
  } else if (mins < 60) {
    tfmt(buf, n, STR_DUR_MIN, (int)mins);
  } else if (mins < 1440) {                          // under a day: "2 h", "1 h 5 min"
    int h = (int)(mins / 60), m = (int)(mins % 60);
    if (m) tfmt(buf, n, STR_DUR_H_MIN, h, m);
    else   tfmt(buf, n, STR_DUR_H, h);
  } else {                                           // a day or more: "3 d", "3 d 4 h"
    int d = (int)(mins / 1440), h = (int)(mins % 1440) / 60;
    if (h) tfmt(buf, n, STR_DUR_D_H, d, h);
    else   tfmt(buf, n, STR_DUR_D, d);
  }
}

View *results_view_push(const char *title, const ResultRow *rows, int count,
                        int32_t start, int32_t end, uint8_t settings, void (*on_select)(void)) {
  (void)settings;                                    // rules row dropped; kept for API compatibility
  // Too big for the ~2 KB stack and view_push copies it anyway, so build the
  // block list in a transient heap buffer (keeps it out of the app image too).
  Block *blocks = malloc(RV_MAX_BLOCKS * sizeof *blocks);
  if (!blocks) return NULL;
  int n = 0;
  // History detail passes a title; post-game results omit this row.
  if (title && title[0]) {
    snprintf(s_date, sizeof s_date, "%s", title);
    blocks[n++] = block_custom(4 + ui_font_cap(UI_FONT_BODY_BOLD) + 4, draw_date, NULL);
  }
  blocks[n++] = block_section(t(STR_RESULTS));
  for (int i = 0; i < count && i < MK_MAX_PLAYERS; i++) {
    ListItem item = list_item_empty();
    standings_fill_row(&item, rows[i].name, rows[i].place, rows[i].score, rows[i].out);
    blocks[n++] = block_item(item);
  }

  blocks[n++] = block_gap(GAP_MD);
  blocks[n++] = block_section(t(STR_STATS));
  char val[48];   // fits a 15-char name + "100% (255 misses)"

  // Accuracy extremes; players with no throws are skipped.
  int hi_i = -1, lo_i = -1, hi_acc = -1, lo_acc = 101;
  for (int i = 0; i < count; i++) {
    if (rows[i].throws == 0) continue;
    int acc = (rows[i].throws - rows[i].misses) * 100 / rows[i].throws;
    if (acc > hi_acc) { hi_acc = acc; hi_i = i; }
    if (acc < lo_acc) { lo_acc = acc; lo_i = i; }
  }
  if (hi_i >= 0) {
    int m = rows[hi_i].misses;
    tfmt(val, sizeof val, m == 1 ? STR_ACC_VALUE_ONE : STR_ACC_VALUE_MANY, rows[hi_i].name, hi_acc, m);
    blocks[n++] = block_field(t(STR_HIGHEST_ACC), val);
  }
  if (lo_i >= 0 && lo_i != hi_i) {
    int m = rows[lo_i].misses;
    tfmt(val, sizeof val, m == 1 ? STR_ACC_VALUE_ONE : STR_ACC_VALUE_MANY, rows[lo_i].name, lo_acc, m);
    blocks[n++] = block_field(t(STR_LOWEST_ACC), val);
  }

  // Average points per turn across the whole game (a miss is a 0-point turn).
  int pts = 0, turns = 0;
  for (int i = 0; i < count; i++) { pts += rows[i].points; turns += rows[i].throws; }
  if (turns > 0) {
    int tenths = (pts * 10 + turns / 2) / turns;     // one decimal, rounded
    tfmt(val, sizeof val, STR_PTS_VALUE, tenths / 10, tenths % 10);
    blocks[n++] = block_field(t(STR_AVG_PER_TURN), val);
  }

  fmt_duration(start, end, val, sizeof val);
  blocks[n++] = block_field(t(STR_DURATION), val);

  View *v = view_push(blocks, n, (ViewOpts){ .size = UI_SIZE_MD, .on_select = on_select });
  free(blocks);
  return v;
}
