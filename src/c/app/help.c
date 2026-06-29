#include "help.h"
#include <pebble.h>
#include "c/lib/ui/pager.h"
#include "c/lib/ui/ui_theme.h"
#include "c/lib/ui/ui_text.h"

// =============================================================================
// Help — the Mölkky rules as a paged screen (pager): page one is the 12-pin
// starting formation drawn as numbered circles, the rest are rule sections. Down
// scrolls then turns to the next page, Up scrolls then back; the dots footer and
// the corner bubbles show where you are. Pure presentation, no model access.
// =============================================================================

#define LPAD 4   // text inset, matching the old single-window layout

// Rules read more comfortably one size up from the default body font.
#define HELP_BODY UI_FONT_BODY_LARGE

// --- text pages: a bold title over wrapped body text ------------------------
typedef struct { const char *title; const char *body; } TextPage;

static const TextPage TEXT_PAGES[] = {
  { "Objective",
    "Be the first to score exactly 50 points." },
  { "Throwing",
    "Take turns throwing the mölkky underarm to knock pins down. Everyone "
    "throws from the same line, 3,5 m away. Stand fallen pins back up where "
    "they land." },
  { "Scoring",
    "One pin down scores that pin's number. Two or more down score the number "
    "of pins felled (not their values)." },
  { "Overshoot & misses",
    "Going over 50 drops you back to 25.\n\n"
    "With \"Lose from 3 misses\" on, three misses in a row knocks you out." },
  { "Winning",
    "The first player to hit exactly 50 wins. The round is finished so anyone "
    "else who also reaches 50 in the same round shares the win." },
};

