#include "history.h"
#include "molkky.h"
#include "c/lib/ui/paged_list.h"
#include "c/lib/ui/view.h"
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

#define HIST_PAGE_SIZE 8

static PagedList  *s_pl;
static MKHistGame  s_view[HIST_PAGE_SIZE];   // the games on the current page
static int         s_view_count;
static int         s_page;                   // current page index (0-based)
static int         s_total;                  // best-known archive size (0 = unknown)
static int         s_unsynced;
static MKSyncState s_sync;
static bool        s_busy;                   // a phone page fetch is in flight

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
  results_view_push(d, rows, n, g->duration, g->settings, NULL);
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
  open_results(&s_view[i]);
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
static void on_page(void *ctx, const MKHistGame *games, int count, int offset, int total) {
  s_busy = false;
  if (total > 0) s_total = total;
  if (count > 0) {
    int n = count > HIST_PAGE_SIZE ? HIST_PAGE_SIZE : count;
    for (int i = 0; i < n; i++) s_view[i] = games[i];
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
