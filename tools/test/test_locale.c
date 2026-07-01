#define _XOPEN_SOURCE 700   // setenv/tzset
// =============================================================================
// test_locale.c — host-side tests for src/c/lib/locale/locale.c.
//
// The engine is pure standard C, so it compiles and runs natively. We exercise
// it both against a tiny inline fixture (to cover fallback and the SDK-strftime
// date branch deterministically) and against the app's real English/Finnish
// tables (src/c/app/strings.c) to catch any table/enum drift.
// =============================================================================
#include "c/lib/locale/locale.h"
#include "strings.h"            // the app's StrId + mk_locale_init()
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int g_fail;
#define CHECK_STR(got, want) do {                                              \
    const char *g_ = (got), *w_ = (want);                                      \
    if (strcmp(g_, w_) != 0) {                                                 \
      printf("  FAIL %s:%d\n    got : \"%s\"\n    want: \"%s\"\n",             \
             __func__, __LINE__, g_, w_); g_fail++;                           \
    }                                                                          \
  } while (0)
#define CHECK_INT(got, want) do {                                              \
    long g_ = (long)(got), w_ = (long)(want);                                  \
    if (g_ != w_) {                                                            \
      printf("  FAIL %s:%d  got %ld want %ld\n", __func__, __LINE__, g_, w_);  \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

// Pack a NULL-tolerant string array exactly as gen_strings.py / the watch
// resource does (u16 count, u16 offsets with 0xFFFF = absent, then the blob), so
// the tests drive the real indexing path. Caller frees.
static uint8_t *mk_pack(const char *const *strs, int count) {
  size_t blob = 0;
  for (int i = 0; i < count; i++) if (strs[i]) blob += strlen(strs[i]) + 1;
  uint8_t *p = malloc(2 + (size_t)count * 2 + blob);
  p[0] = (uint8_t)count; p[1] = (uint8_t)(count >> 8);
  uint8_t *offs = p + 2;
  char *b = (char *)(p + 2 + (size_t)count * 2);
  size_t off = 0;
  for (int i = 0; i < count; i++) {
    if (!strs[i]) { offs[i * 2] = 0xff; offs[i * 2 + 1] = 0xff; continue; }
    offs[i * 2] = (uint8_t)off; offs[i * 2 + 1] = (uint8_t)(off >> 8);
    size_t l = strlen(strs[i]) + 1;
    memcpy(b + off, strs[i], l);
    off += l;
  }
  return p;
}

// ---- inline fixture: two locales, the second deliberately partial ----------
enum { F_HELLO, F_BYE, F_FMT, F_DATE, F__COUNT };
static const char *const FX_EN[F__COUNT] = {
  [F_HELLO] = "Hello", [F_BYE] = "Bye", [F_FMT] = "n=%d", [F_DATE] = "%b %d",
};
static const char *const FX_XX[F__COUNT] = {
  [F_HELLO] = "Moi", /* F_BYE missing → falls back */ [F_FMT] = "luku=%d", [F_DATE] = "%-d.%-m.",
};
static const char *const FX_EN_MONTHS[12] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec",
};

static time_t utc_time(int y, int mon, int mday, int h, int mi) {
  struct tm tmv;
  memset(&tmv, 0, sizeof tmv);
  tmv.tm_year = y - 1900; tmv.tm_mon = mon - 1; tmv.tm_mday = mday;
  tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_isdst = 0;
  return mktime(&tmv);   // TZ forced to UTC in main(), so this is a UTC moment
}

static void test_fixture(void) {
  uint8_t *en = mk_pack(FX_EN, F__COUNT), *xx = mk_pack(FX_XX, F__COUNT);
  Locale FX[] = {
    { .autonym = "English", .sys_locale = "en_US", .months = FX_EN_MONTHS, .pack = en },
    { .autonym = "Xhosa?",  .sys_locale = "xx_XX", .months = NULL,         .pack = xx },
  };
  locale_init(FX, 2, F__COUNT, 0);
  CHECK_INT(locale_count(), 2);
  CHECK_INT(locale_active(), 0);

  CHECK_STR(locale_str(F_HELLO), "Hello");
  locale_set(1);
  CHECK_STR(locale_str(F_HELLO), "Moi");
  CHECK_STR(locale_str(F_BYE),   "Bye");      // missing in XX → English fallback

  // value placement uses the localized template
  char buf[32];
  locale_format(buf, sizeof buf, F_FMT, 7);
  CHECK_STR(buf, "luku=7");
  locale_set(0);
  locale_format(buf, sizeof buf, F_FMT, 7);
  CHECK_STR(buf, "n=7");

  // out-of-range id → "" (never NULL)
  CHECK_STR(locale_str(-1), "");
  CHECK_STR(locale_str(F__COUNT), "");

  // sys matching: exact, prefix, miss
  CHECK_INT(locale_index_for_sys("en_US"), 0);
  CHECK_INT(locale_index_for_sys("xx_XX"), 1);
  CHECK_INT(locale_index_for_sys("en_GB"), 0);   // language prefix
  CHECK_INT(locale_index_for_sys("de_DE"), -1);
  CHECK_INT(locale_index_for_sys(""),      -1);

  // date: own month table (English) vs no-pad numeric (XX)
  time_t t1 = utc_time(2026, 6, 29, 14, 30);
  locale_set(0);
  locale_strftime(buf, sizeof buf, F_DATE, t1);
  CHECK_STR(buf, "Jun 29");
  locale_set(1);
  locale_strftime(buf, sizeof buf, F_DATE, t1);
  CHECK_STR(buf, "29.6.");

  // single-digit day/month honour the no-pad flag
  time_t t2 = utc_time(2026, 3, 5, 9, 7);
  locale_strftime(buf, sizeof buf, F_DATE, t2);
  CHECK_STR(buf, "5.3.");

  // SDK-strftime branch: a locale with no month table (XX) using %b falls to the
  // C library. Under setlocale("C") that yields the standard English abbreviation.
  static const char *const FMT_B[1] = { "%b" };
  uint8_t *cb = mk_pack(FMT_B, 1);
  Locale SYS_ONLY[] = {
    { .autonym = "C", .sys_locale = "C", .months = NULL, .pack = cb },
  };
  locale_init(SYS_ONLY, 1, 1, 0);
  locale_set(0);
  char mb[16];
  locale_strftime(mb, sizeof mb, 0, t1);   // June
  CHECK_STR(mb, "Jun");

  free(en); free(xx); free(cb);
}

