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

enum { ST_PUSH = 1, ST_ACK = 2, ST_GET = 3, ST_PAGE = 4 };

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
static uint32_t s_seq[STORAGE_CACHE_MAX];                 // seq per cache slot (0 = newest)
// 4-byte aligned so callers can read a slot back through a struct pointer whose
// first field is a word (e.g. MKHistGame.date). record_size is a multiple of 4
// for the same reason, so every slot/page offset stays aligned too.
static uint8_t  s_rec[STORAGE_CACHE_MAX][STORAGE_REC_MAX] __attribute__((aligned(4)));
static uint8_t  s_count;
static uint32_t s_next_seq = 1;                            // 0 is reserved as "no seq"
static uint32_t s_acked;
static uint32_t s_total;                                   // best-known phone archive size

// Single-in-flight send state machine.
static enum { TX_NONE, TX_PUSH, TX_GET } s_tx;
static bool     s_want_push;          // plain sync wanted (open / append / connect)
static bool     s_await_ack;          // a push batch is out, waiting for its ACK
static bool     s_get_valid;          // a page fetch is queued
static uint32_t s_get_offset;
static uint8_t  s_get_size;
static uint32_t s_inflight_offset;    // offset of the GET currently in flight
static uint8_t  s_max_batch = 1;      // records per message the negotiated buffers allow

// Scratch buffers (static to keep them off the small app stack). s_page is read
// back as records by the caller, so it is word-aligned like s_rec; s_txbuf only
// holds wire frames (memcpy'd, never struct-accessed), so it needs none.
static uint8_t s_txbuf[(STORAGE_REC_MAX + 4) * STORAGE_CACHE_MAX];
static uint8_t s_page[STORAGE_REC_MAX * STORAGE_CACHE_MAX] __attribute__((aligned(4)));

static uint16_t frame_size(void) { return s_cfg.record_size + 4; }

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
  uint8_t buf[STORAGE_REC_MAX + 4];
  buf[0] = s_seq[i]; buf[1] = s_seq[i] >> 8; buf[2] = s_seq[i] >> 16; buf[3] = s_seq[i] >> 24;
  memcpy(buf + 4, s_rec[i], s_cfg.record_size);
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
  uint8_t buf[STORAGE_REC_MAX + 4];
  for (uint8_t i = 0; i < s_count; i++) {
    if (persist_read_data(s_cfg.base_key + 1 + i, buf, frame_size()) != (int)frame_size()) { s_count = i; break; }
    s_seq[i] = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    memcpy(s_rec[i], buf + 4, s_cfg.record_size);
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
    memcpy(f + 4, s_rec[idx], s_cfg.record_size);
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
static void pump(void) {
  if (s_tx != TX_NONE || !storage_connected()) return;
  uint16_t u = unsynced_count();
  if (u == 0) s_want_push = false;
  // Drain the backlog first (a queued page needs a complete phone archive).
  if (u > 0 && !s_await_ack && (s_want_push || s_get_valid)) {
    if (send_push()) { s_tx = TX_PUSH; s_await_ack = true; s_want_push = false; }
    return;
  }
  if (u == 0 && s_get_valid) {
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
      if (cnt > STORAGE_CACHE_MAX) cnt = STORAGE_CACHE_MAX;
      const uint8_t *src = d->value->data;
      for (uint8_t i = 0; i < cnt; i++)
        memcpy(s_page + (uint16_t)i * s_cfg.record_size, src + (uint16_t)i * fs + 4, s_cfg.record_size);
    }
    if (s_cfg.on_page) s_cfg.on_page(s_cfg.ctx, s_page, cnt, s_inflight_offset, s_total);
  }
}
static void on_inbox_dropped(AppMessageResult reason, void *context) { (void)reason; (void)context; }
static void on_outbox_sent(DictionaryIterator *it, void *context) {
  (void)it; (void)context;
  s_tx = TX_NONE; pump();
}
static void on_outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *context) {
  (void)it; (void)reason; (void)context;
  if (s_tx == TX_PUSH) {
    s_await_ack = false;
    s_want_push = true;                                    // retry on next connect/sync
  } else if (s_tx == TX_GET) {
    if (s_cfg.on_page) s_cfg.on_page(s_cfg.ctx, NULL, 0, s_inflight_offset, s_total);  // release UI
  }
  s_tx = TX_NONE;
  pump();
}
static void on_connection(bool connected) {
  if (connected) { if (unsynced_count() > 0) s_want_push = true; pump(); }
}

// ---- public API ----
void storage_open(const StorageConfig *cfg) {
  s_cfg = *cfg;
  if (s_cfg.record_size > STORAGE_REC_MAX)      s_cfg.record_size = STORAGE_REC_MAX;
  if (s_cfg.cache_capacity > STORAGE_CACHE_MAX) s_cfg.cache_capacity = STORAGE_CACHE_MAX;
  if (s_cfg.cache_capacity < 1)                 s_cfg.cache_capacity = 1;

  load_persisted();

  app_message_register_inbox_received(on_inbox);
  app_message_register_inbox_dropped(on_inbox_dropped);
  app_message_register_outbox_sent(on_outbox_sent);
  app_message_register_outbox_failed(on_outbox_failed);

  uint16_t fs = frame_size();
  uint32_t want    = (uint32_t)fs * s_cfg.cache_capacity + 96;
  uint32_t max_in  = app_message_inbox_size_maximum();
  uint32_t max_out = app_message_outbox_size_maximum();
  uint32_t in  = want < max_in  ? want : max_in;
  uint32_t out = want < max_out ? want : max_out;
  app_message_open(in, out);

  uint32_t smaller = in < out ? in : out;
  s_max_batch = smaller > 96 ? (smaller - 96) / fs : 1;
  if (s_max_batch < 1) s_max_batch = 1;
  if (s_max_batch > STORAGE_CACHE_MAX) s_max_batch = STORAGE_CACHE_MAX;

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
    memcpy(s_rec[i], s_rec[i - 1], s_cfg.record_size);
  }
  s_seq[0] = seq;
  memcpy(s_rec[0], record, s_cfg.record_size);
  s_count = newcount;
  save_all();
  notify_state();
  storage_sync_now();
  return seq;
}

uint8_t     storage_cache_count(void)    { return s_count; }
const void *storage_cache_get(uint8_t i) { return i < s_count ? s_rec[i] : NULL; }

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
