# Icon build

Single source of truth: [`resources/icons.json`](../../resources/icons.json).
Edit it, then from the repo root run:

```sh
npm run icons
```

This rasterizes each icon into `resources/icons/` and rewrites the
`pebble.resources.media` block in `package.json`, so every icon is reachable in C
as `RESOURCE_ID_IMAGE_<NAME>` (e.g. `name: "UNDO"` → `RESOURCE_ID_IMAGE_UNDO`).

## Manifest

```jsonc
{
  "outputDir": "resources/icons",
  "localDir":  "resources/svg-icons",
  "defaults":  { "size": 25, "color": "#000000", "tinted": true },
  "icons": [
    { "name": "MENU_ICON", "from": "local:logo", "file": "logo.png",
      "tinted": false, "menuIcon": true },
    { "name": "UNDO",  "from": "local:undo" },   // resources/svg-icons/undo.svg
    { "name": "HEART", "from": "heart" }          // pixelarticons/svg/heart.svg
  ]
}
```

Per-icon fields (all optional except `name` and `from`):

| field      | meaning                                                                 |
|------------|-------------------------------------------------------------------------|
| `name`     | resource id suffix → `RESOURCE_ID_IMAGE_<name>`                          |
| `from`     | source — see below                                                      |
| `size`     | px (square), default `25`                                               |
| `color`    | replaces `currentColor` in the SVG, default `#000000`                   |
| `fill`     | root fill applied to shapes that don't set their own (e.g. the logo)    |
| `tinted`   | `true` if the C side recolors it at runtime (default `true`)            |
| `menuIcon` | mark as the app launcher icon                                           |
| `file`     | override the output filename (default `<source>-<size>.png`)            |

### `from` sources

- `local:<name>` → `resources/svg-icons/<name>.svg` (your own art)
- `<name>` → `node_modules/pixelarticons/svg/<name>.svg` ([pixelarticons](https://github.com/halfmage/pixelarticons), pinned)
- `png:<file>` → an existing `resources/icons/<file>` copied through as-is, for
  hand-made bitmaps with no SVG source (e.g. the solid shift-lock key)

## Behaviour

- **Incremental** — an icon is re-rendered only when its SVG, size or color
  changed (hashes tracked in `.icons-cache.json`, gitignored).
- **Prune** — outputs this tool previously made that you removed from the
  manifest are deleted. Files it never made are left alone
  and reported.
- **`color` vs `tinted`** — icons drawn through the UI lib (`list_item`,
  `footer`, `t9_keyboard`) are recolored to the row ink at runtime by
  `tint_icon()`, so `color` is a no-op for them; setting a non-black color on a
  `tinted` icon prints a warning. `color` only matters for non-tinted assets
  (the menu logo).
- **`fill`** — outline SVGs (like the logo) leave their shapes unfilled in the
  source; `fill` paints them at build time without baking a color into the art.
  It only applies to shapes with no `fill` of their own, so it's a no-op on
  pixelarticons. Like `color`, it's ignored on `tinted` icons (a warning is
  printed) because `tint_icon()` flattens every opaque pixel to the row ink — a
  two-tone (border + fill) glyph needs a non-tinted draw path in C
  (`ACC_ICON_RAW`), which is how the logo renders in the menu list.

`npm run convert` still runs the old bulk SVG→PNG dump (`svg-to-png.js`) if you
want raw exports without touching the manifest.
