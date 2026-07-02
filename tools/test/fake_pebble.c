// =============================================================================
// fake_pebble.c — in-memory implementation of the pebble.h shim plus a faithful
// port of the phone side (src/pkjs/storage.js) so the real C library can be
// exercised end-to-end on the host.
//
// Transport model (deliberately explicit, so tests control interleavings):
//   * The watch builds one outbox dict (app_message_outbox_begin/dict_write_*)
//     and "sends" it (app_message_outbox_send) — this only marks it pending.
//   * A test then advances the channel: fake_channel_step delivers the pending
//     message to the phone, fires outbox_sent, and delivers the phone's replies
//     to inbox_received. Single message in flight, just like AppMessage.
//   * Drop variants model a lost send (outbox_failed) or a lost reply.
// =============================================================================
#include "pebble.h"
#include "fake_pebble.h"
#include <stdlib.h>

// ---- message keys (link-time globals the library declares extern) ----
// Values are arbitrary but must be distinct; both sides match by these.
uint32_t MESSAGE_KEY_st_type   = 1;
uint32_t MESSAGE_KEY_st_schema = 2;
uint32_t MESSAGE_KEY_st_count  = 3;
uint32_t MESSAGE_KEY_st_offset = 4;
uint32_t MESSAGE_KEY_st_data   = 5;
uint32_t MESSAGE_KEY_st_ack    = 6;
uint32_t MESSAGE_KEY_st_total  = 7;
uint32_t MESSAGE_KEY_st_epoch  = 8;

// Protocol type values — must match storage.c's ST_* and storage.js's TYPE.
enum { ST_PUSH = 1, ST_ACK = 2, ST_GET = 3, ST_PAGE = 4, ST_DEL = 5,
       ST_RELOAD = 6, ST_WIPE_REQ = 7, ST_WIPE = 8, ST_AUX = 9, ST_HELLO = 10 };

// ---- persist (in-memory; survives a re-open within the same process) ----
#define PERSIST_MAX 64
typedef struct { uint32_t key; uint8_t buf[256]; int len; bool used; } PersistEnt;
static PersistEnt g_persist[PERSIST_MAX];

static PersistEnt *persist_find(uint32_t key) {
  for (int i = 0; i < PERSIST_MAX; i++) if (g_persist[i].used && g_persist[i].key == key) return &g_persist[i];
  return NULL;
}
bool persist_exists(const uint32_t key) { return persist_find(key) != NULL; }
int persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size) {
  PersistEnt *e = persist_find(key);
  if (!e) return -1;
  int n = e->len < (int)buffer_size ? e->len : (int)buffer_size;
  memcpy(buffer, e->buf, n);
  return n;
}
int persist_write_data(const uint32_t key, const void *data, const size_t size) {
  PersistEnt *e = persist_find(key);
  if (!e) for (int i = 0; i < PERSIST_MAX; i++) if (!g_persist[i].used) { e = &g_persist[i]; e->used = true; e->key = key; break; }
  if (!e) return -1;
  int n = size < sizeof e->buf ? (int)size : (int)sizeof e->buf;
  memcpy(e->buf, data, n);
  e->len = n;
  return n;
}

// ---- dict read/write ----
static FakeDict g_outbox;          // the watch's current outbox
static FakeDict g_dummy;           // passed to *_sent/*_failed (storage ignores it)

AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator) {
  g_outbox.n = 0;
  *iterator = &g_outbox;
  return APP_MSG_OK;
}
static FakeTuple *dict_put(DictionaryIterator *it, uint32_t key) {
  FakeDict *d = it;
  FakeTuple *t = &d->t[d->n++];
  t->key = key; t->is_data = 0; t->length = 0;
  return t;
}
void dict_write_uint8(DictionaryIterator *it, uint32_t key, uint8_t value) {
  FakeTuple *t = dict_put(it, key); t->v.uint8 = value; t->length = 1;
}
void dict_write_uint32(DictionaryIterator *it, uint32_t key, uint32_t value) {
  FakeTuple *t = dict_put(it, key); t->v.uint32 = value; t->length = 4;
}
void dict_write_data(DictionaryIterator *it, uint32_t key, const uint8_t *data, uint16_t size) {
  FakeTuple *t = dict_put(it, key);
  if (size > FAKE_MAX_DATA) size = FAKE_MAX_DATA;
  memcpy(t->buf, data, size); t->is_data = 1; t->length = size;
}

