// =============================================================================
// storage.js — phone-side mirror of the watch's generic synced store.
//
// The full archive lives here in localStorage (megabytes, and it survives app
// reinstalls — unlike the watch's 4 KB persist). Records are stored OPAQUELY,
// keyed by their monotonic `seq`; this side never interprets a record's bytes,
// which keeps it decoupled from the C struct layout. Protocol shapes:
//   PUSH     (watch->phone): store a batch of records, ACK the highest seq held.
//   GET      (watch->phone): return a page of records older than a cursor.
//   DEL      (watch->phone): forget a record by seq, then re-ACK.
//   RELOAD   (phone->watch): the archive was replaced here (import); tell the watch
//                            to drop its stale cache and reload page 0 from us.
//   WIPE_REQ (phone->watch): the settings page asked to wipe everything; the watch
//                            confirms with the user before acting.
//   WIPE     (watch->phone): clear the entire archive here (the watch confirmed).
//   AUX      (both ways): one opaque app blob (e.g. the player roster) the watch
//                         authors; mirrored here for backup, sent back on an import.
//   HELLO    (phone->watch): sent on every PKJS launch — announces the archive's
//                            epoch, highest seq and size (see hello()).
//
// Every phone->watch message carries the archive's `epoch`: a random identity
// minted when this storage is first seen (so a reinstall that lost localStorage
// presents a new one) and re-minted by restore(). The watch reconciles when the
// epoch it knows stops matching — that is what makes a lost RELOAD or a wiped
// phone recoverable instead of silently corrupting (see storage.c). Writes from
// the watch (PUSH/DEL) carry the watch's last-known epoch and are refused on a
// mismatch: stale seqs can never land in a replaced archive, however the
// messages cross on the wire. The refusal still ACKs, and that ACK's epoch is
// what triggers the watch to reconcile and re-send correctly.
//
// Generic by design: construct with a key prefix so one phone app can host
// several independent stores.
// =============================================================================

var TYPE = { PUSH: 1, ACK: 2, GET: 3, PAGE: 4, DEL: 5, RELOAD: 6, WIPE_REQ: 7, WIPE: 8, AUX: 9, HELLO: 10 };
var KEY = {
  type:   'st_type',
  schema: 'st_schema',
  count:  'st_count',
  offset: 'st_offset',
  data:   'st_data',
  ack:    'st_ack',
  total:  'st_total',
  epoch:  'st_epoch'
};

// ---- little-endian uint32 helpers over plain byte arrays ----
function readU32(arr, off) {
  return (arr[off] | (arr[off + 1] << 8) | (arr[off + 2] << 16) | (arr[off + 3] << 24)) >>> 0;
}
function pushU32(arr, v) {
  arr.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
}
function insertSorted(arr, v) {        // keep ascending, dedupe
  var lo = 0, hi = arr.length;
  while (lo < hi) { var mid = (lo + hi) >> 1; if (arr[mid] < v) lo = mid + 1; else hi = mid; }
  if (arr[lo] === v) return;
  arr.splice(lo, 0, v);
}

// ---- self-contained base64 (no btoa/atob dependency in the JS runtime) ----
var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function b64enc(bytes) {
  var out = '', i;
  for (i = 0; i + 2 < bytes.length; i += 3) {
    var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
  }
  var rem = bytes.length - i;
  if (rem === 1) {
    var a = bytes[i] << 16;
    out += B64[(a >> 18) & 63] + B64[(a >> 12) & 63] + '==';
  } else if (rem === 2) {
    var b = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += B64[(b >> 18) & 63] + B64[(b >> 12) & 63] + B64[(b >> 6) & 63] + '=';
  }
  return out;
}
function b64dec(str) {
  var bytes = [], i, buf = 0, bits = 0;
  for (i = 0; i < str.length; i++) {
    var c = str[i];
    if (c === '=') break;
    var v = B64.indexOf(c);
    if (v < 0) continue;
    buf = (buf << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; bytes.push((buf >> bits) & 0xff); }
  }
  return bytes;
}

