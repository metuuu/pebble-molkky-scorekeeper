# Host tests for the synced storage library

These tests compile the **real, unmodified** `src/c/lib/storage/storage.c` on a
normal machine and drive its send pump through the connect/disconnect/drop
interleavings that are hard to reach on a watch. They are the safety net that has
to exist before the `pump()` state machine is refactored.

## Running

```sh
cd tools/test
make test      # build + run
make clean
```

Requires only a C compiler (`cc`/`gcc`). No Pebble SDK.

## How it works

`storage.c` depends only on `pebble.h`, so that SDK boundary *is* the seam — no
production code is changed.

- **`pebble.h`** — a host shim of the slice of the SDK the library uses (persist,
  AppMessage, connection service).
- **`fake_pebble.c`** — in-memory implementations plus a faithful C port of the
  phone side (`src/pkjs/storage.js`): opaque records keyed by `seq`, idempotent
  push, re-ack on delete, offset paging. The transport is explicit so a test
  controls exactly when a message is delivered, lost on send, or lost on reply.
- **`test_storage.c`** — the scenarios. Each runs in its own forked process so the
  library's file-scope statics start clean (which also lets a test simulate an
  app restart by re-`storage_open()`-ing over surviving persist).

## Scenarios

`basic_sync`, `offline_buffers_without_sending`, `lost_send_requeues`,
`blocked_when_cache_full`, `tombstone_survives_restart`, `wipe_preempts`,
`reload_realigns_and_refills`, `load_page`, plus two **documented gaps** below.

## Gaps this harness surfaced

Two real robustness bugs in the current pump, captured as passing tests that
assert the *actual* (imperfect) behaviour with a loud comment. Both are good
targets for the pump refactor:

1. **`offline_backlog_only_partly_flushes`** — a reconnect pushes only the first
   batch. `pump()` clears `s_want_push` after that batch, and the post-ACK pump
   has no page/tombstone pending, so the rest of a multi-batch offline backlog
   does not drain on its own. In the app (`max_page=8`, `cache_capacity=20`) this
   bites after >8 games are recorded away from the phone; the remainder waits for
   the next trigger (new game / reconnect / opening History).

2. **`lost_ack_is_stuck`** — if a push reaches the phone but its ACK is lost, the
   watch already saw `outbox_sent`, so `s_await_ack` stays set and no retry is
   made, even across a reconnect. The data is safe on the phone (no duplication,
   thanks to idempotent push), but the watch reports it unsynced indefinitely. A
   reconnect should reconcile against the phone's ack.
