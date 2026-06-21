#include "game.h"
#include "molkky.h"
#include "standings.h"
#include "results_view.h"
#include "c/lib/ui/ui_theme.h"
#include "c/lib/ui/ui_text.h"
#include "c/lib/ui/button.h"
#include "c/lib/ui/button_group.h"
#include "c/lib/ui/view.h"

// =============================================================================
// Game board (physical buttons), the touch-only throw grid, and placement.
// Touch is used ONLY on the throw grid; every other screen is button-driven.
// =============================================================================

static View          *s_board_view;
static Window        *s_turn_window;
static Layer         *s_turn_layer;
static UiButtonGroup *s_turn_group;
static Window    *s_place_window;
static MenuLayer *s_place_menu;

static void turn_push(void);
static void placement_push(void);
static void result_push(void);

// ---------------------------- board ----------------------------
// The live scoreboard: one custom row per player (current player highlighted),
// a pinned footer (round + clock on the left, undo action on the right). It's a
// dynamic `view`: board_build rebuilds the rows on every appear/reload, so the
// scores refresh when a turn pops back. Players aren't focusable — Down scrolls,
// then arms the footer's undo; Select throws for the current player (or, when
// the undo is armed, undoes the last throw).

// Fit every player when we can: shrink the row toward a 34px floor as the count
// grows, capped at a comfortable 50px; past the floor the board scrolls. `avail`
// is the real scroll viewport (window minus footer), so this is platform-correct.
static int16_t board_row_height(int count, int avail) {
  if (count < 1) count = 1;
  int h = avail / count;
  if (h > 50) h = 50;
  if (h < 34) h = 34;
  return h;
}

// Draw one player row into `b` (a BLOCK_CUSTOM); data is the player index.
static void board_row_draw(GContext *ctx, GRect b, void *data) {
  int i = (int)(intptr_t)data;
  MKGame *g = mk_game();
  if (i >= g->count) return;
  MKGamePlayer *p = &g->players[i];
  bool current = (i == g->current && !p->retired && !p->out);
  // When undo is armed the focus has moved to the footer, so soften the current
  // player's highlight to the neutral fill (ink stays legible as plain text).
  bool armed = view_action_focused(s_board_view);
  GColor rowbg = current ? (armed ? ui_neutral() : ui_accent()) : ui_background();
  GColor ink = (current && !armed) ? ui_accent_text() : ui_text();  // ink legible on this row's fill

  if (current) {                                    // highlight the current player
    graphics_context_set_fill_color(ctx, rowbg);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
  }
  int xname = 8;
  if (p->place > 0) {                               // finished: crown for 1st-3rd, else the number
    GRect lead = GRect(b.origin.x + 2, b.origin.y, 30, b.size.h);
    if (p->place <= 3) {
      ui_draw_crown(ctx, lead, p->place, false);
    } else {
      char pl[6]; snprintf(pl, sizeof(pl), "%d.", p->place);
      graphics_context_set_text_color(ctx, ink);
      ui_text_draw(ctx, pl, UI_FONT_TITLE, lead, GTextAlignmentCenter, true, GTextOverflowModeFill);
    }
    xname = 36;
  }

  bool show_dots = mk_lose_on_3() && !p->retired && !p->out;
  int rr = show_dots ? 86 : 56;                     // reserve room on the right for dots+score / score

  graphics_context_set_text_color(ctx, p->out ? ui_text_muted() : ink);
  ui_text_draw(ctx, mk_game_player_name(p), UI_FONT_TITLE,
               GRect(b.origin.x + xname, b.origin.y, b.size.w - xname - rr, b.size.h),
               GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);

  if (show_dots) {                                  // miss markers — crisp squares
    int dx = b.origin.x + b.size.w - 88, dy = b.origin.y + b.size.h / 2;
    for (int m = 0; m < 3; m++) {                     // 11px squares, 2px gap (= border width)
      GRect sq = GRect(dx + m * 13 - 5, dy - 5, 11, 11);
      if (m < p->misses) {                           // recorded miss — solid
        graphics_context_set_fill_color(ctx, ui_danger());
        graphics_fill_rect(ctx, sq, 0, GCornerNone);
      } else {                                        // empty — bold 2px border (fill minus inner)
        graphics_context_set_fill_color(ctx, current ? ink : ui_text_muted());
        graphics_fill_rect(ctx, sq, 0, GCornerNone);
        graphics_context_set_fill_color(ctx, rowbg);
        graphics_fill_rect(ctx, GRect(sq.origin.x + 2, sq.origin.y + 2, sq.size.w - 4, sq.size.h - 4), 0, GCornerNone);
      }
    }
  }

  char sc[6];
  graphics_context_set_text_color(ctx, p->out ? ui_text_muted() : ink);
  if (p->out) snprintf(sc, sizeof(sc), "out"); else snprintf(sc, sizeof(sc), "%d", p->score);
  ui_text_draw(ctx, sc, UI_FONT_TITLE,
               GRect(b.origin.x + b.size.w - 46, b.origin.y, 40, b.size.h),
               GTextAlignmentCenter, true, GTextOverflowModeFill);
}

