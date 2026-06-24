#include "history.h"
#include "molkky.h"
#include "c/lib/ui/paged_list.h"
#include "c/lib/ui/view.h"
#include "c/lib/ui/menu.h"
#include "c/lib/ui/dialog.h"
#include "results_view.h"

// =============================================================================
// History: a paged list of past games (newest first) with a results view per
// game, backed by the phone archive.
//
//   * Page 0 is served from the watch's local cache — instant and offline, and
//     always correct because the watch always holds the newest games.
//   * Pages >= 1 (and First/Prev/Next/Last navigation) go through the phone:
//     storage syncs any unsynced games first, then returns the requested page.
//     Only the current page (HIST_PAGE_SIZE games) lives in RAM.
//   * A new game (added elsewhere) makes the watch the newest again, so simply
//     reopening History lands on page 0 with that game shown.
// =============================================================================

#define HIST_PAGE_SIZE MK_HIST_PAGE   // page size == the store's max_page (molkky.c)

static PagedList  *s_pl;
static MKHistGame  s_view[HIST_PAGE_SIZE];   // the games on the current page
static uint32_t    s_view_seq[HIST_PAGE_SIZE]; // each game's store key (for delete), parallel to s_view
static int         s_view_count;
static int         s_page;                   // current page index (0-based)
static int         s_total;                  // best-known archive size (0 = unknown)
static int         s_unsynced;
static MKSyncState s_sync;
static bool        s_busy;                   // a phone page fetch is in flight

// The row whose results are open, for the delete flow (results view → action
// menu → confirm dialog all act on it). Stored as an index into the stable
// s_view/s_view_seq (the page can't change while those screens are up), so no
// per-game copy is held in static RAM.
static int         s_sel_idx = -1;
static View       *s_results_view;           // the open results screen (to unwind on delete)
static Menu       *s_actions_menu;           // the {Go back, Delete} menu over it

static void fmt_date(int32_t t, char *buf, size_t n) {
  time_t tt = (time_t)t;
  struct tm *lt = localtime(&tt);
  strftime(buf, n, "%b %d, %H:%M", lt);
}

// Resolve a stored player id to its current name, or "deleted" if removed.
static const char *result_name(const MKResult *r) {
  const char *name = mk_name_by_id(r->id);
  return name ? name : "deleted";
}

// ---------------------------- results view ----------------------------
// Reuse the end-of-game results screen so a stored game looks the same when
// browsed later — the same standings + Stats section, here titled by its date.
// Select on the screen opens an action menu (Go back / Delete) — see open_actions.
static void open_actions(void);

static void open_results(const MKHistGame *g) {
  if (!g) return;
  char d[24] = "Results";
  fmt_date(g->date, d, sizeof(d));

  static ResultRow rows[MK_MAX_HIST_PLAYERS];
  int n = 0;
  for (int i = 0; i < g->count && i < MK_MAX_HIST_PLAYERS; i++) {
    const MKResult *r = &g->results[i];
    rows[n++] = (ResultRow){
      .name   = result_name(r), .place = r->place, .score = r->score,
      .out    = (r->flags & MK_RES_OUT) != 0,
      .misses = r->misses, .throws = r->throws, .points = r->points,
    };
  }
  // Duration is derived: the record stores absolute start/end, not minutes.
  uint16_t duration = (g->date > g->start) ? (uint16_t)((g->date - g->start) / 60) : 0;
  s_results_view = results_view_push(d, rows, n, duration, g->settings, open_actions);
}

static void fill_game_row(ListItem *out, const MKHistGame *g) {
  fmt_date(g->date, out->title, sizeof out->title);
  if (g->count > 0)
    snprintf(out->subtitle, sizeof out->subtitle, "Winner: %s", result_name(&g->results[0]));
}

// ---------------------------- page loading ----------------------------
static void fill_local_page0(void) {
  int n = mk_hist_count();
  if (n > HIST_PAGE_SIZE) n = HIST_PAGE_SIZE;
  for (int i = 0; i < n; i++) {
    const MKHistGame *g = mk_hist_get(i);
    if (g) s_view[i] = *g;
    s_view_seq[i] = mk_hist_seq_at(i);
  }
  s_view_count = n;
}

// target <= 0 → page 0 from the local cache (no phone needed). target > 0 →
// sync-then-fetch from the phone; the result lands in on_page.
static void go_to_page(int target) {
  if (target <= 0) {
    fill_local_page0();
    s_page = 0;
    if (s_pl) paged_list_top(s_pl);
    return;
  }
  s_busy = true;
  if (s_pl) paged_list_reload(s_pl);                     // show "Loading…"
  if (!mk_hist_load_page(target, HIST_PAGE_SIZE)) {      // unreachable / already busy
    s_busy = false;
    if (s_pl) paged_list_reload(s_pl);
  }
}

// ---------------------------- paged_list callbacks ----------------------------
static uint16_t h_count(void *c) { return s_view_count ? s_view_count : 1; }
static void h_item(void *c, uint16_t i, ListItem *out) {
  if (s_view_count == 0) { snprintf(out->title, sizeof out->title, "No games yet"); return; }
  fill_game_row(out, &s_view[i]);
}
static void h_select(void *c, uint16_t i) {
  if (s_view_count == 0) return;
  s_sel_idx = i;                          // remember the row for the delete flow
  open_results(&s_view[i]);
}

