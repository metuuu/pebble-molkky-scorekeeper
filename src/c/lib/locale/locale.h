#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

// =============================================================================
// locale — a small, generic translation engine for Pebble apps.
//
// The app owns the *string ids* (an enum) and the per-language tables; this lib
// owns lookup, value placement, language switching, system-locale matching, and
// localized date formatting. It deliberately does not include <pebble.h> so it
// stays host-testable — the app passes i18n_get_system_locale()'s result in.
//
// Design notes:
//   * A locale's strings are a packed blob rather than a flash pointer array:
//     on a real Pebble app the whole binary is loaded into the 64 KB-capped RAM
//     image, so full per-language tables no longer fit. The blob ships as a raw
//     *resource* (the 256 KB resource region, separate from the RAM image) and
//     is loaded into the heap on demand — only the active and base locales are
//     ever resident. See the pack format below.
//   * A string entry may be absent — the lookup falls back to the base locale
//     (typically English), so a partial translation degrades gracefully.
//   * Dates go through one seam (locale_strftime). A locale either carries its
//     own month names (formats dates itself — works for any language, supported
//     by the platform or not, e.g. Finnish) or names a platform `sys_locale`
//     and leaves `months` NULL (the SDK's strftime fills %b/%B). Both are
//     first-class; call sites never change.
//
// Pack format (little-endian): a uint16 `count`, then `count` uint16 offsets,
// then a blob of NUL-terminated strings. offset[id] is the string's byte offset
// into the blob; the sentinel 0xFFFF marks an absent entry (→ base fallback).
// =============================================================================

// A language/region the app can render in.
typedef struct {
  const char *autonym;          // the language's own name, for a picker ("English", "Suomi")
  const char *sys_locale;       // ISO code to match i18n_get_system_locale() and to drive the
                                // SDK date path, or NULL when the platform has no such locale
                                // (e.g. Finnish) — then `months` must format dates.
  const char *const *months;    // [12] month names for %b/%B, or NULL → use the SDK's strftime
  uint32_t    resource_id;      // raw resource holding this locale's packed strings (0 = none);
                                // on the watch it is loaded to the heap the first time the locale
                                // is base or active. Ignored when `pack` is already set.
  const uint8_t *pack;          // packed strings held in memory, or NULL to load from resource_id.
                                // Host builds (no resource system) point this at a static blob.
} Locale;

// Register the locale set (kept by reference — must outlive the app, i.e. be
// static/flash). `base` is the index whose strings back any NULL entry. Call
// once at startup, before any lookup. The active locale starts at `base`.
void locale_init(const Locale *locales, int count, int string_count, int base);

int  locale_count(void);
int  locale_active(void);
void locale_set(int index);                 // activate by index (clamped to range)
const char *locale_autonym(int index);      // "" for an out-of-range index
const char *locale_sys(int index);          // NULL for an out-of-range index

// Best match for an i18n_get_system_locale() string ("fi_FI", "en_US", …):
// exact code first, then the language prefix ("fi" matches "fi_FI"). -1 if none.
int  locale_index_for_sys(const char *sys);

// Plain lookup of string `id` in the active locale, falling back to the base
// locale when that entry is NULL. Never returns NULL (unknown id → "").
const char *locale_str(int id);

// printf-style placement: the *template* is the localized string `id`, so each
// language controls word order and which values it places. Always NUL-terminates.
void locale_format(char *buf, size_t n, int id, ...);
void locale_vformat(char *buf, size_t n, int id, va_list ap);

// Localized date/time. The strftime-like pattern is itself a localized string
// (`fmt_id`); month names come from the active locale's table, or the SDK when
// the locale leaves `months` NULL. Supported specs: %Y %y %m %d %H %M %S %b %B
// %%, plus a '-' no-pad flag (e.g. %-d → "9", %d → "09"). Always NUL-terminates.
void locale_strftime(char *buf, size_t n, int fmt_id, time_t t);
