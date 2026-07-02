#include "players.h"
#include "molkky.h"
#include "strings.h"
#include "c/lib/ui/menu.h"
#include "c/lib/ui/view.h"
#include "c/lib/ui/dialog.h"
#include "game.h"
#include "c/lib/multitap_keyboard/multitap_keyboard_window.h"

// =============================================================================
// Roster management (Players screen) and the new-game player picker.
// All navigation is physical buttons; name entry uses the multitap keyboard.
// =============================================================================

// ---------------------------- Players list ----------------------------
static Menu *s_opts_menu;
static int   s_opt_idx;                // active-roster index being edited
static char  s_confirm_title[40];   // "Poistetaanko <name>?" etc. — room for a full name + the word

static Menu *s_arch_menu;              // the archived-players list
static Menu *s_arch_opts;             // options for one archived player
static int   s_arch_idx;              // archived index being edited
static bool  s_confirm_arch;          // pending delete-confirm targets the archived list

static void open_options(int idx);
static void archived_push(void);
static void player_stats_push(int idx);

static void on_pl_add(const char *text, void *ctx) {
  if (text && text[0]) mk_roster_add(text);   // list reloads on reappear
}

// Rows: 0 = + Add player, then the active players, then a trailing "Archived
// players" row when any exist.
static uint16_t pl_count(void *c) {
  int n = mk_roster_count(), a = mk_roster_archived_count();
  return 1 + n + (a > 0 ? 1 : 0);
}
static void pl_item(void *c, uint16_t i, ListItem *out) {
  int n = mk_roster_count();
  if (i == 0) {                                       // user-plus icon shows the "+"
    snprintf(out->title, sizeof out->title, "%s", t(STR_ADD_PLAYER));
    out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER_PLUS };
    return;
  }
  if (i - 1 < n) {
    snprintf(out->title, sizeof out->title, "%s", mk_roster_name(i - 1));
    out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER };
    return;
  }
  int a = mk_roster_archived_count();
  snprintf(out->title, sizeof out->title, "%s", t(STR_ARCHIVED_PLAYERS));
  tfmt(out->subtitle, sizeof out->subtitle, a == 1 ? STR_PLAYER_COUNT_ONE : STR_PLAYER_COUNT_MANY, a);
  out->leading  = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_ARCHIVE };
  out->trailing = (Accessory){ .kind = ACC_CHEVRON };
}
static void pl_select(void *c, uint16_t i) {
  int n = mk_roster_count();
  if (i == 0) { multitap_keyboard_window_push_ex(on_pl_add, "", MK_MAX_NAME - 1, NULL); return; }
  if (i - 1 < n) open_options(i - 1);
  else archived_push();
}

void players_push(void) {
  menu_push(t(STR_PLAYERS), (MenuConfig) {
    .get_count = pl_count, .get_item = pl_item, .on_select = pl_select,
  });
}

// ---------------------------- Delete confirm ----------------------------
// The shared confirm dialog dismisses itself; this runs after the user confirms.
static void do_delete_player(void *ctx) {
  if (s_confirm_arch) {
    mk_roster_archived_delete(s_arch_idx);
    window_stack_remove(menu_window(s_arch_opts), false);          // drop archived options
    if (mk_roster_archived_count() == 0)
      window_stack_remove(menu_window(s_arch_menu), false);        // and the now-empty list
  } else {
    mk_roster_delete(s_opt_idx);
    window_stack_remove(menu_window(s_opts_menu), false);          // drop options → list underneath
  }
}
static void confirm_delete_player(const char *name) {
  tfmt(s_confirm_title, sizeof(s_confirm_title), STR_DELETE_PLAYER_Q, name);
  dialog_confirm_push(s_confirm_title, t(STR_DELETE_PLAYER_BODY),
                      t(STR_DELETE), UI_BTN_DANGER, t(STR_CANCEL),
                      do_delete_player, NULL);
}

