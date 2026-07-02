#include "storage.h"

// =============================================================================
// storage — implementation. See storage.h for the model and contract.
//
// Persist layout (all under cfg.base_key):
//   base_key + 0                : StoreHdr
//   base_key + 1 .. + count     : one slot each, [uint32 seq LE][record bytes]
// The cache is kept newest-first in RAM (index 0 = newest), so seqs strictly
// decrease with index and "unsynced" is always a prefix of the array.
//
// Send sequencing (single message in flight):
//   A page request first drains the unsynced backlog (push batches, each waiting
//   for its ACK), then issues the offset GET. So a page query always runs
//   against a phone that holds the complete archive.
// =============================================================================

// ---- AppMessage protocol ----
// The `st_*` keys are declared in package.json. This SDK emits them as link-time
// globals in message_keys.auto.c but leaves the generated header empty, so we
// declare the externs here to keep the lib self-contained. Names must match
// package.json exactly.
extern uint32_t MESSAGE_KEY_st_type;
extern uint32_t MESSAGE_KEY_st_schema;
extern uint32_t MESSAGE_KEY_st_count;
extern uint32_t MESSAGE_KEY_st_offset;
extern uint32_t MESSAGE_KEY_st_data;
extern uint32_t MESSAGE_KEY_st_ack;
extern uint32_t MESSAGE_KEY_st_total;
extern uint32_t MESSAGE_KEY_st_epoch;

// Beyond the core PUSH/ACK/GET/PAGE/DEL there are four control shapes:
//   ST_RELOAD   (phone->watch): the phone replaced the archive (an import). It
//               carries the phone's post-change highest seq (st_ack) and size
//               (st_total); the watch adopts the new archive (see
//               adopt_phone_archive) and reloads page 0 from the phone.
//   ST_WIPE_REQ (phone->watch): the phone's settings page asked to wipe everything.
//               The lib defers it to the app (on_reset_request) to confirm.
//   ST_WIPE     (watch->phone): clear the entire archive on the phone (sent by
//               storage_reset once the app has confirmed).
//   ST_AUX      (both ways): the opaque app aux blob (see storage_set_aux) — pushed
//               watch->phone when it changes, and phone->watch on an import.
//   ST_HELLO    (phone->watch): sent on every PKJS launch — announces the archive's
//               epoch, highest seq and size so identity changes are noticed early.
//
// Every phone->watch message carries `st_epoch`, the archive's identity: a random
// id the phone mints when its storage is first (re)created and re-mints on every
// import. A mismatch means the archive was replaced or lost behind the watch's
// back — even when the explicit RELOAD after an import never arrived — and the
// watch reconciles before trusting anything else in the message. Writes going the
// other way (PUSH/DEL) carry the watch's last-known epoch, and the phone refuses
// them on a mismatch: a stale-seq write can never land in a replaced archive, no
// matter how the messages cross on the wire.
enum { ST_PUSH = 1, ST_ACK = 2, ST_GET = 3, ST_PAGE = 4, ST_DEL = 5,
       ST_RELOAD = 6, ST_WIPE_REQ = 7, ST_WIPE = 8, ST_AUX = 9, ST_HELLO = 10 };

#define STORE_FMT 2   // v2: StoreHdr gained `epoch`

typedef struct {
  uint16_t fmt;
  uint16_t record_size;
  uint32_t next_seq;
  uint32_t acked_seq;
  uint8_t  count;
  uint8_t  schema;
  uint32_t epoch;     // phone archive identity last reconciled with (0 = never met one)
} StoreHdr;

static StorageConfig s_cfg;
// All RAM the store needs is carved from the caller-provided arena in storage_open,
// so the library has no fixed cache ceiling. Slots and the page buffer are read
// back through struct pointers, so the arena must be 4-byte aligned and record_size
// a multiple of 4 to keep every slot/page offset word-aligned (see StorageConfig).
static uint32_t *s_seq;     // [cache_capacity]  seq per cache slot (index 0 = newest)
static uint8_t  *s_rec;     // cache mirror: cache_capacity slots of record_size (flat)
static uint8_t  *s_page;    // page read-back scratch: up to max_page records
static uint32_t *s_pageseq; // [max_page]  seq per page record (parallel to s_page)
static uint8_t  *s_txbuf;   // push frame scratch: up to max_page frames
static uint8_t  s_count;
static uint8_t  s_max_page = 1;                            // records the page/tx scratch can hold
static uint32_t s_next_seq = 1;                            // 0 is reserved as "no seq"
static uint32_t s_acked;
static uint32_t s_total;                                   // best-known phone archive size
static uint32_t s_epoch;                                   // see the protocol note above

// Pending deletes not yet confirmed by the phone. Persisted (so an offline delete
// survives a restart) and drained by the send pump on every connection. Small and
// fixed: a control structure, not the record cache, so it lives in BSS rather than
// the caller's arena.
static uint32_t s_tomb[STORAGE_MAX_TOMBSTONES];
static uint8_t  s_tomb_count;