// The value-placement engine is ours (the SDK forbids vsnprintf on the watch),
// so cover the conversions and edge cases the app tables don't exercise.
static void test_vformat(void) {
  enum { V_STR, V_MIX, V_NEG, V_PCT, V_CHR, V_UNK, V__COUNT };
  static const char *const V_EN[V__COUNT] = {
    [V_STR] = "hi %s!",   [V_MIX] = "%s=%d",     [V_NEG] = "%d",
    [V_PCT] = "100%% %d", [V_CHR] = "[%c]",      [V_UNK] = "%d%q",
  };
  uint8_t *vp = mk_pack(V_EN, V__COUNT);
  Locale V[] = {
    { .autonym = "v", .sys_locale = "v", .months = NULL, .pack = vp },
  };
  locale_init(V, 1, V__COUNT, 0);
  locale_set(0);

  char buf[32];
  locale_format(buf, sizeof buf, V_STR, "there");
  CHECK_STR(buf, "hi there!");
  locale_format(buf, sizeof buf, V_MIX, "x", 5);
  CHECK_STR(buf, "x=5");
  locale_format(buf, sizeof buf, V_NEG, -42);
  CHECK_STR(buf, "-42");
  locale_format(buf, sizeof buf, V_PCT, 7);
  CHECK_STR(buf, "100% 7");
  locale_format(buf, sizeof buf, V_CHR, 'Q');
  CHECK_STR(buf, "[Q]");
  locale_format(buf, sizeof buf, V_UNK, 3);      // unknown %q emitted verbatim
  CHECK_STR(buf, "3%q");

  // Truncation always leaves room for the terminator.
  char small[4];
  locale_format(small, sizeof small, V_STR, "world");
  CHECK_STR(small, "hi ");

  free(vp);
}

static void test_app_tables(void) {
  mk_locale_init();
  CHECK_INT(locale_count(), 2);

  // English (base) is active until a language is applied.
  CHECK_INT(locale_active(), 0);
  CHECK_STR(locale_str(STR_NEW_GAME), "New game");
  CHECK_STR(locale_str(STR_MISS),     "MISS");
  CHECK_STR(locale_autonym(0), "English");
  CHECK_STR(locale_autonym(1), "Suomi");

  // Finnish.
  locale_set(1);
  CHECK_STR(locale_str(STR_NEW_GAME), "Uusi peli");
  CHECK_STR(locale_str(STR_MISS),     "HUTI");

  // Value placement + plural-by-id.
  char buf[48];
  locale_format(buf, sizeof buf, STR_ROUND, 3);
  CHECK_STR(buf, "Kierros 3");
  locale_format(buf, sizeof buf, STR_PLAYER_COUNT_ONE, 1);
  CHECK_STR(buf, "1 pelaaja");
  locale_format(buf, sizeof buf, STR_PLAYER_COUNT_MANY, 4);
  CHECK_STR(buf, "4 pelaajaa");
  // Localized decimal separator.
  locale_format(buf, sizeof buf, STR_FMT_DECIMAL1, 9, 1);
  CHECK_STR(buf, "9,1");

  // Localized date: English month name vs Finnish numeric "klo" form.
  time_t t1 = utc_time(2026, 6, 29, 14, 30);
  locale_set(0);
  locale_strftime(buf, sizeof buf, STR_FMT_HISTORY_DATE, t1);
  CHECK_STR(buf, "Jun 29, 14:30");
  locale_set(1);
  locale_strftime(buf, sizeof buf, STR_FMT_HISTORY_DATE, t1);
  CHECK_STR(buf, "29.6. klo 14:30");

  // Finnish can't be auto-detected (Pebble has no fi_FI), so a Finnish system
  // locale must not resolve — it's only reachable via the Settings picker.
  CHECK_INT(locale_index_for_sys("en_US"), 0);
  CHECK_INT(locale_index_for_sys("fi_FI"), -1);
}

int main(void) {
  setenv("TZ", "UTC", 1);
  tzset();
  test_fixture();
  test_vformat();
  test_app_tables();
  if (g_fail) { printf("locale: %d check(s) FAILED\n", g_fail); return 1; }
  printf("locale: all checks passed\n");
  return 0;
}
