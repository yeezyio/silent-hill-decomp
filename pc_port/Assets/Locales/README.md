# Localization (Locales)

Runtime localization for the PC port, ported in spirit from
[SilentEngine](https://github.com/Sezzary/SilentEngine)'s `TranslationManager`
and rewritten in plain C (`pc_port/src/pc_locale.c`).

Each language is a folder under `Assets/Locales/` containing two files:

```
Assets/Locales/
  English/
    Metadata.json
    Locale.json
  French/
    Metadata.json
    Locale.json
```

These are copied next to the executable at build time and discovered at startup.

## Metadata.json

```json
{
    "Label":        "English (US)",   // shown in the in-game selector + launcher
    "Comment":      "…",
    "LanguageCode": "en",             // ISO 639, used for "auto" system matching
    "CountryCode":  "US",             // ISO 3166
    "Priority":     1                 // lower sorts first; English is 1
}
```

## Locale.json

A flat `{ "Key": "Value" }` map. **Missing keys fall back to the original
embedded English string**, so a partial translation is fine and English needs no
file at all.

### Text encoding (important)

Values use the game's **native** text encoding, not plain prose:

| You write | Renders as |
|-----------|------------|
| `_`       | a space    |
| `~N`      | newline (in-game messages) |
| `~E`      | end of message |
| `~C2` … `~C7` | text color |
| `~S4`     | yes/no selection prompt |
| `\n` `\t` | newline / tab (menus, item descriptions) |

Keep these codes intact when translating. The safest workflow is to start from
the English value and replace only the words.

### Fonts (unified Unicode atlas)

Text is rendered from **one** glyph atlas, rasterized from SilentEngine's
NotoSans-Bold by `pc_port/tools/build_unified_font.py` (output
`Assets/font/Font16Unified.tim` + the generated `pc_port/include/pc_glyphmap.h`).
It holds the 84 base ASCII glyphs **plus** every non-ASCII glyph the shipped
locales use (European accents + Cyrillic). The renderer decodes the locale values
as **UTF-8** and looks each code point up in the glyph map, so all languages draw
from the same font with no per-locale swap and no transliteration. The atlas is
uploaded once at boot over the stock FONT16 VRAM region; the `SILENT HILL` title
is a separate image and is unaffected.

Author values with real characters (é, ü, ñ, кириллица, …). Spaces are `_`.
If you add a language that needs glyphs not yet in the atlas, re-run
`build_unified_font.py` (it scans every `Locale.json`); the extended row holds up
to 84 glyphs beyond ASCII.

## Keys

Readable keys for menus/UI (e.g. `MainMenu_Start`, `OptionsMenu_Language`,
`InvMenu_Use`) follow SilentEngine's `TranslationKeys.h`. Bulk, index-keyed
categories use a numeric suffix the engine composes at runtime:

| Key                | Source |
|--------------------|--------|
| `Item_<id>`        | inventory item name (`id` = 32 + slot) |
| `ItemDesc_<id>`    | inventory item description |
| `SaveLoc_<id>`     | save-location name |
| `CommonMsg_<idx>`  | shared map message (0–14, every map) |
| `MapMsg_<map>_<idx>` | per-map message, e.g. `MapMsg_map0_s00_15` |

## Adding / completing a language

1. Copy `English/` to a new folder and edit `Metadata.json`.
2. Translate the values in `Locale.json` (keep the codes above).
3. To pull in the full item / message catalogue as a starting point:
   ```
   python pc_port/tools/extract_locale.py --merge Assets/Locales/<Name>/Locale.json
   ```
   This adds every `Item_*`, `ItemDesc_*`, `SaveLoc_*`, `CommonMsg_*` key (with
   the English value) that the file is missing, leaving your edits untouched.

## Selecting a language

- In-game: **Options → Extra Options → Language** (cycles with left/right;
  persists to `config.cfg`).
- Config file: `language = English` / `fr` / `auto` in `config.cfg`.
- `auto` picks the closest match to the system language, else English.
