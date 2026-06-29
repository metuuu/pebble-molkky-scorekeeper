#include "molkky.h"
#include "c/lib/storage/storage.h"
#include "c/lib/ui/menu.h"

// The store keeps one record per persist slot; keep MKHistGame within the lib's
// per-record ceiling and 4-byte aligned (so cache slots stay aligned — see storage.c).
_Static_assert(sizeof(MKHistGame) <= STORAGE_REC_MAX, "MKHistGame exceeds STORAGE_REC_MAX");
_Static_assert(sizeof(MKHistGame) % 4 == 0,           "MKHistGame must be 4-byte aligned");

// Caller-owned backing memory for the synced history store (RAM cache + send/page
// scratch). The storage lib carries no fixed cache; we hand it a buffer sized to
// exactly the window this app keeps. Heap-allocated (not a static array) so its
// ~4.6KB stays out of the app's static RAM image — the binary's virtual-size field
// is a 16-bit count and the whole static+code footprint must fit under 64KB.
// malloc guarantees 8-byte alignment, satisfying the lib's word-aligned readback.
#define MK_STORE_ARENA_BYTES STORAGE_ARENA_BYTES(sizeof(MKHistGame), MK_MAX_HISTORY, MK_HIST_PAGE)
static uint8_t *s_store_arena;   // malloc'd in mk_init, lives for the app's lifetime

// Persist keys — start at 100 to avoid the keyboard's settings keys (1-9).
#define PK_ROSTER     100     // roster header + first ROSTER_K1 entries
#define PK_SETTINGS   101
#define PK_GAME_HDR   102
#define PK_GAME_PLRS  103
#define PK_HIST_CNT   104     // legacy: pre-v4 history count (migrated into the store)
#define PK_SCHEMA     105     // persisted-format version (see migrate_persist)
#define PK_ROSTER2    106     // roster entries ROSTER_K1 .. MK_MAX_PLAYERS-1
#define PK_FINAL_RND  107     // "final round" (shared crowns) setting
#define PK_SHOW_HDR   136     // "show header" (menu title + clock bar) setting
#define PK_STATS      108     // lifetime stats, slots 0 .. ROSTER_K1-1 (parallel to roster)
#define PK_STATS2     109     // lifetime stats, slots ROSTER_K1 .. MK_MAX_PLAYERS-1
#define PK_HIST_0     110     // legacy: pre-v4 history games at 110 .. 110+24
#define MK_HIST_LEGACY_SLOTS 25   // pre-v4 history held up to 25 games (PK_HIST_0 .. +24)
#define PK_STORE_BASE 150     // synced history store: header at 150, slots 151 .. 151+MK_MAX_HISTORY-1

// The roster is split across two persist values: at 16 players a single blob
// (2 + 16*18 = 290 B) would exceed the 256-byte-per-value limit. PK_ROSTER holds
// the header + first 8 entries (146 B); PK_ROSTER2 holds the rest (144 B).
#define ROSTER_K1     8

// Bump when any persisted blob's layout changes; mk_init migrates older data.
// v1: roster gained stable ids + an archived flag; history stores ids, not names.
// v2: MKGamePlayer dropped its name field (names resolve from the roster by id).
// v3: 16-player cap split the roster across PK_ROSTER + PK_ROSTER2.
// v4: history moved into the generic synced storage lib (phone-backed archive);
//     the old PK_HIST_* games are imported into the store on first run (v3 only —
//     earlier history layouts were already discarded by migrate_persist).
// v5: MKGame/GameHdr gained the "final round" shared-crown fields; an in-progress
//     game saved by an older layout is discarded (history is untouched).
// v6: history records gained per-player stats (misses/throws/points), the rules
//     used, and a duration; GameHdr gained the game start time. The wider history
//     record changes the store's layout, so the on-watch cache resets (the phone
//     archive is intentionally not migrated — old records are abandoned); the
//     in-progress game is discarded for the new GameHdr.
// v7: history records store the absolute start time instead of a minute duration
//     (duration is derived on display); the watch now keeps per-player lifetime
//     stats (PK_STATS/PK_STATS2). The record layout changed again, so the cache
//     resets and the phone archive is abandoned (unreleased app — no migration).
// v8: MKGamePlayer gained out_order (eliminated players are ranked by drop order);
//     the wider in-progress player blob is discarded. History records (MKResult)
//     are unaffected, so the store is untouched.
#define MK_SCHEMA     8

// A roster entry. `id` is stable for the player's lifetime and never reused, so
// history can reference a player by id and still show the right (current) name —
// or "deleted" once the player is gone. `archived` hides the player from the
// roster and picker while keeping the id (and thus the name) resolvable.
typedef struct {
  char    name[MK_MAX_NAME];
  uint8_t id;
  bool    archived;
} MKRosterEntry;

// PK_ROSTER payload: header + the first ROSTER_K1 entries. The remaining entries
// live in PK_ROSTER2 as a bare MKRosterEntry[] (see roster_save / mk_init).
typedef struct {
  uint8_t       count;        // active + archived
  uint8_t       next_id;      // monotonic; ids are never reused
  MKRosterEntry entries[ROSTER_K1];
} RosterHead;

// Legacy single-key roster layout (schema < 3, 12-player cap). Kept only so
// migrate_persist can read it and re-write the split layout. MKRosterEntry's own
// layout is unchanged, so the old 12-entry blob still maps cleanly.
typedef struct {
  uint8_t       count, next_id;
  MKRosterEntry entries[12];
} RosterBlobV2;
typedef struct { uint8_t count, current, group_base; bool finishing, playout, finished;
                 int32_t start; } GameHdr;

