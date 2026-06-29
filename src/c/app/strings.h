#pragma once
#include "c/lib/locale/locale.h"

// =============================================================================
// strings — the app's translatable text, on top of the generic locale engine.
//
// `StrId` names every user-facing string; the per-language tables live in
// strings.c. Call sites use the short macros:
//
//   t(STR_NEW_GAME)                       → const char *  (plain lookup)
//   tfmt(buf, sizeof buf, STR_ROUND, n)   → printf-style, the *template* is localized
//   tdate(buf, sizeof buf, STR_FMT_HISTORY_DATE, unix_time)  → localized date
//
// Pluralization is handled by picking the id (…_ONE / …_MANY) at the call site,
// which keeps the engine simple and lets each language define its own forms.
//
// Adding a language is data-only: add a column in strings.c (see LOCALES). A
// missing entry falls back to English, so a partial translation still runs.
// =============================================================================

typedef enum {
  // ---- main menu / common ----
  STR_RESUME_GAME,
  STR_NEW_GAME,
  STR_PLAYERS,
  STR_HISTORY,
  STR_HELP,
  STR_SETTINGS,
  STR_OK,
  STR_DELETE,
  STR_DISCARD,
  STR_RESET,
  STR_GO_BACK,
  STR_OUT,                 // scoreboard: eliminated player
  STR_DELETED,             // a player removed from the roster

  // ---- reset flow ----
  STR_RESET_DONE_TITLE,
  STR_RESET_DONE_BODY,
  STR_RESET_CONFIRM_TITLE,
  STR_RESET_CONFIRM_BODY,

  // ---- in-game menu ----
  STR_GAME,                // in-game menu title
  STR_MAIN_MENU,
  STR_END_GAME,
  STR_DISCARD_GAME,
  STR_END_GAME_Q,
  STR_END_GAME_BODY,
  STR_DISCARD_GAME_Q,
  STR_DISCARD_GAME_BODY,

  // ---- board / turn / placement ----
  STR_ROUND,               // "Round %d"
  STR_HAVE_NEED,           // "have %d / need %d"
  STR_MISS,                // throw grid MISS key
  STR_CONTINUE_PLAYING,
  STR_PLAY_TILL_END,
  STR_GAME_OVER,
  STR_REACHED_50,

  // ---- players / roster ----
  STR_ADD_PLAYER,
  STR_ARCHIVED_PLAYERS,
  STR_NO_ARCHIVED,
  STR_PLAYER_COUNT_ONE,    // "%d player"
  STR_PLAYER_COUNT_MANY,   // "%d players"
  STR_DELETE_PLAYER_Q,     // "Delete %s?"
  STR_DELETE_PLAYER_BODY,
  STR_NO_GAMES_YET,
  STR_PLAY_TO_TRACK,
  STR_GAMES,
  STR_WIN_RATE,
  STR_ACCURACY,
  STR_PTS_PER_TURN,
  STR_STATS,
  STR_RENAME,
  STR_ARCHIVE,
  STR_UNARCHIVE,

  // ---- new-game picker ----
  STR_SYNC_TO_ADD,
  STR_HISTORY_FULL,
  STR_START_GAME,
  STR_START_GAME_N,        // "Start game (%d)"

  // ---- history ----
  STR_RESULTS,
  STR_WINNER,              // "Winner: %s"
  STR_DELETE_GAME_Q,
  STR_DELETE_GAME_BODY,
  STR_LOADING,
  STR_SYNC_NEEDED,
  STR_SYNCING,
  STR_UNSAVED,             // "%d unsaved"
  STR_FMT_HISTORY_DATE,    // strftime-style pattern (tdate)

  // ---- results / stats ----
  STR_HIGHEST_ACC,
  STR_LOWEST_ACC,
  STR_AVG_PER_TURN,
  STR_DURATION,
  STR_ACC_VALUE_ONE,       // "%s %d%% (%d miss)"
  STR_ACC_VALUE_MANY,      // "%s %d%% (%d misses)"
  STR_PTS_VALUE,           // "%d.%d pts"
  STR_FMT_DECIMAL1,        // "%d.%d" — locale decimal separator
  STR_DUR_LT_MIN,          // "< 1 min"
  STR_DUR_MIN,             // "%d min"
  STR_DUR_H_MIN,           // "%d h %d min"
  STR_DUR_H,               // "%d h"

  // ---- help (rules) ----
  STR_HELP_OBJECTIVE_T,
  STR_HELP_OBJECTIVE_B,
  STR_HELP_THROWING_T,
  STR_HELP_THROWING_B,
  STR_HELP_SCORING_T,
  STR_HELP_SCORING_B,
  STR_HELP_OVERSHOOT_T,
  STR_HELP_OVERSHOOT_B,
  STR_HELP_WINNING_T,
  STR_HELP_WINNING_B,
  STR_HELP_PIN_SETUP,
  STR_HELP_PIN_CAPTION,

  // ---- settings ----
  STR_SEC_GAMEPLAY,
  STR_SEC_OTHER,
  STR_LOSE_3,
  STR_FINISH_ROUND,
  STR_TIES_SHARED,
  STR_PAGE_HEADER,
  STR_TITLE_CLOCK,
  STR_PIN_ARTWORK,
  STR_PIN_FORMATION,
  STR_LANGUAGE,

  STR__COUNT,
} StrId;

// Register the locale tables. Call once at startup (before any lookup); the
// active language defaults to English until the app applies a saved/seeded one.
void mk_locale_init(void);

// Short call-site helpers over the engine.
#define t(id)                   locale_str(id)
#define tfmt(buf, n, id, ...)   locale_format((buf), (n), (id), __VA_ARGS__)
#define tdate(buf, n, id, time) locale_strftime((buf), (n), (id), (time))
