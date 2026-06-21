#pragma once
#include <pebble.h>

// =============================================================================
// Mölkky — data model, game logic, and persistence.
// UI lives in the *_screen modules; this file has no windows.
// =============================================================================

#define MK_MAX_PLAYERS 16
#define MK_MAX_HIST_PLAYERS 14   // a history record keeps at most this many results: with
                                 // the per-player stats it's 8 B/result, and 8 B header +
                                 // 14*8 = 120 B keeps MKHistGame a 4-byte multiple under the
                                 // storage lib's 128-byte record ceiling (STORAGE_REC_MAX).
                                 // A 15-16 player game drops its worst-placed finishers.
#define MK_MAX_NAME    16     // bytes incl. NUL (~15 chars)
#define MK_MAX_HISTORY 20     // games kept in the on-watch cache (the full archive lives
                              // on the phone). Each slot is ~124 B of persist (120 B record
                              // + 4 B seq), so 20 ≈ 2.5 KB, leaving room for the roster + an
                              // active game in the ~4 KB budget. Also the most un-backed-up
                              // games that can pile up before the store BLOCKS (see storage.h).
#define MK_WIN         50

// Mölkky's brand accent (a sea-green). Shared by the keyboard's app theme and
// the ui lib's selection/checkbox accent. On the 64-color display it resolves
// to a deep teal-green (~#005555).
#define MK_ACCENT GColorFromRGB(56, 126, 88)

typedef struct {
  uint8_t  score;
  uint8_t  misses;        // current consecutive-miss streak (resets on a hit)
  bool     out;           // eliminated (3 misses)
  bool     retired;       // reached 50 — has a placement
  uint8_t  place;         // 1-based, set when retired/finished
  uint8_t  id;            // roster id; the name resolves via mk_game_player_name()
  uint16_t total_misses;  // cumulative misses this game (for end-of-game stats)
  uint16_t throws;        // turns taken (for the per-turn average)
  uint16_t points;        // cumulative pins knocked, miss = 0 (for the average)
} MKGamePlayer;

typedef struct {
  uint8_t      count;
  int32_t      start_time;  // unix time of the first throw; end - this = game duration.
                            // Persisted in GameHdr so it survives a mid-game restart
                            // (e.g. a game continued the next day).
  MKGamePlayer players[MK_MAX_PLAYERS];
  uint8_t      current;
  // Shared-crown bookkeeping (the "final round" rule). A *tie group* is the set
  // of players who reach 50 within one round; with the setting on they share a
  // place. group_base is how many players had already retired when the open
  // group started, so every member's place is group_base + 1.
  uint8_t      group_base;
  bool         finishing;   // a tie group is open: auto-play the round while a tie is still possible
  bool         playout;     // "play till end of the round": finish the round, then end the game
  bool         finished;
} MKGame;

// MKResult.flags bits, and MKHistGame.settings bits — the rules a game was
// played under (settings are global and can change, so they're stored per game).
#define MK_RES_OUT   0x1   // eliminated (3 misses)
#define MK_RES_WON   0x2   // reached 50
#define MK_SET_LOSE3 0x1   // "Lose from 3 misses" was on
#define MK_SET_FINAL 0x2   // "Final round" (shared crowns) was on

typedef struct {
  uint8_t  id;        // roster player id; resolve to a name via mk_name_by_id().
                      // A deleted player no longer resolves → shown as "deleted".
  uint8_t  place;
  uint8_t  score;     // final score (capped at 50; an overshoot drops to 25)
  uint8_t  flags;     // MK_RES_OUT | MK_RES_WON
  uint8_t  misses;    // total misses this game (saturates at 255)
  uint8_t  throws;    // turns taken           (saturates at 255)
  uint16_t points;    // raw pins knocked over the game (a miss is 0)
} MKResult;           // 8 B

typedef struct {
  int32_t  date;      // unix time the game ended
  uint16_t duration;  // game length in minutes (start = date - duration*60). Saturates
                      // to 0 on a backwards clock; wraps past ~45 days (accepted).
  uint8_t  count;
  uint8_t  settings;  // MK_SET_LOSE3 | MK_SET_FINAL
  MKResult results[MK_MAX_HIST_PLAYERS];   // sorted by place
} MKHistGame;

