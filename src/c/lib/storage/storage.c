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

enum { ST_PUSH = 1, ST_ACK = 2, ST_GET = 3, ST_PAGE = 4, ST_DEL = 5 };

#define STORE_FMT 1

typedef struct {
  uint16_t fmt;
  uint16_t record_size;
  uint32_t next_seq;
  uint32_t acked_seq;
  uint8_t  count;
  uint8_t  schema;
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

// Pending deletes not yet confirmed by the phone. Persisted (so an offline delete
// survives a restart) and drained by the send pump on every connection. Small and
// fixed: a control structure, not the record cache, so it lives in BSS rather than
// the caller's arena.
static uint32_t s_tomb[STORAGE_MAX_TOMBSTONES];
static uint8_t  s_tomb_count;

// Single-in-flight send state machine.
static enum { TX_NONE, TX_PUSH, TX_GET, TX_DEL } s_tx;
static bool     s_want_push;          // plain sync wanted (open / append / connect)
static bool     s_await_ack;          // a push batch is out, waiting for its ACK
static bool     s_get_valid;          // a page fetch is queued
static uint32_t s_get_offset;
static uint8_t  s_get_size;
static uint32_t s_inflight_offset;    // offset of the GET currently in flight
static uint32_t s_inflight_del;       // seq of the DEL currently in flight
static uint8_t  s_max_batch = 1;      // records per message the negotiated buffers allow

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
  StoreHdr h = { STORE_FMT, s_cfg.record_size, s_next_seq, s_acked, s_count, s_cfg.schema };
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
  s_count = 0; s_next_seq = 1; s_acked = 0;
  if (!persist_exists(s_cfg.base_key)) return;
  StoreHdr h;
  if (persist_read_data(s_cfg.base_key, &h, sizeof(h)) != (int)sizeof(h)) return;
  if (h.fmt != STORE_FMT || h.record_size != s_cfg.record_size) return;   // alien layout → start fresh
  s_next_seq = h.next_seq ? h.next_seq : 1;
  s_acked    = h.acked_seq;
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
  dict_write_uint32(it, MESSAGE_KEY_st_offset, seq);
  return app_message_outbox_send() == APP_MSG_OK;
}
static void pump(void) {
  if (s_tx != TX_NONE || !storage_connected()) return;
  uint16_t u = unsynced_count();
  if (u == 0) s_want_push = false;
  // Drain the unsynced push backlog first (a queued page needs a complete phone
  // archive, and a tombstone for an already-deleted slot never collides with one
  // — deletes pull the record out of the cache before it can be pushed).
  if (u > 0 && !s_await_ack && (s_want_push || s_get_valid || s_tomb_count > 0)) {
    if (send_push()) { s_tx = TX_PUSH; s_await_ack = true; s_want_push = false; }
    return;
  }
  // Then drain pending deletes (so a queued page sees the post-delete archive).
  if (u == 0 && s_tomb_count > 0) {
    if (send_del(s_tomb[0])) { s_tx = TX_DEL; s_inflight_del = s_tomb[0]; }
    return;
  }
  if (u == 0 && s_tomb_count == 0 && s_get_valid) {
    if (send_get(s_get_offset, s_get_size)) {
      s_tx = TX_GET; s_inflight_offset = s_get_offset; s_get_valid = false;
    }
  }
}

// ---- AppMessage callbacks ----
static void on_inbox(DictionaryIterator *it, void *context) {
  Tuple *tp = dict_find(it, MESSAGE_KEY_st_type);
  if (!tp) return;
  uint8_t type = tp->value->uint8;
  if (type == ST_ACK) {
    Tuple *a = dict_find(it, MESSAGE_KEY_st_ack);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    s_await_ack = false;
    if (a && a->value->uint32 > s_acked) s_acked = a->value->uint32;
    if (t) s_total = t->value->uint32;
    notify_state();
    pump();                                                // next batch, or the queued GET
  } else if (type == ST_PAGE) {
    Tuple *d = dict_find(it, MESSAGE_KEY_st_data);
    Tuple *t = dict_find(it, MESSAGE_KEY_st_total);
    if (t) s_total = t->value->uint32;
    uint8_t cnt = 0;
    uint16_t fs = frame_size();
    if (d && d->length >= fs) {
      cnt = d->length / fs;
      if (cnt > s_max_page) cnt = s_max_page;                  // s_page holds at most max_page records
      const uint8_t *src = d->value->data;
      for (uint8_t i = 0; i < cnt; i++) {
        const uint8_t *f = src + (uint16_t)i * fs;
        s_pageseq[i] = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        memcpy(s_page + (uint16_t)i * s_cfg.record_size, f + 4, s_cfg.record_size);
      }
    }
    if (s_cfg.on_page) s_cfg.on_page(s_cfg.ctx, s_page, s_pageseq, cnt, s_inflight_offset, s_total);
  }
}
static void on_inbox_dropped(AppMessageResult reason, void *context) { (void)reason; (void)context; }
static void on_outbox_sent(DictionaryIterator *it, void *context) {
  (void)it; (void)context;
  if (s_tx == TX_DEL) {
    // The phone received the tombstone (and removes the record + sends an ACK with
    // the new total). Delivery is enough to retire it — drop it from the backlog.
    tomb_remove(s_inflight_del);
    tomb_save();
  }
  s_tx = TX_NONE; pump();
}
static void on_outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *context) {
  (void)it; (void)reason; (void)context;
  if (s_tx == TX_PUSH) {
    s_await_ack = false;
    s_want_push = true;                                    // retry on next connect/sync
  } else if (s_tx == TX_GET) {
    if (s_cfg.on_page) s_cfg.on_page(s_cfg.ctx, NULL, NULL, 0, s_inflight_offset, s_total);  // release UI
  }
  // TX_DEL: keep the tombstone; the pump retries it on the next connection.
  s_tx = TX_NONE;
  pump();
}
static void on_connection(bool connected) {
  if (connected) { if (unsynced_count() > 0) s_want_push = true; pump(); }
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

StorageSyncState storage_state(void)     { return calc_state(); }
uint16_t         storage_unsynced(void)  { return unsynced_count(); }
uint32_t         storage_total(void)     { return s_total; }
bool             storage_connected(void) { return connection_service_peek_pebble_app_connection(); }

void storage_sync_now(void) {
  if (unsynced_count() == 0) return;
  s_want_push = true;
  pump();
}

bool storage_load_page(uint32_t page_index, uint8_t page_size) {
  if (!storage_connected()) return false;
  if (s_tx != TX_NONE || s_get_valid) return false;        // one request at a time
  if (page_size < 1) page_size = 1;
  if (page_size > s_max_batch) page_size = s_max_batch;
  s_get_offset = page_index * page_size;
  s_get_size = page_size;
  s_get_valid = true;
  pump();
  return true;
}
