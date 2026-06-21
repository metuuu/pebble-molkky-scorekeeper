#include "players.h"
#include "molkky.h"
#include "c/lib/ui/menu.h"
#include "game.h"
#include "c/lib/t9_keyboard/t9_keyboard_window.h"

// =============================================================================
// Roster management (Players screen) and the new-game player picker.
// All navigation is physical buttons; name entry uses the T9 keyboard.
// =============================================================================

// ---------------------------- Players list ----------------------------
static Menu *s_opts_menu;
static Menu *s_confirm_menu;
static int   s_opt_idx;                // active-roster index being edited
static char  s_confirm_title[28];

static Menu *s_arch_menu;              // the archived-players list
static Menu *s_arch_opts;             // options for one archived player
static int   s_arch_idx;              // archived index being edited
static bool  s_confirm_arch;          // pending delete-confirm targets the archived list

static void open_options(int idx);
static void archived_push(void);

// Rows: the active players, then a trailing "Archived players" row when any
// exist. A lone placeholder shows only when there are no players at all.
static uint16_t pl_count(void *c) {
  int n = mk_roster_count(), a = mk_roster_archived_count();
  if (n == 0 && a == 0) return 1;
  return n + (a > 0 ? 1 : 0);
}
static void pl_item(void *c, uint16_t i, ListItem *out) {
  int n = mk_roster_count(), a = mk_roster_archived_count();
  if (n == 0 && a == 0) { snprintf(out->title, sizeof out->title, "No players yet"); return; }
  if (i < n) {
    snprintf(out->title, sizeof out->title, "%s", mk_roster_name(i));
    out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USER };
    return;
  }
  snprintf(out->title, sizeof out->title, "Archived players");
  snprintf(out->subtitle, sizeof out->subtitle, "%d player%s", a, a == 1 ? "" : "s");
  out->leading  = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_ARCHIVE };
  out->trailing = (Accessory){ .kind = ACC_CHEVRON };
}
static void pl_select(void *c, uint16_t i) {
  int n = mk_roster_count(), a = mk_roster_archived_count();
  if (n == 0 && a == 0) return;
  if (i < n) open_options(i);
  else if (a > 0) archived_push();
}

void players_push(void) {
  menu_push("Players", (MenuConfig) {
    .get_count = pl_count, .get_item = pl_item, .on_select = pl_select,
  });
}

// ---------------------------- Delete confirm ----------------------------
static uint16_t cf_count(void *c) { return 2; }
static void cf_item(void *c, uint16_t i, ListItem *out) {
  snprintf(out->title, sizeof out->title, i == 0 ? "Cancel" : "Delete");
  if (i == 1) out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_DELETE };
}
static void cf_select(void *c, uint16_t i) {
  if (i == 0) { window_stack_pop(true); return; }  // back to options
  if (s_confirm_arch) {
    mk_roster_archived_delete(s_arch_idx);
    window_stack_remove(menu_window(s_arch_opts), false);          // drop archived options
    if (mk_roster_archived_count() == 0)
      window_stack_remove(menu_window(s_arch_menu), false);        // and the now-empty list
  } else {
    mk_roster_delete(s_opt_idx);
    window_stack_remove(menu_window(s_opts_menu), false);          // drop options
  }
  window_stack_pop(true);                          // pop confirm -> list underneath
}

// ---------------------------- Player options ----------------------------
static void on_rename(const char *text, void *ctx) {
  if (text && text[0]) {
    mk_roster_rename(s_opt_idx, text);
    // return straight to the (reloaded) players list so the new name shows
    window_stack_remove(menu_window(s_opts_menu), false);
  }
}
static uint16_t opt_count(void *c) { return 3; }
static void opt_item(void *c, uint16_t i, ListItem *out) {
  static const char    *titles[] = { "Rename", "Archive", "Delete" };
  static const uint32_t  icons[]  = { RESOURCE_ID_IMAGE_RENAME,
                                      RESOURCE_ID_IMAGE_ARCHIVE,
                                      RESOURCE_ID_IMAGE_DELETE };
  snprintf(out->title, sizeof out->title, "%s", titles[i]);
  out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = icons[i] };
}
static void opt_select(void *c, uint16_t i) {
  if (i == 0) {
    t9_keyboard_window_push_ex(on_rename, mk_roster_name(s_opt_idx), MK_MAX_NAME - 1, NULL);
  } else if (i == 1) {
    mk_roster_archive(s_opt_idx);
    window_stack_remove(menu_window(s_opts_menu), false);   // back to the players list
  } else {
    s_confirm_arch = false;
    snprintf(s_confirm_title, sizeof(s_confirm_title), "Delete %s?", mk_roster_name(s_opt_idx));
    s_confirm_menu = menu_push(s_confirm_title, (MenuConfig) {
      .get_count = cf_count, .get_item = cf_item, .on_select = cf_select,
    });
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
  snprintf(out->title, sizeof out->title, i == 0 ? "Unarchive" : "Delete");
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
    snprintf(s_confirm_title, sizeof(s_confirm_title), "Delete %s?", mk_roster_archived_name(s_arch_idx));
    s_confirm_menu = menu_push(s_confirm_title, (MenuConfig) {
      .get_count = cf_count, .get_item = cf_item, .on_select = cf_select,
    });
  }
}

static uint16_t al_count(void *c) { int n = mk_roster_archived_count(); return n ? n : 1; }
static void al_item(void *c, uint16_t i, ListItem *out) {
  if (mk_roster_archived_count() == 0) {
    snprintf(out->title, sizeof out->title, "No archived players"); return;
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
  s_arch_menu = menu_push("Archived players", (MenuConfig) {
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
      snprintf(out->title, sizeof out->title, "Sync to add games");
      snprintf(out->subtitle, sizeof out->subtitle, "History buffer full");
      out->disabled = true;
      return;
    }
    int n = sel_count();
    if (n > 0) snprintf(out->title, sizeof out->title, "Start game (%d)", n);
    else       snprintf(out->title, sizeof out->title, "Start game");   // no "(0)" when nothing selected
    out->disabled = n < 2;                            // Start needs >= 2 players
    return;
  }
  if (i == 1) {                                       // user-plus icon shows the "+"
    snprintf(out->title, sizeof out->title, "Add player");
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
    t9_keyboard_window_push_ex(on_add, "", MK_MAX_NAME - 1, NULL);
  } else {
    s_sel[i - 2] = !s_sel[i - 2];
    menu_reload(s_pick_menu);
  }
}

void newgame_push(void) {
  for (int i = 0; i < MK_MAX_PLAYERS; i++) s_sel[i] = false;
  s_pick_menu = menu_push("New game", (MenuConfig) {
    .get_count = pick_count, .get_item = pick_item, .on_select = pick_select,
  });
}