// ---- single-in-flight transaction + queued work ----
// Every outbound message flows through one chokepoint, pump(), which sends the
// highest-priority *ready* job whenever the channel is idle and the phone is
// reachable. `s_inflight` names the job on the wire and is held until that job's
// completion signal lands: an ACK for PUSH/DEL/WIPE, a PAGE for PAGE/REFILL, or
// outbox-sent for AUX (the phone never acks it). JOB_DISCARD holds the slot while
// a now-stale read reply (after an import/reset changed the archive) is swallowed.
//
// A disconnect or reset abandons the in-flight job; the next pump re-drives it.
// Every job is idempotent on the phone (re-push dedupes, re-delete/-wipe re-acks),
// so abandoning and retrying never loses or duplicates data — and a lost ACK no
// longer wedges the push backlog (it just re-pushes on the next connection).
typedef enum { JOB_NONE, JOB_WIPE, JOB_PUSH, JOB_DEL, JOB_PAGE, JOB_AUX, JOB_REFILL,
               JOB_DISCARD } Job;
static Job      s_inflight;
static uint32_t s_inflight_seq;       // DEL: the tombstone seq on the wire
static uint32_t s_inflight_offset;    // PAGE/REFILL: offset of the GET on the wire
static uint8_t  s_inflight_size;      // PAGE/REFILL: record count requested

// Queued intents. PUSH (unsynced_count > 0) and DEL (s_tomb_count > 0) are derived
// from existing state and need no flag; the rest carry an explicit "wanted" bit so
// pump can tell whether the source has work. PUSH having no gate of its own is what
// makes a multi-batch backlog drain to completion: it stays ready until acked.
static bool     s_want_wipe;          // storage_reset queued a store-wide wipe
static bool     s_page_pending;       // a UI page fetch is queued
static uint32_t s_page_offset;
static uint8_t  s_page_size;
static bool     s_refill_pending;     // rebuild the cache after a phone-side import
static uint32_t s_refill_off;         // next archive offset to pull into the cache

// Aux blob (one opaque app value, e.g. the roster) synced on the same channel. The
// lib keeps only a pointer to caller-owned memory (no copy, not persisted) and
// re-pushes whenever dirty. See storage_set_aux.
static const uint8_t *s_aux;
static uint16_t s_aux_len;
static bool     s_aux_dirty;

static uint8_t  s_max_batch = 1;      // records per message the negotiated buffers allow

// Reply watchdog. A job holds the channel until its reply lands — but a reply
// can be lost outright (inbox full, radio blip with no callback). The watchdog
// abandons a transaction whose reply never came, exactly like a reconnect does;
// every job is idempotent, so the re-drive is always safe. `s_txn` stamps each
// transaction so a stale timer can't kill a newer one.
#define REPLY_TIMEOUT_MS 10000
static AppTimer *s_watchdog;
static uint32_t  s_txn;

static uint16_t frame_size(void) { return s_cfg.record_size + 4; }
static uint8_t *rec_slot(uint8_t i) { return s_rec + (uint16_t)i * s_cfg.record_size; }

// ---- derived state ----
static uint16_t unsynced_count(void) {
  uint16_t u = 0;
  for (uint8_t i = 0; i < s_count; i++) {                  // unsynced = leading prefix
    if (s_seq[i] > s_acked) u++; else break;
  }
  return u;
}
static StorageSyncState calc_state(void) {
  uint16_t u = unsynced_count();
  if (u == 0) return STORAGE_SYNCED;
  if (s_count == s_cfg.cache_capacity && u == s_count) return STORAGE_BLOCKED;
  return STORAGE_PENDING;
}
static void notify_state(void) {
  if (s_cfg.on_state) s_cfg.on_state(s_cfg.ctx, calc_state(), unsynced_count(), s_total);
}

// ---- persistence ----
static void save_hdr(void) {
  StoreHdr h = { STORE_FMT, s_cfg.record_size, s_next_seq, s_acked, s_count, s_cfg.schema, s_epoch };
  persist_write_data(s_cfg.base_key, &h, sizeof(h));
}
static void save_slot(uint8_t i) {
  uint8_t *buf = s_txbuf;                                   // scratch is idle outside an in-flight push
  buf[0] = s_seq[i]; buf[1] = s_seq[i] >> 8; buf[2] = s_seq[i] >> 16; buf[3] = s_seq[i] >> 24;
  memcpy(buf + 4, rec_slot(i), s_cfg.record_size);
  persist_write_data(s_cfg.base_key + 1 + i, buf, frame_size());
}
static void save_all(void) {
  save_hdr();
  for (uint8_t i = 0; i < s_count; i++) save_slot(i);
}
static void load_persisted(void) {
  s_count = 0; s_next_seq = 1; s_acked = 0; s_epoch = 0;
  if (!persist_exists(s_cfg.base_key)) return;
  StoreHdr h;
  if (persist_read_data(s_cfg.base_key, &h, sizeof(h)) != (int)sizeof(h)) return;
  if (h.fmt != STORE_FMT || h.record_size != s_cfg.record_size) return;   // alien layout → start fresh
  s_next_seq = h.next_seq ? h.next_seq : 1;
  s_acked    = h.acked_seq;
  s_epoch    = h.epoch;
  s_count    = h.count > s_cfg.cache_capacity ? s_cfg.cache_capacity : h.count;
  uint8_t *buf = s_txbuf;                                   // scratch is idle during open
  for (uint8_t i = 0; i < s_count; i++) {
    if (persist_read_data(s_cfg.base_key + 1 + i, buf, frame_size()) != (int)frame_size()) { s_count = i; break; }
    s_seq[i] = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    memcpy(rec_slot(i), buf + 4, s_cfg.record_size);
  }
}