// ---------------------------- delete flow ----------------------------
// Results view → Select → menu { Go back, Delete } → confirm dialog → delete.
// After a delete we unwind back to the list and refresh the current page.
static void refresh_after_delete(void) {
  if (s_page <= 0) {                      // page 0 is local — re-read the (shrunk) cache
    fill_local_page0();
    if (s_pl) paged_list_top(s_pl);
  } else {
    go_to_page(s_page);                   // best-effort re-fetch of the current page
  }
}

static void do_delete(void *ctx) {
  if (s_sel_idx >= 0 && s_sel_idx < s_view_count) {
    uint32_t seq = s_view_seq[s_sel_idx];
    MKHistGame g = s_view[s_sel_idx];                          // stack copy drives the stats subtraction
    mk_hist_delete(seq, &g);                                   // stats + cache + phone tombstone
  }
  if (s_actions_menu) window_stack_remove(menu_window(s_actions_menu), false);
  if (s_results_view) window_stack_remove(view_window(s_results_view), false);
  s_actions_menu = NULL;
  s_results_view = NULL;                                        // the view frees itself on pop
  refresh_after_delete();
}

static void confirm_delete(void *ctx) {
  dialog_confirm_push("Delete game?", "This removes it from history and statistics.",
                      "Delete", UI_BTN_DANGER, do_delete, NULL);
}

static uint16_t act_count(void *c) { return 2; }
static void act_item(void *c, uint16_t i, ListItem *out) {
  snprintf(out->title, sizeof out->title, i == 0 ? "Go back" : "Delete");
  if (i == 1) out->leading = (Accessory){ .kind = ACC_ICON, .icon_res = RESOURCE_ID_IMAGE_DELETE };
}
static void act_select(void *c, uint16_t i) {
  if (i == 0) { window_stack_pop(true); return; }   // Go back → return to the results screen
  confirm_delete(NULL);
}

static void open_actions(void) {
  s_actions_menu = menu_push("Game", (MenuConfig){
    .get_count = act_count, .get_item = act_item, .on_select = act_select,
  });
}

static void h_pager(void *c, PagerModel *out) {
  int size = HIST_PAGE_SIZE;
  out->page = s_page;
  out->total_pages = s_total > 0 ? (s_total + size - 1) / size : 0;   // 0 → "?"
  // First/Prev can reach page 0 from the local cache, so they only need a prior
  // page. Next/Last always fetch from the phone, so they also need a connection.
  out->has_prev = s_page > 0;
  out->has_next = mk_hist_connected() &&
                  (s_total > 0 ? (s_page + 1) * size < s_total : mk_hist_count() >= size);
  out->busy = s_busy;

  if (s_busy)                       snprintf(out->status, sizeof out->status, "Loading…");
  else if (s_sync == MK_SYNC_BLOCKED) snprintf(out->status, sizeof out->status, "Sync needed");
  else if (s_sync == MK_SYNC_PENDING) {
    if (mk_hist_connected())        snprintf(out->status, sizeof out->status, "Syncing…");
    else                            snprintf(out->status, sizeof out->status, "%d unsaved", s_unsynced);
  } else out->status[0] = '\0';     // synced → show the page indicator
}

static void h_nav(void *c, PagerNav nav) {
  int size = HIST_PAGE_SIZE;
  int last = s_total > 0 ? (s_total - 1) / size : s_page + 1;
  switch (nav) {
    case PAGER_FIRST: go_to_page(0);                       break;
    case PAGER_PREV:  go_to_page(s_page > 0 ? s_page - 1 : 0); break;
    case PAGER_NEXT:  go_to_page(s_page + 1);              break;
    case PAGER_LAST:  go_to_page(last);                    break;
  }
}

static void h_unload(void *c) {
  mk_hist_set_listener(NULL);                              // stop callbacks before the window frees
  s_pl = NULL;
}

// ---------------------------- async listener ----------------------------
static void on_page(void *ctx, const MKHistGame *games, const uint32_t *seqs, int count, int offset, int total) {
  s_busy = false;
  if (total > 0) s_total = total;
  if (count > 0) {
    int n = count > HIST_PAGE_SIZE ? HIST_PAGE_SIZE : count;
    for (int i = 0; i < n; i++) { s_view[i] = games[i]; s_view_seq[i] = seqs ? seqs[i] : 0; }
    s_view_count = n;
    s_page = offset / HIST_PAGE_SIZE;
    if (s_pl) paged_list_top(s_pl);
  } else if (s_pl) {
    paged_list_reload(s_pl);                               // empty/failed → keep the current page, refresh pager
  }
}
static void on_state(void *ctx, MKSyncState state, int unsynced, int total) {
  s_sync = state;
  s_unsynced = unsynced;
  if (total > 0) s_total = total;
  if (s_pl) paged_list_reload(s_pl);
}

void history_push(void) {
  s_page = 0;
  s_busy = false;
  s_sync = mk_hist_sync_state();
  s_unsynced = mk_hist_unsynced();
  s_total = mk_hist_total();
  fill_local_page0();
  mk_hist_set_listener(&(MKHistListener){ .on_page = on_page, .on_state = on_state, .ctx = NULL });

  s_pl = paged_list_push("History", (PagedListConfig){
    .size = UI_SIZE_MD,
    .get_count = h_count, .get_item = h_item, .on_select = h_select,
    .get_pager = h_pager, .on_nav = h_nav, .on_unload = h_unload,
  });
  mk_hist_sync_now();                                      // push unsynced + learn the archive total
}