static int16_t text_wrap_h(const char *s, UiFont f, int w) {
  return graphics_text_layout_get_content_size(
      s, ui_font(f), GRect(0, 0, w, 2000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
}

static void text_page_draw(GContext *ctx, GRect b, void *data) {
  const TextPage *tp = data;
  int x = b.origin.x + LPAD, w = b.size.w - 2 * LPAD;
  int y = b.origin.y + 8;

  int th = ui_font_cap(UI_FONT_TITLE);
  graphics_context_set_text_color(ctx, ui_text());
  graphics_draw_text(ctx, tp->title, ui_font(UI_FONT_TITLE), GRect(x, y, w, th + 4),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  y += th + 6;

  int bh = text_wrap_h(tp->body, HELP_BODY, w);
  graphics_draw_text(ctx, tp->body, ui_font(HELP_BODY), GRect(x, y, w, bh + 4),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

static int16_t text_page_measure(int width, void *data) {
  const TextPage *tp = data;
  int w = width - 2 * LPAD;
  return 8 + ui_font_cap(UI_FONT_TITLE) + 6 + text_wrap_h(tp->body, HELP_BODY, w) + 10;
}

// --- pin-formation page ------------------------------------------------------
#define PIN_R     15
#define ROW_DY    27
#define PIN_ROWS_N 4
#define CAPTION   "Set the 12 pins like this to start."

// Natural height of the pin block: row centers span (rows-1)*ROW_DY, plus a pin
// radius of margin above the first row and below the last.
#define GRID_H  ((PIN_ROWS_N - 1) * ROW_DY + 2 * PIN_R)

#define TOP_PAD    4   // title inset from the top
#define TITLE_GAP  4   // gap between the title and the pin block (kept tight)
#define BOT_PAD    8   // breathing room under the caption (the pager reserves the
                       // dots band itself, so this is just the gap above it)

// The standard formation, top to bottom. dx is the offset from center; rows nest
// hexagonally (odd rows shifted by one radius). 99 = an unused slot in the row.
typedef struct { int dx, value; } Pin;
static const Pin PIN_ROWS[4][4] = {
  { {-32, 7}, {  0, 9}, { 32, 8}, {99, 0} },
  { {-48, 5}, {-16,11}, { 16,12}, {48, 6} },
  { {-32, 3}, {  0,10}, { 32, 4}, {99, 0} },
  { {-16, 1}, { 16, 2}, { 99, 0}, {99, 0} },
};

static void diagram_page_draw(GContext *ctx, GRect b, void *data) {
  int cx = b.origin.x + b.size.w / 2;

  // Title pinned to the top.
  int title_h = ui_font_cap(UI_FONT_TITLE) + 4;
  graphics_context_set_text_color(ctx, ui_text());
  graphics_draw_text(ctx, "Pin setup", ui_font(UI_FONT_TITLE),
                     GRect(b.origin.x, b.origin.y + TOP_PAD, b.size.w, title_h),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  int title_bottom = b.origin.y + TOP_PAD + title_h;

  int cap_w = b.size.w - 2 * LPAD;
  int cap_h = text_wrap_h(CAPTION, HELP_BODY, cap_w);
  int cap_y = b.origin.y + b.size.h - BOT_PAD - cap_h;

  // The pin block scales to fill the band that opens up between the title and
  // the caption. Its natural size is GRID_H tall and (2*max|dx| + 2*PIN_R) wide;
  // we grow it by the smaller of the width/height ratios so it never overflows,
  // clamp to >=100% (the compact layout, where this is pixel-for-pixel the old
  // fixed drawing) and <=175% so it can't get cartoonish on a huge screen.
  int avail_top = title_bottom + TITLE_GAP;
  int avail_h   = cap_y - avail_top;
  int avail_w   = b.size.w - 2 * LPAD;

  int max_dx = 0;
  for (int r = 0; r < PIN_ROWS_N; r++)
    for (int p = 0; p < 4; p++) {
      const Pin *pin = &PIN_ROWS[r][p];
      if (pin->value == 0) continue;
      int a = pin->dx < 0 ? -pin->dx : pin->dx;
      if (a > max_dx) max_dx = a;
    }
  int nat_w = 2 * (max_dx + PIN_R);

  int scale = avail_w * 100 / nat_w;            // percent, width-limited
  int sv    = avail_h * 100 / GRID_H;           // percent, height-limited
  if (sv < scale) scale = sv;
  if (scale < 100) scale = 100;
  if (scale > 175) scale = 175;

  int R      = PIN_R * scale / 100;
  int dy     = ROW_DY * scale / 100;
  int grid_h = (PIN_ROWS_N - 1) * dy + 2 * R;

  // Center the (possibly width-limited) block in the available vertical band.
  int first_cy = avail_top + (avail_h - grid_h) / 2 + R;

  // Number font + vertical fudge, tiered to the scaled radius. The R<18 tier is
  // the original 15px-radius layout, pixel-for-pixel; larger tiers scale the
  // -13/22 box of that tier roughly in proportion to the font size.
  GFont nf; int n_off, n_h;
  if (R >= 24)      { nf = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD); n_off = -20; n_h = 34; }
  else if (R >= 18) { nf = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD); n_off = -17; n_h = 29; }
  else              { nf = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD); n_off = -13; n_h = 22; }

  int sw = R >= 22 ? 4 : 3;

  for (int r = 0; r < PIN_ROWS_N; r++) {
    int cy = first_cy + r * dy;
    for (int p = 0; p < 4; p++) {
      const Pin *pin = &PIN_ROWS[r][p];
      if (pin->value == 0) continue;
      GPoint c = GPoint(cx + pin->dx * scale / 100, cy);

      graphics_context_set_fill_color(ctx, ui_background());
      graphics_fill_circle(ctx, c, R);
      graphics_context_set_stroke_color(ctx, ui_accent());
      graphics_context_set_stroke_width(ctx, sw);
      graphics_draw_circle(ctx, c, R);

      char n[3]; snprintf(n, sizeof(n), "%d", pin->value);
      graphics_context_set_text_color(ctx, ui_text());
      graphics_draw_text(ctx, n, nf,
                         GRect(c.x - R, c.y + n_off, R * 2, n_h),
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    }
  }
  graphics_context_set_stroke_width(ctx, 1);

  graphics_context_set_text_color(ctx, ui_text());
  graphics_draw_text(ctx, CAPTION, ui_font(HELP_BODY),
                     GRect(b.origin.x + LPAD, cap_y, cap_w, cap_h + 4),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// Compact height of the page: title, its gap, the pin block, the caption and
// padding (no image↔caption gap here — that's the slack). Kept tight so on the
// (taller) emery content area the scrollview hands us extra height, which opens
// up between the image and caption rather than scrolling the caption under the
// footer.
static int16_t diagram_page_measure(int width, void *data) {
  int title_h = ui_font_cap(UI_FONT_TITLE) + 4;
  int cap_h = text_wrap_h(CAPTION, HELP_BODY, width - 2 * LPAD);
  return TOP_PAD + title_h + TITLE_GAP + GRID_H + cap_h + BOT_PAD;
}

void help_push(void) {
  Page pages[1 + ARRAY_LENGTH(TEXT_PAGES)];
  pages[0] = (Page){ .draw = diagram_page_draw, .measure = diagram_page_measure };
  for (unsigned i = 0; i < ARRAY_LENGTH(TEXT_PAGES); i++) {
    pages[1 + i] = (Page){
      .draw = text_page_draw, .measure = text_page_measure,
      .data = (void *)&TEXT_PAGES[i],
    };
  }
  pager_push(pages, 1 + ARRAY_LENGTH(TEXT_PAGES), UI_DOTS_FLOATING);
}