static int board_build(Block *out, int cap, int avail_h) {
  // Safety net: if the saved "current" player is already finished (e.g. the app
  // was killed while the placement screen was up), advance to a live player.
  if (mk_game_active()) {
    MKGamePlayer *p = mk_game_current();
    if (p->retired || p->out) mk_game_continue();
  }
  MKGame *g = mk_game();
  int16_t h = board_row_height(g->count, avail_h);
  int n = 0;
  for (int i = 0; i < g->count && n < cap; i++)
    out[n++] = block_custom(h, board_row_draw, (void *)(intptr_t)i);
  return n;
}

static void board_footer(FooterModel *m) {
  MKGamePlayer *p = mk_game_current();
  snprintf(m->left, sizeof(m->left), "Round %d", p ? p->throws + 1 : 1);  // round-robin: current's turns + 1
  m->show_clock = true;
  m->action_icon = RESOURCE_ID_IMAGE_UNDO;
}
static bool board_can_undo(void) { return mk_game_can_undo(); }
static void board_undo(void) { if (mk_game_undo()) view_reload(s_board_view); }
static void board_open_turn(void) {
  MKGamePlayer *p = mk_game_current();
  if (p && !p->retired && !p->out) turn_push();     // always the current player
}

void game_show_board(void) {
  s_board_view = view_push(NULL, 0, (ViewOpts) {
    .size = UI_SIZE_MD, .speed = SCROLL_MED, .build = board_build,
    .on_select = board_open_turn,
    .footer = true, .get_footer = board_footer,
    .can_arm = board_can_undo, .on_action = board_undo,
  });
}

// ------------------------- throw grid (touch + buttons) ---------------------
// A ButtonGroup owns the input: tap a key on a touch watch, or step the focus
// ring with Up/Down and press Select on any watch. Both fire turn_apply(id),
// where id is the pin value 1..12 (or 0 for a MISS). The header (player + score)
// is a separate layer below the buttons.
#define GRID_TOP 36
#define MISS_H   44

static const char *const NUM_LABELS[] = {
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
};

static GRect num_rect(GRect b, int v) {
  int gh = b.size.h - GRID_TOP - MISS_H, rh = gh / 4, cw = b.size.w / 3;
  int i = v - 1, r = i / 3, c = i % 3;
  return GRect(c * cw + 2, GRID_TOP + r * rh + 2, cw - 4, rh - 4);
}

static void turn_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  MKGamePlayer *p = mk_game_current();
  int need = MK_WIN - p->score; if (need < 0) need = 0;

  graphics_context_set_text_color(ctx, ui_text());
  ui_text_draw(ctx, mk_game_player_name(p), UI_FONT_BODY_BOLD, GRect(2, 2, b.size.w - 4, 20),
               GTextAlignmentCenter, false, GTextOverflowModeTrailingEllipsis);
  char sub[40];
  if (p->score == 0) snprintf(sub, sizeof(sub), "have %d / need %d", p->score, need);
  else               snprintf(sub, sizeof(sub), "have %d / need %d to hit %d", p->score, need, MK_WIN);
  ui_text_draw(ctx, sub, UI_FONT_CAPTION, GRect(2, 19, b.size.w - 4, 16),
               GTextAlignmentCenter, false, GTextOverflowModeFill);
}

// id is the pin value (1..12) or 0 for a MISS. The group defers this off the
// input callback, so navigating here is safe.
static void turn_apply(int id, void *ctx) {
  MKThrowResult res = mk_game_throw(id);
  if (res == MK_THROW_NORMAL) {
    window_stack_pop(false);                        // turn -> board (next player)
  } else if (res == MK_THROW_WIN) {
    // A player reached 50 while others play on. Push the victory screen OVER the
    // turn window, then drop the turn window. The board is never exposed here, so
    // board_build can't advance past the winner.
    placement_push();
    window_stack_remove(s_turn_window, false);
  } else {                                          // GAMEOVER: the game is decided
    mk_game_end();                                  // finalize placements + history
    result_push();
    window_stack_remove(s_turn_window, false);
    window_stack_remove(view_window(s_board_view), false);
  }
}

