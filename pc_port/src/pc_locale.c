/* Runtime localization engine for the PC port. See pc_locale.h for the design.
 *
 * Self-contained: a tiny flat-object JSON reader, an on-disk locale registry, and
 * the active key->string store that Loc_Get resolves against. Values are kept as
 * raw UTF-8; the renderer decodes them against the unified glyph atlas. No
 * external dependencies beyond libc + SDL (only for optional system-locale
 * auto-detection). */
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

#define LOC_LOCALES_DIR  "Assets/Locales"
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

static s_LocPair*   s_pairs        = NULL; /* active locale, sorted by key */
static int          s_pairCount    = 0;

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
 * UTF-8 helper (for decoding \uXXXX JSON escapes back to UTF-8)
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

/* Encode a plain display string for menu drawing: keep UTF-8 as-is (the renderer
 * decodes it against the unified atlas) and turn spaces into the renderer's '_'
 * so labels keep their gaps. Writes into `dst`. */
static void ToFontSafe(const char* src, char* dst, size_t dstSize)
{
    size_t i;
    for (i = 0; src[i] != '\0' && i + 1 < dstSize; i++)
        dst[i] = (src[i] == ' ') ? '_' : src[i];
    dst[i] = '\0';
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
        SH_DBG("[LOC] %s not found — localization disabled (English only).", LOC_LOCALES_DIR);
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
    int        count;

    FreePairs(s_pairs, s_pairCount);
    s_pairs     = NULL;
    s_pairCount = 0;
    s_activeIdx = idx;

    if (idx < 0 || idx >= s_localeCount)
        return;

    snprintf(path, sizeof(path), "%s/%s/%s", LOC_LOCALES_DIR, s_locales[idx].name, LOC_LOCALE_FILE);
    count = JsonLoadPairs(path, &pairs);

    /* Values are kept as raw UTF-8; the renderer decodes them against the unified
     * glyph atlas (ASCII + extended accents/Cyrillic). No transliteration. */

    if (count > 0)
        qsort(pairs, (size_t)count, sizeof(pairs[0]), PairCompare);

    s_pairs     = pairs;
    s_pairCount = count;

    SH_DBG("[LOC] Active locale: %s (%d strings)", s_locales[idx].name, count);
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
            SH_DBG("[LOC] Configured language '%s' not found — using default.", want);
            idx = 0;
        }
    }

    LoadActiveLocale(idx);
    SH_DBG("[LOC] %d locale(s) registered, active: %s",
           s_localeCount, (idx >= 0) ? s_locales[idx].name : "(none)");
}
