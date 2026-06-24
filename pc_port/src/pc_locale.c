/* Runtime localization engine for the PC port. See locale.h for the design.
 *
 * Self-contained: a tiny flat-object JSON reader, a UTF-8 -> PSX-glyph
 * transliterator, an on-disk locale registry, and the active key->string store
 * that Loc_Get resolves against. No external dependencies beyond libc + SDL
 * (only for optional system-locale auto-detection). */
#include "pc_locale.h"
#include "pc_config.h"
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL.h>

/* In text_draw.c; swaps the kerning table for a locale's replacement font. */
extern void Gfx_SetFontWidths(const unsigned char* widths);

#define LOC_LOCALES_DIR  "Assets/Locales"
#define LOC_FONT_FILE     "Font16.tim"
#define LOC_FONTMAP_FILE  "Font16.map"
#define LOC_METADATA_FILE "Metadata.json"
#define LOC_LOCALE_FILE   "Locale.json"
#define LOC_MAX_LOCALES   32

/* One registered locale (its on-disk metadata; strings are loaded on demand). */
typedef struct
{
    char name[64];   /* folder name, the canonical id ("English", "French", ...) */
    char label[64];  /* human label from Metadata.json ("English (US)") */
    char langCode[8];/* ISO 639 language code ("en") */
    char country[8]; /* ISO 3166 country code ("US") */
    int  priority;   /* lower sorts first; English ships as 1 */
} s_LocaleInfo;

/* One "Key": "Value" entry of the active Locale.json (value already font-encoded). */
typedef struct
{
    char* key;
    char* val;
} s_LocPair;

static s_LocaleInfo s_locales[LOC_MAX_LOCALES];
static int          s_localeCount  = 0;
static int          s_activeIdx    = -1;

/* When set, accented letters are stored as <accent-marker><base> so Gfx_StringDraw
 * overlays an accent glyph; when clear, they fold to the bare ASCII letter. Driven
 * by config `font_accents`; lets the (font-dependent) overlay be turned off without
 * a rebuild if it renders poorly. */
static int          s_fontAccents  = 1;

static s_LocPair*   s_pairs        = NULL; /* active locale, sorted by key */
static int          s_pairCount    = 0;

/* Replacement font for the active locale (e.g. the Russian Cyrillic codepage).
 * When present, Cyrillic code points transliterate to codepage bytes instead of
 * folding to '?', and the font-override uploader swaps the VRAM atlas. */
static char         s_activeFontPath[512] = {0};
static int          s_activeHasFont       = 0;
static int          s_localeGen           = 0; /* bumped on each activation */

/* Active font's codepage + kerning, loaded from the locale's Font16.map. */
typedef struct { unsigned int cp; unsigned char byte; } s_CodepageEntry;
static s_CodepageEntry s_codepage[128];
static int             s_codepageCount = 0;
static unsigned char   s_glyphWidths[84];

/* Map a code point to its atlas slot byte via the active codepage, or 0 if none. */
static int CodepageByte(unsigned int cp)
{
    int i;
    for (i = 0; i < s_codepageCount; i++)
    {
        if (s_codepage[i].cp == cp)
            return s_codepage[i].byte;
    }
    return 0;
}

/* Load a locale's Font16.map ("W <84 widths>" then "C <cp> <byte>" lines) into
 * s_glyphWidths + s_codepage. Returns 1 on success. */
static int LoadFontMap(const char* path)
{
    FILE* f = fopen(path, "r");
    char  line[256];

    s_codepageCount = 0;
    if (f == NULL)
        return 0;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (line[0] == 'W')
        {
            char* p = line + 1;
            int   i;
            for (i = 0; i < 84; i++)
                s_glyphWidths[i] = (unsigned char)strtol(p, &p, 10);
        }
        else if (line[0] == 'C' && s_codepageCount < (int)(sizeof(s_codepage) / sizeof(s_codepage[0])))
        {
            unsigned cp, byte;
            if (sscanf(line + 1, "%x %x", &cp, &byte) == 2)
            {
                s_codepage[s_codepageCount].cp   = cp;
                s_codepage[s_codepageCount].byte = (unsigned char)byte;
                s_codepageCount++;
            }
        }
    }
    fclose(f);
    return 1;
}

