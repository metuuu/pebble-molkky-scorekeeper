#!/usr/bin/env python3
"""Generate src/c/app/strings.{h,c} from tools/i18n/translations.csv.

The CSV is the single source of truth for the app's translations. Run this
whenever it changes:

    python3 tools/i18n/gen_strings.py        # or: npm run strings

CSV format (UTF-8):
    id,en,fi[,<more langs>]
  * The first language column is the base (index 0): it backs any missing
    translation and is the app's default until a saved/seeded language applies.
  * String rows have an id like STR_NEW_GAME. An empty cell means "no
    translation" -> falls back to the base language at runtime.
  * Meta rows (id starts with '@') describe each language, not a string:
        @autonym      the language's own name, shown in the picker
        @sys_locale   ISO code matched against i18n_get_system_locale()
                      (empty -> the platform has no such locale, e.g. Finnish)
        @month_01..12 month names for the %b/%B date spec; if a language leaves
                      them all empty it formats dates numerically (months=NULL)

Cells hold natural text (real newlines, real quotes); this script C-escapes
them. printf specifiers (%d, %s, %%) and strftime specifiers pass through.
"""
import csv
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
CSV_PATH = os.path.join(ROOT, "tools", "i18n", "translations.csv")
OUT_H = os.path.join(ROOT, "src", "c", "app", "strings.h")
OUT_C = os.path.join(ROOT, "src", "c", "app", "strings.c")

BANNER = ("// GENERATED FILE — do not edit by hand.\n"
          "// Source: tools/i18n/translations.csv\n"
          "// Regenerate: python3 tools/i18n/gen_strings.py  (or: npm run strings)\n")


def c_escape(s):
    out = []
    for ch in s:
        if ch == '\\':   out.append('\\\\')
        elif ch == '"':  out.append('\\"')
        elif ch == '\n': out.append('\\n')
        elif ch == '\t': out.append('\\t')
        elif ch == '\r': continue
        else:            out.append(ch)
    return ''.join(out)


def die(msg):
    sys.stderr.write("gen_strings: error: %s\n" % msg)
    sys.exit(1)


def main():
    with open(CSV_PATH, newline='', encoding='utf-8') as f:
        rows = list(csv.reader(f))
    if not rows:
        die("empty CSV")

    header = rows[0]
    if header[:1] != ['id'] or len(header) < 2:
        die("first column must be 'id', followed by one column per language")
    langs = header[1:]                       # e.g. ['en', 'fi']
    cols = {lang: i + 1 for i, lang in enumerate(langs)}

    meta = {}                                # (metakey, lang) -> value
    strings = []                             # [(STR_id, {lang: text})]
    seen = set()
    for r in rows[1:]:
        if not r or not r[0].strip():
            continue
        rid = r[0].strip()
        vals = {lang: (r[cols[lang]] if cols[lang] < len(r) else '') for lang in langs}
        if rid.startswith('@'):
            for lang in langs:
                meta[(rid, lang)] = vals[lang]
            continue
        if not rid.startswith('STR_') or not rid[4:].replace('_', '').isalnum():
            die("bad string id %r (must look like STR_FOO)" % rid)
        if rid in seen:
            die("duplicate id %r" % rid)
        seen.add(rid)
        strings.append((rid, vals))

    base = langs[0]
    for rid, vals in strings:
        if not vals[base]:
            die("string %r has no %s (base-language) text" % (rid, base))

    # ---- strings.h -------------------------------------------------------
    h = [BANNER, '#pragma once', '#include "c/lib/locale/locale.h"', '']
    h.append('// One id per translatable string; tables live in strings.c. Call sites use')
    h.append('// t(id) / tfmt(buf, n, id, ...) / tdate(buf, n, id, time). See gen_strings.py.')
    h.append('typedef enum {')
    for rid, _ in strings:
        h.append('  %s,' % rid)
    h.append('  STR__COUNT,')
    h.append('} StrId;')
    h.append('')
    h.append('// Register the locale tables. Call once at startup, before any lookup.')
    h.append('void mk_locale_init(void);')
    h.append('')
    h.append('#define t(id)                   locale_str(id)')
    h.append('#define tfmt(buf, n, id, ...)   locale_format((buf), (n), (id), __VA_ARGS__)')
    h.append('#define tdate(buf, n, id, time) locale_strftime((buf), (n), (id), (time))')
    h.append('')
    write(OUT_H, '\n'.join(h))

    # ---- strings.c -------------------------------------------------------
    width = max(len(rid) for rid, _ in strings)
    c = [BANNER, '#include "strings.h"', '']

    def cident(lang):
        return lang.upper().replace('-', '_')

    for lang in langs:
        note = ' (base)' if lang == base else ''
        c.append('// %s%s' % (meta.get(('@autonym', lang), lang), note))
        c.append('static const char *const %s[STR__COUNT] = {' % cident(lang))
        for rid, vals in strings:
            txt = vals[lang]
            if lang != base and txt == '':
                continue                     # NULL entry -> base-language fallback
            c.append('  [%-*s] = "%s",' % (width, rid, c_escape(txt)))
        c.append('};')
        c.append('')

    # month tables (only for languages that supply names)
    month_keys = ['@month_%02d' % k for k in range(1, 13)]
    lang_months = {}
    for lang in langs:
        vals = [meta.get((mk, lang), '') for mk in month_keys]
        if any(vals):
            name = '%s_MONTHS' % cident(lang)
            lang_months[lang] = name
            c.append('static const char *const %s[12] = {' % name)
            c.append('  ' + ', '.join('"%s"' % c_escape(v) for v in vals) + ',')
            c.append('};')
            c.append('')

    # locale set
    c.append('// Index 0 is the base locale: it backs missing entries and is the default.')
    c.append('static const Locale LOCALES[] = {')
    for lang in langs:
        autonym = meta.get(('@autonym', lang), lang)
        sysloc = meta.get(('@sys_locale', lang), '')
        sys_field = '"%s"' % c_escape(sysloc) if sysloc else 'NULL'
        months_field = lang_months.get(lang, 'NULL')
        c.append('  { .autonym = "%s", .sys_locale = %s, .strings = %s, .months = %s },'
                 % (c_escape(autonym), sys_field, cident(lang), months_field))
    c.append('};')
    c.append('')
    c.append('void mk_locale_init(void) {')
    c.append('  locale_init(LOCALES, (int)(sizeof LOCALES / sizeof LOCALES[0]), STR__COUNT, 0);')
    c.append('}')
    c.append('')
    write(OUT_C, '\n'.join(c))

    miss = sum(1 for _, vals in strings for lang in langs[1:] if not vals[lang])
    print("gen_strings: %d strings x %d languages -> strings.{h,c}%s"
          % (len(strings), len(langs),
             ("  (%d untranslated -> English fallback)" % miss) if miss else ""))


def write(path, text):
    if not text.endswith('\n'):
        text += '\n'
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    main()
