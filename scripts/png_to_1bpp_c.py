#!/usr/bin/env python3
"""Convert a PNG into a 1bpp packed C array for the ST7789 splash bitmap.

Output bit convention: the dark pixels in the source become bit=1 (foreground).
With ST7789_DrawBitmap1bpp(..., fg=WHITE, bg=BLACK) that draws dark line-art
as white pixels on a black background.

Pass --invert to flip (light source pixels become bit=1).
"""
import argparse
import sys
from PIL import Image


def main():
    p = argparse.ArgumentParser()
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--name", default="splash_behind_screen")
    p.add_argument("--width", type=int, default=240)
    p.add_argument("--height", type=int, default=320)
    p.add_argument("--threshold", type=int, default=128)
    p.add_argument("--invert", action="store_true",
                   help="Treat light source pixels as foreground (bit=1)")
    p.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270])
    args = p.parse_args()

    src = Image.open(args.input)
    if src.mode != "RGBA":
        src = src.convert("RGBA")

    bg = Image.new("RGBA", src.size, (255, 255, 255, 255))
    src = Image.alpha_composite(bg, src).convert("L")

    if args.rotate:
        src = src.rotate(-args.rotate, expand=True, fillcolor=255)

    sw, sh = src.size
    scale = min(args.width / sw, args.height / sh)
    new_w = max(1, int(round(sw * scale)))
    new_h = max(1, int(round(sh * scale)))
    fitted = src.resize((new_w, new_h), Image.LANCZOS)

    canvas = Image.new("L", (args.width, args.height), 255)
    canvas.paste(fitted, ((args.width - new_w) // 2, (args.height - new_h) // 2))

    px = canvas.load()
    row_bytes = (args.width + 7) // 8
    total = row_bytes * args.height
    out = bytearray(total)

    for y in range(args.height):
        for x in range(args.width):
            v = px[x, y]
            is_fg = (v >= args.threshold) if args.invert else (v < args.threshold)
            if is_fg:
                out[y * row_bytes + (x >> 3)] |= 1 << (7 - (x & 7))

    with open(args.output, "w", encoding="utf-8") as f:
        f.write('#include "splash.h"\n\n')
        f.write(f"const uint16_t {args.name}_w = {args.width};\n")
        f.write(f"const uint16_t {args.name}_h = {args.height};\n\n")
        f.write(f"const uint8_t {args.name}[{total}] = {{\n")
        for i in range(0, total, 16):
            chunk = out[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        f.write("};\n")

    print(f"Wrote {args.output}: {total} bytes "
          f"({args.width}x{args.height}, 1bpp packed MSB-first)",
          file=sys.stderr)


if __name__ == "__main__":
    main()
