#include "results_view.h"
#include "molkky.h"
#include "strings.h"
#include "standings.h"
#include "c/lib/ui/view.h"
#include "c/lib/ui/ui_theme.h"

// Static because result screens can be pushed from a deep call path.
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
  if (mins == 0)        snprintf(buf, n, "%s", t(STR_DUR_LT_MIN));
  else if (mins < 60)   tfmt(buf, n, STR_DUR_MIN, mins);
  else if (mins % 60)   tfmt(buf, n, STR_DUR_H_MIN, mins / 60, mins % 60);
  else                  tfmt(buf, n, STR_DUR_H, mins / 60);
}

View *results_view_push(const char *title, const ResultRow *rows, int count,
                        uint16_t duration, uint8_t settings, void (*on_select)(void)) {
  (void)settings;                                    // rules row dropped; kept for API compatibility
  int n = 0;
  // History detail passes a title; post-game results omit this row.
  if (title && title[0]) {
    snprintf(s_date, sizeof s_date, "%s", title);
    s_blocks[n++] = block_custom(4 + ui_font_cap(UI_FONT_BODY_BOLD) + 4, draw_date, NULL);
  }
  s_blocks[n++] = block_section(t(STR_RESULTS));
  for (int i = 0; i < count && i < MK_MAX_PLAYERS; i++) {
    ListItem item = list_item_empty();
    standings_fill_row(&item, rows[i].name, rows[i].place, rows[i].score, rows[i].out);
    s_blocks[n++] = block_item(item);
  }

  s_blocks[n++] = block_gap(GAP_MD);
  s_blocks[n++] = block_section(t(STR_STATS));
  char val[32];

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
    s_blocks[n++] = block_field(t(STR_HIGHEST_ACC), val);
  }
  if (lo_i >= 0 && lo_i != hi_i) {
    int m = rows[lo_i].misses;
    tfmt(val, sizeof val, m == 1 ? STR_ACC_VALUE_ONE : STR_ACC_VALUE_MANY, rows[lo_i].name, lo_acc, m);
    s_blocks[n++] = block_field(t(STR_LOWEST_ACC), val);
  }

  // Average points per turn across the whole game (a miss is a 0-point turn).
  int pts = 0, turns = 0;
  for (int i = 0; i < count; i++) { pts += rows[i].points; turns += rows[i].throws; }
  if (turns > 0) {
    int tenths = (pts * 10 + turns / 2) / turns;     // one decimal, rounded
    tfmt(val, sizeof val, STR_PTS_VALUE, tenths / 10, tenths % 10);
    s_blocks[n++] = block_field(t(STR_AVG_PER_TURN), val);
  }

  fmt_duration(duration, val, sizeof val);
  s_blocks[n++] = block_field(t(STR_DURATION), val);

  return view_push(s_blocks, n, (ViewOpts){ .size = UI_SIZE_MD, .on_select = on_select });
}
