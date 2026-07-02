// =============================================================================
// test_storage.c — host-side scenario tests for src/c/lib/storage/storage.c.
//
// The real (unmodified) library is compiled against the pebble.h shim and a fake
// phone (fake_pebble.c). Each test runs in its own forked process so the
// library's file-scope statics start clean — which also lets a test simulate an
// app restart simply by calling storage_open() twice (persist survives in-proc).
//
// Focus: the send-pump interleavings that the flag-based state machine handles
// implicitly — offline buffering, reconnect flush, lost send vs lost reply,
// tombstone persistence across restart, BLOCKED cache, wipe preemption, a
// phone-side import (RELOAD + cache refill), and sync-then-fetch paging.
// =============================================================================
#include "c/lib/storage/storage.h"
#include "fake_pebble.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// ---- test config: small but exercises batching (max_page < cache) ----
#define REC  8
#define CAP  4
#define PAGE 2
#define BASE 100

static uint32_t g_arena[(STORAGE_ARENA_BYTES(REC, CAP, PAGE) + 3) / 4 + 4];

// captured callback state
static StorageSyncState g_state;
static uint16_t g_unsynced;
static uint32_t g_total;
static int      g_reset_requests;
static int      g_aux_calls;
static uint16_t g_aux_len;
static int      g_page_count;
static uint32_t g_page_offset, g_page_total;
static uint32_t g_page_seqs[PAGE];
static uint8_t  g_page_first;

static void on_page(void *c, const void *recs, const uint32_t *seqs, uint8_t count, uint32_t offset, uint32_t total) {
  (void)c;
  g_page_count = count; g_page_offset = offset; g_page_total = total;
  for (uint8_t i = 0; i < count && i < PAGE; i++) g_page_seqs[i] = seqs[i];
  if (count) g_page_first = ((const uint8_t *)recs)[0];
}
static void on_state(void *c, StorageSyncState s, uint16_t u, uint32_t t) { (void)c; g_state = s; g_unsynced = u; g_total = t; }
static void on_reset_request(void *c) { (void)c; g_reset_requests++; }
static void on_aux(void *c, const uint8_t *d, uint16_t len) { (void)c; (void)d; g_aux_calls++; g_aux_len = len; }

static void open_store(void) {
  StorageConfig cfg = {
    .record_size = REC, .cache_capacity = CAP, .max_page = PAGE, .schema = 1, .base_key = BASE,
    .arena = g_arena, .arena_size = sizeof g_arena,
    .on_page = on_page, .on_state = on_state, .on_reset_request = on_reset_request, .on_aux = on_aux,
  };
  storage_open(&cfg);
}

static uint32_t append_val(uint8_t v) {
  uint8_t rec[REC] = {0};
  rec[0] = v;
  return storage_append(rec);
}

// ---- tiny assert framework (fork-per-test) ----
#define ASSERT(cond, msg) do { if (!(cond)) { \
  fprintf(stderr, "    FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); _exit(1); } } while (0)
#define ASSERT_EQ(a, b, msg) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) { \
  fprintf(stderr, "    FAIL: %s — got %ld, want %ld  (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); _exit(1); } } while (0)

// ============================== scenarios ==============================

// Connected append pushes immediately and the ACK marks it synced.
static void t_basic_sync(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  uint32_t s = append_val(1);
  ASSERT(s != 0, "append returned a seq");
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 1, "phone holds the record");
  ASSERT(fake_phone_has_seq(s), "phone holds the right seq");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "state SYNCED");
  ASSERT_EQ(storage_unsynced(), 0, "nothing unsynced");
  ASSERT_EQ(storage_total(), 1, "total learned from ACK");
}

// Appends made offline buffer locally; nothing is sent while disconnected.
static void t_offline_buffers_without_sending(void) {
  fake_init(); fake_set_connected(false, false); open_store();
  append_val(1); append_val(2); append_val(3);
  ASSERT_EQ(fake_phone_count(), 0, "nothing reaches phone while offline");
  ASSERT_EQ(storage_unsynced(), 3, "all three buffered");
  ASSERT_EQ(storage_state(), STORAGE_PENDING, "state PENDING offline");
  ASSERT(!fake_outbox_pending(), "no send attempted while disconnected");
}

// storage_syncing() — and thus the "Syncing…" label — is true only while a push
// is genuinely on the wire, never merely because we are pending+connected.
static void t_syncing_only_during_active_transfer(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1);
  ASSERT(storage_syncing(), "syncing while a push is on the wire");
  fake_channel_drain();
  ASSERT(!storage_syncing(), "not syncing once the backlog is drained");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "synced");

  fake_set_connected(false, true);
  append_val(2);                                  // pending, but offline
  ASSERT_EQ(storage_state(), STORAGE_PENDING, "pending while offline");
  ASSERT(!storage_syncing(), "not syncing while disconnected (the reported bug)");
}