/* Case-insensitive string equality (avoids depending on strcasecmp portability). */
static int StrCaseEq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* ============================================================
 * UTF-8 helpers
 * ============================================================ */

/* Encode a Unicode code point as UTF-8 into `out` (>= 4 bytes). Returns length. */
static int Utf8Encode(unsigned int cp, char* out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 sequence starting at `s`. Stores the code point in *cp and
 * returns the number of bytes consumed (1 on malformed input). */
static int Utf8Decode(const unsigned char* s, unsigned int* cp)
{
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
    {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
    {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
    {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

/* ============================================================
 * Transliteration: UTF-8 -> the 84-glyph PSX font ('\'' .. 'z')
 *
 * Bytes < 0x80 pass through untouched — this preserves the game's own text codes
 * (`_` space, `~N`/`~C` markers, '\n', '\t'). Only multi-byte code points are
 * folded down to ASCII; anything with no sensible mapping becomes '?'.
 * ============================================================ */

/* PC accent-overlay markers, emitted before a base letter to tell Gfx_StringDraw
 * to draw an accent glyph over it. They live in the unused 0x0E-0x11 control byte
 * range; the in-game message renderer skips them (rendering the base letter only). */
#define LOC_ACC_ACUTE      0x0E
#define LOC_ACC_GRAVE      0x0F
#define LOC_ACC_CIRCUMFLEX 0x10
#define LOC_ACC_CEDILLA    0x11

/* Decompose a code point into a font-safe base string, optionally setting *marker
 * to a LOC_ACC_* accent to overlay. Diaereses fold to the German digraph (ae/oe/
 * ue — the font has no umlaut glyph); tilde/ogonek/ring/stroke fold to the bare
 * letter. Returns NULL when the code point has no mapping. */
static const char* AccentDecompose(unsigned int cp, char* marker)
{
    *marker = 0;
    switch (cp)
    {
        /* Latin-1 supplement. */
        case 0x00C0: *marker = LOC_ACC_GRAVE;      return "A"; /* A grave */
        case 0x00C1: *marker = LOC_ACC_ACUTE;      return "A"; /* A acute */
        case 0x00C2: *marker = LOC_ACC_CIRCUMFLEX; return "A"; /* A circumflex */
        case 0x00C3: return "A";  /* A tilde */
        case 0x00C4: return "Ae"; /* A diaeresis */
        case 0x00C5: return "A";  /* A ring */
        case 0x00C6: return "AE";
        case 0x00C7: *marker = LOC_ACC_CEDILLA;    return "C"; /* C cedilla */
        case 0x00C8: *marker = LOC_ACC_GRAVE;      return "E";
        case 0x00C9: *marker = LOC_ACC_ACUTE;      return "E";
        case 0x00CA: *marker = LOC_ACC_CIRCUMFLEX; return "E";
        case 0x00CB: return "E"; /* E diaeresis */
        case 0x00CC: *marker = LOC_ACC_GRAVE;      return "I";
        case 0x00CD: *marker = LOC_ACC_ACUTE;      return "I";
        case 0x00CE: *marker = LOC_ACC_CIRCUMFLEX; return "I";
        case 0x00CF: return "I"; /* I diaeresis */
        case 0x00D0: return "D";
        case 0x00D1: return "N"; /* N tilde */
        case 0x00D2: *marker = LOC_ACC_GRAVE;      return "O";
        case 0x00D3: *marker = LOC_ACC_ACUTE;      return "O";
        case 0x00D4: *marker = LOC_ACC_CIRCUMFLEX; return "O";
        case 0x00D5: return "O";  /* O tilde */
        case 0x00D6: return "Oe"; /* O diaeresis */
        case 0x00D8: return "O";  /* O stroke */
        case 0x00D9: *marker = LOC_ACC_GRAVE;      return "U";
        case 0x00DA: *marker = LOC_ACC_ACUTE;      return "U";
        case 0x00DB: *marker = LOC_ACC_CIRCUMFLEX; return "U";
        case 0x00DC: return "Ue"; /* U diaeresis */
        case 0x00DD: *marker = LOC_ACC_ACUTE;      return "Y";
        case 0x00DF: return "ss"; /* sharp s */
        case 0x00E0: *marker = LOC_ACC_GRAVE;      return "a";
        case 0x00E1: *marker = LOC_ACC_ACUTE;      return "a";
        case 0x00E2: *marker = LOC_ACC_CIRCUMFLEX; return "a";
        case 0x00E3: return "a";  /* a tilde */
        case 0x00E4: return "ae"; /* a diaeresis */
        case 0x00E5: return "a";  /* a ring */
        case 0x00E6: return "ae";
        case 0x00E7: *marker = LOC_ACC_CEDILLA;    return "c";
        case 0x00E8: *marker = LOC_ACC_GRAVE;      return "e";
        case 0x00E9: *marker = LOC_ACC_ACUTE;      return "e";
        case 0x00EA: *marker = LOC_ACC_CIRCUMFLEX; return "e";
        case 0x00EB: return "e"; /* e diaeresis */
        case 0x00EC: *marker = LOC_ACC_GRAVE;      return "i";
        case 0x00ED: *marker = LOC_ACC_ACUTE;      return "i";
        case 0x00EE: *marker = LOC_ACC_CIRCUMFLEX; return "i";
        case 0x00EF: return "i"; /* i diaeresis */
        case 0x00F0: return "d";
        case 0x00F1: return "n"; /* n tilde */
        case 0x00F2: *marker = LOC_ACC_GRAVE;      return "o";
        case 0x00F3: *marker = LOC_ACC_ACUTE;      return "o";
        case 0x00F4: *marker = LOC_ACC_CIRCUMFLEX; return "o";
        case 0x00F5: return "o";  /* o tilde */
        case 0x00F6: return "oe"; /* o diaeresis */
        case 0x00F8: return "o";  /* o stroke */
        case 0x00F9: *marker = LOC_ACC_GRAVE;      return "u";
        case 0x00FA: *marker = LOC_ACC_ACUTE;      return "u";
        case 0x00FB: *marker = LOC_ACC_CIRCUMFLEX; return "u";
        case 0x00FC: return "ue"; /* u diaeresis */
        case 0x00FD: *marker = LOC_ACC_ACUTE;      return "y";
        case 0x00FF: return "y"; /* y diaeresis */

        /* Latin Extended-A: the bits FR/DE/IT/ES/PL actually use. Capital and
         * small forms are kept distinct (the PSX font has A-Z and a-z). */
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        case 0x0104: return "A"; case 0x0105: return "a"; /* A/a ogonek (Polish) */
        case 0x0106: *marker = LOC_ACC_ACUTE; return "C";
        case 0x0107: *marker = LOC_ACC_ACUTE; return "c";
        case 0x0118: return "E"; case 0x0119: return "e"; /* E/e ogonek */
        case 0x0141: return "L"; case 0x0142: return "l"; /* L/l stroke */
        case 0x0143: *marker = LOC_ACC_ACUTE; return "N";
        case 0x0144: *marker = LOC_ACC_ACUTE; return "n";
        case 0x015A: *marker = LOC_ACC_ACUTE; return "S";
        case 0x015B: *marker = LOC_ACC_ACUTE; return "s";
        case 0x0179: *marker = LOC_ACC_ACUTE; return "Z";
        case 0x017A: *marker = LOC_ACC_ACUTE; return "z";
        case 0x017B: return "Z"; case 0x017C: return "z"; /* Z/z dot above */

        /* Punctuation that translators paste in from word processors. */
        case 0x2018: case 0x2019: return "'";  /* curly single quotes */
        case 0x201C: case 0x201D: return "'";  /* curly double quotes (no '"' glyph) */
        case 0x2013: case 0x2014: return "-";  /* en/em dash */
        case 0x2026: return "...";             /* ellipsis */
        case 0x00A0: return "_";               /* non-breaking space */
        case 0x00BF: return "?";               /* inverted question mark */
        case 0x00A1: return "!";               /* inverted exclamation */

        default: return NULL;
    }
}

/* Allocate a font-safe copy of a UTF-8 string. Caller frees. */
static char* Transliterate(const char* src)
{
    size_t cap = strlen(src) + 1;
    char*  dst = (char*)malloc(cap);
    size_t len = 0;
    const unsigned char* s = (const unsigned char*)src;

    if (dst == NULL)
        return NULL;

    while (*s != '\0')
    {
        const char* rep;
        int         n;

        if (*s < 0x80)
        {
            /* Grow check kept simple: each ASCII byte costs at most 1. */
            if (len + 1 >= cap)
            {
                char* grown;
                cap *= 2;
                grown = (char*)realloc(dst, cap);
                if (!grown) { free(dst); return NULL; }
                dst = grown;
            }
            dst[len++] = (char)*s++;
            continue;
        }

        {
            unsigned int cp;
            char         marker = 0;
            size_t       rl;
            int          cyr    = 0;

            n   = Utf8Decode(s, &cp);

            /* Codepage: when the active locale ships a replacement font, a mapped
             * code point (e.g. a Cyrillic letter) becomes the byte for its slot. */
            if (s_activeHasFont)
                cyr = CodepageByte(cp);

            if (cyr != 0)
            {
                rep = NULL;
            }
            else
            {
                rep = AccentDecompose(cp, &marker);
                if (rep == NULL)
                {
                    marker = 0;
                    rep    = "?";
                }
            }
            rl = cyr ? 1 : strlen(rep);

            /* +2 headroom: optional accent marker byte + base, plus the NUL. */
            while (len + rl + 2 >= cap)
            {
                char* grown;
                cap *= 2;
                grown = (char*)realloc(dst, cap);
                if (!grown) { free(dst); return NULL; }
                dst = grown;
            }

            if (cyr != 0)
            {
                dst[len++] = (char)cyr;
            }
            else
            {
                if (marker != 0 && s_fontAccents)
                    dst[len++] = marker;
                memcpy(dst + len, rep, rl);
                len += rl;
            }
        }

        s += n;
    }

    dst[len] = '\0';
    return dst;
}

/* Font-encode a plain display string for menu drawing: transliterate, then turn
 * spaces into the renderer's '_' so labels keep their gaps. Writes into `dst`. */
static void ToFontSafe(const char* src, char* dst, size_t dstSize)
{
    char*  t = Transliterate(src);
    size_t i;
    if (t == NULL) { if (dstSize) dst[0] = '\0'; return; }
    for (i = 0; t[i] != '\0' && i + 1 < dstSize; i++)
        dst[i] = (t[i] == ' ') ? '_' : t[i];
    dst[i] = '\0';
    free(t);
}

/* ============================================================
 * Minimal JSON reader for flat { "string": value, ... } objects
 * ============================================================ */

/* Read an entire file into a malloc'd NUL-terminated buffer. Caller frees. */
static char* ReadWholeFile(const char* path)
{
    FILE*  f = fopen(path, "rb");
    long   size;
    char*  buf;
    size_t got;

    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NULL; }

    got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void SkipWs(const char** p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a JSON "..." string at *p (which must point at the opening quote) into a
 * freshly allocated, escape-decoded UTF-8 string. Advances *p past the close. */
static char* ParseJsonString(const char** p)
{
    const char* s = *p + 1; /* skip opening quote */
    size_t cap = 16, len = 0;
    char*  out = (char*)malloc(cap);
    if (out == NULL) return NULL;

    #define LOC_PUT(ch) do { if (len + 1 >= cap) { char* _g; cap *= 2; _g = (char*)realloc(out, cap); if (!_g) { free(out); return NULL; } out = _g; } out[len++] = (char)(ch); } while (0)

    while (*s != '\0' && *s != '"')
    {
        if (*s != '\\')
        {
            LOC_PUT(*s);
            s++;
            continue;
        }

        s++; /* skip backslash */
        switch (*s)
        {
            case '"':  LOC_PUT('"');  s++; break;
            case '\\': LOC_PUT('\\'); s++; break;
            case '/':  LOC_PUT('/');  s++; break;
            case 'b':  LOC_PUT('\b'); s++; break;
            case 'f':  LOC_PUT('\f'); s++; break;
            case 'n':  LOC_PUT('\n'); s++; break;
            case 'r':  LOC_PUT('\r'); s++; break;
            case 't':  LOC_PUT('\t'); s++; break;
            case 'u':
            {
                unsigned int cp = 0;
                int i, ok = 1;
                for (i = 0; i < 4; i++)
                {
                    int h = HexVal(s[1 + i]);
                    if (h < 0) { ok = 0; break; }
                    cp = (cp << 4) | (unsigned)h;
                }
                if (!ok) { s += 1 + i; break; } /* skip 'u' + the i valid hex digits consumed */
                s += 5; /* past 'u' + 4 hex */

                /* UTF-16 surrogate pair -> single code point. */
                if (cp >= 0xD800 && cp <= 0xDBFF && s[0] == '\\' && s[1] == 'u')
                {
                    unsigned int lo = 0;
                    int j, ok2 = 1;
                    for (j = 0; j < 4; j++)
                    {
                        int h = HexVal(s[2 + j]);
                        if (h < 0) { ok2 = 0; break; }
                        lo = (lo << 4) | (unsigned)h;
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        s += 6;
                    }
                }

                {
                    char   enc[4];
                    int    n = Utf8Encode(cp, enc);
                    int    k;
                    for (k = 0; k < n; k++)
                        LOC_PUT(enc[k]);
                }
                break;
            }
            default:
                LOC_PUT(*s);
                s++;
                break;
        }
    }

    if (*s == '"')
        s++;

    #undef LOC_PUT

    out[len] = '\0';
    *p = s;
    return out;
}

/* Parse a JSON value at *p. Strings are decoded; bare tokens (numbers/true/false/
 * null) are captured verbatim. Returns a malloc'd string. */
static char* ParseJsonValue(const char** p)
{
    SkipWs(p);
    if (**p == '"')
        return ParseJsonString(p);

    {
        const char* start = *p;
        size_t      n;
        char*       out;
        while (**p != '\0' && **p != ',' && **p != '}' &&
               **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n')
            (*p)++;
        n   = (size_t)(*p - start);
        out = (char*)malloc(n + 1);
        if (out == NULL) return NULL;
        memcpy(out, start, n);
        out[n] = '\0';
        return out;
    }
}

static void FreePairs(s_LocPair* pairs, int count);

/* Load a flat JSON object into key/value pairs (both malloc'd, values decoded but
 * NOT transliterated). Returns the pair count; *outPairs is malloc'd. */
static int JsonLoadPairs(const char* path, s_LocPair** outPairs)
{
    char*       buf = ReadWholeFile(path);
    const char* p;
    s_LocPair*  pairs;
    int         cap = 64, count = 0;

    *outPairs = NULL;
    if (buf == NULL)
        return 0;

    pairs = (s_LocPair*)malloc(sizeof(s_LocPair) * cap);
    if (pairs == NULL) { free(buf); return 0; }

    p = buf;
    SkipWs(&p);
    if (*p != '{') { free(buf); free(pairs); return 0; }
    p++;

    for (;;)
    {
        char* key;
        char* val;

        SkipWs(&p);
        if (*p == '}' || *p == '\0')
            break;
        if (*p != '"')   /* malformed — stop gracefully */
            break;

        key = ParseJsonString(&p);
        SkipWs(&p);
        if (*p != ':') { free(key); break; }
        p++;
        val = ParseJsonValue(&p);

        if (key != NULL && val != NULL)
        {
            if (count >= cap)
            {
                s_LocPair* grown;
                cap *= 2;
                grown = (s_LocPair*)realloc(pairs, sizeof(s_LocPair) * cap);
                if (grown == NULL)
                {
                    free(key);
                    free(val);
                    FreePairs(pairs, count);
                    free(buf);
                    *outPairs = NULL;
                    return 0;
                }
                pairs = grown;
            }
            pairs[count].key = key;
            pairs[count].val = val;
            count++;
        }
        else
        {
            free(key);
            free(val);
        }

        SkipWs(&p);
        if (*p == ',') { p++; continue; }
        break;
    }

    free(buf);
    *outPairs = pairs;
    return count;
}

static void FreePairs(s_LocPair* pairs, int count)
{
    int i;
    if (pairs == NULL)
        return;
    for (i = 0; i < count; i++)
    {
        free(pairs[i].key);
        free(pairs[i].val);
    }
    free(pairs);
}

static int PairCompare(const void* a, const void* b)
{
    return strcmp(((const s_LocPair*)a)->key, ((const s_LocPair*)b)->key);
}

/* ============================================================
 * Registry
 * ============================================================ */

static int LocaleCompare(const void* a, const void* b)
{
    const s_LocaleInfo* l0 = (const s_LocaleInfo*)a;
    const s_LocaleInfo* l1 = (const s_LocaleInfo*)b;
    if (l0->priority != l1->priority)
        return l0->priority - l1->priority;
    return strcmp(l0->label, l1->label);
}

static void CopyField(char* dst, size_t dstSize, const s_LocPair* pairs, int count, const char* key)
{
    int i;
    dst[0] = '\0';
    for (i = 0; i < count; i++)
    {
        if (strcmp(pairs[i].key, key) == 0)
        {
            strncpy(dst, pairs[i].val, dstSize - 1);
            dst[dstSize - 1] = '\0';
            return;
        }
    }
}

static void RegisterLocale(const char* folderName, const char* localesDir)
{
    char       metaPath[512];
    s_LocPair* meta;
    int        metaCount;
    s_LocaleInfo* info;
    char       prio[16];

    if (s_localeCount >= LOC_MAX_LOCALES)
        return;

    snprintf(metaPath, sizeof(metaPath), "%s/%s/%s", localesDir, folderName, LOC_METADATA_FILE);
    metaCount = JsonLoadPairs(metaPath, &meta);

    info = &s_locales[s_localeCount];
    memset(info, 0, sizeof(*info));
    strncpy(info->name, folderName, sizeof(info->name) - 1);

    CopyField(info->label,    sizeof(info->label),    meta, metaCount, "Label");
    CopyField(info->langCode, sizeof(info->langCode), meta, metaCount, "LanguageCode");
    CopyField(info->country,  sizeof(info->country),  meta, metaCount, "CountryCode");
    prio[0] = '\0';
    CopyField(prio, sizeof(prio), meta, metaCount, "Priority");
    info->priority = (prio[0] != '\0') ? atoi(prio) : 1000;

    if (info->label[0] == '\0')
        strncpy(info->label, folderName, sizeof(info->label) - 1);

    FreePairs(meta, metaCount);
    s_localeCount++;
}

static int IsDirectory(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (st.st_mode & S_IFDIR) != 0;
}

static void ScanLocales(void)
{
    DIR*           dir = opendir(LOC_LOCALES_DIR);
    struct dirent* ent;

    s_localeCount = 0;
    if (dir == NULL)
    {
        SH_LOG("[LOC] %s not found — localization disabled (English only).", LOC_LOCALES_DIR);
        return;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        char sub[512];
        if (ent->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", LOC_LOCALES_DIR, ent->d_name);
        if (!IsDirectory(sub))
            continue;
        RegisterLocale(ent->d_name, LOC_LOCALES_DIR);
    }
    closedir(dir);

    qsort(s_locales, (size_t)s_localeCount, sizeof(s_locales[0]), LocaleCompare);
}

/* ============================================================
 * Active locale
 * ============================================================ */

static void LoadActiveLocale(int idx)
{
    char       path[512];
    s_LocPair* pairs;
    int        count, i;

    FreePairs(s_pairs, s_pairCount);
    s_pairs     = NULL;
    s_pairCount = 0;
    s_activeIdx = idx;
    s_localeGen++;

    /* Detect a replacement font (e.g. the Russian Cyrillic codepage). Must be set
     * BEFORE transliterating, since Cyrillic mapping depends on it. */
    s_activeHasFont     = 0;
    s_activeFontPath[0] = '\0';
    s_codepageCount     = 0;
    Gfx_SetFontWidths(NULL);
    if (idx >= 0 && idx < s_localeCount)
    {
        struct stat st;
        snprintf(s_activeFontPath, sizeof(s_activeFontPath), "%s/%s/%s",
                 LOC_LOCALES_DIR, s_locales[idx].name, LOC_FONT_FILE);
        if (stat(s_activeFontPath, &st) == 0)
        {
            char mapPath[512];
            s_activeHasFont = 1;
            snprintf(mapPath, sizeof(mapPath), "%s/%s/%s",
                     LOC_LOCALES_DIR, s_locales[idx].name, LOC_FONTMAP_FILE);
            if (LoadFontMap(mapPath))
                Gfx_SetFontWidths(s_glyphWidths);
        }
        else
        {
            s_activeFontPath[0] = '\0';
        }
    }

    if (idx < 0 || idx >= s_localeCount)
        return;

    snprintf(path, sizeof(path), "%s/%s/%s", LOC_LOCALES_DIR, s_locales[idx].name, LOC_LOCALE_FILE);
    count = JsonLoadPairs(path, &pairs);

    /* Fold every value down to the font glyph set up front so Loc_Get is a pure
     * table lookup at draw time. */
    for (i = 0; i < count; i++)
    {
        char* safe = Transliterate(pairs[i].val);
        if (safe != NULL)
        {
            free(pairs[i].val);
            pairs[i].val = safe;
        }
    }

    if (count > 0)
        qsort(pairs, (size_t)count, sizeof(pairs[0]), PairCompare);

    s_pairs     = pairs;
    s_pairCount = count;

    SH_LOG("[LOC] Active locale: %s (%d strings)", s_locales[idx].name, count);
}

const char* Loc_Get(const char* key, const char* fallback)
{
    int lo = 0, hi = s_pairCount - 1;

    if (key == NULL)
        return (fallback != NULL) ? fallback : "";

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        int c   = strcmp(key, s_pairs[mid].key);
        if (c == 0)
            return s_pairs[mid].val;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }

    return (fallback != NULL) ? fallback : "";
}

/* Path to the active locale's replacement font atlas, or "" if it uses the
 * stock font. Used by the font-override uploader. */
const char* Loc_ActiveFontPath(void)
{
    return s_activeFontPath;
}

/* Bumped on every locale activation; lets the font uploader re-apply on change. */
int Loc_Generation(void)
{
    return s_localeGen;
}

/* ============================================================
 * Id-composing helpers
 * ============================================================ */

const char* Loc_Item(int itemId, const char* fallback)
{
    char key[32];
    snprintf(key, sizeof(key), "Item_%d", itemId);
    return Loc_Get(key, fallback);
}

const char* Loc_ItemDesc(int itemId, const char* fallback)
{
    char key[32];
    snprintf(key, sizeof(key), "ItemDesc_%d", itemId);
    return Loc_Get(key, fallback);
}

const char* Loc_SaveLocation(int locationId, const char* fallback)
{
    char key[32];
    snprintf(key, sizeof(key), "SaveLoc_%d", locationId);
    return Loc_Get(key, fallback);
}

const char* Loc_MapMsg(const char* mapName, int idx, const char* fallback)
{
    char key[96];

    /* The first 15 messages are shared by every map; let a single CommonMsg_<idx>
     * entry translate them across all maps, with a per-map override still possible. */
    if (mapName != NULL)
    {
        const char* hit;
        snprintf(key, sizeof(key), "MapMsg_%s_%d", mapName, idx);
        hit = Loc_Get(key, NULL);
        if (hit[0] != '\0')
            return hit;
    }

    if (idx < 15)
    {
        const char* hit;
        snprintf(key, sizeof(key), "CommonMsg_%d", idx);
        hit = Loc_Get(key, NULL);
        if (hit[0] != '\0')
            return hit;
    }

    return (fallback != NULL) ? fallback : "";
}

/* ============================================================
 * Language selection
 * ============================================================ */

int Loc_Count(void)
{
    return s_localeCount;
}

int Loc_ActiveIndex(void)
{
    return s_activeIdx;
}

const char* Loc_NameAt(int idx)
{
    if (idx < 0 || idx >= s_localeCount)
        return "";
    return s_locales[idx].name;
}

const char* Loc_LabelAt(int idx)
{
    static char buf[64];
    if (idx < 0 || idx >= s_localeCount)
        return "";
    ToFontSafe(s_locales[idx].label, buf, sizeof(buf));
    return buf;
}

const char* Loc_ActiveLabel(void)
{
    return Loc_LabelAt(s_activeIdx);
}

void Loc_SetActiveIndex(int idx)
{
    if (idx < 0 || idx >= s_localeCount || idx == s_activeIdx)
        return;

    LoadActiveLocale(idx);
    PcConfig_SaveKeyValue("language", s_locales[idx].name);
    SH_DBG_ECHO("[LOC] Language -> %s", s_locales[idx].name);
}

void Loc_CycleActive(int dir)
{
    int next;
    if (s_localeCount <= 1 || s_activeIdx < 0)
        return;
    next = (s_activeIdx + dir + s_localeCount) % s_localeCount;
    Loc_SetActiveIndex(next);
}

/* ============================================================
 * Boot
 * ============================================================ */

/* Best system-locale match for "auto": first registered locale (in priority
 * order) whose language code is among the OS preferred locales. */
static int AutoPickLocale(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_Locale*   prefs = SDL_GetPreferredLocales();
    int           pick  = -1;

    if (prefs != NULL)
    {
        int i;
        for (i = 0; prefs[i].language != NULL && pick < 0; i++)
        {
            int j;
            for (j = 0; j < s_localeCount; j++)
            {
                if (strcmp(s_locales[j].langCode, prefs[i].language) == 0)
                {
                    pick = j;
                    break;
                }
            }
        }
        SDL_free(prefs);
    }
    if (pick >= 0)
        return pick;
#endif
    return (s_localeCount > 0) ? 0 : -1; /* lowest priority sorts first */
}

/* Publish "name:label|name:label|..." so the launcher can list this build's
 * languages, mirroring how control styles are surfaced. */
static void PublishLocales(void)
{
    char list[1024];
    int  i, len = 0;

    list[0] = '\0';
    for (i = 0; i < s_localeCount && len < (int)sizeof(list) - 1; i++)
    {
        int w = snprintf(list + len, sizeof(list) - (size_t)len, "%s%s:%s",
                         (i > 0) ? "|" : "", s_locales[i].name, s_locales[i].label);
        if (w < 0)
            break;
        len += w;
    }
    PcConfig_SaveKeyValue("languages", list);
}

void Loc_Init(void)
{
    const char* want = g_PcConfig.language;
    int         idx  = -1;
    int         i;

    s_fontAccents = g_PcConfig.fontAccents;

    ScanLocales();
    if (s_localeCount == 0)
        return;

    PublishLocales();

    /* Resolve the configured language: "auto"/empty -> system match, otherwise
     * match folder name first, then language code (case-insensitive). */
    if (want[0] == '\0' || StrCaseEq(want, "auto"))
    {
        idx = AutoPickLocale();
    }
    else
    {
        for (i = 0; i < s_localeCount; i++)
        {
            if (StrCaseEq(s_locales[i].name, want))
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            for (i = 0; i < s_localeCount; i++)
            {
                if (StrCaseEq(s_locales[i].langCode, want))
                {
                    idx = i;
                    break;
                }
            }
        }
        if (idx < 0)
        {
            SH_LOG("[LOC] Configured language '%s' not found — using default.", want);
            idx = 0;
        }
    }

    LoadActiveLocale(idx);
    SH_LOG("[LOC] %d locale(s) registered, active: %s",
           s_localeCount, (idx >= 0) ? s_locales[idx].name : "(none)");
}