// Retrying send for phone->watch messages. Control messages (RELOAD / AUX /
// WIPE_REQ / HELLO) aren't replies the watch waits for, so a dropped send doesn't
// self-heal; replies (ACK / PAGE) do have a watch-side watchdog, but retrying
// here settles them in milliseconds instead of a timeout later. Retry a few times
// on a transient failure (e.g. the channel is briefly busy). PKJS only runs while
// the watchapp is open, so if the watch is genuinely unreachable we log and give
// up rather than block. `done(ok)` fires exactly once: on success, or after the
// final failed attempt (so a caller can chain the next send either way).
function sendControl(msg, label, done) {
  var tries = 4;
  (function attempt() {
    Pebble.sendAppMessage(msg,
      function () { if (done) done(true); },
      function () {
        if (--tries > 0) { setTimeout(attempt, 300); return; }
        console.log('storage: ' + label + ' did not reach the watch');
        if (done) done(false);
      });
  })();
}

function Store(prefix) { this.p = prefix; }

Store.prototype._seqKey = function () { return this.p + ':seqs'; };
Store.prototype._recKey = function (seq) { return this.p + ':r:' + seq; };
Store.prototype._epochKey  = function () { return this.p + ':epoch'; };
Store.prototype._reloadKey = function () { return this.p + ':reload'; };

// The archive's identity. Minted lazily — so a fresh install, including one that
// lost its old localStorage, presents a new epoch — and re-minted on restore().
Store.prototype._epoch = function () {
  var e = parseInt(localStorage.getItem(this._epochKey()), 10);
  return e > 0 ? e : this._mintEpoch();
};
Store.prototype._mintEpoch = function () {
  var e = (Math.floor(Math.random() * 0xFFFFFFFE) + 1) >>> 0;   // nonzero uint32
  localStorage.setItem(this._epochKey(), String(e));
  return e;
};
Store.prototype._seqs = function () {
  var s = localStorage.getItem(this._seqKey());
  if (!s) return [];
  try {
    var a = JSON.parse(s);
    if (Array.isArray(a)) return a;
  } catch (e) { /* fall through to the rebuild */ }
  // A corrupt index would otherwise break every push AND the config page (so
  // not even a restore could heal it). The records are individually keyed, so
  // rebuild the index from them instead.
  var rebuilt = [], pre = this.p + ':r:';
  for (var i = 0; i < localStorage.length; i++) {
    var k = localStorage.key(i);
    if (k && k.indexOf(pre) === 0) insertSorted(rebuilt, (+k.slice(pre.length)) >>> 0);
  }
  this._saveSeqs(rebuilt);
  return rebuilt;
};
Store.prototype._saveSeqs = function (a) { localStorage.setItem(this._seqKey(), JSON.stringify(a)); };

// Dispatch an inbound AppMessage payload from the watch.
Store.prototype.handle = function (payload) {
  // A restore still owes the watch its RELOAD. Until that lands, the watch's
  // view of the archive predates the import: applying its writes (stale seqs
  // that collide with restored records) or answering its reads would corrupt or
  // confuse. Drop the message and re-send the RELOAD instead — the watch's own
  // retry machinery re-drives whatever it was doing once it has reconciled.
  if (localStorage.getItem(this._reloadKey())) { this._resendReload(); return; }
  switch (payload[KEY.type]) {
    case TYPE.PUSH: this._onPush(payload); break;
    case TYPE.GET:  this._onGet(payload);  break;
    case TYPE.DEL:  this._onDelete(payload); break;
    case TYPE.WIPE: this._onWipe(payload); break;
    case TYPE.AUX:  this._onAux(payload);  break;
    default: break;
  }
};

// The watch (after confirming on its screen) asked to clear the whole archive.
Store.prototype._onWipe = function (p) {
  this._clear();
  localStorage.removeItem(this.p + ':schema');
  localStorage.removeItem(this._auxKey());
  this._sendAck();                         // archive is now empty (ack 0, total 0)
};

// The watch pushed its aux blob (e.g. roster + stats). Mirror it for the backup.
Store.prototype._auxKey = function () { return this.p + ':aux'; };
Store.prototype._onAux = function (p) {
  var data = p[KEY.data];
  if (data == null) localStorage.removeItem(this._auxKey());
  else localStorage.setItem(this._auxKey(), b64enc(data));   // store opaquely, like records
};

// Send the stored aux blob to the watch (used on an import to restore the roster).
// No-op when there's none — an older backup without players leaves the roster as-is.
Store.prototype._sendAux = function () {
  var a = localStorage.getItem(this._auxKey());
  if (a == null) return;
  var msg = {};
  msg[KEY.type] = TYPE.AUX;
  msg[KEY.data] = b64dec(a);
  sendControl(msg, 'aux');
};