static MKRosterEntry s_roster[MK_MAX_PLAYERS];
static MKLifetime    s_lifetime[MK_MAX_PLAYERS];  // parallel to s_roster, by slot
static int           s_roster_count;   // active + archived (total entries)
static uint8_t       s_next_id = 1;    // 0 is reserved as "no player"
static bool       s_lose_on_3 = true;
static bool       s_final_round = true;
static bool       s_show_header = false;
static MKGame     s_game;
static bool       s_game_active;
static MKGame     s_undo;          // pre-throw snapshot for one-level undo (RAM only)
static bool       s_can_undo;

// History is owned by the generic storage lib (see storage.h). molkky is the
// domain adapter: it builds MKHistGame records, and forwards the lib's async
// page / sync-state callbacks to whichever history view is listening.
static MKHistListener s_hist_listener;
// Set by the app (main.c) — invoked when the phone asks to wipe everything, so a
// confirmation UI can be shown from wherever the user currently is. molkky stays
// window-free; it only forwards the request.
static void (*s_reset_request_cb)(void);

// The roster + lifetime stats are mirrored to the phone (for the settings page and
// for backup) as the storage lib's "aux blob". molkky owns the byte layout; the lib
// and the phone treat it opaquely. push_players() (re)serializes and queues a sync;
// players_apply() restores it from a phone import. See player_blob_* below.
static void push_players(void);
static void players_apply(const uint8_t *data, uint16_t len);
static bool s_players_ready;        // mirror only after the store is open (set in mk_init)
static bool s_suppress_aux_push;    // true while applying an inbound blob (don't echo it back)
static void store_on_page(void *ctx, const void *recs, const uint32_t *seqs, uint8_t count, uint32_t offset, uint32_t total) {
  if (s_hist_listener.on_page)
    s_hist_listener.on_page(s_hist_listener.ctx, (const MKHistGame *)recs, seqs, count, (int)offset, (int)total);
}
static void store_on_state(void *ctx, StorageSyncState st, uint16_t unsynced, uint32_t total) {
  if (!s_hist_listener.on_state) return;
  MKSyncState ms = st == STORAGE_SYNCED  ? MK_SYNC_OK
                 : st == STORAGE_BLOCKED ? MK_SYNC_BLOCKED
                                         : MK_SYNC_PENDING;
  s_hist_listener.on_state(s_hist_listener.ctx, ms, unsynced, (int)total);
}
// The phone's settings page asked to wipe everything. Forward it to the app's
// handler (set via mk_on_reset_request) so it can confirm with the user; the actual
// wipe runs in mk_reset_all() only if they accept.
static void store_on_reset_request(void *ctx) {
  if (s_reset_request_cb) s_reset_request_cb();
}
// The phone sent the player blob back (an import restored it) — apply it.
static void store_on_aux(void *ctx, const uint8_t *data, uint16_t len) {
  players_apply(data, len);
}

// Tiny xorshift PRNG (avoids depending on libc rand()).
static uint32_t s_rng = 1;
static uint32_t mk_rand(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng;
}

// ---- roster index helpers ----
// The public roster/archived lists are *views* over s_roster filtered by the
// archived flag; these map a list index to its physical slot.
static int active_slot(int i) {
  int n = 0;
  for (int s = 0; s < s_roster_count; s++)
    if (!s_roster[s].archived) { if (n == i) return s; n++; }
  return -1;
}
static int archived_slot(int j) {
  int n = 0;
  for (int s = 0; s < s_roster_count; s++)
    if (s_roster[s].archived) { if (n == j) return s; n++; }
  return -1;
}
static uint8_t roster_new_id(void) {
  uint8_t id = s_next_id++;
  if (s_next_id == 0) s_next_id = 1;   // skip the 0 sentinel on the (unreachable) wrap
  return id;
}
static void roster_remove_slot(int s) {
  for (int k = s; k < s_roster_count - 1; k++) {
    s_roster[k]   = s_roster[k + 1];
    s_lifetime[k] = s_lifetime[k + 1];   // lifetime stats are parallel by slot
  }
  s_roster_count--;
}
static int slot_by_id(uint8_t id) {
  if (id == 0) return -1;
  for (int s = 0; s < s_roster_count; s++) if (s_roster[s].id == id) return s;
  return -1;
}

// ---- persistence helpers ----
static void roster_save(void) {
  RosterHead h;
  h.count = s_roster_count;
  h.next_id = s_next_id;
  memcpy(h.entries, s_roster, sizeof(h.entries));           // entries 0 .. ROSTER_K1-1
  persist_write_data(PK_ROSTER, &h, sizeof(h));
  persist_write_data(PK_ROSTER2, s_roster + ROSTER_K1,      // entries ROSTER_K1 ..
                     sizeof(MKRosterEntry) * (MK_MAX_PLAYERS - ROSTER_K1));
  push_players();                                            // mirror the change to the phone
}
static void stats_save(void) {
  // Split like the roster: 16 * 18 B = 288 B would exceed the 256-byte per-value
  // limit, so the first ROSTER_K1 entries go in PK_STATS, the rest in PK_STATS2.
  persist_write_data(PK_STATS,  s_lifetime,
                     sizeof(MKLifetime) * ROSTER_K1);
  persist_write_data(PK_STATS2, s_lifetime + ROSTER_K1,
                     sizeof(MKLifetime) * (MK_MAX_PLAYERS - ROSTER_K1));
  push_players();                                            // stats ride the same blob
}

