#include "standings.h"
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
  // center the crown within box
  int ox = box.origin.x + (box.size.w - CROWN_W) / 2;
  int oy = box.origin.y + (box.size.h - CROWN_H) / 2;
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
  ui_draw_crown(ctx, box, (int)(intptr_t)data, false);
}

void standings_fill_row(ListItem *item, const char *name, int place, int score, bool out) {
  snprintf(item->title, sizeof(item->title), "%s", name);

  // left badge: a crown for the podium (1st-3rd), otherwise just the number
  if (place >= 1 && place <= 3) {
    item->leading = (Accessory){ .kind = ACC_CUSTOM,
      .custom = { standings_crown_acc, (void *)(intptr_t)place } };
  } else if (place > 0) {
    item->leading.kind = ACC_VALUE;
    snprintf(item->leading.value, sizeof(item->leading.value), "%d.", place);
  }

  // an eliminated, unplaced player shows "out" and renders muted
  item->trailing.kind = ACC_VALUE;
  if (out && place == 0) {
    item->disabled = true;
    snprintf(item->trailing.value, sizeof(item->trailing.value), "out");
  } else {
    snprintf(item->trailing.value, sizeof(item->trailing.value), "%d", score);
  }
}
