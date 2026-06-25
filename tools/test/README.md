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

`basic_sync`, `offline_buffers_without_sending`, `offline_backlog_fully_flushes`,
`lost_send_requeues`, `lost_ack_heals_on_reconnect`, `blocked_when_cache_full`,
`tombstone_survives_restart`, `wipe_preempts`, `reload_realigns_and_refills`,
`reset_during_inflight_read_discards_stale_page`, `load_page`.

## The pump these tests guard

`storage.c`'s send pump is a table of work sources (`begin_wipe`, `begin_push`,
`begin_del`, `begin_page`, `begin_aux`, `begin_refill`) tried in priority order;
one job is in flight at a time (`s_inflight`), held until its completion signal —
an ACK for push/delete/wipe, a PAGE for the reads, outbox-sent for the
ack-less aux. A disconnect or a reset abandons the in-flight job and the next pump
re-drives it; every job is idempotent on the phone, so retrying never loses or
duplicates data.

## Two bugs this harness caught and the refactor fixed

These started as failing/known-gap tests and now assert the corrected behaviour:

1. **Multi-batch offline backlog half-flushed on reconnect** — the old pump
   cleared a `s_want_push` latch after the first batch. Removed: PUSH is ready
   whenever `unsynced_count() > 0`, so the backlog now drains to completion on a
   single reconnect (`offline_backlog_fully_flushes`).

2. **A lost ACK wedged a record as permanently "unsynced"** — the old `s_await_ack`
   never cleared if the ACK was dropped after delivery. Now a reconnect abandons
   the dangling transaction and re-pushes (idempotent), reconciling it to synced
   (`lost_ack_heals_on_reconnect`).