// A multi-batch offline backlog drains to completion on a single reconnect. (The
// batch size here is 2, so 3 buffered records take more than one push.) PUSH stays
// ready while unsynced_count() > 0, so the post-ACK pump keeps sending until the
// backlog is empty — no half-flush waiting for another trigger.
static void t_offline_backlog_fully_flushes(void) {
  fake_init(); fake_set_connected(false, false); open_store();
  append_val(1); append_val(2); append_val(3);
  ASSERT_EQ(storage_unsynced(), 3, "three buffered offline");

  fake_set_connected(true, true);                 // fires the connection handler
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 3, "entire backlog flushes on reconnect");
  ASSERT_EQ(storage_unsynced(), 0, "nothing left unsynced");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "SYNCED after a single reconnect");
}

// A lost send (outbox_failed) is re-queued and succeeds on the retry.
static void t_lost_send_requeues(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(7);
  ASSERT(fake_outbox_pending(), "a push is in flight");
  fake_channel_drop_send();                        // lost in transit
  ASSERT_EQ(fake_phone_count(), 0, "phone never saw the dropped push");
  ASSERT_EQ(storage_unsynced(), 1, "still unsynced after drop");
  fake_channel_drain();                            // the requeued push now lands
  ASSERT_EQ(fake_phone_count(), 1, "retry delivered the record");
  ASSERT_EQ(storage_unsynced(), 0, "synced after retry");
}

// A push reaches the phone but its ACK is lost. The watch still believes the
// record is unsynced — until the next connection, which abandons the dangling
// transaction and re-pushes. Because push is idempotent on the phone, the record
// reconciles to synced with no duplication.
static void t_lost_ack_heals_on_reconnect(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  fake_phone_hello(); fake_channel_drain();        // adopt the phone's epoch, as a real launch does
  uint32_t s = append_val(9);
  fake_channel_drop_reply();                       // phone stores it, ACK lost
  ASSERT(fake_phone_has_seq(s), "phone actually stored the record");
  ASSERT_EQ(storage_unsynced(), 1, "watch still thinks it is unsynced before reconnect");

  fake_set_connected(false, true);
  fake_set_connected(true, true);                  // reconnect re-drives the push
  fake_channel_drain();
  ASSERT_EQ(storage_unsynced(), 0, "reconnect reconciles it to synced");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "state SYNCED after healing");
  ASSERT_EQ(fake_phone_count(), 1, "still exactly one copy (idempotent re-push)");
}

// A reply lost after the phone acted (dropped ACK) no longer wedges the session
// until a reconnect: the reply watchdog abandons the transaction and the pump
// re-drives it. Idempotent re-push reconciles without duplication.
static void t_lost_reply_recovers_by_timeout(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  fake_phone_hello(); fake_channel_drain();        // adopt the phone's epoch, as a real launch does
  uint32_t s = append_val(9);
  fake_channel_drop_reply();                       // phone stored it, ACK lost
  ASSERT(fake_phone_has_seq(s), "phone holds the record");
  ASSERT_EQ(storage_unsynced(), 1, "watch still believes it is unsynced");

  ASSERT(fake_fire_timers() > 0, "a reply watchdog was armed");
  fake_channel_drain();                            // the re-driven push reconciles
  ASSERT_EQ(storage_unsynced(), 0, "healed without a reconnect");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "SYNCED");
  ASSERT_EQ(fake_phone_count(), 1, "still exactly one copy");
}

// A cache full of unsynced records goes BLOCKED and refuses further appends.
static void t_blocked_when_cache_full(void) {
  fake_init(); fake_set_connected(false, false); open_store();
  for (int i = 0; i < CAP; i++) ASSERT(append_val(i + 1) != 0, "append within capacity");
  ASSERT_EQ(storage_state(), STORAGE_BLOCKED, "BLOCKED when full of unsynced");
  ASSERT_EQ(append_val(99), 0, "append refused while BLOCKED");
  ASSERT_EQ(storage_unsynced(), CAP, "still exactly capacity unsynced");
}

// An offline delete persists a tombstone that survives a restart and drains on
// the next connection.
static void t_tombstone_survives_restart(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  uint32_t s = append_val(5);
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 1, "record synced before delete");

  fake_set_connected(false, true);
  ASSERT(storage_delete(s), "offline delete accepted");
  ASSERT_EQ(storage_cache_count(), 0, "removed from local cache");

  open_store();                                    // simulate app restart (persist survives)
  fake_set_connected(true, true);                  // reconnect → tombstone drains
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 0, "tombstone removed it from the phone after restart");
}

