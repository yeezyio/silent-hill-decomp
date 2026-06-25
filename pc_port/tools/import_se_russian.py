#!/usr/bin/env python3
"""Import the SilentEngine Russian translation into our locale format.

SilentEngine keys text by readable/named ids (`Item_HealthDrink`, `M1S00Msg_1`),
whereas our runtime composes numeric/structural keys (`Item_<id>`,
`MapMsg_map1_s00_15`). This tool bridges the two and rewrites the values from
SilentEngine's `{N}{T}{C3}` markup into the decomp's native text encoding
(`_` space, `~N`/`~E`/`~C2` map codes, `\\n`/`\\t` menu codes), then writes
`Assets/Locales/Russian/Locale.json`.

Mappings (all verified against SilentEngine's EnglishUs == our embedded English):
  * Item_/ItemDesc_/SaveLoc_/CommonMsg_ : matched by normalized English text.
  * MapMsg_                              : structural — SE `[Mm]{X}S{YY}Msg_{n}`
                                           is our `MapMsg_map{X}_s{YY}_{n+14}`.
  * Menu/UI keys (MainMenu_/OptionsMenu_/InvMenu_) : kept from the existing
                                           hand-checked Russian file.

The SE Russian + EnglishUs `Locale.json` files are read from --se (a checkout of
the localization branch). `extract_locale.py` is invoked internally (as a
subprocess) for the English reference. A self-check converts SE *English* and
asserts it reproduces
our embedded English exactly (code sequence + text) before any Russian is
written; it aborts on mismatch.

Usage:
    python import_se_russian.py --se /path/to/SilentEngine
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))

SE_CODE = re.compile(r'\{([A-Za-z])([0-9]*)(\([^)]*\))?\}')


# --------------------------------------------------------------------------- #
# Encoding converters: SilentEngine markup -> our native text encoding.
# --------------------------------------------------------------------------- #

def conv_map(s):
    """SE map/common-message value -> native map encoding (`~` codes).

    Rendered spaces become '_'. Each code becomes '~<tag><arg>' plus one literal
    ' ' filler, since the map renderer always consumes two bytes after '~' then
    one more (so a trailing separator is required). {T} is the inset tab the
    embedded English writes as a literal '\\t' (a no-op), not the ~T code. A jump
    code consumes exactly one positioning char, which SE writes as the {N}/{T}
    immediately following it."""
    out = []
    i, n = 0, len(s)
    while i < n:
        m = SE_CODE.match(s, i)
        if m:
            tag, dig, paren = m.group(1), m.group(2), m.group(3) or ''
            if tag == 'T':
                out.append('\t')
                i = m.end()
                continue
            if tag == 'J':
                out.append('~J' + dig + paren)
                nxt = SE_CODE.match(s, m.end())
                if nxt and nxt.group(1) == 'N':
                    out.append('\n'); i = nxt.end()
                elif nxt and nxt.group(1) == 'T':
                    out.append('\t'); i = nxt.end()
                else:
                    out.append(' '); i = m.end()
                continue
            out.append('~' + tag + dig + paren + ' ')
            i = m.end()
            continue
        c = s[i]
        out.append('_' if c == ' ' else c)
        i += 1
    return ''.join(out)


def conv_menu(s):
    """SE menu/item/desc/saveloc value -> native menu encoding.

    Rendered spaces become '_'. {N} -> '\\n', {T} -> '\\t' (no-op, mirrors the
    embedded English), {Cn} -> raw colour byte chr(n). Other codes are dropped
    (they do not occur in menu strings)."""
    out = []
    i, n = 0, len(s)
    while i < n:
        m = SE_CODE.match(s, i)
        if m:
            tag, dig = m.group(1), m.group(2)
            if tag == 'N':
                out.append('\n')
            elif tag == 'T':
                out.append('\t')
            elif tag == 'C' and dig and 2 <= int(dig) <= 7:
                out.append(chr(int(dig)))  # menu colour byte (0x02-0x07)
            i = m.end()
            continue
        c = s[i]
        out.append('_' if c == ' ' else c)
        i += 1
    return ''.join(out)


# --------------------------------------------------------------------------- #
# Validation helpers.
# --------------------------------------------------------------------------- #

def _norm(s):
    """Letters/digits only, lower-cased; strips all native + SE codes. Used to
    compare text content across encodings (keeps Cyrillic)."""
    s = re.sub(r'~[A-Za-z]\d?(\([^)]*\))?', ' ', s)
    s = re.sub(r'\{[^}]*\}', ' ', s)
    s = s.replace('_', ' ').replace('\n', ' ').replace('\t', ' ')
    for b in range(1, 8):
        s = s.replace(chr(b), ' ')
    return re.sub(r'[^0-9A-Za-zЀ-ӿ]', '', s).lower()


def _map_codes(s):
    """Ordered control-code tags of a native map string (args stripped)."""
    codes = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '~':
            tag = s[i + 1] if i + 1 < n else ''
            if tag == 'J':
                j = s.find(')', i)
                codes.append('~J'); i = (j + 1) if j > 0 else i + 2
            elif tag in 'CSL':
                codes.append('~' + tag + (s[i + 2] if i + 2 < n else '')); i += 3
            else:
                codes.append('~' + tag); i += 2
            continue
        i += 1
    return codes


def _menu_struct(s):
    return (s.count('\n'), [c for c in s if 1 <= ord(c) < 8])


def structkey(sk):
    m = re.match(r'^([Mm])(\d+)S(\d+)Msg_(\d+)$', sk)
    return (f"MapMsg_map{int(m.group(2))}_s{int(m.group(3)):02d}_{int(m.group(4)) + 14}"
            if m else None)


# --------------------------------------------------------------------------- #

def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def english_reference():
    """Run extract_locale.py to get our embedded English (native encoding)."""
    out = subprocess.check_output(
        [sys.executable, os.path.join(HERE, "extract_locale.py")],
        cwd=REPO)
    return json.loads(out.decode("utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--se", required=True,
                    help="path to a SilentEngine checkout (localization branch)")
    ap.add_argument("--ru",
                    help="path to the SE Russian Locale.json (defaults to "
                         "<se>/Assets/Locales/Russian/Locale.json)")
    args = ap.parse_args()

    se_loc = os.path.join(args.se, "Assets", "Locales")
    ru = load_json(args.ru or os.path.join(se_loc, "Russian", "Locale.json"))
    en_se = load_json(os.path.join(se_loc, "EnglishUs", "Locale.json"))
    our_en = english_reference()

    # ---- self-check: converting SE English must reproduce our embedded English.
    failures = []

    # MapMsg: structural; code sequence + text must match.
    for sk, sv in en_se.items():
        ok = structkey(sk)
        if not ok or ok not in our_en:
            continue
        conv = conv_map(sv)
        if _map_codes(conv) != _map_codes(our_en[ok]) or _norm(conv) != _norm(our_en[ok]):
            failures.append(("MapMsg", sk, ok))

    # Named categories: text-matched; build english->ourkey indices.
    def index(prefix):
        idx = {}
        for k, v in our_en.items():
            if k.startswith(prefix):
                idx.setdefault(_norm(v), []).append(k)
        return idx

    # Per category: (english-text index, value converter, structural validator).
    # The validator (alongside _norm) gates the self-check; keeping it next to the
    # converter makes it impossible for the two to drift.
    cat = {
        "Item_": (index("Item_"), conv_menu, _menu_struct),
        "ItemDesc_": (index("ItemDesc_"), conv_menu, _menu_struct),
        "SaveLoc_": (index("SaveLoc_"), conv_menu, _menu_struct),
        "CommonMsg_": (index("CommonMsg_"), conv_map, _map_codes),
    }

    def lookup(prefix, en_val):
        cand = cat[prefix][0].get(_norm(en_val))
        return cand[0] if cand and len(cand) == 1 else None

    for sk, sv in en_se.items():
        for prefix in cat:
            if sk.startswith(prefix):
                ourk = lookup(prefix, sv)
                if ourk is None:
                    break
                _, conv_fn, validate = cat[prefix]
                conv = conv_fn(sv)
                if (validate(conv) != validate(our_en[ourk]) or
                        _norm(conv) != _norm(our_en[ourk])):
                    failures.append((prefix, sk, ourk))
                break

    if failures:
        print(f"[abort] {len(failures)} self-check mismatch(es); not writing.")
        for c, sk, ok in failures[:25]:
            print(f"   {c} {sk} -> {ok}")
        sys.exit(1)
    print("[ok] self-check passed: SE English reproduces embedded English exactly.")

    # ---- build the Russian map (preserve existing hand-checked menu values).
    out_path = os.path.join(REPO, "pc_port", "Assets", "Locales", "Russian", "Locale.json")
    existing = load_json(out_path) if os.path.exists(out_path) else {}
    result = dict(existing)  # keep menu/UI translations already present

    n_map = n_named = 0
    for sk, rv in ru.items():
        ok = structkey(sk)
        if ok and ok in our_en:
            result[ok] = conv_map(rv)
            n_map += 1
            continue
        for prefix in cat:
            if sk.startswith(prefix):
                ourk = lookup(prefix, en_se.get(sk, ""))
                if ourk is not None:
                    result[ourk] = cat[prefix][1](rv)
                    n_named += 1
                break

    result["_comment"] = ("Russian translation imported from SilentEngine via "
                          "tools/import_se_russian.py. Menu/UI keys are hand-checked; "
                          "items, descriptions, save locations and all map messages "
                          "are auto-converted.")

    # Stable, readable ordering: comment, menus, then sorted bulk categories.
    def sortkey(k):
        order = ["_comment", "MainMenu_", "OptionsMenu_", "InvMenu_",
                 "Item_", "ItemDesc_", "SaveLoc_", "CommonMsg_", "MapMsg_"]
        for i, p in enumerate(order):
            if k == p or k.startswith(p):
                # numeric-aware secondary sort
                nums = tuple(int(x) for x in re.findall(r'\d+', k))
                return (i, nums, k)
        return (len(order), (), k)

    ordered = {k: result[k] for k in sorted(result, key=sortkey)}
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(ordered, f, ensure_ascii=False, indent=4)
        f.write("\n")

    # Report Cyrillic glyph coverage need.
    cps = set()
    for v in ordered.values():
        for ch in v:
            if ord(ch) >= 0x80:
                cps.add(ch)
    print(f"[done] wrote {out_path}")
    print(f"       map messages: {n_map}, named entries: {n_named}, "
          f"total keys: {len(ordered)}")
    print(f"       distinct non-ASCII glyphs used: {len(cps)}")


if __name__ == "__main__":
    main()