// Forget a record by seq (the seq rides in the offset slot). Idempotent: deleting
// an absent seq just re-ACKs the unchanged archive. ACK afterwards so the watch
// learns the new total (and so a tombstone for an already-gone game still settles).
Store.prototype._onDelete = function (p) {
  if (p[KEY.epoch] !== this._epoch()) { this._sendAck(); return; }   // stale-epoch write refused
  var seq = (p[KEY.offset] || 0) >>> 0;
  var seqs = this._seqs();
  var idx = seqs.indexOf(seq);
  if (idx >= 0) {
    seqs.splice(idx, 1);
    this._saveSeqs(seqs);
    localStorage.removeItem(this._recKey(seq));
  }
  this._sendAck();
};

Store.prototype._onPush = function (p) {
  if (p[KEY.epoch] !== this._epoch()) { this._sendAck(); return; }   // stale-epoch write refused
  var data = p[KEY.data], count = p[KEY.count];
  if (data && count) {
    var frame = data.length / count;     // every frame is [seq u32][record], equal size
    var recSize = frame - 4;
    var seqs = this._seqs();
    for (var i = 0; i < count; i++) {
      var off = i * frame;
      var seq = readU32(data, off);
      this._setRec(seq, data.slice(off + 4, off + 4 + recSize));
      insertSorted(seqs, seq);           // idempotent: re-pushed seq is a no-op
    }
    this._saveSeqs(seqs);
    if (p[KEY.schema] != null) localStorage.setItem(this.p + ':schema', String(p[KEY.schema]));
  }
  this._sendAck();
};

Store.prototype._setRec = function (seq, bytes) {
  localStorage.setItem(this._recKey(seq), b64enc(bytes));
};

Store.prototype._sendAck = function () {
  var seqs = this._seqs();
  var msg = {};
  msg[KEY.type] = TYPE.ACK;
  msg[KEY.epoch] = this._epoch();
  msg[KEY.ack] = seqs.length ? seqs[seqs.length - 1] : 0;   // highest seq we now hold
  msg[KEY.total] = seqs.length;                             // archive size, for the pager
  sendControl(msg, 'ack');
};

// Announce the archive identity and size. Called on every PKJS launch (the
// watchapp just opened): a wiped phone, or a restore whose RELOAD never landed,
// is exposed to the watch here — before any stale-assumption traffic flows.
Store.prototype.hello = function () {
  if (localStorage.getItem(this._reloadKey())) { this._resendReload(); return; }
  var seqs = this._seqs();
  var msg = {};
  msg[KEY.type] = TYPE.HELLO;
  msg[KEY.epoch] = this._epoch();
  msg[KEY.ack] = seqs.length ? seqs[seqs.length - 1] : 0;
  msg[KEY.total] = seqs.length;
  sendControl(msg, 'hello');
};

// Tell the watch the archive was replaced wholesale from this side (an import).
// Unlike an ACK — which only advances the watch's view of what it pushed — RELOAD
// makes the phone authoritative: the watch drops its stale cache, realigns its seq
// space to ours (so a record it never pushed can't collide with the restored data),
// and reloads page 0 from us.
Store.prototype._sendReload = function (done) {
  var seqs = this._seqs();
  var msg = {};
  msg[KEY.type] = TYPE.RELOAD;
  msg[KEY.epoch] = this._epoch();
  msg[KEY.ack] = seqs.length ? seqs[seqs.length - 1] : 0;   // highest seq we now hold (0 if empty)
  msg[KEY.total] = seqs.length;
  sendControl(msg, 'reload', done);
};

// Deliver the RELOAD a restore owes the watch (see restore() and handle()); the
// pending marker is cleared only once the send is confirmed delivered, and the
// roster follows so the two never race on the single in-flight channel.
Store.prototype._resendReload = function () {
  if (this._reloadInFlight) return;                         // one attempt at a time
  var self = this;
  this._reloadInFlight = true;
  this._sendReload(function (ok) {
    self._reloadInFlight = false;
    if (!ok) return;                                        // still owed; retried on the next trigger
    localStorage.removeItem(self._reloadKey());
    self._sendAux();
  });
};

// Ask the watch to wipe everything. The watch confirms with the user and, if
// accepted, sends WIPE back — which _onWipe clears the archive for. We deliberately
// don't clear here: nothing is deleted until the user confirms on the watch.
Store.prototype.requestWipe = function () {
  var msg = {};
  msg[KEY.type] = TYPE.WIPE_REQ;
  sendControl(msg, 'wipe-request');
};