static void turn_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_turn_layer = layer_create(b);
  layer_set_update_proc(s_turn_layer, turn_draw);   // header only; buttons are the group
  layer_add_child(root, s_turn_layer);

  s_turn_group = ui_button_group_create(w, b,
      (UiButtonGroupHandlers){ .on_click = turn_apply }, NULL);
  for (int v = 1; v <= 12; v++) {
    ui_button_group_add(s_turn_group, (UiButton){
      .id = v, .frame = num_rect(b, v),
      .look = { .style = UI_BTN_SOLID, .scheme = UI_BTN_NEUTRAL,
                .label = NUM_LABELS[v], .font = UI_FONT_TITLE, .radius = 4 },
    });
  }
  ui_button_group_add(s_turn_group, (UiButton){
    .id = 0, .frame = GRect(2, b.size.h - MISS_H + 2, b.size.w - 4, MISS_H - 6),
    .look = { .style = UI_BTN_SOLID, .scheme = UI_BTN_DANGER,
              .label = "MISS", .font = UI_FONT_TITLE, .radius = 4 },
  });
}
static void turn_unload(Window *w) {
  ui_button_group_destroy(s_turn_group);
  layer_destroy(s_turn_layer); window_destroy(s_turn_window);
  s_turn_window = NULL; s_turn_layer = NULL; s_turn_group = NULL;
}
static void turn_push(void) {
  s_turn_window = window_create();
  window_set_background_color(s_turn_window, ui_background());
  window_set_window_handlers(s_turn_window, (WindowHandlers) {
    .load = turn_load, .unload = turn_unload,
  });
  window_stack_push(s_turn_window, true);
}

// ---------------------------- placement (victory) ----------------------------
// The victory screen celebrates the player who just reached 50 (the highest
// placement so far) and offers the actions below. The full standings live in
// History.
static int place_winner_index(void) {
  MKGame *g = mk_game();
  int best = -1;
  for (int i = 0; i < g->count; i++)
    if (g->players[i].retired && (best < 0 || g->players[i].place > g->players[best].place))
      best = i;
  return best;
}

// Section-1 actions, built per-appear since availability depends on game state:
//   Continue playing      — only when >2 players are still in contention
//   Play till end of round — whenever an active player still owes a throw this round
//   End game               — always
typedef enum { PLACE_CONTINUE, PLACE_PLAYOUT, PLACE_END } PlaceAct;
static PlaceAct s_place_acts[3];
static int      s_place_act_n;
static void place_build_acts(void) {
  s_place_act_n = 0;
  if (mk_game_active_count() > 2)      s_place_acts[s_place_act_n++] = PLACE_CONTINUE;
  if (mk_game_round_has_remaining())   s_place_acts[s_place_act_n++] = PLACE_PLAYOUT;
  s_place_acts[s_place_act_n++] = PLACE_END;
}
static const char *place_act_label(PlaceAct a) {
  switch (a) {
    case PLACE_CONTINUE: return "Continue playing";
    case PLACE_PLAYOUT:  return "Play till end of round";
    default:             return "End game";
  }
}

static uint16_t place_sections(MenuLayer *ml, void *c) { return 2; }
static uint16_t place_rows(MenuLayer *ml, uint16_t s, void *c) {
  if (s == 0) return 1;                             // just the winner
  place_build_acts();
  return s_place_act_n;
}
static int16_t place_h(MenuLayer *ml, MenuIndex *i, void *c) { return i->section == 0 ? 60 : 42; }
static int16_t place_header_h(MenuLayer *ml, uint16_t s, void *c) { return s == 0 ? 4 : 6; }
static void place_header(GContext *g, const Layer *cl, uint16_t s, void *c) { }  // spacing only

static void place_draw(GContext *ctx, const Layer *cl, MenuIndex *idx, void *c) {
  if (idx->section != 0) {
    if (idx->row < s_place_act_n)
      menu_cell_basic_draw(ctx, cl, place_act_label(s_place_acts[idx->row]), NULL, NULL);
    return;
  }
  GRect b = layer_get_bounds(cl);
  int wi = place_winner_index();
  if (wi < 0) {                                     // everyone was eliminated — no winner
    graphics_context_set_text_color(ctx, ui_text());
    ui_text_draw(ctx, "Game over", UI_FONT_TITLE, b, GTextAlignmentCenter, true, GTextOverflowModeFill);
    return;
  }
  MKGamePlayer *p = &mk_game()->players[wi];
  ui_draw_crown(ctx, GRect(b.origin.x + 8, b.origin.y, 32, b.size.h), p->place, false);
  graphics_context_set_text_color(ctx, ui_text());
  ui_text_draw(ctx, mk_game_player_name(p), UI_FONT_TITLE, GRect(b.origin.x + 48, b.origin.y + 10, b.size.w - 54, 28),
               GTextAlignmentLeft, false, GTextOverflowModeTrailingEllipsis);
  ui_text_draw(ctx, "reached 50", UI_FONT_CAPTION, GRect(b.origin.x + 48, b.origin.y + 38, b.size.w - 54, 16),
               GTextAlignmentLeft, false, GTextOverflowModeFill);
}