// ---- pending-delete tombstones ----
// Persisted right after the slots so an offline delete isn't lost on restart.
static uint16_t tomb_key(void) { return s_cfg.base_key + s_cfg.cache_capacity + 1; }
static void tomb_save(void) {
  uint8_t buf[2 + STORAGE_MAX_TOMBSTONES * 4];
  buf[0] = s_tomb_count; buf[1] = 0;                       // count is < 256, high byte unused
  for (uint8_t i = 0; i < s_tomb_count; i++) {
    uint8_t *f = buf + 2 + (uint16_t)i * 4;
    f[0] = s_tomb[i]; f[1] = s_tomb[i] >> 8; f[2] = s_tomb[i] >> 16; f[3] = s_tomb[i] >> 24;
  }
  persist_write_data(tomb_key(), buf, 2 + (uint16_t)s_tomb_count * 4);
}
static void tomb_load(void) {
  s_tomb_count = 0;
  if (!persist_exists(tomb_key())) return;
  uint8_t buf[2 + STORAGE_MAX_TOMBSTONES * 4];
  int n = persist_read_data(tomb_key(), buf, sizeof(buf));
  if (n < 2) return;
  uint8_t cnt = buf[0];
  if (cnt > STORAGE_MAX_TOMBSTONES) cnt = STORAGE_MAX_TOMBSTONES;
  if (2 + (int)cnt * 4 > n) cnt = (uint8_t)((n - 2) / 4);  // truncated blob → keep what fits
  for (uint8_t i = 0; i < cnt; i++) {
    const uint8_t *f = buf + 2 + (uint16_t)i * 4;
    s_tomb[i] = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
  }
  s_tomb_count = cnt;
}
static bool tomb_contains(uint32_t seq) {
  for (uint8_t i = 0; i < s_tomb_count; i++) if (s_tomb[i] == seq) return true;
  return false;
}
static void tomb_remove(uint32_t seq) {
  for (uint8_t i = 0; i < s_tomb_count; i++) {
    if (s_tomb[i] != seq) continue;
    for (uint8_t j = i; j + 1 < s_tomb_count; j++) s_tomb[j] = s_tomb[j + 1];
    s_tomb_count--;
    return;
  }
}

