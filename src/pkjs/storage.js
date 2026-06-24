// =============================================================================
// storage.js — phone-side mirror of the watch's generic synced store.
//
// The full archive lives here in localStorage (megabytes, and it survives app
// reinstalls — unlike the watch's 4 KB persist). Records are stored OPAQUELY,
// keyed by their monotonic `seq`; this side never interprets a record's bytes,
// which keeps it decoupled from the C struct layout. Only two protocol shapes:
//   PUSH (watch->phone): store a batch of records, ACK the highest seq held.
//   GET  (watch->phone): return a page of records older than a cursor.
//
// Generic by design: construct with a key prefix so one phone app can host
// several independent stores.
// =============================================================================

var TYPE = { PUSH: 1, ACK: 2, GET: 3, PAGE: 4, DEL: 5 };
var KEY = {
  type:   'st_type',
  schema: 'st_schema',
  count:  'st_count',
  offset: 'st_offset',
  data:   'st_data',
  ack:    'st_ack',
  total:  'st_total'
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

function Store(prefix) { this.p = prefix; }

Store.prototype._seqKey = function () { return this.p + ':seqs'; };
Store.prototype._recKey = function (seq) { return this.p + ':r:' + seq; };
Store.prototype._seqs = function () {
  var s = localStorage.getItem(this._seqKey());
  return s ? JSON.parse(s) : [];
};
Store.prototype._saveSeqs = function (a) { localStorage.setItem(this._seqKey(), JSON.stringify(a)); };

// Dispatch an inbound AppMessage payload from the watch.
Store.prototype.handle = function (payload) {
  switch (payload[KEY.type]) {
    case TYPE.PUSH: this._onPush(payload); break;
    case TYPE.GET:  this._onGet(payload);  break;
    case TYPE.DEL:  this._onDelete(payload); break;
    default: break;
  }
};

// Forget a record by seq (the seq rides in the offset slot). Idempotent: deleting
// an absent seq just re-ACKs the unchanged archive. ACK afterwards so the watch
// learns the new total (and so a tombstone for an already-gone game still settles).
Store.prototype._onDelete = function (p) {
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
  msg[KEY.ack] = seqs.length ? seqs[seqs.length - 1] : 0;   // highest seq we now hold
  msg[KEY.total] = seqs.length;                             // archive size, for the pager
  Pebble.sendAppMessage(msg);
};

// Offset paging over the global newest-first list. The watch always syncs before
// fetching, so by here the archive is complete and a page is a pure offset slice.
// ---- backup / restore (generic, opaque) ------------------------------------
// snapshot() and restore() are the canonical export/import format: records stay
// opaque (the same base64 we hold), keyed by seq, so a round-trip is lossless
// and this stays decoupled from any record's byte layout. An app that wants a
// human-readable view decodes the records on top of snapshot() (see
// molkky_history.js) without this layer needing to know the struct.

// Whole-archive snapshot. Shape mirrors what restore() consumes.
Store.prototype.snapshot = function () {
  var seqs = this._seqs(), recs = [];
  for (var i = 0; i < seqs.length; i++) {
    var d = localStorage.getItem(this._recKey(seqs[i]));
    if (d != null) recs.push({ seq: seqs[i], data: d });   // data already base64
  }
  var schema = localStorage.getItem(this.p + ':schema');
  return { store: this.p, schema: schema == null ? null : Number(schema), records: recs };
};

Store.prototype._clear = function () {
  var seqs = this._seqs();
  for (var i = 0; i < seqs.length; i++) localStorage.removeItem(this._recKey(seqs[i]));
  this._saveSeqs([]);
};

// Load a snapshot(). mode 'replace' wipes the archive first; 'merge' (default)
// unions by seq — a re-seen seq overwrites, exactly like an idempotent PUSH.
// ACKs the watch afterwards so its pager learns the new total. Returns the new
// record count. Throws on a malformed snapshot.
Store.prototype.restore = function (snap, mode) {
  if (!snap || !Array.isArray(snap.records)) throw new Error('invalid snapshot');
  if (mode === 'replace') this._clear();
  var seqs = this._seqs();
  for (var i = 0; i < snap.records.length; i++) {
    var r = snap.records[i];
    if (!r || typeof r.data !== 'string' || r.seq == null) throw new Error('invalid record at ' + i);
    var seq = r.seq >>> 0;
    localStorage.setItem(this._recKey(seq), r.data);   // base64 stored verbatim
    insertSorted(seqs, seq);
  }
  this._saveSeqs(seqs);
  if (snap.schema != null) localStorage.setItem(this.p + ':schema', String(snap.schema));
  this._sendAck();
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
  msg[KEY.count] = picked.length;
  msg[KEY.offset] = offset;
  msg[KEY.total] = total;
  if (picked.length) {
    var bytes = [];
    for (var i = 0; i < picked.length; i++) {
      pushU32(bytes, picked[i]);
      var rec = b64dec(localStorage.getItem(this._recKey(picked[i])));
      for (var m = 0; m < rec.length; m++) bytes.push(rec[m]);
    }
    msg[KEY.data] = bytes;
  }
  Pebble.sendAppMessage(msg);
};

module.exports = Store;
// Exposed so app-specific decoders (e.g. molkky_history.js) can turn a
// snapshot()'s opaque base64 records back into bytes without re-rolling base64.
module.exports.b64enc = b64enc;
module.exports.b64dec = b64dec;
