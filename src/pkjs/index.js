// =============================================================================
// PebbleKit JS entry point. Hosts the phone-side history archive and bridges
// the watch's storage AppMessages to it. Runs inside the Core Devices app's JS
// sandbox; only alive while the Mölkky watchapp is open, so syncing happens on
// app launch and after each finished game — there is no background daemon.
// =============================================================================

var Store = require('./storage');

// One archive for Mölkky's game history. The prefix namespaces its localStorage
// keys, so adding more synced stores later is just another `new Store(...)`.
var history = new Store('mkhist');

Pebble.addEventListener('ready', function () {
  console.log('Mölkky PKJS ready');
});

Pebble.addEventListener('appmessage', function (e) {
  history.handle(e.payload);
});