// ---- send state machine ----
static bool send_push(void) {
  uint16_t u = unsynced_count();
  if (u == 0) return false;
  uint8_t batch = u < s_max_batch ? u : s_max_batch;
  uint16_t fs = frame_size();
  // Unsynced records are cache indices [0 .. u-1] (index 0 newest). Send oldest
  // first so the phone's ACK of the highest stored seq advances cleanly.
  for (uint8_t k = 0; k < batch; k++) {
    uint8_t idx = u - 1 - k;
    uint8_t *f = s_txbuf + (uint16_t)k * fs;
    f[0] = s_seq[idx]; f[1] = s_seq[idx] >> 8; f[2] = s_seq[idx] >> 16; f[3] = s_seq[idx] >> 24;
    memcpy(f + 4, rec_slot(idx), s_cfg.record_size);
  }
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_uint8(it, MESSAGE_KEY_st_type, ST_PUSH);
  dict_write_uint32(it, MESSAGE_KEY_st_epoch, s_epoch);    // phone refuses stale-epoch writes
  dict_write_uint8(it, MESSAGE_KEY_st_schema, s_cfg.schema);
  dict_write_uint8(it, MESSAGE_KEY_st_count, batch);
  dict_write_data(it, MESSAGE_KEY_st_data, s_txbuf, (uint16_t)batch * fs);
  return app_message_outbox_send() == APP_MSG_OK;
}
static bool send_get(uint32_t offset, uint8_t count) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_uint8(it, MESSAGE_KEY_st_type, ST_GET);
  dict_write_uint32(it, MESSAGE_KEY_st_offset, offset);
  dict_write_uint8(it, MESSAGE_KEY_st_count, count);
  return app_message_outbox_send() == APP_MSG_OK;
}
// A tombstone carries just the seq to forget (reuses st_offset as a uint32 slot).
static bool send_del(uint32_t seq) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_uint8(it, MESSAGE_KEY_st_type, ST_DEL);
  dict_write_uint32(it, MESSAGE_KEY_st_epoch, s_epoch);    // phone refuses stale-epoch writes
  dict_write_uint32(it, MESSAGE_KEY_st_offset, seq);
  return app_message_outbox_send() == APP_MSG_OK;
}
// A wipe is a bare command — the phone clears its whole archive and re-ACKs.
static bool send_wipe(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_uint8(it, MESSAGE_KEY_st_type, ST_WIPE);
  return app_message_outbox_send() == APP_MSG_OK;
}
// Push the aux blob to the phone (it mirrors it for backup). Delivery is success.
static bool send_aux(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_uint8(it, MESSAGE_KEY_st_type, ST_AUX);
  dict_write_data(it, MESSAGE_KEY_st_data, s_aux, s_aux_len);
  return app_message_outbox_send() == APP_MSG_OK;
}
// ---- work sources ----
// Each returns true if it put a message on the wire (and recorded it as the
// in-flight job). pump() tries them in priority order and stops at the first that
// fires, so a higher-priority source draining (e.g. PUSH over several batches)
// naturally runs to completion before a lower one starts. The ordering encodes
// the invariants: a wipe preempts everything (the local store is already cleared);
// the push backlog drains before deletes and reads (a page query needs a complete
// phone archive, a tombstone needs the record to have left the cache first); the
// aux blob and the post-import cache refill come last.
static bool begin_wipe(void) {
  if (!s_want_wipe || !send_wipe()) return false;
  s_inflight = JOB_WIPE;                                   // cleared on its ACK
  return true;
}
static bool begin_push(void) {
  if (unsynced_count() == 0 || !send_push()) return false;
  s_inflight = JOB_PUSH;                                   // completes on its ACK
  notify_state();                                          // a transfer started → UI can show "Syncing…"
  return true;
}
static bool begin_del(void) {
  if (s_tomb_count == 0 || !send_del(s_tomb[0])) return false;
  s_inflight = JOB_DEL; s_inflight_seq = s_tomb[0];        // tombstone retired on its ACK
  return true;
}
static bool begin_page(void) {
  if (!s_page_pending || !send_get(s_page_offset, s_page_size)) return false;
  s_inflight = JOB_PAGE; s_inflight_offset = s_page_offset; s_inflight_size = s_page_size;
  s_page_pending = false;
  return true;
}
static bool begin_aux(void) {
  if (!s_aux_dirty || !s_aux || !s_aux_len || !send_aux()) return false;
  s_inflight = JOB_AUX;                                    // no ACK — completes on outbox-sent
  return true;
}
static bool begin_refill(void) {
  if (!s_refill_pending) return false;
  if (s_refill_off >= s_total) { s_refill_pending = false; return false; }  // whole archive pulled
  uint8_t want = (uint8_t)(s_cfg.cache_capacity - s_count);
  if (want > s_max_batch) want = s_max_batch;
  if (want == 0) { s_refill_pending = false; return false; }                // cache already full
  if (!send_get(s_refill_off, want)) return false;
  s_inflight = JOB_REFILL; s_inflight_offset = s_refill_off; s_inflight_size = want;
  return true;
}

// One message in flight at a time; highest-priority ready source wins.
static bool (*const SOURCES[])(void) = {
  begin_wipe, begin_push, begin_del, begin_page, begin_aux, begin_refill,
};

static void pump(void);

static void watchdog_fire(void *data) {
  s_watchdog = NULL;
  if (s_inflight == JOB_NONE || (uint32_t)(uintptr_t)data != s_txn) return;
  // The reply never came and no transport callback fired. Abandon the
  // transaction like a reconnect would; pump re-drives it from queued state.
  if (s_inflight == JOB_PAGE && s_cfg.on_page)
    s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);
  s_inflight = JOB_NONE;
  pump();
  notify_state();
}
static void watchdog_arm(void) {
  s_txn++;
  if (s_watchdog) app_timer_cancel(s_watchdog);
  s_watchdog = app_timer_register(REPLY_TIMEOUT_MS, watchdog_fire, (void *)(uintptr_t)s_txn);
}

static void pump(void) {
  if (s_inflight != JOB_NONE || !storage_connected()) return;
  for (size_t i = 0; i < sizeof SOURCES / sizeof *SOURCES; i++)
    if (SOURCES[i]()) { watchdog_arm(); return; }
}

// A pending read's reply is now stale because the archive changed underfoot (an
// import or a reset). Release the UI if a user page was waiting, then hold the
// channel as JOB_DISCARD so the next pump waits for that one in-flight reply to
// arrive and be swallowed (the link is FIFO, so it is the next read reply) before
// any new work — no request can be mistaken for the stale one.
static void invalidate_inflight_read(void) {
  if (s_inflight == JOB_PAGE && s_cfg.on_page)
    s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);
  if (s_inflight == JOB_PAGE || s_inflight == JOB_REFILL) {
    s_inflight = JOB_DISCARD;
    watchdog_arm();                       // the awaited stale reply can be lost too
  }
}