// ---- player blob (roster + lifetime stats, mirrored to the phone) ----
// One self-describing little-endian blob, so the watch can back up / restore the
// players and the phone can show real names instead of #id. Per player (PB_ENTRY
// bytes): id u8, archived u8, name[MK_MAX_NAME], then the MKLifetime fields —
// games u16, wins u16, throws u32, misses u32, points u32, place_sum u16. Header:
// version u8, count u8, next_id u8, pad u8. Keep this in lockstep with the phone's
// decoder in molkky_history.js.
#define PB_VER   1
#define PB_ENTRY (2 + MK_MAX_NAME + (2 + 2 + 4 + 4 + 4 + 2))
static uint8_t s_player_blob[4 + MK_MAX_PLAYERS * PB_ENTRY];

static void pb_w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void pb_w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint16_t pb_r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t pb_r32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t players_serialize(void) {
  int n = s_roster_count > MK_MAX_PLAYERS ? MK_MAX_PLAYERS : s_roster_count;
  uint8_t *b = s_player_blob;
  b[0] = PB_VER; b[1] = (uint8_t)n; b[2] = s_next_id; b[3] = 0;
  uint16_t o = 4;
  for (int i = 0; i < n; i++) {
    const MKRosterEntry *e = &s_roster[i];
    const MKLifetime   *L = &s_lifetime[i];
    b[o++] = e->id;
    b[o++] = e->archived ? 1 : 0;
    memcpy(b + o, e->name, MK_MAX_NAME); o += MK_MAX_NAME;
    pb_w16(b + o, L->games);     o += 2;
    pb_w16(b + o, L->wins);      o += 2;
    pb_w32(b + o, L->throws);    o += 4;
    pb_w32(b + o, L->misses);    o += 4;
    pb_w32(b + o, L->points);    o += 4;
    pb_w16(b + o, L->place_sum); o += 2;
  }
  return o;
}

static void push_players(void) {
  if (!s_players_ready || s_suppress_aux_push) return;   // not before open / not while applying
  storage_set_aux(s_player_blob, players_serialize());
}

// Replace the whole roster + stats from a phone-restored blob. Full replace only
// (the import is replace-only), keyed by the ids the games already reference.
static void players_apply(const uint8_t *b, uint16_t len) {
  if (!b || len < 4 || b[0] != PB_VER) return;
  int n = b[1] > MK_MAX_PLAYERS ? MK_MAX_PLAYERS : b[1];
  if ((uint16_t)(4 + n * PB_ENTRY) > len) n = (len - 4) / PB_ENTRY;   // truncated → keep what fits
  memset(s_lifetime, 0, sizeof(s_lifetime));
  uint16_t o = 4;
  for (int i = 0; i < n; i++) {
    MKRosterEntry *e = &s_roster[i];
    MKLifetime   *L = &s_lifetime[i];
    e->id = b[o++];
    e->archived = b[o++] != 0;
    memcpy(e->name, b + o, MK_MAX_NAME); e->name[MK_MAX_NAME - 1] = '\0'; o += MK_MAX_NAME;
    L->games     = pb_r16(b + o); o += 2;
    L->wins      = pb_r16(b + o); o += 2;
    L->throws    = pb_r32(b + o); o += 4;
    L->misses    = pb_r32(b + o); o += 4;
    L->points    = pb_r32(b + o); o += 4;
    L->place_sum = pb_r16(b + o); o += 2;
  }
  s_roster_count = n;
  s_next_id = b[2] ? b[2] : 1;
  for (int i = 0; i < n; i++)                              // keep ids monotonic past the restored set
    if (s_roster[i].id >= s_next_id) s_next_id = (uint8_t)(s_roster[i].id + 1);
  if (s_next_id == 0) s_next_id = 1;
  s_suppress_aux_push = true;                              // persist without echoing the blob back
  roster_save();
  stats_save();
  s_suppress_aux_push = false;
}

static void game_save(void) {
  if (!s_game_active) return;
  GameHdr h = { s_game.count, s_game.current, s_game.group_base,
                s_game.finishing, s_game.playout, s_game.finished, s_game.start_time };
  persist_write_data(PK_GAME_HDR, &h, sizeof(h));
  persist_write_data(PK_GAME_PLRS, s_game.players, s_game.count * sizeof(MKGamePlayer));
}
static void game_clear_persist(void) {
  persist_delete(PK_GAME_HDR);
  persist_delete(PK_GAME_PLRS);
}

