#include "strings.h"

// =============================================================================
// strings — English (base) and Finnish (suomi) tables for the locale engine.
//
// Entries are keyed by StrId via designated initializers, so reordering the enum
// or leaving a gap can never silently misalign a translation. A NULL/omitted
// Finnish entry falls back to the English text at the same id.
//
// Conventions in format templates:
//   %d/%s    placed values (printf) — word order is the language's to choose.
//   Finnish decimals use a comma (",") where English uses a point (".").
//   The history date pattern is strftime-style (see locale_strftime): English
//   shows an abbreviated month name; Finnish uses the numeric "d.m." form with
//   "klo" before the time, so it needs no month-name table at all.
// =============================================================================

static const char *const EN[STR__COUNT] = {
  [STR_RESUME_GAME]         = "Resume game",
  [STR_NEW_GAME]            = "New game",
  [STR_PLAYERS]             = "Players",
  [STR_HISTORY]             = "History",
  [STR_HELP]                = "Help",
  [STR_SETTINGS]            = "Settings",
  [STR_OK]                  = "OK",
  [STR_DELETE]              = "Delete",
  [STR_DISCARD]             = "Discard",
  [STR_RESET]               = "Reset",
  [STR_GO_BACK]             = "Go back",
  [STR_OUT]                 = "out",
  [STR_DELETED]             = "deleted",

  [STR_RESET_DONE_TITLE]    = "Reset complete",
  [STR_RESET_DONE_BODY]     = "All games and statistics were deleted.",
  [STR_RESET_CONFIRM_TITLE] = "Reset everything?",
  [STR_RESET_CONFIRM_BODY]  = "Deletes every game and statistic on the watch and phone. This can't be undone.",

  [STR_GAME]                = "Game",
  [STR_MAIN_MENU]           = "Main menu",
  [STR_END_GAME]            = "End game",
  [STR_DISCARD_GAME]        = "Discard game",
  [STR_END_GAME_Q]          = "End game?",
  [STR_END_GAME_BODY]       = "Finish and show the results.",
  [STR_DISCARD_GAME_Q]      = "Discard game?",
  [STR_DISCARD_GAME_BODY]   = "End the game without saving results.",

  [STR_ROUND]               = "Round %d",
  [STR_HAVE_NEED]           = "have %d / need %d",
  [STR_MISS]                = "MISS",
  [STR_CONTINUE_PLAYING]    = "Continue playing",
  [STR_PLAY_TILL_END]       = "Play till end of round",
  [STR_GAME_OVER]           = "Game over",
  [STR_REACHED_50]          = "reached 50",

  [STR_ADD_PLAYER]          = "Add player",
  [STR_ARCHIVED_PLAYERS]    = "Archived players",
  [STR_NO_ARCHIVED]         = "No archived players",
  [STR_PLAYER_COUNT_ONE]    = "%d player",
  [STR_PLAYER_COUNT_MANY]   = "%d players",
  [STR_DELETE_PLAYER_Q]     = "Delete %s?",
  [STR_DELETE_PLAYER_BODY]  = "This player is removed for good.",
  [STR_NO_GAMES_YET]        = "No games yet",
  [STR_PLAY_TO_TRACK]       = "Play a game to track stats",
  [STR_GAMES]               = "Games",
  [STR_WIN_RATE]            = "Win rate",
  [STR_ACCURACY]            = "Accuracy",
  [STR_PTS_PER_TURN]        = "Pts / turn",
  [STR_STATS]               = "Stats",
  [STR_RENAME]              = "Rename",
  [STR_ARCHIVE]             = "Archive",
  [STR_UNARCHIVE]           = "Unarchive",

  [STR_SYNC_TO_ADD]         = "Sync to add games",
  [STR_HISTORY_FULL]        = "History buffer full",
  [STR_START_GAME]          = "Start game",
  [STR_START_GAME_N]        = "Start game (%d)",

  [STR_RESULTS]             = "Results",
  [STR_WINNER]              = "Winner: %s",
  [STR_DELETE_GAME_Q]       = "Delete game?",
  [STR_DELETE_GAME_BODY]    = "This removes it from history and statistics.",
  [STR_LOADING]             = "Loading…",
  [STR_SYNC_NEEDED]         = "Sync needed",
  [STR_SYNCING]             = "Syncing…",
  [STR_UNSAVED]             = "%d unsaved",
  [STR_FMT_HISTORY_DATE]    = "%b %d, %H:%M",

  [STR_HIGHEST_ACC]         = "Highest accuracy",
  [STR_LOWEST_ACC]          = "Lowest accuracy",
  [STR_AVG_PER_TURN]        = "Avg per turn",
  [STR_DURATION]            = "Duration",
  [STR_ACC_VALUE_ONE]       = "%s %d%% (%d miss)",
  [STR_ACC_VALUE_MANY]      = "%s %d%% (%d misses)",
  [STR_PTS_VALUE]           = "%d.%d pts",
  [STR_FMT_DECIMAL1]        = "%d.%d",
  [STR_DUR_LT_MIN]          = "< 1 min",
  [STR_DUR_MIN]             = "%d min",
  [STR_DUR_H_MIN]           = "%d h %d min",
  [STR_DUR_H]               = "%d h",

  [STR_HELP_OBJECTIVE_T]    = "Objective",
  [STR_HELP_OBJECTIVE_B]    = "Be the first to score exactly 50 points.",
  [STR_HELP_THROWING_T]     = "Throwing",
  [STR_HELP_THROWING_B]     = "Take turns throwing the mölkky underarm to knock pins down. Everyone "
                              "throws from the same line, 3,5 m away. Stand fallen pins back up where "
                              "they land.",
  [STR_HELP_SCORING_T]      = "Scoring",
  [STR_HELP_SCORING_B]      = "One pin down scores that pin's number. Two or more down score the number "
                              "of pins felled (not their values).",
  [STR_HELP_OVERSHOOT_T]    = "Overshoot & misses",
  [STR_HELP_OVERSHOOT_B]    = "Going over 50 drops you back to 25.\n\n"
                              "With \"Lose from 3 misses\" on, three misses in a row knocks you out.",
  [STR_HELP_WINNING_T]      = "Winning",
  [STR_HELP_WINNING_B]      = "The first player to hit exactly 50 wins. The round is finished so anyone "
                              "else who also reaches 50 in the same round shares the win.",
  [STR_HELP_PIN_SETUP]      = "Pin setup",
  [STR_HELP_PIN_CAPTION]    = "Set the 12 pins like this to start.",

  [STR_SEC_GAMEPLAY]        = "Gameplay",
  [STR_SEC_OTHER]           = "Other",
  [STR_LOSE_3]              = "Lose from 3 misses",
  [STR_FINISH_ROUND]        = "Finish round",
  [STR_TIES_SHARED]         = "Ties shared places",
  [STR_PAGE_HEADER]         = "Page header",
  [STR_TITLE_CLOCK]         = "Title and clock bar",
  [STR_PIN_ARTWORK]         = "Pin artwork",
  [STR_PIN_FORMATION]       = "The 12-pin formation",
  [STR_LANGUAGE]            = "Language",
};

