#include "locale.h"
#include <string.h>
#include <stdio.h>
#include <locale.h>

// =============================================================================
// locale — implementation. See locale.h for the model and contract.
// Pure standard C (no <pebble.h>) so the same code runs under the host test
// harness (tools/test) as on the watch.
// =============================================================================

static const Locale *s_locales;
static int           s_count;
static int           s_string_count;
static int           s_base;       // index that backs NULL string entries
static int           s_active;

void locale_init(const Locale *locales, int count, int string_count, int base) {
  s_locales      = locales;
  s_count        = count;
  s_string_count = string_count;
  s_base         = (base >= 0 && base < count) ? base : 0;
  s_active       = s_base;
}

int locale_count(void)  { return s_count; }
int locale_active(void) { return s_active; }

const char *locale_autonym(int i) {
  return (s_locales && i >= 0 && i < s_count) ? s_locales[i].autonym : "";
}
const char *locale_sys(int i) {
  return (s_locales && i >= 0 && i < s_count) ? s_locales[i].sys_locale : NULL;
}

void locale_set(int i) {
  if (!s_locales || s_count <= 0) return;
  if (i < 0)        i = 0;
  if (i >= s_count) i = s_count - 1;
  s_active = i;
  // Date seam: a locale with no month table but a platform `sys_locale` formats
  // dates through the SDK's strftime, so align the C locale to it. Locales that
  // carry their own month names format dates themselves and don't touch it.
  const Locale *L = &s_locales[i];
  if (!L->months && L->sys_locale) setlocale(LC_TIME, L->sys_locale);
}

// Match a language tag like "fi_FI" / "fi-FI" / "fi": exact `sys_locale` first,
// then the 2-letter language prefix. Returns the locale index, or -1.
int locale_index_for_sys(const char *sys) {
  if (!s_locales || !sys || !sys[0]) return -1;
  for (int i = 0; i < s_count; i++) {                 // exact code
    const char *c = s_locales[i].sys_locale;
    if (c && strcmp(c, sys) == 0) return i;
  }
  for (int i = 0; i < s_count; i++) {                 // language prefix ("fi" ~ "fi_FI")
    const char *c = s_locales[i].sys_locale;
    if (c && c[0] && c[1] && c[0] == sys[0] && c[1] == sys[1]) return i;
  }
  return -1;
}

const char *locale_str(int id) {
  if (!s_locales || id < 0 || id >= s_string_count) return "";
  const char *const *t = s_locales[s_active].strings;
  const char *s = t ? t[id] : NULL;
  if (!s) {                                           // fall back to the base locale
    const char *const *b = s_locales[s_base].strings;
    s = b ? b[id] : NULL;
  }
  return s ? s : "";
}

void locale_vformat(char *buf, size_t n, int id, va_list ap) {
  if (!buf || n == 0) return;
  vsnprintf(buf, n, locale_str(id), ap);
}

void locale_format(char *buf, size_t n, int id, ...) {
  va_list ap;
  va_start(ap, id);
  locale_vformat(buf, n, id, ap);
  va_end(ap);
}

// ---------------------------- date formatting ----------------------------
// A tiny strftime, formatting from struct tm so it needs no GNU extensions and
// can pull %b/%B from the locale's own table. `pos`-returning helpers keep the
// output within `n` (always leaving room for the terminating NUL).

static size_t put_str(char *buf, size_t n, size_t pos, const char *s) {
  while (s && *s && pos + 1 < n) buf[pos++] = *s++;
  return pos;
}

static size_t put_uint(char *buf, size_t n, size_t pos, unsigned v, int min_width) {
  char tmp[12];
  int len = 0;
  do { tmp[len++] = (char)('0' + v % 10); v /= 10; } while (v && len < (int)sizeof tmp);
  while (len < min_width && len < (int)sizeof tmp) tmp[len++] = '0';  // leading zeros
  while (len > 0 && pos + 1 < n) buf[pos++] = tmp[--len];
  return pos;
}

void locale_strftime(char *buf, size_t n, int fmt_id, time_t t) {
  if (!buf || n == 0) return;
  const char *fmt = locale_str(fmt_id);
  struct tm tmz;
  struct tm *lt = localtime(&t);
  if (lt) tmz = *lt; else memset(&tmz, 0, sizeof tmz);
  const Locale *L = (s_locales && s_active < s_count) ? &s_locales[s_active] : NULL;

  size_t pos = 0;
  for (const char *p = fmt; *p && pos + 1 < n; p++) {
    if (*p != '%') { buf[pos++] = *p; continue; }
    p++;
    int nopad = 0;
    if (*p == '-') { nopad = 1; p++; }
    switch (*p) {
      case 'Y': pos = put_uint(buf, n, pos, (unsigned)(tmz.tm_year + 1900), 0); break;
      case 'y': pos = put_uint(buf, n, pos, (unsigned)((tmz.tm_year + 1900) % 100), nopad ? 0 : 2); break;
      case 'm': pos = put_uint(buf, n, pos, (unsigned)(tmz.tm_mon + 1), nopad ? 0 : 2); break;
      case 'd': pos = put_uint(buf, n, pos, (unsigned)tmz.tm_mday,       nopad ? 0 : 2); break;
      case 'H': pos = put_uint(buf, n, pos, (unsigned)tmz.tm_hour,       nopad ? 0 : 2); break;
      case 'M': pos = put_uint(buf, n, pos, (unsigned)tmz.tm_min,        nopad ? 0 : 2); break;
      case 'S': pos = put_uint(buf, n, pos, (unsigned)tmz.tm_sec,        nopad ? 0 : 2); break;
      case 'b':
      case 'B': {
        int mon = tmz.tm_mon;
        if (mon < 0 || mon > 11) break;
        if (L && L->months && L->months[mon]) {
          pos = put_str(buf, n, pos, L->months[mon]);
        } else {                                      // SDK path (respects setlocale)
          char mb[20];
          mb[0] = '\0';
          strftime(mb, sizeof mb, *p == 'b' ? "%b" : "%B", &tmz);
          pos = put_str(buf, n, pos, mb);
        }
        break;
      }
      case '%': buf[pos++] = '%'; break;
      case '\0': p--; break;                          // trailing '%': stop cleanly
      default:                                        // unknown spec → emit verbatim
        if (pos + 1 < n) buf[pos++] = '%';
        if (*p && pos + 1 < n) buf[pos++] = *p;
        break;
    }
  }
  buf[pos] = '\0';
}