// Upgrade persisted data written by an older schema. v0 (pre-ids) → v1: keep the
// roster names but assign fresh ids and clear the archived flag; the in-progress
// game and history used incompatible layouts (and names can't be mapped to ids),
// so both are reset. Runs once, gated by PK_SCHEMA.
static void migrate_persist(int from) {
  if (from < 1) {
    if (persist_exists(PK_ROSTER)) {
      // v0 stored 12 bare names; dimensions are pinned to the old cap, not the
      // current MK_MAX_PLAYERS, so the size check still matches the old blob.
      struct { uint8_t count; char names[12][MK_MAX_NAME]; } old;
      if (persist_read_data(PK_ROSTER, &old, sizeof(old)) == (int)sizeof(old)) {
        RosterBlobV2 rb; rb.count = 0; rb.next_id = 1;
        memset(rb.entries, 0, sizeof(rb.entries));
        int c = old.count > 12 ? 12 : old.count;
        for (int i = 0; i < c; i++) {
          strncpy(rb.entries[i].name, old.names[i], MK_MAX_NAME - 1);
          rb.entries[i].name[MK_MAX_NAME - 1] = '\0';
          rb.entries[i].id = rb.next_id++;
          rb.entries[i].archived = false;
          rb.count++;
        }
        persist_write_data(PK_ROSTER, &rb, sizeof(rb));   // still single-key here; from<3 splits it
      }
    }
    persist_delete(PK_HIST_CNT);
    for (int i = 0; i < MK_HIST_LEGACY_SLOTS; i++) persist_delete(PK_HIST_0 + i);
    persist_delete(PK_GAME_HDR);
    persist_delete(PK_GAME_PLRS);
  }
  if (from < 2) {
    // A v1 saved game stored the larger (named) MKGamePlayer; discard it so the
    // new, smaller layout is never read over the old bytes.
    persist_delete(PK_GAME_HDR);
    persist_delete(PK_GAME_PLRS);
  }
  if (from < 3) {
    // Roster moved from one persist value to two. Re-read the old single-key blob
    // (≤12 entries) and re-write it split across PK_ROSTER + PK_ROSTER2.
    if (persist_exists(PK_ROSTER)) {
      RosterBlobV2 old;
      if (persist_read_data(PK_ROSTER, &old, sizeof(old)) == (int)sizeof(old)) {
        RosterHead h;
        h.count = old.count;
        h.next_id = old.next_id ? old.next_id : 1;
        memcpy(h.entries, old.entries, sizeof(h.entries));            // first ROSTER_K1
        persist_write_data(PK_ROSTER, &h, sizeof(h));
        persist_write_data(PK_ROSTER2, old.entries + ROSTER_K1,       // remaining (12 - ROSTER_K1)
                           sizeof(MKRosterEntry) * (12 - ROSTER_K1));
      }
    }
  }
  if (from < 5) {
    // GameHdr grew the final-round fields; an older in-progress game can't be
    // reinterpreted, so drop it (history lives in the store and is untouched).
    persist_delete(PK_GAME_HDR);
    persist_delete(PK_GAME_PLRS);
  }
  if (from < 6) {
    // GameHdr grew the start time; drop any in-progress game saved without it.
    // The history store layout also changed — the lib resets its on-watch cache
    // on the size mismatch, and the phone archive is intentionally abandoned.
    persist_delete(PK_GAME_HDR);
    persist_delete(PK_GAME_PLRS);
  }
  if (from < 7) {
    // The history record layout changed (start time replaces duration), so the
    // store resets its cache on the size mismatch; the in-progress game is
    // unaffected (GameHdr is unchanged) but lifetime stats start empty. Nothing
    // to migrate — PK_STATS/PK_STATS2 simply don't exist yet and read as zero.
  }
  if (from < 8) {
    // MKGamePlayer grew out_order; the old player blob can't be reread at the new
    // size, so discard the in-progress game (history is unaffected).
    persist_delete(PK_GAME_HDR);
    persist_delete(PK_GAME_PLRS);
  }
}

// Move pre-v4 on-watch history (PK_HIST_CNT + PK_HIST_0..) into the storage lib.
// Only v3 records share the current MKHistGame layout, so each read is size-
// checked; mismatched (older) layouts are dropped rather than misread.
//
// The cache holds at most MK_MAX_HISTORY, so keep the NEWEST that many (the old
// array is newest-first → indices 0 .. keep-1) and append them oldest-first so
// seqs increase with age — appending exactly the cache size never trips BLOCKED.
// Any games beyond the cache are dropped: this is the first v4 run, the phone
// archive is empty, so they can't be offloaded, and the newest are what to keep.
// Imported games start unsynced and push to the phone on the next connection.
static void hist_import_legacy(void) {
  if (!persist_exists(PK_HIST_CNT)) return;
  int n = persist_read_int(PK_HIST_CNT);
  if (n > MK_HIST_LEGACY_SLOTS) n = MK_HIST_LEGACY_SLOTS;
  int keep = n < MK_MAX_HISTORY ? n : MK_MAX_HISTORY;
  for (int i = keep - 1; i >= 0; i--) {                    // newest `keep`, oldest-first
    MKHistGame hg;
    if (persist_exists(PK_HIST_0 + i) &&
        persist_read_data(PK_HIST_0 + i, &hg, sizeof(hg)) == (int)sizeof(hg))
      storage_append(&hg);
  }
  for (int i = 0; i < MK_HIST_LEGACY_SLOTS; i++) persist_delete(PK_HIST_0 + i);
  persist_delete(PK_HIST_CNT);
}