// dict_find returns from a small ring of scratch Tuples so a handler can hold
// several results at once (e.g. ACK reads st_ack and st_total together). Data
// pointers are re-derived from the live FakeTuple buffer, never stored.
Tuple *dict_find(const DictionaryIterator *it, uint32_t key) {
  const FakeDict *d = it;
  for (int i = 0; i < d->n; i++) {
    if (d->t[i].key != key) continue;
    static Tuple ring[8];
    static TupleValue val[8];
    static int r = 0;
    int s = r++ & 7;
    if (d->t[i].is_data) val[s].data = d->t[i].buf;
    else                 val[s] = d->t[i].v;
    ring[s].value = &val[s];
    ring[s].length = d->t[i].length;
    return &ring[s];
  }
  return NULL;
}
// Direct FakeTuple lookup for the phone side.
static FakeTuple *ft_find(FakeDict *d, uint32_t key) {
  for (int i = 0; i < d->n; i++) if (d->t[i].key == key) return &d->t[i];
  return NULL;
}

// ---- registered callbacks ----
static AppMessageInboxReceived cb_recv;
static AppMessageOutboxSent    cb_sent;
static AppMessageOutboxFailed  cb_failed;
void app_message_register_inbox_received(AppMessageInboxReceived cb) { cb_recv = cb; }
void app_message_register_inbox_dropped(AppMessageInboxDropped cb)   { (void)cb; }
void app_message_register_outbox_sent(AppMessageOutboxSent cb)       { cb_sent = cb; }
void app_message_register_outbox_failed(AppMessageOutboxFailed cb)   { cb_failed = cb; }
AppMessageResult app_message_open(uint32_t in, uint32_t out)         { (void)in; (void)out; return APP_MSG_OK; }
uint32_t app_message_inbox_size_maximum(void)  { return 512; }
uint32_t app_message_outbox_size_maximum(void) { return 512; }

// ---- app timers (never fire on their own; tests fire them via fake_fire_timers) ----
#define TIMER_MAX 8
struct AppTimer { bool used; AppTimerCallback cb; void *data; };
static struct AppTimer g_timers[TIMER_MAX];
AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback cb, void *data) {
  (void)timeout_ms;
  for (int i = 0; i < TIMER_MAX; i++) if (!g_timers[i].used) {
    g_timers[i].used = true; g_timers[i].cb = cb; g_timers[i].data = data;
    return &g_timers[i];
  }
  return NULL;
}
void app_timer_cancel(AppTimer *t) { if (t) t->used = false; }
int fake_fire_timers(void) {
  int n = 0;
  for (int i = 0; i < TIMER_MAX; i++) if (g_timers[i].used) {
    g_timers[i].used = false;                       // one-shot
    g_timers[i].cb(g_timers[i].data);
    n++;
  }
  return n;
}

// ---- connection ----
static bool g_connected;
static void (*g_conn_handler)(bool);
bool connection_service_peek_pebble_app_connection(void) { return g_connected; }
void connection_service_subscribe(ConnectionHandlers h) { g_conn_handler = h.pebble_app_connection_handler; }

// ---- transport queues ----
static bool g_outbox_pending;
AppMessageResult app_message_outbox_send(void) { g_outbox_pending = true; return APP_MSG_OK; }
bool fake_outbox_pending(void) { return g_outbox_pending; }

#define INBOX_MAX 16
static FakeDict g_inbox[INBOX_MAX];
static int g_inbox_n;
static FakeDict *inbox_new(void) {
  FakeDict *d = &g_inbox[g_inbox_n++];
  d->n = 0;
  return d;
}
static void i_u8(FakeDict *d, uint32_t k, uint8_t v)  { FakeTuple *t = &d->t[d->n++]; t->key = k; t->v.uint8 = v; t->length = 1; t->is_data = 0; }
static void i_u32(FakeDict *d, uint32_t k, uint32_t v){ FakeTuple *t = &d->t[d->n++]; t->key = k; t->v.uint32 = v; t->length = 4; t->is_data = 0; }
static void i_data(FakeDict *d, uint32_t k, const uint8_t *b, int len) {
  FakeTuple *t = &d->t[d->n++]; t->key = k;
  if (len > FAKE_MAX_DATA) len = FAKE_MAX_DATA;
  memcpy(t->buf, b, len); t->v.data = t->buf; t->length = len; t->is_data = 1;
}