// ---- AppMessage callbacks / archive-identity reconciliation ----
// The phone archive changed identity: it was replaced with real content (an
// import, or first contact with an existing archive). The phone is authoritative
// for everything it holds — but records the watch never managed to push exist
// nowhere else, so those are kept: renumbered into the new seq space above the
// phone's highest (they can't be mistaken for, or collide with, archive records)
// and re-pushed. The synced tail of the cache is a stale mirror of the old
// archive and is dropped; the cache then refills from the new archive behind the
// kept records. Any in-flight transaction was against the old archive: reads are
// invalidated, and a push/delete/wipe won't be answered — release it so pump
// re-drives cleanly under the new identity.
static void adopt_phone_archive(uint32_t epoch, uint32_t high, uint32_t total) {
  uint16_t keep = unsynced_count();                            // never-pushed records, newest-first prefix
  s_count = (uint8_t)keep;
  s_tomb_count = 0; tomb_save();                               // tombstones named old-archive seqs
  if (epoch) s_epoch = epoch;
  s_acked = high;
  s_total = total;
  if (s_next_seq <= high) s_next_seq = high + 1;               // never reuse a phone seq
  if (s_next_seq == 0)    s_next_seq = 1;
  for (int i = (int)keep - 1; i >= 0; i--)                     // renumber oldest-first, keeping order
    s_seq[i] = s_next_seq++;
  save_all();

  // Refill behind the kept records. They re-push first (pump priority), so by
  // the time the refill reads, archive offsets 0..keep-1 are those very records.
  s_refill_pending = total > 0 && s_count < s_cfg.cache_capacity;
  s_refill_off     = keep;
  invalidate_inflight_read();                                  // a read in flight queried the old archive
  if (s_inflight == JOB_PUSH || s_inflight == JOB_DEL || s_inflight == JOB_WIPE)
    s_inflight = JOB_NONE;                                     // its reply will never come

  notify_state();
  pump();                                                      // re-push the kept records, then refill
}

// The phone lost its archive (app reinstall / cleared storage): a fresh epoch
// over an empty archive — nothing was imported, the data is simply gone. The
// watch is now the only holder of its cached records: adopt the new identity and
// mark everything unsynced so the whole cache re-uploads. Seqs are kept — the
// phone holds nothing they could collide with (at most a just-pushed batch of
// these very records, which a re-push overwrites idempotently).
static void adopt_empty_phone(uint32_t epoch) {
  s_epoch = epoch;
  s_acked = 0;
  s_total = 0;
  s_tomb_count = 0; tomb_save();                               // deletes named records that are gone anyway
  s_refill_pending = false;                                    // nothing to refill from an empty archive
  save_all();
  invalidate_inflight_read();                                  // a read in flight queried the old archive
  notify_state();
  pump();
}

