#!/usr/bin/env python3
"""Extract the decomp's embedded English strings into a localization template.

The runtime localization system (pc_port/src/locale.c) resolves text by key and
falls back to the embedded English string when a key is missing, so this tool is
a convenience for *translators*: it dumps the bulk, index-keyed categories
(item names/descriptions, save-location names, shared map messages) as
Locale.json entries with the same keys the engine composes at runtime:

    Item_<id>       item name        (id = 32 + array slot)
    ItemDesc_<id>   item description (id = 32 + array slot)
    SaveLoc_<id>    save-location name
    CommonMsg_<idx> shared map message (indices 0..14, common to every map)

Values are emitted verbatim in the game's native text encoding ('_' = space,
'~N'/'~C2'/'~E' control codes), so a translated copy drops straight into the
renderer.

Usage:
    python extract_locale.py                 # print extracted keys as JSON
    python extract_locale.py --merge PATH    # merge into an existing Locale.json
                                             # (keeps values already present)
"""

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))


def strip_comments(text):
    """Remove // and /* */ comments without touching string literals."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def keep_ntsc(text):
    """Filter out NTSCJ / non-NTSC preprocessor branches, keeping NTSC content."""
    def cond_of(expr):
        if "VERSION_REGION_IS(NTSCJ)" in expr:
            return False
        if "VERSION_REGION_IS(NTSC)" in expr:
            return True
        return True  # unrelated #if (e.g. SH_PC_PORT): keep

    frames = []  # each: {"active": bool, "taken": bool}
    emit_lines = []
    for line in text.splitlines():
        s = line.strip()
        parent = all(f["active"] for f in frames)
        if s.startswith("#if"):
            cond = cond_of(s)
            frames.append({"active": parent and cond, "taken": cond})
            continue
        if s.startswith("#elif"):
            if frames:
                f = frames[-1]
                cond = cond_of(s)
                f["active"] = parent_excluding(frames) and (not f["taken"]) and cond
                f["taken"] = f["taken"] or cond
            continue
        if s.startswith("#else"):
            if frames:
                f = frames[-1]
                f["active"] = parent_excluding(frames) and (not f["taken"])
                f["taken"] = True
            continue
        if s.startswith("#endif"):
            if frames:
                frames.pop()
            continue
        if all(f["active"] for f in frames):
            emit_lines.append(line)
    return "\n".join(emit_lines)


def parent_excluding(frames):
    return all(f["active"] for f in frames[:-1])


def tokenize_slots(body):
    """Yield one entry per array slot: a string (concatenated literals) or None
    for a NULL slot."""
    slots = []
    i, n = 0, len(body)
    while i < n:
        c = body[i]
        if c.isspace() or c == ',':
            i += 1
            continue
        if c == '"':
            parts = []
            # Gather adjacent string literals separated only by whitespace.
            while i < n and body[i] == '"':
                i += 1
                buf = []
                while i < n and body[i] != '"':
                    if body[i] == '\\' and i + 1 < n:
                        buf.append(body[i:i + 2])
                        i += 2
                        continue
                    buf.append(body[i])
                    i += 1
                i += 1  # closing quote
                parts.append("".join(buf))
                while i < n and body[i].isspace():
                    i += 1
            slots.append(decode_c_escapes("".join(parts)))
            continue
        # NULL or other identifier slot.
        m = re.match(r'[A-Za-z_]\w*', body[i:])
        if m:
            tok = m.group(0)
            i += len(tok)
            slots.append(None if tok == "NULL" else None)
            continue
        i += 1
    return slots


def decode_c_escapes(s):
    """Turn C escape sequences into their literal characters (\\n, \\t, \\xHH)."""
    return (s.replace('\\n', '\n').replace('\\t', '\t')
             .replace('\\"', '"').replace('\\\\', '\\'))


def extract_array(path, name):
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    text = keep_ntsc(text)
    m = re.search(re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', text)
    if not m:
        return []
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    body = text[start:i - 1]
    # Drop remaining preprocessor directives (e.g. the `#include` of the shared
    # message header) so their path string isn't tokenized as an array slot.
    body = "\n".join(ln for ln in body.splitlines() if not ln.lstrip().startswith("#"))
    return tokenize_slots(body)


def extract_fragment(path):
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    text = keep_ntsc(text)
    return tokenize_slots(text)


def extract_map_messages(root):
    """Walk every map overlay for its MAP_MESSAGES[] array and emit per-map keys
    `MapMsg_<mapName>_<idx>`. Indices start at 15 when the array begins with the
    shared `map_msg_common.h` include (which supplies indices 0-14)."""
    import glob

    out = {}
    for path in glob.glob(os.path.join(root, "src", "maps", "*", "*.c")):
        raw = open(path, encoding="utf-8", errors="replace").read()
        mm_pos = raw.find("MAP_MESSAGES")
        if mm_pos < 0:
            continue
        slots = extract_array(path, "MAP_MESSAGES")
        if not slots:
            continue
        map_name = os.path.basename(os.path.dirname(path))
        # The shared header supplies indices 0-14; look for its include only inside
        # the MAP_MESSAGES region so an unrelated include elsewhere can't mis-offset.
        base = 15 if "map_msg_common" in raw[mm_pos:] else 0
        for j, val in enumerate(slots):
            if val:
                out[f"MapMsg_{map_name}_{base + j}"] = val
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--merge", metavar="PATH",
                    help="merge extracted keys into an existing Locale.json "
                         "(existing values are kept)")
    ap.add_argument("--root", default=REPO_ROOT, help="decomp repo root")
    args = ap.parse_args()

    src = lambda *p: os.path.join(args.root, *p)
    out = {}

    names = extract_array(src("src", "bodyprog", "items", "item_screens_3.c"),
                          "INVENTORY_ITEM_NAMES")
    for idx, val in enumerate(names):
        if val:
            out[f"Item_{32 + idx}"] = val

    descs = extract_array(src("src", "bodyprog", "items", "item_screens_3.c"),
                          "g_ItemDescriptions")
    for idx, val in enumerate(descs):
        if val:
            out[f"ItemDesc_{32 + idx}"] = val

    locs = extract_array(src("src", "screens", "saveload", "saveload.c"),
                         "g_Savegame_SaveLocationNames")
    for idx, val in enumerate(locs):
        if val:
            out[f"SaveLoc_{idx}"] = val

    common = extract_fragment(src("include", "maps", "shared", "map_msg_common.h"))
    for idx, val in enumerate(common):
        if val:
            out[f"CommonMsg_{idx}"] = val

    map_msgs = extract_map_messages(args.root)
    out.update(map_msgs)

    sys.stderr.write(
        f"[extract] items={sum(1 for v in names if v)} "
        f"descs={sum(1 for v in descs if v)} "
        f"saveLocs={sum(1 for v in locs if v)} "
        f"commonMsgs={sum(1 for v in common if v)} "
        f"mapMsgs={len(map_msgs)}\n")

    if args.merge:
        existing = {}
        if os.path.exists(args.merge):
            existing = json.load(open(args.merge, encoding="utf-8"))
        added = 0
        for k, v in out.items():
            if k not in existing:
                existing[k] = v
                added += 1
        with open(args.merge, "w", encoding="utf-8") as f:
            json.dump(existing, f, ensure_ascii=False, indent=4)
            f.write("\n")
        sys.stderr.write(f"[extract] merged {added} new key(s) into {args.merge}\n")
    else:
        json.dump(out, sys.stdout, ensure_ascii=False, indent=4)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
