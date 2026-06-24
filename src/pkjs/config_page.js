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
// location.hash.
//
// CEILING: the fragment carries the whole base64 archive, which grows without
// bound, while a URL does not — a few thousand games will eventually exceed what
// openURL/the webview will accept and the page would load truncated. If history
// ever gets that large, move the payload off the URL (e.g. an in-page fetch or a
// chunked hand-off) rather than stuffing more into the fragment.
//
// Payload (JSON, URI-encoded):
//   { raw: <string>, stats: <array>, count: <number>, players: <number> }
//
//   raw     : the lossless snapshot, shown verbatim (Export — what Import eats).
//   stats   : per-player lifetime aggregates (array of objects).
//   count   : number of stored games.
//   players : roster size (for the Players tile) — distinct from stats.length,
//             which only counts players that have actually played a game.
//
// Actions return to the app via `pebblejs://close#<encoded JSON>`, handled by
// the `webviewclosed` listener in index.js.
// =============================================================================

// The hosted page. Serve docs/config.html via GitHub Pages (Settings → Pages →
// Deploy from branch: main, folder: /docs) so this resolves.
var PAGE_URL = 'https://metuuu.github.io/pebble-molkky/config.html';

// rawJson is already-stringified JSON (shown verbatim in the Export textarea);
// statsJson is a stringified array (embedded as a live value); count and players
// are numbers (stored games and roster size).
function buildUrl(rawJson, statsJson, count, players) {
  var stats;
  try { stats = statsJson ? JSON.parse(statsJson) : []; }
  catch (e) { stats = []; }
  var payload = {
    raw: String(rawJson),
    stats: stats,
    count: count | 0,
    players: players | 0
  };
  return PAGE_URL + '#' + encodeURIComponent(JSON.stringify(payload));
}

module.exports = { buildUrl: buildUrl };