// reset() preempts everything: the queued WIPE drains and clears the phone.
static void t_wipe_preempts(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1); append_val(2);
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 2, "two records synced");

  storage_reset();
  ASSERT_EQ(storage_cache_count(), 0, "local cache cleared immediately");
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 0, "phone archive wiped");
  ASSERT_EQ(storage_total(), 0, "total reset to 0");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "SYNCED after wipe");
}

// A phone-side import (RELOAD) makes the phone authoritative: the watch drops its
// stale cache, realigns its seq space, and refills page 0 from the phone.
static void t_reload_realigns_and_refills(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1); append_val(2);
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 2, "initial records synced");

  // Phone imports a different archive with higher seqs (10,11,12), oldest-first.
  uint32_t seqs[3] = {10, 11, 12};
  uint8_t  recs[3 * REC] = {0};
  recs[0 * REC] = 0xA0; recs[1 * REC] = 0xB0; recs[2 * REC] = 0xC0;
  fake_phone_import(seqs, recs, 3, REC);
  fake_channel_drain();

  ASSERT_EQ(storage_total(), 3, "total taken from the imported archive");
  ASSERT_EQ(storage_unsynced(), 0, "nothing unsynced after reload");
  ASSERT_EQ(storage_cache_count(), 3, "cache refilled (min of cap and archive)");
  ASSERT_EQ(storage_cache_seq(0), 12, "newest record is first in the cache");

  uint32_t after = append_val(0x55);               // new seq must not collide with 12
  ASSERT(after > 12, "new seq realigned above the imported archive");
}

// A reset issued while a page fetch is on the wire must not let the now-stale PAGE
// reply land on the wiped store. The in-flight read is invalidated (UI released),
// the channel holds as JOB_DISCARD until that one reply is swallowed, and only
// then does the queued WIPE go out.
static void t_reset_during_inflight_read_discards_stale_page(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  for (int i = 0; i < 5; i++) { append_val(i + 1); fake_channel_drain(); }
  ASSERT_EQ(fake_phone_count(), 5, "five records on the phone");

  ASSERT(storage_load_page(1, PAGE), "page request accepted");
  fake_deliver_outbox_only();                     // GET reaches the phone; its PAGE is queued, not delivered
  ASSERT_EQ(fake_inbox_depth(), 1, "the PAGE reply is waiting");

  g_page_count = -1;
  storage_reset();                                // wipe while the read is in flight
  ASSERT_EQ(g_page_count, 0, "the in-flight page fetch was released (count 0)");
  ASSERT_EQ(fake_phone_count(), 5, "wipe is held until the stale reply is swallowed");

  fake_deliver_one_inbox();                       // the stale PAGE arrives — must be discarded, not applied
  fake_channel_drain();                           // now the WIPE goes out
  ASSERT_EQ(fake_phone_count(), 0, "phone wiped after the stale reply was swallowed");
  ASSERT_EQ(storage_cache_count(), 0, "local cache empty");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "SYNCED after wipe");
}

// An import whose RELOAD never reaches the watch must not let stale-seq pushes
// corrupt the restored archive: the phone refuses writes made under the old
// epoch (and keeps re-sending the owed RELOAD), and the watch reconciles from
// the mismatch — renumbering its new game above the import instead of
// overwriting a restored one.
static void t_missed_reload_heals_via_epoch(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1); append_val(2);
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 2, "two games synced before the import");

  uint32_t seqs[3] = {10, 11, 12};
  uint8_t  recs[3 * REC] = {0};
  fake_phone_import(seqs, recs, 3, REC);
  fake_inbox_clear();                              // the RELOAD is lost in transit...
  fake_phone_set_reload_pending();                 // ...and the phone knows it's still owed

  uint32_t s = append_val(3);                      // a new game under the stale seq space
  ASSERT(s <= 12, "the new seq would collide with the restored archive");
  fake_channel_drain();                            // push dropped → RELOAD re-sent → adopt → re-push
  ASSERT(fake_phone_has_seq(10) && fake_phone_has_seq(11) && fake_phone_has_seq(12),
         "restored games survive untouched");
  ASSERT_EQ(fake_phone_count(), 4, "the new game was appended, not overwritten");
  ASSERT(fake_phone_highest_seq() > 12, "the new game was renumbered above the import");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "fully reconciled");
  ASSERT_EQ(storage_total(), 4, "total covers restored + new");
}

