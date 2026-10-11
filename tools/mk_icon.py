#!/usr/bin/env python3
"""Draws the Homebrew Channel icon (128x48 PNG) with the app's library-facade logo.

Usage: tools/mk_icon.py dist/apps/iachannel/icon.png
Needs Pillow; uses assets/fonts/NotoSans-Bold.ttf.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(out):
    img = Image.new("RGBA", (128, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, 127, 47), radius=8, fill=(36, 38, 43, 255))
    c = (242, 242, 242, 255)
    # The library facade (assets/ui/logo.png), scaled to 40 px with nearest-neighbour.
    logo = Image.open(os.path.join(ROOT, "assets/ui/logo.png")).convert("RGBA")
    logo = logo.resize((40, round(40 * logo.height / logo.width)), Image.NEAREST)
    img.alpha_composite(logo, (5, (44 - logo.height) // 2))
    font = ImageFont.truetype(os.path.join(ROOT, "assets/fonts/NotoSans-Bold.ttf"), 15)
    small = ImageFont.truetype(os.path.join(ROOT, "assets/fonts/NotoSans-Bold.ttf"), 11)
    d.text((50, 6), "Internet", font=font, fill=c)
    d.text((50, 23), "Archive", font=font, fill=c)
    d.rectangle((0, 44, 127, 47), fill=(74, 158, 232, 255))
    img.save(out)


if __name__ == "__main__":
    main(sys.argv[1])
