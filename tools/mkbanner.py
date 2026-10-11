#!/usr/bin/env python3
"""Builds the channel banner, opening.bnr (the WAD's content 0), from scratch.

    tools/mkbanner.py out/opening.bnr [--png-dir dir]

opening.bnr is an IMET header followed by a U8 archive holding meta/icon.bin, meta/banner.bin and
meta/sound.bin. icon.bin and banner.bin are IMD5-wrapped, LZ77-compressed U8 archives with a
layout (arc/blyt/*.brlyt), an animation (arc/anim/*.brlan) and textures (arc/timg/*.tpl). The
layouts here are the minimum the Wii Menu needs: one picture pane per texture, each with a
plain material (no TEV stages, so the layout library's default texture stage is used).
The channel has no sound: sound.bin is 0.1 s of silence as BNS (DSP-ADPCM, the format the Wii
Menu's own banners use), because the Wii Menu expects the file.

Format references: wiibrew.org (Opening.bnr, U8 archive, TPL, BRLYT, BRLAN) and the benzin
banner tools. The art is drawn with Pillow from assets/ui/logo.png and the Noto fonts.
Needs Pillow and libWiiPy (for LZ77; pip install libWiiPy).
"""
import argparse
import hashlib
import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_BOLD = os.path.join(ROOT, "assets/fonts/NotoSans-Bold.ttf")
FONT_REG = os.path.join(ROOT, "assets/fonts/NotoSans-Regular.ttf")
CHANNEL_NAME = "Internet Archive"

BG = (26, 27, 30)
BG2 = (44, 47, 54)
TEXT = (242, 242, 242)
DIM = (170, 174, 182)
ACCENT = (74, 158, 232)


# ---- art ----

def logo(size):
    """The library facade, scaled with nearest-neighbour to keep the pixel art crisp."""
    im = Image.open(os.path.join(ROOT, "assets/ui/logo.png")).convert("RGBA")
    return im.resize((size, round(size * im.height / im.width)), Image.NEAREST)


def gradient(w, h, top, bottom):
    im = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return im


def centered(d, y, text, font, fill, w):
    tw = d.textlength(text, font=font)
    d.text(((w - tw) / 2, y), text, font=font, fill=fill)


def draw_banner_bg():
    """608x456 background; the logo is a separate, animated pane."""
    w, h = 608, 456
    im = gradient(w, h, BG2, BG)
    d = ImageDraw.Draw(im)
    centered(d, 268, "Internet Archive", ImageFont.truetype(FONT_BOLD, 50), TEXT, w)
    centered(d, 338, "Books  ·  Music  ·  Movies  ·  Wayback Machine",
             ImageFont.truetype(FONT_REG, 22), DIM, w)
    centered(d, 372, "Unofficial Wii client for archive.org", ImageFont.truetype(FONT_REG, 18), ACCENT, w)
    d.rectangle((0, h - 10, w, h), fill=ACCENT + (255,))
    return im


