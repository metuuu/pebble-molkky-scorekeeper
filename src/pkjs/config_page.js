// =============================================================================
// config_page.js — builds the URL for the settings webview the Core Devices app
// opens for Mölkky (the gear icon). The page itself is hosted (docs/config.html
// on GitHub Pages): the current app opens the showConfiguration URL in a real
// webview and will not render a self-contained data: URL — only Clay is
// special-cased to work offline.
//
// PKJS can't inject values into a hosted page, so the watch/phone data rides in
// the URL *fragment* (#...), which never reaches GitHub's servers and has far
// more length headroom than a query string. The page reads it from
// location.hash. Payload (JSON, URI-encoded):
//   { decoded: <string>, raw: <string>, stats: <array>, count: <number> }
//
//   decoded : human-readable games, shown verbatim (View).
//   raw     : the lossless snapshot, shown verbatim (Export — what Import eats).
//   stats   : per-player lifetime aggregates (array of objects).
//   count   : number of stored games.
//
// Actions return to the app via `pebblejs://close#<encoded JSON>`, handled by
// the `webviewclosed` listener in index.js.
// =============================================================================

// The hosted page. Serve docs/config.html via GitHub Pages (Settings → Pages →
// Deploy from branch: main, folder: /docs) so this resolves.
var PAGE_URL = 'https://metuuu.github.io/pebble-molkky/config.html';

// decodedJson / rawJson are already-stringified JSON (shown verbatim in
// textareas); statsJson is a stringified array (embedded as a live value);
// count is a number.
function buildUrl(decodedJson, rawJson, statsJson, count) {
  var stats;
  try { stats = statsJson ? JSON.parse(statsJson) : []; }
  catch (e) { stats = []; }
  var payload = {
    decoded: String(decodedJson),
    raw: String(rawJson),
    stats: stats,
    count: count | 0
  };
  return PAGE_URL + '#' + encodeURIComponent(JSON.stringify(payload));
}

module.exports = { buildUrl: buildUrl };
