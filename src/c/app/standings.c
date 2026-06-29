#include "standings.h"
#include "strings.h"
#include "c/lib/ui/ui_align.h"
#include "c/lib/ui/ui_theme.h"
#include "c/lib/ui/ui_text.h"

// A slightly taller crown (24x22) so the place number sits comfortably inside.
#define CROWN_W 24
#define CROWN_H 22
static GPath *s_crown;
static const GPathInfo CROWN_PATH = {
  .num_points = 9,
  .points = (GPoint[]) {{0,22},{0,8},{4,13},{8,3},{12,13},{16,3},{20,13},{24,8},{24,22}},
};

void ui_draw_crown(GContext *ctx, GRect box, int number, bool selected) {
  if (!s_crown) s_crown = gpath_create(&CROWN_PATH);
  GPoint o = ui_rect_center(box, GSize(CROWN_W, CROWN_H)).origin;
  int ox = o.x, oy = o.y;
  gpath_move_to(s_crown, GPoint(ox, oy));
  // 1st = gold, 2nd = silver, 3rd = bronze
  GColor medal = GColorYellow;
  if (number == 2) medal = GColorLightGray;
  else if (number == 3) medal = GColorWindsorTan;
  graphics_context_set_fill_color(ctx, medal);
  gpath_draw_filled(ctx, s_crown);

  char n[4]; snprintf(n, sizeof(n), "%d", number);
  graphics_context_set_text_color(ctx, GColorBlack);
  ui_text_draw(ctx, n, UI_FONT_CAPTION_BOLD, GRect(ox, oy + 7, CROWN_W, 14),
               GTextAlignmentCenter, false, GTextOverflowModeFill);
}

void standings_crown_acc(GContext *ctx, GRect box, RowColors colors, void *data) {
  // The crown's number (its visual focus) sits a few px below the crown's
  // geometric center, so a plain center-in-cell reads low against the name. Lift
  // the crown so the number lines up with the title's optical center.
  box.origin.y -= 3;
  ui_draw_crown(ctx, box, (int)(intptr_t)data, false);
}

// Leading badge for a non-podium finisher (place 4+, or an eliminated player):
// the rank number "4." in a smaller font than the crowned places carry, so the
// podium reads as the visual focus.
static void standings_place_acc(GContext *ctx, GRect box, RowColors col, void *data) {
  char n[8]; snprintf(n, sizeof n, "%d.", (int)(intptr_t)data);
  box.origin.y += 2;                                   // drop the number to sit with the name baseline
  graphics_context_set_text_color(ctx, col.fg);
  ui_text_draw(ctx, n, UI_FONT_BODY_BOLD, box, GTextAlignmentCenter, true, GTextOverflowModeFill);
}

// Trailing accessory for an eliminated player: "out (score)" with the score in
// a lighter gray than the muted "out", right-aligned in the slot.
static void standings_out_acc(GContext *ctx, GRect box, RowColors col, void *data) {
  int score = (int)(intptr_t)data;
  char paren[8]; snprintf(paren, sizeof paren, "(%d)", score);

  GRect r = box; r.size.w -= 6;                        // small right margin
  graphics_context_set_text_color(ctx, ui_neutral());  // light gray "(score)"
  ui_text_draw(ctx, paren, UI_FONT_BODY_BOLD, r, GTextAlignmentRight, true, GTextOverflowModeFill);

  GRect ob = r; ob.size.w -= 30;                        // reserve the parenthesized score
  graphics_context_set_text_color(ctx, col.fg);        // muted "out"
  ui_text_draw(ctx, t(STR_OUT), UI_FONT_BODY_BOLD, ob, GTextAlignmentRight, true, GTextOverflowModeFill);
}

void standings_fill_row(ListItem *item, const char *name, int place, int score, bool out) {
  snprintf(item->title, sizeof(item->title), "%s", name);
  item->content_dy = 2;                               // nudge the name/score down a touch (crown stays put)

  // left badge: a crown for the podium (1st-3rd) — never for an eliminated
  // player, who shows just the rank number — otherwise the plain number
  if (!out && place >= 1 && place <= 3) {
    item->leading = (Accessory){ .kind = ACC_CUSTOM,
      .custom = { standings_crown_acc, (void *)(intptr_t)place } };
  } else if (place > 0) {
    item->leading = (Accessory){ .kind = ACC_CUSTOM,
      .custom = { standings_place_acc, (void *)(intptr_t)place } };
  }

  // an eliminated player shows "out (score)" on the right and renders muted
  if (out) {
    item->disabled = true;
    item->trailing = (Accessory){ .kind = ACC_CUSTOM,
      .custom = { standings_out_acc, (void *)(intptr_t)score } };
  } else {
    item->trailing.kind = ACC_VALUE;
    snprintf(item->trailing.value, sizeof(item->trailing.value), "%d", score);
  }
}
