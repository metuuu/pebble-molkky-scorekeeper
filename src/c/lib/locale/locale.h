#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

// Small translation engine for Pebble apps.
// Strings are stored as packed blobs so locale tables can live in resources
// instead of the 64 KB app image. Missing entries fall back to the base locale.
//
// Pack format: u16 count, u16 offsets[count], then NUL-terminated strings.
// Offset 0xFFFF marks a missing entry.

// A language/region the app can render in.
typedef struct {
  const char *autonym;          // the language's own name, for a picker ("English", "Suomi")
  const char *sys_locale;       // ISO code for system-locale matching and SDK dates, or NULL.
  const char *const *months;    // [12] month names for %b/%B, or NULL → use the SDK's strftime
  uint32_t    resource_id;      // raw resource holding packed strings (0 = none)
  const uint8_t *pack;          // packed strings held in memory, or NULL to load from resource_id.
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