void mk_init(void) {
  s_rng = (uint32_t)time(NULL) ^ 0x9e3779b9u; if (!s_rng) s_rng = 1;

  int schema = persist_exists(PK_SCHEMA) ? persist_read_int(PK_SCHEMA) : 0;
  if (schema < MK_SCHEMA) {
    migrate_persist(schema);
    persist_write_int(PK_SCHEMA, MK_SCHEMA);
  }

  if (persist_exists(PK_ROSTER)) {
    RosterHead h; persist_read_data(PK_ROSTER, &h, sizeof(h));
    s_roster_count = h.count > MK_MAX_PLAYERS ? MK_MAX_PLAYERS : h.count;
    s_next_id = h.next_id ? h.next_id : 1;
    memcpy(s_roster, h.entries, sizeof(h.entries));            // entries 0 .. ROSTER_K1-1
    if (persist_exists(PK_ROSTER2))                            // entries ROSTER_K1 ..
      persist_read_data(PK_ROSTER2, s_roster + ROSTER_K1,
                        sizeof(MKRosterEntry) * (MK_MAX_PLAYERS - ROSTER_K1));
  } else {
    s_roster_count = 0;
    s_next_id = 1;
  }

  // Lifetime stats, parallel to the roster. Absent keys (fresh install / older
  // schema) leave the all-zero default, so every player starts with no games.
  memset(s_lifetime, 0, sizeof(s_lifetime));
  if (persist_exists(PK_STATS))
    persist_read_data(PK_STATS,  s_lifetime,
                      sizeof(MKLifetime) * ROSTER_K1);
  if (persist_exists(PK_STATS2))
    persist_read_data(PK_STATS2, s_lifetime + ROSTER_K1,
                      sizeof(MKLifetime) * (MK_MAX_PLAYERS - ROSTER_K1));

  s_lose_on_3 = persist_exists(PK_SETTINGS) ? persist_read_bool(PK_SETTINGS) : true;
  s_final_round = persist_exists(PK_FINAL_RND) ? persist_read_bool(PK_FINAL_RND) : true;
  s_show_header = persist_exists(PK_SHOW_HDR) ? persist_read_bool(PK_SHOW_HDR) : false;
  menu_set_header_enabled(s_show_header);

  // Open the synced history store. The watch keeps the newest MK_MAX_HISTORY
  // games as an offline cache; the phone holds the full archive. The arena backs
  // the store for the whole app lifetime (Pebble reclaims app heap on exit, so we
  // never free it). This ~4.6KB alloc has the full app heap (~65KB) behind it, so
  // it won't realistically fail; only open the store if it succeeded.
  s_store_arena = malloc(MK_STORE_ARENA_BYTES);
  if (s_store_arena)
  storage_open(&(StorageConfig){
    .record_size    = sizeof(MKHistGame),
    .cache_capacity = MK_MAX_HISTORY,
    .max_page       = MK_HIST_PAGE,   // pages/batches are MK_HIST_PAGE — scratch sized to match
    .schema         = MK_SCHEMA,
    .base_key       = PK_STORE_BASE,
    .arena          = s_store_arena,
    .arena_size     = MK_STORE_ARENA_BYTES,
    .on_page          = store_on_page,
    .on_state         = store_on_state,
    .on_reset_request = store_on_reset_request,
    .on_aux           = store_on_aux,
  });
  if (schema < 4) hist_import_legacy();   // one-time: move pre-v4 on-watch games into the store

  if (persist_exists(PK_GAME_HDR)) {
    GameHdr h; persist_read_data(PK_GAME_HDR, &h, sizeof(h));
    s_game.count = h.count; s_game.current = h.current;
    s_game.group_base = h.group_base; s_game.finishing = h.finishing;
    s_game.playout = h.playout; s_game.finished = h.finished;
    s_game.start_time = h.start;
    persist_read_data(PK_GAME_PLRS, s_game.players, h.count * sizeof(MKGamePlayer));
    s_game_active = true;
  } else {
    s_game_active = false;
  }

  // The store is open and the roster + stats are loaded: start mirroring them to the
  // phone (it pushes on the next connection, so a backup is current and the settings
  // page can show names).
  s_players_ready = true;
  push_players();
}

// ---- roster (active players) ----
int mk_roster_count(void) {
  int n = 0;
  for (int s = 0; s < s_roster_count; s++) if (!s_roster[s].archived) n++;
  return n;
}
const char *mk_roster_name(int i) {
  int s = active_slot(i);
  return s >= 0 ? s_roster[s].name : "";
}
bool mk_roster_add(const char *name) {
  if (!name || !name[0] || s_roster_count >= MK_MAX_PLAYERS) return false;
  int slot = s_roster_count++;
  MKRosterEntry *e = &s_roster[slot];
  strncpy(e->name, name, MK_MAX_NAME - 1);
  e->name[MK_MAX_NAME - 1] = '\0';
  e->id = roster_new_id();
  e->archived = false;
  memset(&s_lifetime[slot], 0, sizeof(MKLifetime));   // a new player starts at zero
  roster_save(); stats_save(); return true;
}
void mk_roster_rename(int i, const char *name) {
  int s = active_slot(i);
  if (s < 0 || !name || !name[0]) return;
  strncpy(s_roster[s].name, name, MK_MAX_NAME - 1);
  s_roster[s].name[MK_MAX_NAME - 1] = '\0'; roster_save();
}
void mk_roster_delete(int i) {
  int s = active_slot(i);
  if (s < 0) return;
  roster_remove_slot(s); roster_save(); stats_save();   // drops the player's lifetime stats
}
void mk_roster_archive(int i) {
  int s = active_slot(i);
  if (s < 0) return;
  s_roster[s].archived = true; roster_save();
}

// ---- archived players ----
int mk_roster_archived_count(void) {
  int n = 0;
  for (int s = 0; s < s_roster_count; s++) if (s_roster[s].archived) n++;
  return n;
}
const char *mk_roster_archived_name(int j) {
  int s = archived_slot(j);
  return s >= 0 ? s_roster[s].name : "";
}
void mk_roster_unarchive(int j) {
  int s = archived_slot(j);
  if (s < 0) return;
  s_roster[s].archived = false; roster_save();
}
void mk_roster_archived_delete(int j) {
  int s = archived_slot(j);
  if (s < 0) return;
  roster_remove_slot(s); roster_save(); stats_save();   // drops the player's lifetime stats
}

const char *mk_name_by_id(uint8_t id) {
  if (id == 0) return NULL;
  for (int s = 0; s < s_roster_count; s++)
    if (s_roster[s].id == id) return s_roster[s].name;
  return NULL;
}