static void place_end(void) {
  mk_game_end();
  result_push();                                     // push result OVER placement
  window_stack_remove(s_place_window, false);
  window_stack_remove(view_window(s_board_view), false);
}
static void place_continue(void) {
  if (mk_game_continue()) window_stack_pop(true);    // pop placement -> board
  else place_end();
}
static void place_playout(void) {
  mk_game_play_out();                                // finish the round, then the game ends
  window_stack_pop(true);                            // pop placement -> board
}
static void place_do_select(MenuIndex idx) {
  if (idx.section != 1 || idx.row >= s_place_act_n) return;
  switch (s_place_acts[idx.row]) {
    case PLACE_CONTINUE: place_continue(); break;
    case PLACE_PLAYOUT:  place_playout();  break;
    case PLACE_END:      place_end();      break;
  }
}

// Manual click config so Back can mean "continue".
static void place_up(ClickRecognizerRef r, void *c) { menu_layer_set_selected_next(s_place_menu, true, MenuRowAlignCenter, true); }
static void place_down(ClickRecognizerRef r, void *c) { menu_layer_set_selected_next(s_place_menu, false, MenuRowAlignCenter, true); }
static void place_sel(ClickRecognizerRef r, void *c) { place_do_select(menu_layer_get_selected_index(s_place_menu)); }
static void place_back(ClickRecognizerRef r, void *c) { place_continue(); }
static void place_click(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, place_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, place_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, place_sel);
  window_single_click_subscribe(BUTTON_ID_BACK, place_back);
}

static void place_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_place_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_place_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = place_sections, .get_num_rows = place_rows,
    .get_cell_height = place_h, .get_header_height = place_header_h,
    .draw_header = place_header, .draw_row = place_draw,
  });
  menu_layer_set_normal_colors(s_place_menu, ui_background(), ui_text());
  menu_layer_set_highlight_colors(s_place_menu, ui_accent(), ui_accent_text());
  layer_add_child(root, menu_layer_get_layer(s_place_menu));
  window_set_click_config_provider(w, place_click);
  // start on the first action row so Select acts; standings sit above
  menu_layer_set_selected_index(s_place_menu, (MenuIndex){ .section = 1, .row = 0 },
                                MenuRowAlignBottom, false);
}
static void place_unload(Window *w) {
  menu_layer_destroy(s_place_menu); window_destroy(s_place_window);
  s_place_window = NULL; s_place_menu = NULL;
}
static void placement_push(void) {
  s_place_window = window_create();
  window_set_background_color(s_place_window, ui_background());
  window_set_window_handlers(s_place_window, (WindowHandlers) {
    .load = place_load, .unload = place_unload,
  });
  window_stack_push(s_place_window, true);
}

// ---------------------------- result screen ----------------------------
// Shown once the game is decided (last player completes, or End game pressed).
// Final standings + a Stats section, rendered by the shared results_view so a
// just-finished game matches how History shows it later. No on-screen buttons —
// Select/Back return to the main menu.
static void result_leave(void) { window_stack_pop(true); }

static void result_push(void) {
  MKGame *g = mk_game();
  // static, not on the stack: this is built from a deep call (throw -> end ->
  // result_push) and an on-stack array at 16 players faults the app. Only one
  // result screen exists at a time.
  static ResultRow rows[MK_MAX_PLAYERS];
  int n = 0;
  // Emit by place ascending; ties (same place) list every player who shares it.
  for (int place = 1; place <= g->count; place++) {
    for (int i = 0; i < g->count; i++) {
      MKGamePlayer *p = &g->players[i];
      if (p->place != place) continue;
      rows[n++] = (ResultRow){
        .name   = mk_game_player_name(p), .place = p->place, .score = p->score, .out = p->out,
        .misses = p->total_misses > 255 ? 255 : (uint8_t)p->total_misses,
        .throws = p->throws       > 255 ? 255 : (uint8_t)p->throws,
        .points = p->points,
      };
    }
  }
  int32_t secs = (int32_t)time(NULL) - g->start_time;
  uint16_t duration = (secs > 0) ? (uint16_t)(secs / 60) : 0;
  uint8_t  settings = (mk_lose_on_3() ? MK_SET_LOSE3 : 0) | (mk_final_round() ? MK_SET_FINAL : 0);
  results_view_push("Results", rows, n, duration, settings, result_leave);
}