// =============================================================================
// Fake phone — a C port of src/pkjs/storage.js (opaque records keyed by seq).
// =============================================================================
#define PHONE_MAX 64
static uint32_t ph_seq[PHONE_MAX];                 // ascending
static uint8_t  ph_rec[PHONE_MAX][FAKE_MAX_DATA];
static int      ph_reclen[PHONE_MAX];
static int      ph_n;
static uint8_t  ph_aux[FAKE_MAX_DATA];
static int      ph_auxlen;
static bool     ph_has_aux;
static uint32_t ph_epoch;           // archive identity (storage.js `_epoch`)
static bool     ph_reload_pending;  // a restore owes the watch a RELOAD (storage.js `:reload`)
static bool     ph_reload_stuck;    // the RELOAD's delivery ack never arrives — resends
                                    //   never settle the owed marker (a flaky send callback)
static int      ph_schema = -1;     // records' schema; -1 = none stored (storage.js `:schema`)

static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void     wr_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int ph_index(uint32_t seq) { for (int i = 0; i < ph_n; i++) if (ph_seq[i] == seq) return i; return -1; }
static void ph_insert(uint32_t seq, const uint8_t *rec, int len) {
  int idx = ph_index(seq);
  if (idx < 0) {                                   // keep ascending
    int p = ph_n;
    while (p > 0 && ph_seq[p - 1] > seq) { ph_seq[p] = ph_seq[p - 1]; memcpy(ph_rec[p], ph_rec[p - 1], FAKE_MAX_DATA); ph_reclen[p] = ph_reclen[p - 1]; p--; }
    ph_seq[p] = seq; idx = p; ph_n++;
  }
  memcpy(ph_rec[idx], rec, len); ph_reclen[idx] = len;
}
static void ph_remove(uint32_t seq) {
  int idx = ph_index(seq);
  if (idx < 0) return;
  for (int i = idx; i + 1 < ph_n; i++) { ph_seq[i] = ph_seq[i + 1]; memcpy(ph_rec[i], ph_rec[i + 1], FAKE_MAX_DATA); ph_reclen[i] = ph_reclen[i + 1]; }
  ph_n--;
}
static void ph_send_ack(void) {
  FakeDict *d = inbox_new();
  i_u8(d, MESSAGE_KEY_st_type, ST_ACK);
  i_u32(d, MESSAGE_KEY_st_epoch, ph_epoch);
  i_u32(d, MESSAGE_KEY_st_ack, ph_n ? ph_seq[ph_n - 1] : 0);
  i_u32(d, MESSAGE_KEY_st_total, ph_n);
}
static void ph_send_reload(void) {
  FakeDict *d = inbox_new();
  i_u8(d, MESSAGE_KEY_st_type, ST_RELOAD);
  i_u32(d, MESSAGE_KEY_st_epoch, ph_epoch);
  i_u32(d, MESSAGE_KEY_st_ack, ph_n ? ph_seq[ph_n - 1] : 0);
  i_u32(d, MESSAGE_KEY_st_total, ph_n);
}

