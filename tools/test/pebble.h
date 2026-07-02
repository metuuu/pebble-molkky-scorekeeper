#pragma once
// =============================================================================
// Host shim for the slice of the Pebble SDK that src/c/lib/storage/storage.c
// uses. This lets the *unmodified* library compile and run on a normal machine
// so the send pump can be driven through tricky connect/disconnect/drop
// interleavings under a fake phone. See fake_pebble.c for the implementation and
// test_storage.c for the scenarios.
//
// Only the surface storage.c actually touches is modelled (persist, AppMessage,
// connection service). It is deliberately NOT a general Pebble emulator.
// =============================================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// ---- AppMessage result codes (only OK is inspected by storage.c) ----
typedef enum {
  APP_MSG_OK = 0,
  APP_MSG_SEND_TIMEOUT,
  APP_MSG_SEND_REJECTED,
  APP_MSG_NOT_CONNECTED,
  APP_MSG_BUSY,
} AppMessageResult;

// ---- Dictionary / tuple model ----
// A tuple's value is read by storage.c as ->value->uint8 / ->value->uint32 /
// ->value->data, with the byte length in ->length.
typedef union {
  uint8_t        uint8;
  uint32_t       uint32;
  const uint8_t *data;
} TupleValue;

typedef struct {
  TupleValue *value;
  uint16_t    length;
} Tuple;

// Backing storage for one written tuple. `data` payloads live in `buf` and are
// re-derived on read (the dict may be copied by value, so a stored pointer would
// dangle — see dict_find in fake_pebble.c).
#define FAKE_MAX_TUPLES 8
#define FAKE_MAX_DATA   512
typedef struct {
  uint32_t key;
  uint16_t length;
  uint8_t  is_data;
  TupleValue v;                 // scalar payloads (uint8/uint32)
  uint8_t  buf[FAKE_MAX_DATA];  // data payloads
} FakeTuple;

typedef struct {
  FakeTuple t[FAKE_MAX_TUPLES];
  int       n;
} FakeDict;

// storage.c only ever holds a DictionaryIterator*, so aliasing it to FakeDict is
// enough for the shim.
typedef FakeDict DictionaryIterator;

// ---- persist ----
bool persist_exists(const uint32_t key);
int  persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size);
int  persist_write_data(const uint32_t key, const void *data, const size_t size);

// ---- AppMessage ----
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator);
AppMessageResult app_message_outbox_send(void);
void dict_write_uint8(DictionaryIterator *it, uint32_t key, uint8_t value);
void dict_write_uint32(DictionaryIterator *it, uint32_t key, uint32_t value);
void dict_write_data(DictionaryIterator *it, uint32_t key, const uint8_t *data, uint16_t size);
Tuple *dict_find(const DictionaryIterator *it, uint32_t key);

typedef void (*AppMessageInboxReceived)(DictionaryIterator *iterator, void *context);
typedef void (*AppMessageInboxDropped)(AppMessageResult reason, void *context);
typedef void (*AppMessageOutboxSent)(DictionaryIterator *iterator, void *context);
typedef void (*AppMessageOutboxFailed)(DictionaryIterator *iterator, AppMessageResult reason, void *context);

void app_message_register_inbox_received(AppMessageInboxReceived cb);
void app_message_register_inbox_dropped(AppMessageInboxDropped cb);
void app_message_register_outbox_sent(AppMessageOutboxSent cb);
void app_message_register_outbox_failed(AppMessageOutboxFailed cb);
AppMessageResult app_message_open(uint32_t size_inbound, uint32_t size_outbound);
uint32_t app_message_inbox_size_maximum(void);
uint32_t app_message_outbox_size_maximum(void);

// ---- app timer (storage.c's reply watchdog) ----
typedef struct AppTimer AppTimer;
typedef void (*AppTimerCallback)(void *data);
AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *callback_data);
void      app_timer_cancel(AppTimer *timer);

// ---- connection service ----
typedef struct {
  void (*pebble_app_connection_handler)(bool connected);
  void (*pebble_bluetooth_connection_handler)(bool connected);
} ConnectionHandlers;

bool connection_service_peek_pebble_app_connection(void);
void connection_service_subscribe(ConnectionHandlers handlers);
