#!/usr/bin/env python3
"""Build per-locale FONT16 atlases (NotoSans) + codepage maps for the PC port.

Every locale gets its own 12x16 atlas rasterized from SilentEngine's NotoSans-Bold
so the look is consistent across languages:

  * Latin locales (English, French, ...): the 84 ASCII slots ('..z) in NotoSans.
    Accents keep using the engine's overlay (base Latin already fills the atlas).
  * Russian: digits/punctuation/cursor stay ASCII; the letter slots hold Cyrillic,
    and a codepage maps each Cyrillic code point to its slot byte.

For each locale it writes Assets/Locales/<Name>/{Font16.tim, Font16.map}. The map
is a tiny text format the engine loads:  "W <84 widths>" then "C <cp> <byte>"...
"""
import os, struct, sys
from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(__file__)
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
GLYPH_W, GLYPH_H, COLS, OFFSET = 12, 16, 21, 0x27

CYR_UPPER = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
CYR_LOWER = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"

def scratch():
    return r"C:\Users\yeezy01\AppData\Local\Temp\claude\D--silent-dev\bb15be0c-a7ae-44ba-9741-9fc8b110f276\scratchpad"
def font_path():
    return os.path.join(scratch(), "SilentEngine", "Assets", "Fonts", "NotoSans-Bold.ttf")
def loc_dir(name):
    return os.path.join(REPO, "pc_port", "Assets", "Locales", name)

def slot_x(i):
    return (i // COLS) * 256 + (i % COLS) * GLYPH_W

def load_clut():
    """16-entry grayscale CLUT from the original FONT16 (keeps the AA ramp)."""
    data = open(os.path.join(scratch(), "FONT16.TIM"), "rb").read()
    p = 8
    clutLen = struct.unpack('<I', data[p:p+4])[0]
    return data[p+12:p+12+32]  # 16 * 2 bytes

FONT = None
def raster(ch):
    """Rasterize a char into a 12x16 cell (levels 0..15) and return (levels, width)."""
    cell = Image.new("L", (GLYPH_W, GLYPH_H), 0)
    if ch != ' ':
        d = ImageDraw.Draw(cell)
        bb = FONT.getbbox(ch)
        gw = bb[2] - bb[0]
        # left-align with a 1px margin; baseline near the bottom of the cell
        ox = 1 - bb[0]
        oy = GLYPH_H - 3 - bb[3]
        d.text((ox, oy), ch, fill=255, font=FONT)
    lv = [[cell.getpixel((x, y)) >> 4 for x in range(GLYPH_W)] for y in range(GLYPH_H)]
    if ch == ' ':
        return lv, 6
    adv = int(round(FONT.getlength(ch)))
    return lv, max(3, min(GLYPH_W, adv + 1))

def write_tim(slots, path):
    """slots: list of 84 level-grids (each GLYPH_H x GLYPH_W). Atlas is 1024x16."""
    W, H = 1024, GLYPH_H
    grid = [[0]*W for _ in range(H)]
    for i, lv in enumerate(slots):
        x0 = slot_x(i)
        for y in range(H):
            for x in range(GLYPH_W):
                grid[y][x0+x] = lv[y][x]
    pix = bytearray()
    for y in range(H):
        for xw in range(W//4):
            w = 0
            for n in range(4):
                w |= (grid[y][xw*4+n] & 0xF) << (n*4)
            pix += struct.pack('<H', w)
    body = bytearray()
    body += struct.pack('<I', 0x10) + struct.pack('<I', 0x08)
    body += struct.pack('<I', 12+32) + struct.pack('<HHHH', 0, 0, 16, 1) + load_clut()
    body += struct.pack('<I', 12+len(pix)) + struct.pack('<HHHH', 0, 0, W//4, H) + bytes(pix)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(body)

def write_map(widths, codepage, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write("W " + " ".join(str(w) for w in widths) + "\n")
        for cp, byte in codepage:
            f.write(f"C 0x{cp:04X} 0x{byte:02X}\n")

def build_latin(names):
    slots, widths = [], []
    for i in range(84):
        ch = ' ' if i == 56 else chr(OFFSET + i)
        lv, w = raster(ch)
        slots.append(lv); widths.append(w)
    for nm in names:
        if os.path.isdir(loc_dir(nm)):
            write_tim(slots, os.path.join(loc_dir(nm), "Font16.tim"))
            write_map(widths, [], os.path.join(loc_dir(nm), "Font16.map"))
            print(f"[latin]    {nm}: NotoSans 84-slot atlas + map")
    return slots, widths

def build_russian(name="Russian"):
    keep = set(b"0123456789.,-?") | {ord('_'), ord('['), ord(']')}
    free = [i for i in range(84) if (OFFSET + i) not in keep and i != 56]
    cyr = list(CYR_UPPER) + list(CYR_LOWER)
    assert len(cyr) <= len(free)
    slots, widths, codepage = [], [], []
    for i in range(84):
        ch = ' ' if i == 56 else chr(OFFSET + i)
        lv, w = raster(ch); slots.append(lv); widths.append(w)
    for ch, slot in zip(cyr, free):
        lv, w = raster(ch); slots[slot] = lv; widths[slot] = w
        codepage.append((ord(ch), OFFSET + slot))
    if os.path.isdir(loc_dir(name)):
        write_tim(slots, os.path.join(loc_dir(name), "Font16.tim"))
        write_map(widths, codepage, os.path.join(loc_dir(name), "Font16.map"))
        print(f"[cyrillic] {name}: {len(cyr)} Cyrillic glyphs + {len(codepage)}-entry codepage")

def main():
    global FONT
    FONT = ImageFont.truetype(font_path(), 14)
    build_latin(["English", "French"])
    build_russian("Russian")
    print("done")

if __name__ == "__main__":
    main()