// ---- lifetime stats ----
int         mk_stats_count(void)      { return mk_roster_count(); }   // active players
const char *mk_stats_name(int i)      { return mk_roster_name(i); }
const MKLifetime *mk_stats_get(int i) {
  int s = active_slot(i);
  return s >= 0 ? &s_lifetime[s] : NULL;
}

// Fold a finished game's per-player figures into the lifetime totals. Counts
// every player (not just the MK_MAX_HIST_PLAYERS that fit a history record), and
// uses the wide in-game counters before they saturate into the narrow record.
// A player deleted mid-game (no roster slot) is skipped. Saturates rather than
// wraps so a very long-lived total never rolls over.
static void stats_record_game(const MKGame *g) {
  for (int i = 0; i < g->count; i++) {
    const MKGamePlayer *p = &g->players[i];
    int s = slot_by_id(p->id);
    if (s < 0) continue;
    MKLifetime *L = &s_lifetime[s];
    if (L->games < 0xFFFF) L->games++;
    if (p->place == 1 && L->wins < 0xFFFF) L->wins++;
    if (L->throws <= 0xFFFFFFFFu - p->throws)       L->throws += p->throws;       else L->throws = 0xFFFFFFFFu;
    if (L->misses <= 0xFFFFFFFFu - p->total_misses) L->misses += p->total_misses; else L->misses = 0xFFFFFFFFu;
    if (L->points <= 0xFFFFFFFFu - p->points)       L->points += p->points;       else L->points = 0xFFFFFFFFu;
    if (L->place_sum <= 0xFFFF - p->place)          L->place_sum += p->place;     else L->place_sum = 0xFFFF;
  }
  stats_save();
}

// Reverse stats_record_game for one stored game — "forget" it when it's deleted.
// Works from the history record (the wide live counters are long gone), so it's
// exact for a normal game and only drifts in two already-documented record-edge
// cases: a per-player counter that saturated into the narrow field, or a game so
// large its worst-placed finishers never made the record. A player whose roster
// slot is gone is skipped (their lifetime was dropped with them). Every figure is
// clamped at 0 so an inexact subtraction can never underflow.
static void stats_unrecord_game(const MKHistGame *hg) {
  for (int i = 0; i < hg->count; i++) {
    const MKResult *r = &hg->results[i];
    int s = slot_by_id(r->id);
    if (s < 0) continue;
    MKLifetime *L = &s_lifetime[s];
    if (L->games) L->games--;
    if (r->place == 1 && L->wins) L->wins--;
    L->throws    = L->throws    >= r->throws ? L->throws    - r->throws : 0;
    L->misses    = L->misses    >= r->misses ? L->misses    - r->misses : 0;
    L->points    = L->points    >= r->points ? L->points    - r->points : 0;
    L->place_sum = L->place_sum >= r->place  ? L->place_sum - r->place  : 0;
  }
  stats_save();
}

// ---- settings ----
bool mk_lose_on_3(void) { return s_lose_on_3; }
void mk_set_lose_on_3(bool v) { s_lose_on_3 = v; persist_write_bool(PK_SETTINGS, v); }
bool mk_final_round(void) { return s_final_round; }
void mk_set_final_round(bool v) { s_final_round = v; persist_write_bool(PK_FINAL_RND, v); }
bool mk_show_header(void) { return s_show_header; }
void mk_set_show_header(bool v) {
  s_show_header = v;
  persist_write_bool(PK_SHOW_HDR, v);
  menu_set_header_enabled(v);
}

// ---- game ----
bool mk_game_active(void) { return s_game_active && !s_game.finished; }
MKGame *mk_game(void) { return &s_game; }
MKGamePlayer *mk_game_current(void) { return &s_game.players[s_game.current]; }
const char *mk_game_player_name(const MKGamePlayer *p) {
  const char *n = p ? mk_name_by_id(p->id) : NULL;
  return n ? n : "?";   // a roster entry deleted mid-game (rare) falls back to "?"
}

void mk_game_start(const bool *sel) {
  MKGame *g = &s_game;
  g->count = 0; g->current = 0; g->group_base = 0;
  g->finishing = false; g->playout = false; g->finished = false;
  g->start_time = (int32_t)time(NULL);   // for the game-duration stat
  // sel[] is indexed over the *active* roster (what the picker shows), so walk
  // entries skipping archived ones and advance the selection index in lockstep.
  int ai = 0;
  for (int s = 0; s < s_roster_count && g->count < MK_MAX_PLAYERS; s++) {
    if (s_roster[s].archived) continue;
    bool take = sel[ai++];
    if (!take) continue;
    MKGamePlayer *p = &g->players[g->count++];
    p->id = s_roster[s].id;
    p->score = 0; p->misses = 0; p->out = false; p->retired = false; p->place = 0;
    p->total_misses = 0; p->throws = 0; p->points = 0;
  }
  for (int i = g->count - 1; i > 0; i--) {            // Fisher–Yates shuffle
    int j = mk_rand() % (i + 1);
    MKGamePlayer t = g->players[i]; g->players[i] = g->players[j]; g->players[j] = t;
  }
  s_game_active = true;
  s_can_undo = false;                                  // a fresh game has nothing to undo
  game_save();
}

int mk_game_active_count(void) {
  int n = 0;
  for (int i = 0; i < s_game.count; i++)
    if (!s_game.players[i].retired && !s_game.players[i].out) n++;
  return n;
}

static bool mk_active(int i) { return !s_game.players[i].retired && !s_game.players[i].out; }