// Offset paging over the global newest-first list. The watch always syncs before
// fetching, so by here the archive is complete and a page is a pure offset slice.
// ---- backup / restore (generic, opaque) ------------------------------------
// snapshot() and restore() are the canonical export/import format: records stay
// opaque (the same base64 we hold), keyed by seq, so a round-trip is lossless
// and this stays decoupled from any record's byte layout. An app that wants a
// human-readable view decodes the records on top of snapshot() (see
// molkky_history.js) without this layer needing to know the struct.

// Whole-archive snapshot. Shape mirrors what restore() consumes. `aux` carries the
// opaque app blob (e.g. the roster) verbatim so a backup includes the players too.
Store.prototype.snapshot = function () {
  var seqs = this._seqs(), recs = [];
  for (var i = 0; i < seqs.length; i++) {
    var d = localStorage.getItem(this._recKey(seqs[i]));
    if (d != null) recs.push({ seq: seqs[i], data: d });   // data already base64
  }
  var schema = localStorage.getItem(this.p + ':schema');
  var aux = localStorage.getItem(this._auxKey());          // base64 or null
  return { store: this.p, schema: schema == null ? null : Number(schema),
           records: recs, aux: aux == null ? null : aux };
};

Store.prototype._clear = function () {
  var seqs = this._seqs();
  for (var i = 0; i < seqs.length; i++) localStorage.removeItem(this._recKey(seqs[i]));
  this._saveSeqs([]);
};

// Upgrade every stored record written under an older app schema, in place.
// The app calls this on every launch (this JS ships inside the watchapp bundle,
// so `appSchema` is always the schema the watch runs): after an app update the
// archive would otherwise keep serving old-layout records, which the watch
// refuses — history would read as empty against a correct total, forever.
// `fn(bytes, from)` returns the upgraded bytes or null when that schema can't
// be read; any null aborts with the archive untouched (the watch keeps refusing
// its pages, which stays safe). Records are upgraded in memory first, so a
// conversion failure can't half-migrate; an interrupted write pass re-runs on
// the next launch (fn must be idempotent — see molkky_history.migrateRecord).
// No-op when the schemas already match or nothing is stored. True if migrated.
Store.prototype.migrateSchema = function (appSchema, fn) {
  var cur = localStorage.getItem(this.p + ':schema');
  if (cur == null || appSchema == null || Number(cur) === Number(appSchema)) return false;
  var from = Number(cur), seqs = this._seqs(), staged = [];
  for (var i = 0; i < seqs.length; i++) {
    var d = localStorage.getItem(this._recKey(seqs[i]));
    if (d == null) continue;                       // dangling seq — nothing to upgrade
    var nb = fn(b64dec(d), from);
    if (!nb) return false;                         // unknown schema — leave everything as-is
    staged.push({ seq: seqs[i], data: b64enc(nb) });
  }
  for (var j = 0; j < staged.length; j++)
    localStorage.setItem(this._recKey(staged[j].seq), staged[j].data);
  localStorage.setItem(this.p + ':schema', String(appSchema));
  return true;
};