typedef enum { MK_THROW_NORMAL, MK_THROW_WIN, MK_THROW_GAMEOVER } MKThrowResult;

void mk_init(void);

// ---- roster (active players) ----
int         mk_roster_count(void);
const char *mk_roster_name(int i);
bool        mk_roster_add(const char *name);     // false if empty/full
void        mk_roster_rename(int i, const char *name);
void        mk_roster_delete(int i);             // gone for good; old boards show "deleted"
void        mk_roster_archive(int i);            // hide from the roster, keep the name

// ---- archived players ----
// Archived players keep their id, so their name still resolves in old scoreboards
// and they can be brought back. They just don't appear in the roster / picker.
int         mk_roster_archived_count(void);
const char *mk_roster_archived_name(int j);
void        mk_roster_unarchive(int j);
void        mk_roster_archived_delete(int j);

// Resolve a stored history id to its current name (active or archived). Returns
// NULL when no such player exists any more (deleted).
const char *mk_name_by_id(uint8_t id);

// ---- settings ----
bool mk_lose_on_3(void);
void mk_set_lose_on_3(bool v);
// "Final round": when on, players who reach 50 in the same round share a crown
// and equal-score non-finishers share a place (1,1,3,3,5). When off, places are
// strictly ordered by who crowned first (1,2,3,4,5).
bool mk_final_round(void);
void mk_set_final_round(bool v);

// ---- game ----
bool          mk_game_active(void);              // a game exists (for Continue)
MKGame       *mk_game(void);
void          mk_game_start(const bool *selected);   // selected[] over roster
MKGamePlayer *mk_game_current(void);
const char   *mk_game_player_name(const MKGamePlayer *p);  // roster name, "?" if gone
int           mk_game_active_count(void);        // not out, not retired
MKThrowResult mk_game_throw(int value);          // 1..12, or 0 = miss
bool          mk_game_continue(void);            // advance to next active; false if none
bool          mk_game_round_has_remaining(void); // an active player still owes a throw this round
void          mk_game_play_out(void);            // play the rest of the round, then end (see MKGame.playout)
void          mk_game_end(void);                 // finalize, save to history, clear

// One-level undo of the most recent throw. The snapshot is taken inside
// mk_game_throw and lives in RAM only (lost if the app is killed). Restoring it
// reverts every coupled stat at once — score, misses, points, throws, placement.
bool          mk_game_can_undo(void);
bool          mk_game_undo(void);                // restore pre-throw state; false if nothing to undo

// ---- history (backed by the generic synced storage lib) ----
// The watch keeps the newest MK_MAX_HISTORY games as an offline-browsable cache;
// the full archive lives on the phone (PebbleKit JS + localStorage). These two
// accessors read the on-watch cache (0 = newest).
int               mk_hist_count(void);
const MKHistGame *mk_hist_get(int i);

// Backup / sync status of the on-watch games.
typedef enum {
  MK_SYNC_OK,        // every game is backed up to the phone
  MK_SYNC_PENDING,   // some games aren't backed up yet (will push when the phone is near)
  MK_SYNC_BLOCKED,   // the on-watch buffer is full of un-backed-up games
} MKSyncState;
MKSyncState mk_hist_sync_state(void);
int         mk_hist_unsynced(void);      // games not yet backed up to the phone
int         mk_hist_total(void);         // best-known total games in the archive (0 if unknown)
bool        mk_hist_connected(void);     // phone reachable right now
void        mk_hist_sync_now(void);      // best-effort push (no-op if nothing/unreachable)
bool        mk_hist_can_record(void);    // false when BLOCKED — refuse to start a new game

// Paging the phone archive (sync-then-fetch). A history view registers a
// listener, then calls mk_hist_load_page(); the page (and every sync-state
// change) arrives via the listener on the main loop. `offset` is the global
// index of the page's first game (0 = newest); `total` is the archive size.
typedef struct {
  void (*on_page)(void *ctx, const MKHistGame *games, int count, int offset, int total);
  void (*on_state)(void *ctx, MKSyncState state, int unsynced, int total);
  void *ctx;
} MKHistListener;
void mk_hist_set_listener(const MKHistListener *l);   // NULL to clear
bool mk_hist_load_page(int page, int page_size);      // false if unreachable / request in flight
