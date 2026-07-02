#pragma once
// =============================================================================
// Test-control surface for the host shim. The SDK-shaped functions live in
// pebble.h; these are the knobs a test uses to drive the fake transport and to
// inspect the fake phone's archive.
// =============================================================================
#include <stdint.h>
#include <stdbool.h>

// Reset all shim + phone state. Call once at the start of every test.
void fake_init(void);

// ---- connection ----
// Set the link state. When `fire` is true and the state changes, the registered
// pebble_app_connection_handler is invoked (as the real connection service would).
void fake_set_connected(bool connected, bool fire);

// ---- channel pumping ----
// Deliver one in-flight outbox message to the phone (if any), fire outbox_sent,
// then deliver any inbox messages the phone produced. Returns non-zero if it did
// anything. Models the normal happy path of one transport round-trip.
int  fake_channel_step(void);

// Run the channel until nothing is pending and the inbox is empty (bounded).
void fake_channel_drain(void);

// Fine-grained delivery, for constructing precise interleavings: deliver the
// in-flight outbox to the phone and fire outbox_sent, but leave the phone's reply
// queued; and deliver exactly one queued inbox message. Together they let a test
// drop a control message (e.g. a reset) in between a read and its reply.
void fake_deliver_outbox_only(void);
void fake_deliver_one_inbox(void);
int  fake_inbox_depth(void);
void fake_inbox_clear(void);          // every queued phone->watch message is lost

// A queued outbox message is lost in transit: fire outbox_failed, phone never
// sees it. (Models a drop / disconnect mid-send.)
void fake_channel_drop_send(void);

// The phone receives the message and acts on it, but its reply is lost: fire
// outbox_sent, discard the phone's reply. (Models a dropped ACK/PAGE.)
void fake_channel_drop_reply(void);

bool fake_outbox_pending(void);

// ---- timers ----
// The shim never fires app timers on its own; a test fires all armed ones to
// model a timeout elapsing. Returns how many fired.
int fake_fire_timers(void);

// ---- phone inspection ----
int  fake_phone_count(void);
bool fake_phone_has_seq(uint32_t seq);
uint32_t fake_phone_highest_seq(void);
int  fake_phone_aux_len(void);

// ---- phone-initiated control messages (queued; delivered on the next step) ----
// Replace the phone archive wholesale (simulates a phone-side import) then queue
// a RELOAD for the watch. `recs` is `n` records of `rec_size` bytes, oldest-first
// with seqs in `seqs`.
void fake_phone_import(const uint32_t *seqs, const uint8_t *recs, int n, int rec_size);
void fake_phone_request_wipe(void);   // queue WIPE_REQ
void fake_phone_send_aux(const uint8_t *data, int len); // store + queue AUX to watch
void fake_phone_hello(void);          // queue HELLO (as PKJS does on every launch)
void fake_phone_lose_storage(void);   // simulate reinstall: archive gone, new epoch
void fake_phone_set_reload_pending(void); // re-arm the owed-RELOAD guard (after a dropped RELOAD)