static void phone_handle(FakeDict *msg) {
  FakeTuple *tp = ft_find(msg, MESSAGE_KEY_st_type);
  if (!tp) return;
  // A restore still owes the watch its RELOAD (storage.js handle()). A message
  // carrying the current epoch proves the watch has adopted the import — the
  // owed marker settles on that evidence and the message is served normally.
  // Anything else is dropped and the RELOAD re-sent; queuing counts as
  // delivered in the fake transport, so the marker clears here — unless a test
  // models a lost delivery ack (ph_reload_stuck / fake_inbox_clear).
  if (ph_reload_pending) {
    FakeTuple *et = ft_find(msg, MESSAGE_KEY_st_epoch);
    if (et && et->v.uint32 == ph_epoch) {
      ph_reload_pending = false;                     // proof of adoption — serve the message
    } else {
      ph_send_reload();
      if (!ph_reload_stuck) ph_reload_pending = false;
      return;
    }
  }
  switch (tp->v.uint8) {
    case ST_PUSH: {
      FakeTuple *et = ft_find(msg, MESSAGE_KEY_st_epoch);
      if (!et || et->v.uint32 != ph_epoch) { ph_send_ack(); break; }  // stale-epoch write refused
      FakeTuple *dt = ft_find(msg, MESSAGE_KEY_st_data);
      FakeTuple *ct = ft_find(msg, MESSAGE_KEY_st_count);
      if (dt && ct && ct->v.uint8) {
        int count = ct->v.uint8;
        int frame = dt->length / count;
        int recsize = frame - 4;
        for (int i = 0; i < count; i++) {
          const uint8_t *f = dt->buf + i * frame;
          ph_insert(rd_u32(f), f + 4, recsize);
        }
        FakeTuple *st = ft_find(msg, MESSAGE_KEY_st_schema);
        if (st) ph_schema = st->v.uint8;
      }
      ph_send_ack();
      break;
    }
    case ST_GET: {
      uint32_t off = ft_find(msg, MESSAGE_KEY_st_offset)->v.uint32;
      int want = ft_find(msg, MESSAGE_KEY_st_count)->v.uint8;
      uint32_t picked[PHONE_MAX]; int np = 0;
      for (int k = 0; k < want; k++) {
        int idx = ph_n - 1 - (int)(off + k);       // newest-first
        if (idx < 0) break;
        picked[np++] = ph_seq[idx];
      }
      FakeDict *d = inbox_new();
      i_u8(d, MESSAGE_KEY_st_type, ST_PAGE);
      i_u32(d, MESSAGE_KEY_st_epoch, ph_epoch);
      if (ph_schema >= 0) i_u8(d, MESSAGE_KEY_st_schema, (uint8_t)ph_schema);
      i_u8(d, MESSAGE_KEY_st_count, (uint8_t)np);
      i_u32(d, MESSAGE_KEY_st_offset, off);
      i_u32(d, MESSAGE_KEY_st_total, ph_n);
      if (np) {
        uint8_t bytes[FAKE_MAX_DATA]; int bn = 0;
        for (int i = 0; i < np; i++) {
          int idx = ph_index(picked[i]);
          wr_u32(bytes + bn, picked[i]); bn += 4;
          memcpy(bytes + bn, ph_rec[idx], ph_reclen[idx]); bn += ph_reclen[idx];
        }
        i_data(d, MESSAGE_KEY_st_data, bytes, bn);
      }
      break;
    }
    case ST_DEL: {
      FakeTuple *et = ft_find(msg, MESSAGE_KEY_st_epoch);
      if (!et || et->v.uint32 != ph_epoch) { ph_send_ack(); break; }  // stale-epoch write refused
      ph_remove(ft_find(msg, MESSAGE_KEY_st_offset)->v.uint32);
      ph_send_ack();
      break;
    }
    case ST_WIPE: {
      ph_n = 0; ph_has_aux = false; ph_auxlen = 0;
      ph_send_ack();
      break;
    }
    case ST_AUX: {
      FakeTuple *dt = ft_find(msg, MESSAGE_KEY_st_data);
      if (dt && dt->length) { memcpy(ph_aux, dt->buf, dt->length); ph_auxlen = dt->length; ph_has_aux = true; }
      else { ph_has_aux = false; ph_auxlen = 0; }
      break;                                        // AUX is fire-and-forget (no ack)
    }
    default: break;
  }
}

// ---- channel driving ----
static void deliver_inbox(void) {
  int target = g_inbox_n;                           // process only what's queued now
  int done = 0;
  while (done < target && g_inbox_n > 0) {
    FakeDict m = g_inbox[0];                         // copy (queue may shift / grow)
    for (int i = 1; i < g_inbox_n; i++) g_inbox[i - 1] = g_inbox[i];
    g_inbox_n--;
    if (cb_recv) cb_recv(&m, NULL);
    done++;
  }
}
int fake_channel_step(void) {
  int did = 0;
  if (g_connected && g_outbox_pending) {
    g_outbox_pending = false;
    phone_handle(&g_outbox);                         // may enqueue replies
    if (cb_sent) cb_sent(&g_dummy, NULL);            // may pump → new pending
    did = 1;
  }
  if (g_inbox_n) { deliver_inbox(); did = 1; }
  return did;
}
void fake_channel_drain(void) {
  for (int i = 0; i < 100; i++) if (!fake_channel_step()) break;
}
void fake_deliver_outbox_only(void) {
  if (!(g_connected && g_outbox_pending)) return;
  g_outbox_pending = false;
  phone_handle(&g_outbox);                            // phone replies, but we leave it queued
  if (cb_sent) cb_sent(&g_dummy, NULL);
}
void fake_deliver_one_inbox(void) {
  if (!g_inbox_n) return;
  FakeDict m = g_inbox[0];
  for (int i = 1; i < g_inbox_n; i++) g_inbox[i - 1] = g_inbox[i];
  g_inbox_n--;
  if (cb_recv) cb_recv(&m, NULL);
}
int fake_inbox_depth(void) { return g_inbox_n; }
void fake_channel_drop_send(void) {
  if (!g_outbox_pending) return;
  g_outbox_pending = false;
  if (cb_failed) cb_failed(&g_dummy, APP_MSG_SEND_REJECTED, NULL);
}
void fake_channel_drop_reply(void) {
  if (!(g_connected && g_outbox_pending)) return;
  g_outbox_pending = false;
  phone_handle(&g_outbox);                            // phone acts...
  g_inbox_n = 0;                                       // ...but its reply is lost
  if (cb_sent) cb_sent(&g_dummy, NULL);
}