static void on_inbox(DictionaryIterator *it, void *context) {
  (void)context;
  Tuple *tp = dict_find(it, MESSAGE_KEY_st_type);
  if (!tp) return;
  uint8_t type = tp->value->uint8;
  Tuple *ep = dict_find(it, MESSAGE_KEY_st_epoch);
  uint32_t epoch = ep ? ep->value->uint32 : 0;

  if (type == ST_RELOAD) {
    // Explicit archive replacement (an import). Always adopt.
    Tuple *a = dict_find(it, MESSAGE_KEY_st_ack);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    adopt_phone_archive(epoch, a ? a->value->uint32 : 0, t ? t->value->uint32 : 0);
    return;
  }

  // Epoch mismatch outside a RELOAD: the archive changed identity behind our
  // back — an import whose RELOAD never reached us, a phone that lost its
  // storage, or a first contact (s_epoch 0). Reconcile instead of applying the
  // message under stale assumptions. The phone refused any write we sent under
  // the old epoch, so nothing of ours landed in the archive described here.
  if (epoch && epoch != s_epoch &&
      (type == ST_HELLO || type == ST_ACK || type == ST_PAGE)) {
    Tuple *a = dict_find(it, MESSAGE_KEY_st_ack);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    uint32_t high  = a ? a->value->uint32 : 0;
    uint32_t total = t ? t->value->uint32 : 0;
    if (type == ST_PAGE) {
      // This reply answers our in-flight read, but from an archive we didn't
      // know. Consume it here (release a waiting UI page) and drop the data.
      if (s_inflight == JOB_PAGE && s_cfg.on_page)
        s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);
      if (s_inflight == JOB_PAGE || s_inflight == JOB_REFILL || s_inflight == JOB_DISCARD)
        s_inflight = JOB_NONE;
    }
    if (type == ST_ACK &&
        (s_inflight == JOB_PUSH || s_inflight == JOB_DEL || s_inflight == JOB_WIPE)) {
      if (s_inflight == JOB_WIPE) s_want_wipe = false;         // a wipe applies regardless of epoch
      s_inflight = JOB_NONE;                                   // reply consumed by the reconcile
    }
    if (high == 0 && total == 0) adopt_empty_phone(epoch);
    else adopt_phone_archive(epoch, high, total);
    return;
  }

  if (type == ST_ACK) {
    Tuple *a = dict_find(it, MESSAGE_KEY_st_ack);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    if (a && a->value->uint32 > s_acked) s_acked = a->value->uint32;
    if (t) s_total = t->value->uint32;
    // An ACK is the completion for the in-flight push/delete/wipe — they are the
    // only jobs that wait for one (aux gets none; reads get a PAGE). Only those
    // three can be in flight here, so retire whichever it is.
    if (s_inflight == JOB_DEL)  { tomb_remove(s_inflight_seq); tomb_save(); }
    if (s_inflight == JOB_WIPE) s_want_wipe = false;
    if (s_inflight == JOB_PUSH || s_inflight == JOB_DEL || s_inflight == JOB_WIPE)
      s_inflight = JOB_NONE;
    notify_state();
    pump();                                                // next batch / delete, or a queued read
  } else if (type == ST_HELLO) {
    // Same-epoch hello: the phone just came alive. Adopt its view of the archive
    // size and kick any queued work (offline backlog, tombstones, refill).
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    if (t) s_total = t->value->uint32;
    notify_state();
    pump();
  } else if (type == ST_WIPE_REQ) {
    // A wipe is destructive, so the lib only relays the request; the app confirms
    // (e.g. a watch dialog) and calls storage_reset() if the user accepts.
    if (s_cfg.on_reset_request) s_cfg.on_reset_request(s_cfg.ctx);
  } else if (type == ST_AUX) {
    // The phone sent the aux blob back (an import restored it). Hand it to the app.
    Tuple *d = dict_find(it, MESSAGE_KEY_st_data);
    if (s_cfg.on_aux) s_cfg.on_aux(s_cfg.ctx, d ? d->value->data : NULL, d ? d->length : 0);
  } else if (type == ST_PAGE) {
    // The reply to a read we issued. If that read was invalidated (archive changed
    // underfoot), swallow this one stale reply and let pump resume.
    if (s_inflight == JOB_DISCARD) { s_inflight = JOB_NONE; pump(); return; }
    if (s_inflight != JOB_PAGE && s_inflight != JOB_REFILL) return;  // stray — ignore

    Tuple *d = dict_find(it, MESSAGE_KEY_st_data);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    if (t) s_total = t->value->uint32;
    uint16_t fs = frame_size();
    uint8_t cnt = (d && d->length >= fs) ? d->length / fs : 0;
    const uint8_t *src = cnt ? d->value->data : NULL;

    if (s_inflight == JOB_REFILL) {
      s_inflight = JOB_NONE;
      // A wipe requested mid-refill wins — don't repopulate a cache we're clearing.
      if (s_want_wipe) { s_refill_pending = false; pump(); return; }
      // A refill page: append its records (contiguous, newest-first) into the cache
      // slots after whatever earlier refill pages filled, rebuilding page 0 offline.
      // A record can already be cached (a kept record, or an append that landed
      // mid-refill and shifted the archive offsets) — skip those, never duplicate.
      for (uint8_t i = 0; i < cnt && s_count < s_cfg.cache_capacity; i++) {
        const uint8_t *f = src + (uint16_t)i * fs;
        uint32_t sq = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        s_refill_off++;
        bool held = false;
        for (uint8_t j = 0; j < s_count; j++) if (s_seq[j] == sq) { held = true; break; }
        if (held) continue;
        s_seq[s_count] = sq;
        memcpy(rec_slot(s_count), f + 4, s_cfg.record_size);
        s_count++;
      }
      save_all();
      // Stop when the phone returned a short page (archive exhausted) or the cache filled.
      if (cnt < s_inflight_size || s_count >= s_cfg.cache_capacity) s_refill_pending = false;
      notify_state();
      pump();                                                  // pull the next refill page if still wanted
      return;
    }

    s_inflight = JOB_NONE;                                     // JOB_PAGE
    if (cnt > s_max_page) cnt = s_max_page;                    // s_page holds at most max_page records
    for (uint8_t i = 0; i < cnt; i++) {
      const uint8_t *f = src + (uint16_t)i * fs;
      s_pageseq[i] = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
      memcpy(s_page + (uint16_t)i * s_cfg.record_size, f + 4, s_cfg.record_size);
    }
    if (s_cfg.on_page) s_cfg.on_page(s_cfg.ctx, s_page, s_pageseq, cnt, s_inflight_offset, s_total);
    pump();
  }
}
static void on_inbox_dropped(AppMessageResult reason, void *context) {
  (void)reason; (void)context;
  // A phone->watch message was dropped by the runtime. If it was the reply our
  // in-flight job waits for, nothing else will complete it — release the slot
  // (a waiting UI page gets its failure callback) and re-drive; every job is
  // idempotent, so re-sending is always safe. Dropped phone-initiated control
  // messages (RELOAD, AUX) are covered by the phone's own retries.
  if (s_inflight == JOB_PAGE && s_cfg.on_page)
    s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);
  s_inflight = JOB_NONE;
  pump();
  notify_state();
}
static void on_outbox_sent(DictionaryIterator *it, void *context) {
  (void)it; (void)context;
  // AUX is the only job whose completion is delivery — the phone doesn't ack it.
  // Everything else keeps the channel held until its inbox reply (ACK or PAGE).
  if (s_inflight == JOB_AUX) { s_aux_dirty = false; s_inflight = JOB_NONE; pump(); }
}
static void on_outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *context) {
  (void)it; (void)reason; (void)context;
  // The send didn't go out. Leave each queued intent set (PUSH stays ready while
  // unsynced > 0, the tombstone/wipe/aux/refill flags stay) so the next pump — on
  // reconnect or the next trigger — retries; just release a waiting UI page and
  // free the in-flight slot.
  if (s_inflight == JOB_PAGE && s_cfg.on_page)
    s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);
  s_inflight = JOB_NONE;
  pump();
  notify_state();        // a transfer stopped (or was retried) → refresh the sync label
}
static void on_connection(bool connected) {
  // A fresh connection voids any transaction left dangling by the previous drop —
  // most importantly a push whose ACK never arrived. Releasing the slot lets pump
  // re-drive from scratch; re-pushing is idempotent on the phone, so the lost-ACK
  // record syncs cleanly instead of being stuck "unsynced" forever.
  if (connected) { s_inflight = JOB_NONE; pump(); }
  notify_state();        // connectivity changed → refresh the sync label (a drop clears "Syncing…")
}

