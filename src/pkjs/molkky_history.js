// =============================================================================
// molkky_history.js — the one phone-side place that knows the MKHistGame byte
// layout. The storage lib keeps records opaque on purpose (decoupled from the C
// struct); this decoder is the deliberate, contained exception that turns a
// Store.snapshot() into human-readable games for the config webview.
//
// MUST stay in sync with the structs in src/c/app/molkky.h:
//
//   MKResult (8 B):  id u8@0  place u8@1  score u8@2  flags u8@3
//                    misses u8@4  throws u8@5  points u16@6
//   MKHistGame:      date i32@0  start i32@4  count u8@8  settings u8@9
//                    _pad[2]@10  results[count] @12  (MKResult, sorted by place)
//                    (duration is derived: date - start)
//
// Flag bits: MK_RES_OUT 0x1, MK_RES_WON 0x2 (flags); MK_SET_LOSE3 0x1,
// MK_SET_FINAL 0x2 (settings).
//
// NOTE: records store the roster `id`, not the name. Names come from the player
// blob the watch mirrors via the storage aux channel (decodePlayers below); a game
// played by a player no longer in that roster falls back to `#<id>`.
// =============================================================================

var Store = require('./storage');

// The record schema this app writes — MK_SCHEMA in molkky.c. The decoder and
// the migrations below must track it.
var SCHEMA = 9;
var REC_BYTES = 140;   // sizeof(MKHistGame): 12 B header + 16 × 8 B results

// Upgrade one record's bytes from an older schema to the current one. Returns
// the upgraded bytes, or null when that schema can't be read. Schemas 7 and 8
// wrote today's field layout with 14 result slots (124 B) — a strict prefix of
// the current record, so the upgrade is zero-padding the tail. Idempotent:
// bytes already at the current size pass through unchanged.
function migrateRecord(bytes, from) {
  if (from === SCHEMA) return bytes;
  if (from === 7 || from === 8) {
    while (bytes.length < REC_BYTES) bytes.push(0);
    return bytes;
  }
  return null;   // pre-v7 layout, or a newer app's export — not readable here
}

// Upgrade a parsed backup (Store.snapshot() shape) to the current schema, so an
// export made by an older app version still imports. Returns the snapshot to
// restore (a new object when records were upgraded); throws when the backup's
// schema is unknown. Call before Store.restore() — the store keeps records
// opaque on purpose, so schema knowledge stays here.
function migrateSnapshot(snap) {
  if (!snap || snap.schema == null || Number(snap.schema) === SCHEMA) return snap;
  var from = Number(snap.schema);
  var recs = snap.records || [], out = [];
  for (var i = 0; i < recs.length; i++) {
    var nb = migrateRecord(Store.b64dec(recs[i].data), from);
    if (!nb) throw new Error('backup is from an incompatible app version');
    out.push({ seq: recs[i].seq, data: Store.b64enc(nb) });
  }
  return { store: snap.store, schema: SCHEMA, records: out, aux: snap.aux };
}

function readU16(a, o) { return (a[o] | (a[o + 1] << 8)) >>> 0; }
function readI32(a, o) {
  return (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)); // signed
}

function isoFromUnix(sec) {
  if (!sec) return null;
  return new Date(sec * 1000).toISOString();
}

function decodeResult(a, o) {
  var flags = a[o + 3];
  return {
    player: a[o] >>> 0,          // roster id (no name on the phone)
    place:  a[o + 1] >>> 0,
    score:  a[o + 2] >>> 0,
    won:    !!(flags & 0x2),     // MK_RES_WON
    out:    !!(flags & 0x1),     // MK_RES_OUT
    misses: a[o + 4] >>> 0,
    throws: a[o + 5] >>> 0,
    points: readU16(a, o + 6)
  };
}

// Decode one record's bytes into a readable game. Tolerates a short/garbled
// record by clamping `count` to what the buffer can hold.
function decodeGame(a) {
  var date = readI32(a, 0);
  var start = readI32(a, 4);
  var count = a[8] >>> 0;
  var settings = a[9] >>> 0;
  // bytes 10-11 are padding
  var maxFit = Math.floor((a.length - 12) / 8);
  if (count > maxFit) count = maxFit < 0 ? 0 : maxFit;
  var players = [];
  for (var i = 0; i < count; i++) players.push(decodeResult(a, 12 + i * 8));
  return {
    date: date,
    endedISO:   isoFromUnix(date),
    startedISO: isoFromUnix(start),
    durationMin: (start && date > start) ? Math.round((date - start) / 60) : 0,
    lose3:      !!(settings & 0x1),   // MK_SET_LOSE3
    finalRound: !!(settings & 0x2),   // MK_SET_FINAL
    players: players
  };
}

