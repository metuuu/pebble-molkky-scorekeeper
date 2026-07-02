#include <pebble.h>
#include "molkky.h"
#include "strings.h"
#include "c/lib/ui/menu.h"
#include "game.h"
#include "players.h"
#include "history.h"
#include "help.h"
#include "app_settings.h"
#include "c/lib/multitap_keyboard/multitap_keyboard.h"
#include "c/lib/ui/ui_theme.h"
#include "c/lib/ui/dialog.h"

// Main menu and app-wide setup.
// Touch is only used on the throw-result grid; other screens use buttons.

static Menu *s_menu;

enum { K_CONTINUE, K_NEW, K_PLAYERS, K_HISTORY, K_HELP, K_SETTINGS };

// Maps a visible row to an action. The "Continue" row only exists while a game
// is in progress, so it shifts the rest down by one when present.
static int row_kind(int row) {
  if (mk_game_active()) {
    if (row == 0) return K_CONTINUE;
    row--;
  }
  switch (row) {
    case 0:  return K_NEW;
    case 1:  return K_PLAYERS;
    case 2:  return K_HISTORY;
    case 3:  return K_HELP;
    default: return K_SETTINGS;
  }
}

static uint16_t menu_count(void *c) { return mk_game_active() ? 6 : 5; }

static void menu_item(void *c, uint16_t i, ListItem *out) {
  switch (row_kind(i)) {
    case K_CONTINUE: snprintf(out->title, sizeof out->title, "%s", t(STR_RESUME_GAME)); break;  // no icon
    case K_NEW:      snprintf(out->title, sizeof out->title, "%s", t(STR_NEW_GAME));
                     out->leading = (Accessory){ .kind = ACC_ICON_RAW, .icon_res = RESOURCE_ID_IMAGE_LOGO }; break;
    case K_PLAYERS:  snprintf(out->title, sizeof out->title, "%s", t(STR_PLAYERS));
                     out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_USERS }; break;
    case K_HISTORY:  snprintf(out->title, sizeof out->title, "%s", t(STR_HISTORY));
                     out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_CHART }; break;
    case K_HELP:     snprintf(out->title, sizeof out->title, "%s", t(STR_HELP));
                     out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_INFO }; break;
    default:         snprintf(out->title, sizeof out->title, "%s", t(STR_SETTINGS));
                     out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_SETTINGS }; break;
  }
}

static void menu_select(void *c, uint16_t i) {
  switch (row_kind(i)) {
    case K_CONTINUE: game_show_board();   break;
    case K_NEW:      newgame_push();      break;
    case K_PLAYERS:  players_push();      break;
    case K_HISTORY:  history_push();      break;
    case K_HELP:     help_push();         break;
    default:         app_settings_push(); break;
  }
}

// Phone settings requested a full reset; confirm on the watch before wiping.
static void reset_do(void *ctx) {
  mk_reset_all();
  dialog_push((DialogConfig){
    .title = t(STR_RESET_DONE_TITLE),
    .text  = t(STR_RESET_DONE_BODY),
    .buttons = { { .label = t(STR_OK), .scheme = UI_BTN_NEUTRAL } },
    .button_count = 1,
  });
}
static void reset_request(void) {
  dialog_confirm_push(t(STR_RESET_CONFIRM_TITLE), t(STR_RESET_CONFIRM_BODY),
                      t(STR_RESET), UI_BTN_DANGER, t(STR_CANCEL), reset_do, NULL);
}

static void init(void) {
  mk_init();
  mk_on_reset_request(reset_request);   // phone settings page → confirm-and-wipe on the watch
  // Share Mölkky's palette with the app UI and keyboard.
  ui_theme_set((UiTheme){
    .background = GColorWhite, .text = GColorBlack, .text_muted = GColorDarkGray,
    .neutral = GColorLightGray, .accent = MK_ACCENT, .accent_text = GColorWhite,
    // Danger colors use the UI lib defaults.
  });
  multitap_keyboard_set_app_theme("Mölkky", ui_theme_get());
  // Header logo, used when the optional header setting is enabled.
  menu_set_header_icon(RESOURCE_ID_IMAGE_LOGO);
  s_menu = menu_push("Mölkky", (MenuConfig) {
    .get_count = menu_count, .get_item = menu_item, .on_select = menu_select,
  });
}

static void deinit(void) { }

int main(void) {
  init();
  app_event_loop();
  deinit();
}