// ---------------------------- Player stats ----------------------------
// Lifetime totals for one player, rendered with the shared block view. Values
// are derived from the running MKLifetime sums (see molkky.c) — averages are
// rounded to one decimal / nearest percent.
static void player_stats_push(int idx) {
  const MKLifetime *L = mk_stats_get(idx);
  Block b[7];
  char v0[12], v1[20], v2[8], v3[16];
  int n = 0;
  b[n++] = block_section(mk_stats_name(idx));
  if (!L || L->games == 0) {
    b[n++] = block_field(t(STR_NO_GAMES_YET), t(STR_PLAY_TO_TRACK));
    view_push(b, n, (ViewOpts){ .size = UI_SIZE_SM });
    return;
  }
  snprintf(v0, sizeof v0, "%d", (int)L->games);
  b[n++] = block_field_inline(t(STR_GAMES), v0);
  int winpct = (int)((L->wins * 100u + L->games / 2) / L->games);
  snprintf(v1, sizeof v1, "%d%% (%d)", winpct, (int)L->wins);
  b[n++] = block_field_inline(t(STR_WIN_RATE), v1);
  if (L->throws) {
    int acc = (int)(((L->throws - L->misses) * 100u + L->throws / 2) / L->throws);
    snprintf(v2, sizeof v2, "%d%%", acc);
    b[n++] = block_field_inline(t(STR_ACCURACY), v2);
    int pts10 = (int)((L->points * 10u + L->throws / 2) / L->throws);
    tfmt(v3, sizeof v3, STR_FMT_DECIMAL1, pts10 / 10, pts10 % 10);
    b[n++] = block_field_inline(t(STR_PTS_PER_TURN), v3);
  }
  view_push(b, n, (ViewOpts){ .size = UI_SIZE_SM });
}

// ---------------------------- Player options ----------------------------
static void on_rename(const char *text, void *ctx) {
  if (text && text[0]) {
    mk_roster_rename(s_opt_idx, text);
    // return straight to the (reloaded) players list so the new name shows
    window_stack_remove(menu_window(s_opts_menu), false);
  }
}
static uint16_t opt_count(void *c) { return 4; }
static void opt_item(void *c, uint16_t i, ListItem *out) {
  static const StrId     titles[] = { STR_STATS, STR_RENAME, STR_ARCHIVE, STR_DELETE };
  static const uint32_t  icons[]  = { RESOURCE_ID_IMAGE_CHART,
                                      RESOURCE_ID_IMAGE_RENAME,
                                      RESOURCE_ID_IMAGE_ARCHIVE,
                                      RESOURCE_ID_IMAGE_DELETE };
  snprintf(out->title, sizeof out->title, "%s", t(titles[i]));
  out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = icons[i] };
}
static void opt_select(void *c, uint16_t i) {
  if (i == 0) {
    player_stats_push(s_opt_idx);
  } else if (i == 1) {
    multitap_keyboard_window_push_ex(on_rename, mk_roster_name(s_opt_idx), MK_MAX_NAME - 1, NULL);
  } else if (i == 2) {
    mk_roster_archive(s_opt_idx);
    window_stack_remove(menu_window(s_opts_menu), false);   // back to the players list
  } else {
    s_confirm_arch = false;
    confirm_delete_player(mk_roster_name(s_opt_idx));
  }
}
static void open_options(int idx) {
  s_opt_idx = idx;
  s_opts_menu = menu_push(mk_roster_name(idx), (MenuConfig) {
    .get_count = opt_count, .get_item = opt_item, .on_select = opt_select,
  });
}

// ---------------------------- Archived players ----------------------------
// Options for one archived player: bring them back, or delete for good.
static uint16_t ao_count(void *c) { return 2; }
static void ao_item(void *c, uint16_t i, ListItem *out) {
  snprintf(out->title, sizeof out->title, "%s", t(i == 0 ? STR_UNARCHIVE : STR_DELETE));
  out->leading = (Accessory){ .kind = ACC_ICON,
    .icon_res = i == 0 ? RESOURCE_ID_IMAGE_USER : RESOURCE_ID_IMAGE_DELETE };
}
static void ao_select(void *c, uint16_t i) {
  if (i == 0) {
    mk_roster_unarchive(s_arch_idx);
    window_stack_remove(menu_window(s_arch_opts), false);          // back to the archived list
    if (mk_roster_archived_count() == 0)
      window_stack_remove(menu_window(s_arch_menu), false);        // none left -> players list
  } else {
    s_confirm_arch = true;
    confirm_delete_player(mk_roster_archived_name(s_arch_idx));
  }
}