// Decode the player blob (storage aux) the watch mirrors — see player_blob_* in
// molkky.c. Returns { namesById: {id: name}, players: [{id, name, archived, ...}] }.
// Layout (little-endian): header ver u8, count u8, next_id u8, pad u8; then per
// player id u8, archived u8, name[16], games u16, wins u16, throws u32, misses u32,
// points u32, place_sum u16 (36 B). Tolerates a missing/short blob.
var PB_ENTRY = 2 + 16 + (2 + 2 + 4 + 4 + 4 + 2);   // 36
function pbU16(a, o) { return (a[o] | (a[o + 1] << 8)) >>> 0; }
function pbU32(a, o) {
  return (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)) >>> 0;
}
function pbName(a, o) {                              // 16 bytes, NUL-terminated, UTF-8
  var bin = '';
  for (var k = 0; k < 16; k++) { var c = a[o + k]; if (!c) break; bin += String.fromCharCode(c); }
  try { return decodeURIComponent(escape(bin)); } catch (e) { return bin; }
}
function decodePlayers(auxB64) {
  var out = { namesById: {}, players: [] };
  if (!auxB64) return out;
  var a;
  try { a = Store.b64dec(auxB64); } catch (e) { return out; }
  if (a.length < 4 || a[0] !== 1) return out;
  var n = a[1], o = 4;
  for (var i = 0; i < n && o + PB_ENTRY <= a.length; i++, o += PB_ENTRY) {
    var p = {
      id: a[o], archived: a[o + 1] !== 0, name: pbName(a, o + 2),
      games: pbU16(a, o + 18), wins: pbU16(a, o + 20),
      throws: pbU32(a, o + 22), misses: pbU32(a, o + 26),
      points: pbU32(a, o + 30), placeSum: pbU16(a, o + 34)
    };
    out.namesById[p.id] = p.name;
    out.players.push(p);
  }
  return out;
}

// Resolve a roster id to a display name. namesById may be absent (older backup).
function playerLabel(namesById, id) {
  var nm = namesById && namesById[id];
  return (nm != null && nm !== '') ? nm : '#' + id;
}

// Turn a Store.snapshot() into { schema, count, games[] }, newest game first.
// `namesById` (from decodePlayers) adds a readable `name` to each result.
function decodeArchive(snap, namesById) {
  var recs = (snap && snap.records) || [];
  var games = [];
  for (var i = 0; i < recs.length; i++) {
    try {
      var g = decodeGame(Store.b64dec(recs[i].data));
      g.seq = recs[i].seq;
      for (var j = 0; j < g.players.length; j++)
        g.players[j].name = playerLabel(namesById, g.players[j].player);
      games.push(g);
    } catch (err) { /* skip an unreadable record rather than fail the whole view */ }
  }
  games.sort(function (x, y) { return y.seq - x.seq; });
  return { schema: snap ? snap.schema : null, count: games.length, games: games };
}

// Roll a decoded archive up into per-player lifetime stats — the phone's exact,
// full-history counterpart to the watch's running MKLifetime totals. Keyed by
// roster id; `namesById` (from decodePlayers) labels each row, falling back to #id.
// Returns rows sorted by games played (desc), then id.
function aggregatePlayers(archive, namesById) {
  var games = (archive && archive.games) || [];
  var byId = {};
  for (var i = 0; i < games.length; i++) {
    var ps = games[i].players || [];
    for (var j = 0; j < ps.length; j++) {
      var p = ps[j];
      var L = byId[p.player];
      if (!L) L = byId[p.player] = { id: p.player, games: 0, wins: 0,
                                     throws: 0, misses: 0, points: 0 };
      L.games++;
      if (p.place === 1) L.wins++;
      L.throws += p.throws;
      L.misses += p.misses;
      L.points += p.points;
    }
  }
  var out = [];
  for (var k in byId) {
    if (!byId.hasOwnProperty(k)) continue;
    var s = byId[k];
    out.push({
      id:          s.id,
      name:        playerLabel(namesById, s.id),
      games:       s.games,
      wins:        s.wins,
      winPct:      s.games  ? Math.round(s.wins * 100 / s.games) : 0,
      accuracyPct: s.throws ? Math.round((s.throws - s.misses) * 100 / s.throws) : 0,
      avgPoints:   s.throws ? Math.round(s.points * 10 / s.throws) / 10 : 0
    });
  }
  out.sort(function (a, b) { return (b.games - a.games) || (a.id - b.id); });
  return out;
}

module.exports = { decodeArchive: decodeArchive, decodeGame: decodeGame,
                   aggregatePlayers: aggregatePlayers, decodePlayers: decodePlayers,
                   SCHEMA: SCHEMA, migrateRecord: migrateRecord,
                   migrateSnapshot: migrateSnapshot };