void fake_set_connected(bool connected, bool fire) {
  bool changed = connected != g_connected;
  g_connected = connected;
  if (fire && changed && g_conn_handler) g_conn_handler(connected);
}

// ---- phone inspection / control ----
int  fake_phone_count(void)            { return ph_n; }
bool fake_phone_has_seq(uint32_t seq)  { return ph_index(seq) >= 0; }
uint32_t fake_phone_highest_seq(void)  { return ph_n ? ph_seq[ph_n - 1] : 0; }
int  fake_phone_aux_len(void)          { return ph_has_aux ? ph_auxlen : -1; }

void fake_phone_import(const uint32_t *seqs, const uint8_t *recs, int n, int rec_size) {
  ph_n = 0;
  for (int i = 0; i < n; i++) ph_insert(seqs[i], recs + i * rec_size, rec_size);
  ph_epoch += 1;                                      // restore() mints a fresh identity
  ph_reload_pending = true;                           // ...and owes the watch a RELOAD
  ph_send_reload();
  ph_reload_pending = false;                          // queued = delivered in the fake transport
}
void fake_phone_set_reload_pending(void) { ph_reload_pending = true; }
void fake_phone_set_reload_stuck(void)   { ph_reload_pending = true; ph_reload_stuck = true; }
void fake_phone_resend_reload(void)      { ph_send_reload(); }
void fake_phone_set_schema(int schema)   { ph_schema = schema; }
void fake_phone_lose_storage(void) {
  // The phone app was reinstalled / its localStorage cleared: archive and aux are
  // gone and the lazily-minted epoch comes up different.
  ph_n = 0; ph_has_aux = false; ph_auxlen = 0;
  ph_epoch += 101;
}
void fake_phone_hello(void) {
  FakeDict *d = inbox_new();
  i_u8(d, MESSAGE_KEY_st_type, ST_HELLO);
  i_u32(d, MESSAGE_KEY_st_epoch, ph_epoch);
  i_u32(d, MESSAGE_KEY_st_ack, ph_n ? ph_seq[ph_n - 1] : 0);
  i_u32(d, MESSAGE_KEY_st_total, ph_n);
}
void fake_inbox_clear(void) { g_inbox_n = 0; }
void fake_phone_request_wipe(void) {
  FakeDict *d = inbox_new();
  i_u8(d, MESSAGE_KEY_st_type, ST_WIPE_REQ);
}
void fake_phone_send_aux(const uint8_t *data, int len) {
  memcpy(ph_aux, data, len); ph_auxlen = len; ph_has_aux = true;
  FakeDict *d = inbox_new();
  i_u8(d, MESSAGE_KEY_st_type, ST_AUX);
  i_data(d, MESSAGE_KEY_st_data, data, len);
}

void fake_init(void) {
  memset(g_persist, 0, sizeof g_persist);
  memset(&g_outbox, 0, sizeof g_outbox);
  g_outbox_pending = false;
  g_inbox_n = 0;
  cb_recv = NULL; cb_sent = NULL; cb_failed = NULL;
  g_connected = false; g_conn_handler = NULL;
  ph_n = 0; ph_auxlen = 0; ph_has_aux = false;
  ph_epoch = 0xE0C4; ph_reload_pending = false; ph_reload_stuck = false; ph_schema = -1;
  memset(g_timers, 0, sizeof g_timers);
}