// The phone app lost its storage (reinstall / cleared data): its HELLO shows a
// fresh epoch over an empty archive. The watch keeps its cache and re-uploads
// it — the empty phone is not authoritative for anything.
static void t_wiped_phone_reuploads_cache(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1); append_val(2); append_val(3);
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 3, "three games synced");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "synced before the loss");

  fake_phone_lose_storage();
  ASSERT_EQ(fake_phone_count(), 0, "phone archive gone");
  fake_phone_hello();                              // the next PKJS launch announces the loss
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 3, "cache re-uploaded to the wiped phone");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "resynced");
  ASSERT_EQ(storage_total(), 3, "total re-learned from the re-upload");
}

// A reload arriving while the watch holds unsynced games must not drop them:
// they are renumbered into the new seq space, re-pushed after the adopt, and the
// cache refills behind them.
static void t_reload_preserves_unsynced(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  append_val(1);
  fake_channel_drain();                            // one synced
  fake_set_connected(false, true);
  append_val(0x66);                                // one unsynced, recorded offline

  uint32_t seqs[2] = {10, 11};
  uint8_t  recs[2 * REC] = {0};
  fake_phone_import(seqs, recs, 2, REC);           // import while the watch is away
  fake_set_connected(true, true);                  // reconnect: re-push and RELOAD interleave
  fake_channel_drain();
  ASSERT_EQ(fake_phone_count(), 3, "imported two + preserved one");
  ASSERT(fake_phone_highest_seq() > 11, "preserved game renumbered above the import");
  ASSERT_EQ(storage_state(), STORAGE_SYNCED, "everything synced");
  ASSERT_EQ(storage_cache_count(), 3, "cache holds the preserved + refilled records");
}

// Paging beyond the local cache: sync-then-fetch returns the requested page.
static void t_load_page(void) {
  fake_init(); fake_set_connected(true, false); open_store();
  for (int i = 0; i < 5; i++) { append_val(i + 1); fake_channel_drain(); }  // 5 synced; cache holds newest 4
  ASSERT_EQ(fake_phone_count(), 5, "five records on the phone");
  ASSERT_EQ(storage_total(), 5, "total is five");

  g_page_count = -1;
  ASSERT(storage_load_page(1, PAGE), "page request accepted");  // page 1, size 2 → offset 2
  fake_channel_drain();
  ASSERT_EQ(g_page_count, 2, "page returned two records");
  ASSERT_EQ(g_page_offset, 2, "page offset is 2");
  ASSERT_EQ(g_page_total, 5, "page reports total 5");
  // newest-first global order is seqs 5,4,3,2,1 → offset 2 is seqs 3,2
  ASSERT_EQ(g_page_seqs[0], 3, "first record on page 1 is seq 3");
  ASSERT_EQ(g_page_seqs[1], 2, "second record on page 1 is seq 2");
}

// ---- runner ----
typedef void (*test_fn)(void);
static int run(const char *name, test_fn fn) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) { fn(); _exit(0); }
  int status = 0; waitpid(pid, &status, 0);
  int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

int main(void) {
  printf("storage pump scenarios:\n");
  int fails = 0;
  fails += run("basic_sync", t_basic_sync);
  fails += run("offline_buffers_without_sending", t_offline_buffers_without_sending);
  fails += run("syncing_only_during_active_transfer", t_syncing_only_during_active_transfer);
  fails += run("offline_backlog_fully_flushes", t_offline_backlog_fully_flushes);
  fails += run("lost_send_requeues", t_lost_send_requeues);
  fails += run("lost_ack_heals_on_reconnect", t_lost_ack_heals_on_reconnect);
  fails += run("lost_reply_recovers_by_timeout", t_lost_reply_recovers_by_timeout);
  fails += run("blocked_when_cache_full", t_blocked_when_cache_full);
  fails += run("tombstone_survives_restart", t_tombstone_survives_restart);
  fails += run("wipe_preempts", t_wipe_preempts);
  fails += run("reload_realigns_and_refills", t_reload_realigns_and_refills);
  fails += run("reset_during_inflight_read_discards_stale_page", t_reset_during_inflight_read_discards_stale_page);
  fails += run("missed_reload_heals_via_epoch", t_missed_reload_heals_via_epoch);
  fails += run("wiped_phone_reuploads_cache", t_wiped_phone_reuploads_cache);
  fails += run("reload_preserves_unsynced", t_reload_preserves_unsynced);
  fails += run("load_page", t_load_page);
  printf("%s (%d failing)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails);
  return fails ? 1 : 0;
}
