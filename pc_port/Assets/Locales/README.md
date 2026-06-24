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

### Fonts & accents (best-effort)

The PSX font has 84 glyphs (`'` … `z`). Accents are handled at load time:

- **Acute / grave / circumflex / cedilla** (á à â ç, é è ê, …) render as an
  **accent mark overlaid** on the base letter, drawn in screen-space text
  (menus, items, save UI). In in-game *message boxes* the base letter is shown
  without the mark. Set `font_accents = 0` in `config.cfg` to fold these to plain
  ASCII instead (e.g. if the overlay position needs work on your display).
- **Umlauts** fold to the German digraph: `ä`→`ae`, `ö`→`oe`, `ü`→`ue`, `ß`→`ss`.
- **Tilde, ring, ogonek, stroke** (ñ ã, å, ą ę, ł ø) fold to the bare letter —
  the font has no mark for them.
- Anything else outside the font becomes `?`. CJK and other scripts are not
  supported by this bitmap font.

You can author values with real accented characters (é, ü, ñ, …); the engine
converts them. Spaces must still be `_`.

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