// ---- public API ----
void storage_open(const StorageConfig *cfg) {
  s_cfg = *cfg;
  if (s_cfg.record_size < 1)               s_cfg.record_size = 1;
  if (s_cfg.record_size > STORAGE_REC_MAX) s_cfg.record_size = STORAGE_REC_MAX;
  if (s_cfg.cache_capacity < 1)            s_cfg.cache_capacity = 1;

  // Page/tx scratch is sized to max_page (0 → the whole cache), so a store paged
  // in small chunks doesn't reserve scratch for the full cache depth.
  uint8_t page = s_cfg.max_page ? s_cfg.max_page : s_cfg.cache_capacity;
  if (page > s_cfg.cache_capacity) page = s_cfg.cache_capacity;

  // Carve the caller's arena into seq table + cache mirror + page/tx scratch.
  // Shrink cache_capacity to whatever the arena can actually back so nothing overruns.
  while (s_cfg.cache_capacity > 1 &&
         STORAGE_ARENA_BYTES(s_cfg.record_size, s_cfg.cache_capacity, page) > s_cfg.arena_size)
    s_cfg.cache_capacity--;
  if (page > s_cfg.cache_capacity) page = s_cfg.cache_capacity;   // cache shrank under it
  s_max_page = page;
  uint8_t *p = (uint8_t *)s_cfg.arena;
  s_seq     = (uint32_t *)p; p += STORAGE_ALIGN4((size_t)s_cfg.cache_capacity * sizeof(uint32_t));
  s_rec     = p;             p += STORAGE_ALIGN4((size_t)s_cfg.cache_capacity * s_cfg.record_size);
  s_page    = p;             p += STORAGE_ALIGN4((size_t)s_max_page * s_cfg.record_size);
  s_pageseq = (uint32_t *)p; p += STORAGE_ALIGN4((size_t)s_max_page * sizeof(uint32_t));
  s_txbuf   = p;

  load_persisted();
  tomb_load();

  // Transient channel state is not persisted: a fresh open (real restart, or a
  // re-open) has nothing in flight and no queued read/wipe/aux. Tombstones (loaded
  // above) and unsynced cache records are the only sync work that survives.
  s_inflight = JOB_NONE;
  s_want_wipe = false; s_page_pending = false; s_refill_pending = false; s_aux_dirty = false;

  app_message_register_inbox_received(on_inbox);
  app_message_register_inbox_dropped(on_inbox_dropped);
  app_message_register_outbox_sent(on_outbox_sent);
  app_message_register_outbox_failed(on_outbox_failed);

  uint16_t fs = frame_size();
  uint32_t want    = (uint32_t)fs * s_max_page + 96;          // one full page/batch fits a message
  uint32_t max_in  = app_message_inbox_size_maximum();
  uint32_t max_out = app_message_outbox_size_maximum();
  uint32_t in  = want < max_in  ? want : max_in;
  uint32_t out = want < max_out ? want : max_out;
  app_message_open(in, out);

  uint32_t smaller = in < out ? in : out;
  s_max_batch = smaller > 96 ? (smaller - 96) / fs : 1;
  if (s_max_batch < 1) s_max_batch = 1;
  if (s_max_batch > s_max_page) s_max_batch = s_max_page;     // scratch holds at most max_page frames

  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = on_connection });

  notify_state();
  storage_sync_now();   // best-effort; no-op if the phone isn't reachable yet
}