def draw_banner_logo():
    im = Image.new("RGBA", (192, 192), (0, 0, 0, 0))
    lg = logo(184)
    im.alpha_composite(lg, ((192 - lg.width) // 2, (192 - lg.height) // 2))
    return im


def draw_icon():
    """128x96 Wii Menu icon."""
    w, h = 128, 96
    im = gradient(w, h, BG2, BG)
    lg = logo(52)
    im.alpha_composite(lg, ((w - lg.width) // 2, 8))
    d = ImageDraw.Draw(im)
    centered(d, 62, "Internet Archive", ImageFont.truetype(FONT_BOLD, 14), TEXT, w)
    d.rectangle((0, h - 5, w, h), fill=ACCENT + (255,))
    return im


# ---- TPL (RGB5A3, CMPR) ----

def rgb5a3(p):
    r, g, b, a = p
    if a >= 0xE0:
        return 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
    return (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)


def rgb565(c):
    r, g, b = (max(0, min(255, round(v))) for v in c[:3])
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def unpack565(v):
    r, g, b = v >> 11, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def cmpr_block(pixels):
    """One 4x4 DXT1 block (opaque, 4-colour mode): the endpoints are the darkest and brightest
    pixels, and each pixel takes the nearest of the four interpolated colours."""
    lum = [p[0] * 299 + p[1] * 587 + p[2] * 114 for p in pixels]
    c0, c1 = rgb565(pixels[lum.index(max(lum))]), rgb565(pixels[lum.index(min(lum))])
    if c0 == c1:
        return struct.pack(">HHI", c0, c1, 0)
    if c0 < c1:
        c0, c1 = c1, c0
    a, b = unpack565(c0), unpack565(c1)
    palette = [a, b, [(2 * x + y) / 3 for x, y in zip(a, b)], [(x + 2 * y) / 3 for x, y in zip(a, b)]]
    bits = 0
    for p in pixels:
        i = min(range(4), key=lambda k: sum((p[j] - palette[k][j]) ** 2 for j in range(3)))
        bits = bits << 2 | i
    return struct.pack(">HHI", c0, c1, bits)


def tpl(im, cmpr=False):
    """One image: RGB5A3 in 4x4 tiles, or (cmpr, for opaque art) CMPR at 4 bits per pixel, a
    quarter of the size. The Wii Menu's banner heap is small, so large textures should be CMPR.
    Dimensions must be multiples of 4 (RGB5A3) or 8 (CMPR)."""
    w, h = im.size
    px = im.load()
    out = bytearray()
    if cmpr:
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sy, sx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                    out += cmpr_block([px[tx + sx + x, ty + sy + y] for y in range(4) for x in range(4)])
    else:
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for y in range(ty, ty + 4):
                    for x in range(tx, tx + 4):
                        v = rgb5a3(px[x, y]) if x < w and y < h else 0
                        out += struct.pack(">H", v)
    # Header, a one-entry image table at 0x0C, the image header at 0x14, pixel data at 0x40.
    header = struct.pack(">III", 0x0020AF30, 1, 0x0C) + struct.pack(">II", 0x14, 0)
    img = struct.pack(">HHIIIIIIfBBBB", h, w, 14 if cmpr else 5, 0x40, 0, 0, 1, 1, 0.0, 0, 0, 0, 0)
    head = header + img
    head += b"\0" * (0x40 - len(head))
    return head + bytes(out)


# ---- BRLYT ----

def section(magic, body):
    body = bytes(body)
    body += b"\0" * (-len(body) % 4)
    return magic + struct.pack(">I", len(body) + 8) + body


def name(s, n):
    b = s.encode()
    assert len(b) < n, s
    return b + b"\0" * (n - len(b))


def pane_body(nm, w, h, x=0.0, y=0.0, alpha=255):
    # flags (visible), origin (centre), alpha, pad, name, user data, translate, rotate, scale, size
    return (struct.pack(">BBBB", 1, 4, alpha, 0) + name(nm, 16) + b"\0" * 8 +
            struct.pack(">fff fff ff ff".replace(" ", ""), x, y, 0, 0, 0, 0, 1, 1, w, h))


def material(nm, tex_index):
    body = name(nm, 20)
    body += struct.pack(">4h", 0, 0, 0, 0)            # black colour (TEV colour 1)
    body += struct.pack(">4h", 255, 255, 255, 255)    # white colour (TEV colour 2)
    body += struct.pack(">4h", 255, 255, 255, 255)    # TEV colour 3
    body += b"\xff" * 16                               # konstant colours
    flags = 1 | 1 << 4 | 1 << 8                       # one texture map, SRT and texcoord gen
    body += struct.pack(">I", flags)
    body += struct.pack(">HBB", tex_index, 0, 0)      # texture, clamp, clamp
    body += struct.pack(">5f", 0, 0, 0, 1, 1)         # identity SRT
    body += struct.pack(">BBBB", 1, 4, 0x1E, 0)       # GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY
    return body


def brlyt(width, height, pictures):
    """pictures: (pane name, texture file name, w, h, x, y)."""
    secs = [section(b"lyt1", struct.pack(">B3xff", 1, width, height))]

    names = [p[1] for p in pictures]
    table = b""
    strings = b""
    for n in names:
        table += struct.pack(">II", len(names) * 8 + len(strings), 0)
        strings += n.encode() + b"\0"
    secs.append(section(b"txl1", struct.pack(">HH", len(names), 0) + table + strings))

    mats = [material(p[0], i) for i, p in enumerate(pictures)]
    offs = []
    off = 8 + 4 + 4 * len(mats)
    for m in mats:
        offs.append(off)
        off += len(m)
    secs.append(section(b"mat1", struct.pack(">HH", len(mats), 0) + b"".join(struct.pack(">I", o) for o in offs) +
                        b"".join(mats)))

    secs.append(section(b"pan1", pane_body("RootPane", width, height)))
    secs.append(section(b"pas1", b""))
    for i, (nm, _, w, h, x, y) in enumerate(pictures):
        body = pane_body(nm, w, h, x, y) + struct.pack(">4I", *([0xFFFFFFFF] * 4))
        body += struct.pack(">HBB", i, 1, 0)
        body += struct.pack(">8f", 0, 0, 1, 0, 0, 1, 1, 1)
        secs.append(section(b"pic1", body))
    secs.append(section(b"pae1", b""))
    secs.append(section(b"grp1", name("RootGroup", 16) + struct.pack(">HH", 0, 0)))

    data = b"".join(secs)
    return struct.pack(">4sHHIHH", b"RLYT", 0xFEFF, 8, 16 + len(data), 16, len(secs)) + data


# ---- BRLAN ----

def brlan(frames, entries):
    """entries: (pane name, [(target, [(frame, value, slope), ...]), ...]) as pane SRT (RLPA) curves.
    Targets: 0/1 translate x/y, 5 rotate z, 6/7 scale x/y."""
    blobs = []
    for pane, curves in entries:
        tag = bytearray(b"RLPA" + struct.pack(">B3x", len(curves)))
        tag_offs_at = len(tag)
        tag += b"\0" * (4 * len(curves))
        for ci, (target, keys) in enumerate(curves):
            struct.pack_into(">I", tag, tag_offs_at + 4 * ci, len(tag))
            # index (texture/colour slot, unused for panes), target, curve type 2 (hermite)
            tag += struct.pack(">BBBBHHI", 0, target, 2, 0, len(keys), 0, 12)
            for k in keys:
                tag += struct.pack(">fff", *k)
        entry = name(pane, 20) + struct.pack(">BBHI", 1, 0, 0, 20 + 8)
        blobs.append(entry + bytes(tag))
    body = bytearray(struct.pack(">HBBHHI", frames, 1, 0, 0, len(blobs), 0x14))
    table_at = len(body)
    body += b"\0" * (4 * len(blobs))
    for i, b in enumerate(blobs):
        struct.pack_into(">I", body, table_at + 4 * i, len(body) + 8)
        body += b
    sec = section(b"pai1", body)
    return struct.pack(">4sHHIHH", b"RLAN", 0xFEFF, 8, 16 + len(sec), 16, 1) + sec


# ---- U8, IMD5, sound, IMET ----

def u8(files):
    """files: {"arc/blyt/banner.brlyt": bytes, ...} -> U8 archive (directories derived)."""
    tree = {}
    for path, data in files.items():
        node = tree
        parts = path.split("/")
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = data

    nodes = []  # [type, name, data or parent, size or end]
    def walk(d, parent):
        for k in sorted(d, key=lambda s: (isinstance(d[s], dict), s.lower())):
            v = d[k]
            if isinstance(v, dict):
                idx = len(nodes)
                nodes.append([1, k, parent, None])
                walk(v, idx)
                nodes[idx][3] = len(nodes)
            else:
                nodes.append([0, k, v, len(v)])
    nodes.append([1, "", 0, None])
    walk(tree, 0)
    nodes[0][3] = len(nodes)

    strings = b""
    name_offs = []
    for n in nodes:
        name_offs.append(len(strings))
        strings += n[1].encode() + b"\0"
    header_size = 12 * len(nodes) + len(strings)
    data_off = (0x20 + header_size + 0x3F) & ~0x3F
    data = b""
    table = b""
    for n, no in zip(nodes, name_offs):
        if n[0] == 1:
            table += struct.pack(">I II", 1 << 24 | no, n[2], n[3])
        else:
            data += b"\0" * (-len(data) % 0x20)
            table += struct.pack(">III", no, data_off + len(data), n[3])
            data += n[2]
    head = struct.pack(">IIII", 0x55AA382D, 0x20, header_size, data_off) + b"\0" * 16 + table + strings
    head += b"\0" * (data_off - len(head))
    return head + data


def lz77(data):
    try:
        from libWiiPy.archive.lz77 import compress_lz77
    except ImportError:
        return data
    c = compress_lz77(data)
    if not c.startswith(b"LZ77"):
        c = b"LZ77" + c
    return c


def imd5(data):
    return b"IMD5" + struct.pack(">I", len(data)) + b"\0" * 8 + hashlib.md5(data).digest() + data


SOUND_RATE = 32000


def bns_silence(ms=100):
    """A short silent BNS (the Wii Menu's banner sound format): the channel has no sound, but the
    Wii Menu expects meta/sound.bin. DSP-ADPCM frames of zeros, the same on both channels, no
    loop. Layout: header 0x20, INFO 0xA0 (offsets inside it are from INFO+8), DATA with each
    channel's frames one after the other."""
    frames = (SOUND_RATE * ms // 1000 + 13) // 14
    data = bytes(8 * frames)  # 14 samples per 8-byte frame: header (predictor 0, scale 0), nibbles 0
    nsamples = frames * 14
    payload = data + data
    payload += b"\0" * (-len(payload) % 32)
    info = bytearray(0x98)
    struct.pack_into(">BBBBHHII", info, 0, 0, 0, 2, 0, SOUND_RATE, 0, 0, nsamples)
    struct.pack_into(">II", info, 0x10, 0x18, 0)
    struct.pack_into(">II", info, 0x18, 0x20, 0x2C)          # per-channel info
    struct.pack_into(">III", info, 0x20, 0, 0x38, 0)          # channel 1: data at 0
    struct.pack_into(">III", info, 0x2C, len(data), 0x68, 0)  # channel 2
    # ADPCM info at 0x38 and 0x68 stays zero: coefficients, gain, predictor/scale, history.
    info_chunk = b"INFO" + struct.pack(">I", 0xA0) + bytes(info)
    data_chunk = b"DATA" + struct.pack(">I", 8 + len(payload)) + payload
    header = b"BNS " + struct.pack(">IIHHIIII", 0xFEFF0100, 0x20 + len(info_chunk) + len(data_chunk), 0x20, 2,
                                   0x20, len(info_chunk), 0x20 + len(info_chunk), len(data_chunk))
    return header + info_chunk + data_chunk


def imet(sizes, title):
    h = bytearray(0x600)
    h[0x40:0x44] = b"IMET"
    struct.pack_into(">II", h, 0x44, 0x600, 3)
    struct.pack_into(">III", h, 0x4C, *sizes)  # icon, banner, sound
    for i in range(10):
        n = title.encode("utf-16-be")[:82]
        h[0x5C + 84 * i:0x5C + 84 * i + len(n)] = n
    h[0x5F0:0x600] = hashlib.md5(bytes(h)).digest()
    return bytes(h)


def build(png_dir=None):
    bg, lg, ic = draw_banner_bg(), draw_banner_logo(), draw_icon()
    if png_dir:
        os.makedirs(png_dir, exist_ok=True)
        preview = bg.copy()
        preview.alpha_composite(lg, ((608 - 192) // 2, 52))
        preview.save(os.path.join(png_dir, "banner.png"))
        ic.save(os.path.join(png_dir, "icon.png"))

    # Layout coordinates are centred, y up. The logo bobs gently over a 4 s loop.
    banner = u8({
        "arc/blyt/banner.brlyt": brlyt(608, 456, [("Background", "bg.tpl", 608, 456, 0, 0),
                                                  ("Logo", "logo.tpl", 192, 192, 0, 80)]),
        "arc/anim/banner.brlan": brlan(240, [("Logo", [(1, [(0, 80, 0), (120, 88, 0), (240, 80, 0)])])]),
        "arc/timg/bg.tpl": tpl(bg, cmpr=True),
        "arc/timg/logo.tpl": tpl(lg),
    })
    icon = u8({
        "arc/blyt/icon.brlyt": brlyt(128, 96, [("Icon", "icon.tpl", 128, 96, 0, 0)]),
        "arc/anim/icon.brlan": brlan(60, [("Icon", [(6, [(0, 1, 0), (60, 1, 0)])])]),
        "arc/timg/icon.tpl": tpl(ic),
    })
    icon_bin = imd5(lz77(icon))
    banner_bin = imd5(lz77(banner))
    sound_bin = imd5(bns_silence())
    meta = u8({"meta/icon.bin": icon_bin, "meta/banner.bin": banner_bin, "meta/sound.bin": sound_bin})
    return imet([len(icon_bin), len(banner_bin), len(sound_bin)], CHANNEL_NAME) + meta


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--png-dir", help="also write banner.png and icon.png previews here")
    a = ap.parse_args()
    data = build(a.png_dir)
    with open(a.out, "wb") as f:
        f.write(data)
    print(f"{a.out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