// Finnish (suomi). ä = ä, ö = ö — Latin-1, covered by Pebble's system
// fonts, so no custom font is needed.
static const char *const FI[STR__COUNT] = {
  [STR_RESUME_GAME]         = "Jatka peliä",
  [STR_NEW_GAME]            = "Uusi peli",
  [STR_PLAYERS]             = "Pelaajat",
  [STR_HISTORY]             = "Historia",
  [STR_HELP]                = "Ohje",
  [STR_SETTINGS]            = "Asetukset",
  [STR_OK]                  = "OK",
  [STR_DELETE]              = "Poista",
  [STR_DISCARD]             = "Hylkää",
  [STR_RESET]               = "Nollaa",
  [STR_GO_BACK]             = "Takaisin",
  [STR_OUT]                 = "ulkona",
  [STR_DELETED]             = "poistettu",

  [STR_RESET_DONE_TITLE]    = "Nollaus valmis",
  [STR_RESET_DONE_BODY]     = "Kaikki pelit ja tilastot poistettiin.",
  [STR_RESET_CONFIRM_TITLE] = "Nollataanko kaikki?",
  [STR_RESET_CONFIRM_BODY]  = "Poistaa kaikki pelit ja tilastot kellosta ja puhelimesta. Tätä ei voi perua.",

  [STR_GAME]                = "Peli",
  [STR_MAIN_MENU]           = "Päävalikko",
  [STR_END_GAME]            = "Lopeta peli",
  [STR_DISCARD_GAME]        = "Hylkää peli",
  [STR_END_GAME_Q]          = "Lopetetaanko peli?",
  [STR_END_GAME_BODY]       = "Päätä peli ja näytä tulokset.",
  [STR_DISCARD_GAME_Q]      = "Hylätäänkö peli?",
  [STR_DISCARD_GAME_BODY]   = "Lopeta peli tallentamatta tuloksia.",

  [STR_ROUND]               = "Kierros %d",
  [STR_HAVE_NEED]           = "on %d / tarvitsee %d",
  [STR_MISS]                = "HUTI",
  [STR_CONTINUE_PLAYING]    = "Jatka pelaamista",
  [STR_PLAY_TILL_END]       = "Pelaa kierros loppuun",
  [STR_GAME_OVER]           = "Peli ohi",
  [STR_REACHED_50]          = "saavutti 50",

  [STR_ADD_PLAYER]          = "Lisää pelaaja",
  [STR_ARCHIVED_PLAYERS]    = "Arkistoidut pelaajat",
  [STR_NO_ARCHIVED]         = "Ei arkistoituja pelaajia",
  [STR_PLAYER_COUNT_ONE]    = "%d pelaaja",
  [STR_PLAYER_COUNT_MANY]   = "%d pelaajaa",
  [STR_DELETE_PLAYER_Q]     = "Poistetaanko %s?",
  [STR_DELETE_PLAYER_BODY]  = "Pelaaja poistetaan pysyvästi.",
  [STR_NO_GAMES_YET]        = "Ei vielä pelejä",
  [STR_PLAY_TO_TRACK]       = "Pelaa peli nähdäksesi tilastot",
  [STR_GAMES]               = "Pelit",
  [STR_WIN_RATE]            = "Voitto-%",
  [STR_ACCURACY]            = "Tarkkuus",
  [STR_PTS_PER_TURN]        = "Pisteet/vuoro",
  [STR_STATS]               = "Tilastot",
  [STR_RENAME]              = "Nimeä uudelleen",
  [STR_ARCHIVE]             = "Arkistoi",
  [STR_UNARCHIVE]           = "Palauta arkistosta",

  [STR_SYNC_TO_ADD]         = "Synkronoi lisätäksesi pelejä",
  [STR_HISTORY_FULL]        = "Historiapuskuri täynnä",
  [STR_START_GAME]          = "Aloita peli",
  [STR_START_GAME_N]        = "Aloita peli (%d)",

  [STR_RESULTS]             = "Tulokset",
  [STR_WINNER]              = "Voittaja: %s",
  [STR_DELETE_GAME_Q]       = "Poistetaanko peli?",
  [STR_DELETE_GAME_BODY]    = "Poistaa pelin historiasta ja tilastoista.",
  [STR_LOADING]             = "Ladataan…",
  [STR_SYNC_NEEDED]         = "Synkronointi tarvitaan",
  [STR_SYNCING]             = "Synkronoidaan…",
  [STR_UNSAVED]             = "%d tallentamatta",
  [STR_FMT_HISTORY_DATE]    = "%-d.%-m. klo %H:%M",

  [STR_HIGHEST_ACC]         = "Paras tarkkuus",
  [STR_LOWEST_ACC]          = "Heikoin tarkkuus",
  [STR_AVG_PER_TURN]        = "Ka./vuoro",
  [STR_DURATION]            = "Kesto",
  [STR_ACC_VALUE_ONE]       = "%s %d%% (%d huti)",
  [STR_ACC_VALUE_MANY]      = "%s %d%% (%d hutia)",
  [STR_PTS_VALUE]           = "%d,%d p",
  [STR_FMT_DECIMAL1]        = "%d,%d",
  [STR_DUR_LT_MIN]          = "< 1 min",
  [STR_DUR_MIN]             = "%d min",
  [STR_DUR_H_MIN]           = "%d h %d min",
  [STR_DUR_H]               = "%d h",

  [STR_HELP_OBJECTIVE_T]    = "Tavoite",
  [STR_HELP_OBJECTIVE_B]    = "Ole ensimmäinen, joka saa tasan 50 pistettä.",
  [STR_HELP_THROWING_T]     = "Heittäminen",
  [STR_HELP_THROWING_B]     = "Heittäkää vuorotellen mölkkyä alakautta kaataaksenne keiloja. "
                              "Kaikki heittävät samalta viivalta 3,5 m päästä. Nostakaa kaatuneet "
                              "keilat pystyyn siihen, mihin ne jäivät.",
  [STR_HELP_SCORING_T]      = "Pisteytys",
  [STR_HELP_SCORING_B]      = "Yksi kaatunut keila antaa keilan numeron verran pisteitä. Kaksi tai useampi "
                              "antaa kaadettujen keilojen lukumäärän (ei niiden numeroita).",
  [STR_HELP_OVERSHOOT_T]    = "Yliheitto ja hudit",
  [STR_HELP_OVERSHOOT_B]    = "Yli 50 menevä pudottaa takaisin 25:een.\n\n"
                              "Kun \"Putoaa 3 hudista\" on päällä, kolme hutia peräkkäin pudottaa pelistä.",
  [STR_HELP_WINNING_T]      = "Voittaminen",
  [STR_HELP_WINNING_B]      = "Ensimmäinen tasan 50 saavuttanut voittaa. Kierros pelataan loppuun, joten "
                              "jokainen, joka myös saavuttaa 50 samalla kierroksella, jakaa voiton.",
  [STR_HELP_PIN_SETUP]      = "Keilojen asettelu",
  [STR_HELP_PIN_CAPTION]    = "Aseta 12 keilaa näin ennen aloitusta.",

  [STR_SEC_GAMEPLAY]        = "Pelitapa",
  [STR_SEC_OTHER]           = "Muut",
  [STR_LOSE_3]              = "Putoaa 3 hudista",
  [STR_FINISH_ROUND]        = "Loppukierros",
  [STR_TIES_SHARED]         = "Tasapelit jakavat sijat",
  [STR_PAGE_HEADER]         = "Sivun otsake",
  [STR_TITLE_CLOCK]         = "Otsikko ja kellopalkki",
  [STR_PIN_ARTWORK]         = "Keilakuvitus",
  [STR_PIN_FORMATION]       = "12 keilan muodostelma",
  [STR_LANGUAGE]            = "Kieli",
};

// English abbreviated month names for the %b in the history date pattern. Finnish
// formats dates numerically (no %b), so it needs no month table — `months` NULL.
static const char *const EN_MONTHS[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

// The locale set. Index 0 (English) is the base: it backs any missing entry and
// is the default until a saved/seeded language is applied. Add a language by
// appending a row here and a column in the tables above.
static const Locale LOCALES[] = {
  { .autonym = "English", .sys_locale = "en_US", .strings = EN, .months = EN_MONTHS },
  { .autonym = "Suomi",   .sys_locale = NULL,    .strings = FI, .months = NULL      },
};

void mk_locale_init(void) {
  locale_init(LOCALES, (int)(sizeof LOCALES / sizeof LOCALES[0]), STR__COUNT, 0);
}
