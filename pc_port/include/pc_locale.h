#ifndef PC_LOCALE_H
#define PC_LOCALE_H

/* Runtime localization for the PC port.
 *
 * Design ported in spirit from SilentEngine's TranslationManager
 * (https://github.com/Sezzary/SilentEngine), rewritten in plain C and adapted to
 * the decompiled game's host:
 *   - Locale data lives on disk as Assets/Locales/<Name>/{Metadata,Locale}.json,
 *     the same layout SilentEngine uses.
 *   - Locale.json is a flat { "Key": "Value" } map; values are authored in the
 *     game's NATIVE text encoding (`_` for space, `~N`/`~C2`/`~E` control codes),
 *     so a translated string drops straight into the existing 12x16 renderer.
 *   - Lookups fall back to the original embedded English string, so a missing or
 *     absent locale renders byte-identically to the stock decomp (no regression).
 *   - Values are kept as raw UTF-8; the renderer decodes them against one unified
 *     glyph atlas (ASCII + extended accents/Cyrillic). Code points absent from the
 *     atlas are silently skipped.
 */

/* Scan Assets/Locales/, read every Metadata.json, then activate the locale named
 * by g_PcConfig.language ("auto" picks the best system-locale match, else the
 * lowest-priority locale). Call once at boot, after PcConfig_Load. */
void Loc_Init(void);

/* Resolve a translation key in the active locale. Returns the translated string
 * (raw UTF-8 in the game's native text encoding) when present, otherwise `fallback`
 * (the original decomp string) so English/untranslated text is unchanged.
 * Returns "" only when the key is missing and `fallback` is NULL. The returned
 * pointer stays valid until the active locale changes. */
const char* Loc_Get(const char* key, const char* fallback);

/* Key-composing helpers for the string tables that the game indexes by a stable
 * integer id. Each defers to Loc_Get with the original string as the fallback. */
const char* Loc_Item(int itemId, const char* fallback);
const char* Loc_ItemDesc(int itemId, const char* fallback);
const char* Loc_SaveLocation(int locationId, const char* fallback);

/* In-game map message: key is "MapMsg_<mapName>_<idx>" (per-map text) or, for the
 * 15 shared messages common to every map, the engine also tries "CommonMsg_<idx>".
 * `mapName` is the active overlay's short name, e.g. "map0_s00". */
const char* Loc_MapMsg(const char* mapName, int idx, const char* fallback);

/* --- Language selection (Options menu + launcher publish) --- */

int         Loc_Count(void);             /* registered locale count */
int         Loc_ActiveIndex(void);       /* index of active locale, or -1 */
const char* Loc_NameAt(int idx);         /* folder name, e.g. "English", or "" */
const char* Loc_LabelAt(int idx);        /* human label, e.g. "English (US)", or "" */
const char* Loc_ActiveLabel(void);       /* label of the active locale, or "" */
void        Loc_SetActiveIndex(int idx); /* activate by index + persist to config */
void        Loc_CycleActive(int dir);    /* +1/-1 with wraparound + persist */

#endif /* PC_LOCALE_H */
