<div align="center">

# Mölkky

**A scorekeeper for the Finnish lawn game [Mölkky](https://en.wikipedia.org/wiki/Mölkky), built for the Pebble smartwatch.**

Score up to 16 players, track lifetime stats, and keep a full game history that syncs to your phone.

<br>

<img src="screenshots/cover-image.png" width="200" alt="Mölkky">

</div>

---

## Features

| | |
|---|---|
| **Throw scoring** | Record each throw from a 1–12 / MISS grid. |
| **Up to 16 players** | Keep a persistent roster with rename, archive, and delete. Names are entered with a multitap keyboard. |
| **Rule options** | Toggle *lose from three misses* and a *final round* rule where players who reach 50 in the same round share a crown. |
| **Undo** | One-level undo of the most recent throw. |
| **History & stats** | Browse past games and per-player lifetime stats — accuracy, points per turn, wins, and average finish. |
| **Phone sync** | The watch caches recent games offline; the full archive lives on the paired phone and backs up automatically. |
| **Languages** | English and Finnish (suomi). The language auto-seeds from the watch's system locale and can be switched any time in Settings. |

## Controls

Most screens use Pebble's physical buttons (up / select / down / back). During a game, the throw grid also accepts taps for quick score entry.

## Screenshots

<div align="center">

| Main menu | Players | Keyboard |
|:---:|:---:|:---:|
| <img src="screenshots/main-menu.png" width="150"> | <img src="screenshots/players.png" width="150"> | <img src="screenshots/keyboard.png" width="150"> |
| **In-game** | **Results** | **Player stats** |
| <img src="screenshots/game.png" width="150"> | <img src="screenshots/results.png" width="150"> | <img src="screenshots/player-stats.png" width="150"> |

</div>

## Building

Requires the [Pebble SDK](https://developer.repebble.com). The app targets
**emery** (Pebble Time 2).

```sh
pebble build                          # compile the app
pebble install --emulator emery       # run in the emulator
pebble install --phone <ip>           # install to a paired phone
pebble emu-app-config --emulator emery # open the settings page (export/import history)
```

## Architecture

```
src/c/app/       Game model, logic, persistence, app screens, and string tables
src/c/lib/       Reusable Pebble libraries — UI widgets, synced storage, multitap keyboard, localization
src/pkjs/        PebbleKit JS — phone-side history archive and settings page
resources/       Icons (generated from SVG via tools/icon-converter)
tools/           Icon converter, translation generator (tools/i18n), and native tests
package.json     App metadata — UUID, target platform, resources, message keys
wscript          Build rules
```

The code under `src/c/lib/` is generic and theme-driven, so the UI
widgets, storage, keyboard, and localization engine can be reused across Pebble
apps. The locale engine (`src/c/lib/locale/`) owns lookup, value placement,
system-locale matching, and localized dates; the app's `StrId` enum and
per-language tables in `src/c/app/strings.{h,c}` are **generated** from
`tools/i18n/translations.csv` (`npm run strings`). Mölkky-specific
presentation — crowns, medals, and standings — stays in `src/c/app/`. Game
history is stored on the watch as a rolling cache and mirrored to the phone,
which holds the complete archive.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
