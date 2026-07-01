#!/usr/bin/env python3
"""Image utilities for the VoxelBench GI benchmark.

convert : batch-convert results/images/*.ppm -> *.png (skip up-to-date ones)
montage : build a labeled grid image from a list of (label, image) pairs

Usage:
    python3 tools/img.py convert
    python3 tools/img.py montage out.png label1 img1 label2 img2 ...
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RESULTS = os.path.join(ROOT, "results")
IMAGES_DIR = os.path.join(RESULTS, "images")

CAPTION_H = 14
PADDING = 4
SCALE = 2
MAX_COLS = 4
CAPTION_BG = (40, 40, 40)
CAPTION_FG = (255, 255, 255)


def convert(images_dir=IMAGES_DIR):
    """Convert every .ppm in images_dir to a .png alongside it.

    Skips files whose .png already exists and is newer than the .ppm.
    Returns the list of output png paths that were (re)written.
    """
    written = []
    if not os.path.isdir(images_dir):
        return written

    for name in sorted(os.listdir(images_dir)):
        if not name.lower().endswith(".ppm"):
            continue
        ppm_path = os.path.join(images_dir, name)
        png_path = os.path.splitext(ppm_path)[0] + ".png"

        if os.path.exists(png_path):
            try:
                if os.path.getmtime(png_path) >= os.path.getmtime(ppm_path):
                    continue  # already converted and up to date
            except OSError:
                pass

        try:
            with Image.open(ppm_path) as im:
                im.save(png_path)
        except Exception as e:
            print(f"warning: failed to convert {ppm_path}: {e}", file=sys.stderr)
            continue

        written.append(png_path)

    return written


def _load_image(path):
    """Open an image (ppm or png) as RGB."""
    with Image.open(path) as im:
        return im.convert("RGB").copy()


def _label_cell(img, label, font=None):
    """Return a new image: caption bar (CAPTION_H px, dark gray bg, white
    text) stacked above the (2x nearest-neighbor scaled) image."""
    w, h = img.size
    big = img.resize((w * SCALE, h * SCALE), Image.NEAREST)

    cell = Image.new("RGB", (big.width, CAPTION_H + big.height), CAPTION_BG)
    cell.paste(big, (0, CAPTION_H))

    draw = ImageDraw.Draw(cell)
    if font is None:
        font = ImageFont.load_default()

    text = str(label)
    # vertically center text in the caption bar
    try:
        bbox = draw.textbbox((0, 0), text, font=font)
        text_h = bbox[3] - bbox[1]
        ty = max(0, (CAPTION_H - text_h) // 2 - bbox[1])
    except Exception:
        ty = 1

    draw.text((3, ty), text, fill=CAPTION_FG, font=font)
    return cell


def montage(out_path, items):
    """items: list of (label, image_path) pairs.

    Builds a grid (max MAX_COLS columns) of labeled cells. Each cell is the
    source image scaled 2x with nearest-neighbor, with a 14px caption bar
    above it. Cells are separated by PADDING px of background.
    """
    if not items:
        raise ValueError("montage requires at least one (label, image) pair")

    cells = []
    font = ImageFont.load_default()
    for label, img_path in items:
        img = _load_image(img_path)
        cells.append(_label_cell(img, label, font=font))

    n = len(cells)
    cols = min(MAX_COLS, n)
    rows = (n + cols - 1) // cols

    cell_w = max(c.width for c in cells)
    cell_h = max(c.height for c in cells)

    out_w = cols * cell_w + (cols + 1) * PADDING
    out_h = rows * cell_h + (rows + 1) * PADDING

    out = Image.new("RGB", (out_w, out_h), CAPTION_BG)

    for i, cell in enumerate(cells):
        r, c = divmod(i, cols)
        x = PADDING + c * (cell_w + PADDING)
        y = PADDING + r * (cell_h + PADDING)
        out.paste(cell, (x, y))

    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    out.save(out_path)
    return out_path


def _cli_montage(argv):
    if len(argv) < 3 or len(argv) % 2 != 1:
        print(
            "usage: img.py montage <out.png> <label1> <img1> <label2> <img2> ...",
            file=sys.stderr,
        )
        return 1
    out_path = argv[0]
    rest = argv[1:]
    items = list(zip(rest[0::2], rest[1::2]))
    montage(out_path, items)
    print(f"wrote {out_path} ({len(items)} cell(s))")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__, file=sys.stderr)
        return 1

    cmd, rest = argv[0], argv[1:]
    if cmd == "convert":
        written = convert()
        print(f"converted {len(written)} image(s)")
        for p in written:
            print(f"  {p}")
        return 0
    elif cmd == "montage":
        return _cli_montage(rest)
    else:
        print(f"unknown command: {cmd}", file=sys.stderr)
        print(__doc__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
