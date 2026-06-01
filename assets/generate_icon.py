#!/usr/bin/env python3
"""Resample icon-256.png to the launcher/appstore sizes (16, 32, 64).

icon-256.png is the source of truth. Run from this directory:

    python3 generate_icon.py
"""
import os
from PIL import Image

OUT  = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(OUT, "icon-256.png")
SIZES = (16, 32, 64)


def main():
    src = Image.open(SRC).convert("RGBA")
    for sz in SIZES:
        im = src.resize((sz, sz), Image.LANCZOS)
        path = os.path.join(OUT, f"icon{sz}.png")
        im.save(path)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