// Round-robin advance: next active player in turn order, wrapping into a new round.
static bool mk_advance(void) {
  int n = s_game.count;
  for (int k = 1; k <= n; k++) {
    int i = (s_game.current + k) % n;
    if (mk_active(i)) { s_game.current = i; return true; }
  }
  return false;
}

// Turn order is the fixed array order, so within a round players throw by
// increasing index: those at index > current still owe a throw this round; those
// below already threw (or will start the next round). These three helpers all
// look only forward, never wrapping.
static int mk_retired_count(void) {
  int n = 0;
  for (int i = 0; i < s_game.count; i++) if (s_game.players[i].retired) n++;
  return n;
}
static bool mk_round_remaining(void) {            // an active player still owes a throw
  for (int i = s_game.current + 1; i < s_game.count; i++) if (mk_active(i)) return true;
  return false;
}
static bool mk_round_tie_possible(void) {         // someone left this round could still reach 50
  for (int i = s_game.current + 1; i < s_game.count; i++)
    if (mk_active(i) && s_game.players[i].score >= 38 && s_game.players[i].score <= 49) return true;
  return false;
}
static bool mk_advance_round(void) {              // next active player this round (no wrap)
  for (int i = s_game.current + 1; i < s_game.count; i++)
    if (mk_active(i)) { s_game.current = i; return true; }
  return false;
}

// One step of "play till end of the round": hand off to the next in-round player,
// or end the game once the round is complete.
static MKThrowResult mk_playout_step(void) {
  if (mk_advance_round()) return MK_THROW_NORMAL;
  s_game.playout = false;
  return MK_THROW_GAMEOVER;
}

MKThrowResult mk_game_throw(int v) {
  s_undo = s_game; s_can_undo = true;                 // snapshot the whole game before mutating
  MKGamePlayer *p = &s_game.players[s_game.current];
  p->throws++;                                        // every turn counts toward the average
  bool retired_now = false;
  if (v == 0) {                                       // miss
    p->misses++; p->total_misses++;
    if (s_lose_on_3 && p->misses >= 3 && !p->out) {   // eliminated
      int dropped = 0;                                // 0-based drop order: later out ranks higher
      for (int i = 0; i < s_game.count; i++) if (s_game.players[i].out) dropped++;
      p->out_order = (uint8_t)dropped;
      p->out = true;
    }
  } else {                                            // a hit clears the miss streak
    p->misses = 0; p->points += v;
    int ns = p->score + v;
    if (ns == MK_WIN) {
      p->score = MK_WIN;
      // Open a tie group on the first 50; later 50s this round join it (same place).
      if (!s_game.finishing) s_game.group_base = mk_retired_count();
      p->retired = true; p->place = s_game.group_base + 1;
      retired_now = true;
    } else {
      p->score = (ns > MK_WIN) ? 25 : ns;             // overshoot drops to 25
    }
  }

  // A tie group stays open while we're resolving a 50 (this throw or an earlier
  // one this round) and the final-round rule can still produce a tie — i.e. an
  // active player who hasn't thrown yet this round is within one throw of 50.
  bool in_finish = retired_now || s_game.finishing;
  bool tie_open  = in_finish && s_final_round && mk_round_tie_possible();
  s_game.finishing = tie_open;

  // Completing the round (the final-round auto-play, or an explicit play-out) wins
  // over the "fewer than two remain" game-over: the trigger player's 50 is exactly
  // what should still let the others in this round take their equalizing throw.
  MKThrowResult res;
  if (s_game.playout) {
    res = mk_playout_step();                          // finish the round, then end
  } else if (tie_open) {
    mk_advance_round(); res = MK_THROW_NORMAL;        // auto-play: let the next player try to tie
  } else if (in_finish) {
    // A 50 with no tie left to play: the victory screen, or straight to the result
    // when nobody else is still in contention (e.g. a two-player game).
    res = mk_game_active_count() <= 1 ? MK_THROW_GAMEOVER : MK_THROW_WIN;
  } else if (mk_game_active_count() <= 1) {
    res = MK_THROW_GAMEOVER;                          // last player standing wins
  } else {
    mk_advance(); res = MK_THROW_NORMAL;              // an ordinary turn
  }
  if (res == MK_THROW_GAMEOVER) { s_game.finishing = false; s_game.playout = false; }
  game_save();
  return res;
}

bool mk_game_continue(void) {
  s_game.finishing = false; s_game.playout = false;   // full play-on for placements, not a round play-out
  bool ok = mk_advance();
  game_save();
  return ok;
}

bool mk_game_round_has_remaining(void) { return mk_round_remaining(); }

void mk_game_play_out(void) {
  s_game.playout = true; s_game.finishing = false;    // finish this round, then end
  mk_advance_round();                                 // hand off to the next in-round player
  game_save();
}

bool mk_game_can_undo(void) { return s_can_undo; }
bool mk_game_undo(void) {
  if (!s_can_undo) return false;
  s_game = s_undo;                                     // restore every coupled stat at once
  s_can_undo = false;                                  // one level only
  game_save();
  return true;
}