// Load a snapshot() as a full replace (the only import mode): wipe the archive,
// then restore records + the aux blob (players). Pushes the games (RELOAD) and the
// roster (AUX) to the watch so the restore shows up there too. Returns the new
// record count. Throws on a malformed snapshot — WITHOUT having touched the live
// archive (see the two-pass note below).
Store.prototype.restore = function (snap) {
  if (!snap || !Array.isArray(snap.records)) throw new Error('invalid snapshot');
  // Schema compatibility is the caller's job: records are opaque here, so only
  // the app layer can upgrade an older backup or refuse an unreadable one (see
  // molkky_history.migrateSnapshot — run it on `snap` before calling this).
  // Comparing against the archive being replaced would be the wrong guard
  // anyway: it can be stale (pre-update records) or empty, while what matters
  // is whether the *watch* can read what lands here.
  // Pass 1 — validate every record up front, before clearing anything. A malformed
  // entry partway through must not leave us half-wiped: if we cleared first and then
  // threw, the existing archive would be gone and the new data only partly written.
  var clean = [], seqs = [];
  for (var i = 0; i < snap.records.length; i++) {
    var r = snap.records[i];
    if (!r || typeof r.data !== 'string' || r.seq == null) throw new Error('invalid record at ' + i);
    var seq = r.seq >>> 0;
    clean.push({ seq: seq, data: r.data });
    insertSorted(seqs, seq);
  }
  // Pass 2 — stage the records before touching the live archive. localStorage
  // writes can throw (quota); staged first, a failure here aborts the restore
  // with the current archive fully intact. Leftover stage keys from an earlier
  // interrupted run are swept first.
  var self = this;
  function stageKey(seq) { return self.p + ':stage:' + seq; }
  for (var w = localStorage.length - 1; w >= 0; w--) {
    var sk = localStorage.key(w);
    if (sk && sk.indexOf(this.p + ':stage:') === 0) localStorage.removeItem(sk);
  }
  try {
    for (var s = 0; s < clean.length; s++) localStorage.setItem(stageKey(clean[s].seq), clean[s].data);
  } catch (e) {
    for (var u = 0; u < clean.length; u++) localStorage.removeItem(stageKey(clean[u].seq));
    throw new Error('not enough storage for the backup');
  }
  // Pass 3 — commit. Mark the RELOAD as owed BEFORE touching the archive: if
  // the watch is unreachable (or we die) before it lands, handle() keeps
  // dropping inbound traffic and re-sending it — and hello() retries on the
  // next launch — so the watch can't write into the restored data under its
  // stale seq space.
  localStorage.setItem(this._reloadKey(), '1');
  this._clear();
  for (var j = 0; j < clean.length; j++) {
    localStorage.setItem(this._recKey(clean[j].seq), clean[j].data);   // base64 stored verbatim
    localStorage.removeItem(stageKey(clean[j].seq));                   // each move shrinks usage
  }
  this._saveSeqs(seqs);
  if (snap.schema != null) localStorage.setItem(this.p + ':schema', String(snap.schema));
  else localStorage.removeItem(this.p + ':schema');
  // Only replace the roster mirror when the backup actually carries one. A backup
  // taken before players were synced (no aux blob) leaves the existing roster as-is,
  // matching the watch — whose AUX restore is a no-op then — so the two sides agree
  // instead of the phone silently dropping names the watch still holds.
  if (typeof snap.aux === 'string') localStorage.setItem(this._auxKey(), snap.aux);
  // The archive changed identity: mint a fresh epoch, then reconcile the watch
  // with a RELOAD (adopt the new archive, reload page 0) followed by an AUX (the
  // restored roster + stats). _resendReload chains the two and clears the owed
  // marker only once the RELOAD is confirmed delivered.
  this._mintEpoch();
  this._resendReload();
  return seqs.length;
};

Store.prototype._onGet = function (p) {
  var offset = p[KEY.offset] || 0;       // 0 = newest
  var want = p[KEY.count] || 1;
  var seqs = this._seqs();                // ascending; newest is at the end
  var total = seqs.length;

  var picked = [];                        // newest-first
  for (var k = 0; k < want; k++) {
    var idx = total - 1 - (offset + k);   // newest-first index → ascending index
    if (idx < 0) break;
    picked.push(seqs[idx]);
  }

  var msg = {};
  msg[KEY.type] = TYPE.PAGE;
  msg[KEY.epoch] = this._epoch();
  msg[KEY.offset] = offset;
  msg[KEY.total] = total;
  // Echo the records' schema so the watch can refuse a page written by a
  // different app version (e.g. an old backup restored here) instead of
  // reinterpreting its bytes.
  var schema = localStorage.getItem(this.p + ':schema');
  if (schema != null) msg[KEY.schema] = Number(schema) & 0xff;
  var bytes = [], got = 0;
  for (var i = 0; i < picked.length; i++) {
    var d = localStorage.getItem(this._recKey(picked[i]));
    if (d == null) continue;   // dangling seq (missing record) — skip it rather than
                               // throw, so the watch always gets its PAGE reply
    pushU32(bytes, picked[i]);
    var rec = b64dec(d);
    for (var m = 0; m < rec.length; m++) bytes.push(rec[m]);
    got++;
  }
  msg[KEY.count] = got;
  if (bytes.length) msg[KEY.data] = bytes;
  sendControl(msg, 'page');
};

module.exports = Store;
// Exposed so app-specific decoders (e.g. molkky_history.js) can turn a
// snapshot()'s opaque base64 records back into bytes without re-rolling base64.
module.exports.b64enc = b64enc;
module.exports.b64dec = b64dec;
