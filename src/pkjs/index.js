// =============================================================================
// PebbleKit JS entry point. Hosts the phone-side history archive and bridges
// the watch's storage AppMessages to it. Runs inside the Core Devices app's JS
// sandbox; only alive while the Mölkky watchapp is open, so syncing happens on
// app launch and after each finished game — there is no background daemon.
// =============================================================================

var Store = require('./storage');
var histView = require('./molkky_history');
var configPage = require('./config_page');

// One archive for Mölkky's game history. The prefix namespaces its localStorage
// keys, so adding more synced stores later is just another `new Store(...)`.
var history = new Store('mkhist');

Pebble.addEventListener('ready', function () {
  console.log('Mölkky PKJS ready');
});

Pebble.addEventListener('appmessage', function (e) {
  history.handle(e.payload);
});

// ---- settings webview (gear icon in the Core Devices app) -------------------
// Opens a self-contained page showing the stored games and offering export /
// import. See config_page.js. Requires "configurable" in package.json
// capabilities for the gear to appear.
Pebble.addEventListener('showConfiguration', function () {
  var snap = history.snapshot();
  var players = histView.decodePlayers(snap.aux);          // {} if the watch hasn't synced players yet
  var archive = histView.decodeArchive(snap, players.namesById);
  var decoded = JSON.stringify(archive, null, 2);
  var stats = JSON.stringify(histView.aggregatePlayers(archive, players.namesById));
  var raw = JSON.stringify(snap);
  Pebble.openURL(configPage.buildUrl(decoded, raw, stats, snap.records.length));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var res;
  try { res = JSON.parse(decodeURIComponent(e.response)); }
  catch (err) { console.log('config: bad response'); return; }
  if (res.action === 'reset-request') {         // user asked to wipe — the watch confirms, then wipes both sides
    try {
      history.requestWipe();
      console.log('reset: asked the watch to confirm');
    } catch (err) {
      console.log('reset request failed: ' + err.message);
    }
    return;
  }
  if (res.action !== 'import') return;          // "Done"/back: nothing to do
  var snap;
  try { snap = JSON.parse(res.raw); }
  catch (err) { console.log('import: not valid JSON'); return; }
  try {
    var n = history.restore(snap);              // full replace (games + players)
    console.log('import: archive now ' + n + ' game(s)');
  } catch (err) {
    console.log('import failed: ' + err.message);
  }
});