static void hist_add(const MKGame *g) {
  MKHistGame hg;
  hg.date  = (int32_t)time(NULL);
  hg.start = g->start_time;                                   // duration is derived on display
  hg._pad[0] = hg._pad[1] = 0;                                // deterministic record bytes
  hg.settings = (mk_lose_on_3() ? MK_SET_LOSE3 : 0) |
                (mk_final_round() ? MK_SET_FINAL : 0);

  // Emit sorted by place, capped at MK_MAX_HIST_PLAYERS. Since this walks places
  // ascending, an over-cap game drops its worst-placed finishers. The wider
  // per-turn counters saturate into the record's narrower fields.
  int idx = 0;
  for (int place = 1; place <= g->count && idx < MK_MAX_HIST_PLAYERS; place++) {
    for (int i = 0; i < g->count && idx < MK_MAX_HIST_PLAYERS; i++) {
      const MKGamePlayer *p = &g->players[i];
      if (p->place != place) continue;
      MKResult *r = &hg.results[idx++];
      r->id = p->id; r->place = p->place; r->score = p->score;
      r->flags  = (p->out ? MK_RES_OUT : 0) | (p->retired ? MK_RES_WON : 0);
      r->misses = p->total_misses > 255 ? 255 : (uint8_t)p->total_misses;
      r->throws = p->throws       > 255 ? 255 : (uint8_t)p->throws;
      r->points = p->points;
    }
  }
  hg.count = idx;

  // Hand the finished game to the storage lib: it assigns a seq, keeps it in the
  // on-watch cache, and queues it for backup to the phone. A new game can't be
  // started while BLOCKED (see mk_hist_can_record / the picker), so by the time
  // we get here there is always room — the append never silently drops a game.
  storage_append(&hg);
}

// Standings key for unplaced (never-reached-50) players. True when a outranks b:
// still-standing ranks above eliminated; among the standing, higher score wins;
// among the eliminated, whoever dropped later (higher out_order) ranks higher.
static bool mk_better(const MKGamePlayer *a, const MKGamePlayer *b) {
  if (a->out != b->out) return !a->out;          // not-out ranks above out
  if (a->out)           return a->out_order > b->out_order;
  return a->score > b->score;
}

void mk_game_end(void) {
  MKGame *g = &s_game;
  int base = mk_retired_count();             // players already placed by reaching 50
  // Everyone gets a place. Eliminated players rank below the still-standing and,
  // among themselves, by drop order (last out ranks higher) — see mk_better.
  if (s_final_round) {
    // Competition ranking with ties: equal-standing players share a place, and
    // the next distinct standing skips accordingly (… 3, 3, 5).
    for (int i = 0; i < g->count; i++) {
      if (g->players[i].place) continue;
      int better = 0;
      for (int j = 0; j < g->count; j++)
        if (!g->players[j].place && j != i && mk_better(&g->players[j], &g->players[i])) better++;
      g->players[i].place = base + 1 + better;
    }
  } else {
    // Strict order: unique sequential places by the same key.
    int next = base + 1;
    for (;;) {
      int best = -1;
      for (int i = 0; i < g->count; i++) {
        if (g->players[i].place) continue;
        if (best < 0 || mk_better(&g->players[i], &g->players[best])) best = i;
      }
      if (best < 0) break;
      g->players[best].place = next++;
    }
  }
  g->finished = true;
  hist_add(g);
  stats_record_game(g);     // fold this game into the players' lifetime totals
  s_game_active = false;
  game_clear_persist();
}

// Throw the in-progress game away unsaved: no history, no lifetime stats — just
// drop the active flag and wipe the persisted snapshot. Callers gate this behind
// a confirmation.
void mk_game_discard(void) {
  s_game_active = false;
  s_can_undo = false;
  game_clear_persist();
}

// ---- history (delegates to the generic storage lib) ----
int mk_hist_count(void) { return storage_cache_count(); }
const MKHistGame *mk_hist_get(int i) {
  return (i >= 0) ? (const MKHistGame *)storage_cache_get((uint8_t)i) : NULL;
}
uint32_t mk_hist_seq_at(int i) {
  return (i >= 0) ? storage_cache_seq((uint8_t)i) : 0;
}

void mk_hist_delete(uint32_t seq, const MKHistGame *g) {
  if (seq == 0) return;
  if (g) stats_unrecord_game(g);   // forget it from the lifetime totals first
  storage_delete(seq);             // drop from the cache + tombstone for the phone
}

void mk_on_reset_request(void (*cb)(void)) { s_reset_request_cb = cb; }

// Wipe everything: the player roster, the lifetime stats, and the synced game store
// (watch cache + the phone's archive). roster_save/stats_save also push the now-empty
// player blob to the phone; storage_reset clears the phone's games (and aux mirror).
void mk_reset_all(void) {
  s_roster_count = 0;
  s_next_id = 1;
  memset(s_lifetime, 0, sizeof(s_lifetime));
  s_suppress_aux_push = true;        // no empty-roster push — storage_reset's WIPE clears the phone's mirror
  roster_save();
  stats_save();
  s_suppress_aux_push = false;
  storage_reset();
}

MKSyncState mk_hist_sync_state(void) {
  switch (storage_state()) {
    case STORAGE_SYNCED:  return MK_SYNC_OK;
    case STORAGE_BLOCKED: return MK_SYNC_BLOCKED;
    default:              return MK_SYNC_PENDING;
  }
}
int  mk_hist_unsynced(void)    { return storage_unsynced(); }
int  mk_hist_total(void)       { return (int)storage_total(); }
bool mk_hist_connected(void)   { return storage_connected(); }
bool mk_hist_syncing(void)     { return storage_syncing(); }
void mk_hist_sync_now(void)    { storage_sync_now(); }
bool mk_hist_can_record(void)  { return storage_state() != STORAGE_BLOCKED; }

void mk_hist_set_listener(const MKHistListener *l) {
  s_hist_listener = l ? *l : (MKHistListener){ 0 };
}
bool mk_hist_load_page(int page, int page_size) {
  if (page < 0 || page_size < 1) return false;
  if (page_size > 255) page_size = 255;
  return storage_load_page((uint32_t)page, (uint8_t)page_size);
}
