# Translations

`translations.csv` is the single source of truth for the app's UI text. The
generator turns it into the compiled string tables the app uses.

## Files

- `translations.csv` — edit this. One row per string, one column per language.
- `gen_strings.py` — from the CSV it generates `src/c/app/strings.h` (the `StrId`
  enum + macros), `src/c/app/strings.c` (locale registration), and one packed
  blob per language at `resources/data/locale_<lang>.bin`.

Each language's strings are packed into a blob that ships as a **raw resource**,
not a flash table: a Pebble app's whole binary loads into the 64 KB-capped RAM
image, so full per-language tables no longer fit. Resources live in the separate
256 KB resource region and are loaded to the heap on demand — only the active
and base locales are ever resident. Every language needs a matching entry in
`package.json`:

```json
{ "type": "raw", "name": "LOCALE_<LANG>", "file": "data/locale_<lang>.bin" }
```

`strings.h`, `strings.c`, and the `.bin` blobs are **generated** — don't edit
them by hand; edit the CSV and regenerate.

## Regenerate

```sh
npm run strings          # or: python3 tools/i18n/gen_strings.py
```

Then rebuild the app (`pebble build`). The host test
`tools/test/run_locale_tests` exercises the generated tables.

## CSV format

```
id,en,fi
@autonym,English,Suomi
@sys_locale,en_US,
@month_01,Jan,
…
STR_NEW_GAME,New game,Uusi peli
STR_ROUND,Round %d,Kierros %d
```

- The **first language column is the base** (index 0). It backs any missing
  translation and is the app's default until a saved/seeded language applies.
- **String rows** have an id like `STR_NEW_GAME`. An empty cell means
  "untranslated" → falls back to the base language at runtime (the app still
  runs with a partial translation).
- **Meta rows** (id starts with `@`) describe a language, not a string:
  - `@autonym` — the language's own name, shown in the Settings picker.
  - `@sys_locale` — ISO code matched against `i18n_get_system_locale()` for
    auto-seeding. Leave empty if the platform has no such locale (e.g. Finnish:
    Pebble has no `fi_FI`, so Finnish is only reachable via the Settings picker).
  - `@month_01`…`@month_12` — month names for the `%b`/`%B` date spec. If a
    language leaves them all empty it formats dates numerically (e.g. Finnish
    `29.6. klo 14:30`) and needs no month table.

Cells hold natural text — real newlines and quotes are fine (the generator
C-escapes them). `printf` specifiers (`%d`, `%s`, `%%`) and `strftime`
specifiers pass through unchanged, so word order and pluralization stay in the
translator's hands.

## Adding a language

1. Add a column (e.g. `de`) to `translations.csv`, fill in `@autonym`,
   optionally `@sys_locale` (and `@month_*` if you want named months), and as
   many string cells as you can — blanks fall back to English.
2. Add the raw resource to `package.json` (the generator prints a warning with
   the exact line if it's missing):
   ```json
   { "type": "raw", "name": "LOCALE_DE", "file": "data/locale_de.bin" }
   ```
3. `npm run strings && pebble build`.

No C changes are needed; the new language appears in the Settings picker
automatically.

## Adding a string

1. Add a row with a new `STR_…` id and at least the base-language text.
2. `npm run strings`, then reference `STR_…` from the C code via
   `t()` / `tfmt()` / `tdate()`.