uint32_t storage_append(const void *record) {
  if (calc_state() == STORAGE_BLOCKED) return 0;
  uint32_t seq = s_next_seq++;
  if (s_next_seq == 0) s_next_seq = 1;                     // (unreachable) wrap guard
  uint8_t cap = s_cfg.cache_capacity;
  uint8_t newcount = s_count < cap ? s_count + 1 : cap;    // when full, the oldest (synced) slot is dropped
  for (int i = newcount - 1; i > 0; i--) {
    s_seq[i] = s_seq[i - 1];
    memcpy(rec_slot(i), rec_slot(i - 1), s_cfg.record_size);
  }
  s_seq[0] = seq;
  memcpy(rec_slot(0), record, s_cfg.record_size);
  s_count = newcount;
  save_all();
  notify_state();
  storage_sync_now();
  return seq;
}

uint8_t     storage_cache_count(void)    { return s_count; }
const void *storage_cache_get(uint8_t i) { return i < s_count ? rec_slot(i) : NULL; }
uint32_t    storage_cache_seq(uint8_t i) { return i < s_count ? s_seq[i] : 0; }

bool storage_delete(uint32_t seq) {
  if (seq == 0) return false;
  // Drop it from the cache if it's a held slot, compacting the newest-first array.
  for (uint8_t i = 0; i < s_count; i++) {
    if (s_seq[i] != seq) continue;
    for (uint8_t j = i; j + 1 < s_count; j++) {
      s_seq[j] = s_seq[j + 1];
      memcpy(rec_slot(j), rec_slot(j + 1), s_cfg.record_size);
    }
    s_count--;
    save_all();                                            // rewrite header (count) + remaining slots
    break;
  }
  // Queue a tombstone for the phone (idempotent — a re-delete adds nothing and,
  // crucially, doesn't double-count the total). If the seq was already backed up,
  // the phone holds it, so optimistically drop the local total now (the DEL's ACK
  // confirms); an unsynced seq was never counted there, so leave the total alone.
  if (!tomb_contains(seq)) {
    if (seq <= s_acked && s_total > 0) s_total--;
    if (s_tomb_count < STORAGE_MAX_TOMBSTONES) {
      s_tomb[s_tomb_count++] = seq;
      tomb_save();
    }
  }
  notify_state();
  pump();                                                  // push it now if the phone is near
  return true;
}

void storage_set_aux(const void *data, uint16_t len) {
  s_aux = (const uint8_t *)data;
  s_aux_len = data ? len : 0;
  s_aux_dirty = true;
  pump();                                                  // push now if the phone is near
}

void storage_reset(void) {
  // Wipe the on-watch cache and pending deletes. s_next_seq stays monotonic so a
  // game recorded after the reset still gets a fresh seq (the phone is empty, so it
  // can't collide). acked/total drop to 0 — the archive is now empty everywhere.
  s_count = 0;
  s_tomb_count = 0; tomb_save();
  s_acked = 0;
  s_total = 0;
  s_refill_pending = false;                                // abandon any queued reload
  s_aux_dirty = false;                                     // the WIPE clears the phone's aux mirror too
  invalidate_inflight_read();                              // a read against the old archive is now stale
  save_all();
  s_want_wipe = true;                                      // tell the phone to clear too (offline-safe)
  notify_state();
  pump();
}

StorageSyncState storage_state(void)     { return calc_state(); }
uint16_t         storage_unsynced(void)  { return unsynced_count(); }
uint32_t         storage_total(void)     { return s_total; }
bool             storage_connected(void) { return connection_service_peek_pebble_app_connection(); }
// True only while a push batch is actually on the wire and the phone is reachable
// — distinct from "pending + connected", which can sit stale (an idle backlog, or
// a connection peek that lags reality). Lets the UI say "Syncing…" only when bytes
// are really moving, never just because we happen to look connected.
bool             storage_syncing(void)   { return s_inflight == JOB_PUSH && storage_connected(); }

void storage_sync_now(void) {
  // PUSH is ready whenever unsynced_count() > 0, so a bare pump drains the whole
  // backlog (across batches) on its own — no "want push" latch to half-empty it.
  pump();
}

bool storage_load_page(uint32_t page_index, uint8_t page_size) {
  if (!storage_connected()) return false;
  if (s_inflight != JOB_NONE || s_page_pending) return false;  // one request at a time
  if (s_refill_pending) return false;                          // a post-import cache refill owns the read slot
  if (page_size < 1) page_size = 1;
  if (page_size > s_max_batch) page_size = s_max_batch;
  s_page_offset = page_index * page_size;
  s_page_size = page_size;
  s_page_pending = true;
  pump();
  return true;
}