static uint16_t al_count(void *c) { int n = mk_roster_archived_count(); return n ? n : 1; }
static void al_item(void *c, uint16_t i, ListItem *out) {
  if (mk_roster_archived_count() == 0) {
    snprintf(out->title, sizeof out->title, "%s", t(STR_NO_ARCHIVED)); return;
  }
  snprintf(out->title, sizeof out->title, "%s", mk_roster_archived_name(i));
  out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER };
}
static void al_select(void *c, uint16_t i) {
  if (mk_roster_archived_count() == 0) return;
  s_arch_idx = i;
  s_arch_opts = menu_push(mk_roster_archived_name(i), (MenuConfig) {
    .get_count = ao_count, .get_item = ao_item, .on_select = ao_select,
  });
}
static void archived_push(void) {
  s_arch_menu = menu_push(t(STR_ARCHIVED_PLAYERS), (MenuConfig) {
    .get_count = al_count, .get_item = al_item, .on_select = al_select,
  });
}

// ---------------------------- New-game picker ----------------------------
static Menu *s_pick_menu;
static bool  s_sel[MK_MAX_PLAYERS];

static int sel_count(void) {
  int n = 0; for (int i = 0; i < mk_roster_count(); i++) if (s_sel[i]) n++;
  return n;
}
static void on_add(const char *text, void *ctx) {
  if (text && text[0] && mk_roster_add(text))
    s_sel[mk_roster_count() - 1] = true;            // auto-check the new player
}

// Rows: 0 = Start game, 1 = + Add player, 2.. = roster players.
static uint16_t pick_count(void *c) {
  return mk_roster_count() + 2;
}
static void pick_item(void *c, uint16_t i, ListItem *out) {
  if (i == 0) {
    if (!mk_hist_can_record()) {                      // history buffer full of un-backed-up games
      snprintf(out->title, sizeof out->title, "%s", t(STR_SYNC_TO_ADD));
      snprintf(out->subtitle, sizeof out->subtitle, "%s", t(STR_HISTORY_FULL));
      out->disabled = true;
      return;
    }
    int n = sel_count();
    if (n > 0) tfmt(out->title, sizeof out->title, STR_START_GAME_N, n);
    else       snprintf(out->title, sizeof out->title, "%s", t(STR_START_GAME));   // no "(0)" when nothing selected
    out->disabled = n < 2;                            // Start needs >= 2 players
    return;
  }
  if (i == 1) {                                       // user-plus icon shows the "+"
    snprintf(out->title, sizeof out->title, "%s", t(STR_ADD_PLAYER));
    out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER_PLUS };
    return;
  }
  snprintf(out->title, sizeof out->title, "%s", mk_roster_name(i - 2));
  out->leading  = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER };
  out->trailing = (Accessory){ .kind = ACC_CHECKBOX, .checked = s_sel[i - 2] };
}
static void pick_select(void *c, uint16_t i) {
  if (i == 0) {                                     // disabled Start never reaches here
    mk_game_start(s_sel);
    game_show_board();
    window_stack_remove(menu_window(s_pick_menu), false);
  } else if (i == 1) {
    multitap_keyboard_window_push_ex(on_add, "", MK_MAX_NAME - 1, NULL);
  } else {
    s_sel[i - 2] = !s_sel[i - 2];
    menu_reload(s_pick_menu);
  }
}

void newgame_push(void) {
  for (int i = 0; i < MK_MAX_PLAYERS; i++) s_sel[i] = false;
  s_pick_menu = menu_push(t(STR_NEW_GAME), (MenuConfig) {
    .get_count = pick_count, .get_item = pick_item, .on_select = pick_select,
  });
}
