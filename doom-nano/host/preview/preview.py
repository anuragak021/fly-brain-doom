"""Preview what the OLED will show for captured Doom frames, and measure dither flicker.

For each chosen frame it writes a comparison PNG:
    original (palette RGB) | gray 128x64 | Bayer 4x4 on the OLED | Floyd-Steinberg on the OLED
The OLED panels are produced by the C pipeline (liboled_pipeline.so), packed to SSD1306 bytes,
then unpacked independently, so they are exactly what the panel will light.

Example:
    .venv/bin/python host/preview/preview.py host/captures/freedoom1.npz --frames 40 120 \
        --outdir docs/preview --stats
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from oledpipe import (  # noqa: E402
    DITHER_BAYER4,
    DITHER_FS,
    DITHER_NAMES,
    DITHER_OUTLINE,
    OLED_H,
    OLED_W,
    OledPipeline,
    unpack_ssd1306,
)

PANEL_SCALE = 3  # each OLED pixel drawn as 3x3 so detail is visible in the PNG
LABEL_H = 14
GAP = 6
OLED_ON_RGB = (180, 225, 255)  # the module's pale blue-white pixels


def oled_image(bits: np.ndarray) -> Image.Image:
    rgb = np.zeros((OLED_H, OLED_W, 3), dtype=np.uint8)
    rgb[bits.astype(bool)] = OLED_ON_RGB
    return Image.fromarray(rgb).resize(
        (OLED_W * PANEL_SCALE, OLED_H * PANEL_SCALE), Image.Resampling.NEAREST
    )


def sheet(frame: np.ndarray, palette: np.ndarray, pipe: OledPipeline) -> Image.Image:
    gray = pipe.downscale(frame)
    panels = [("original 320x200", Image.fromarray(palette[frame]))]
    panels.append(
        (
            "gray 128x64 (crop + area avg)",
            Image.fromarray(gray).resize(
                (OLED_W * PANEL_SCALE, OLED_H * PANEL_SCALE), Image.Resampling.NEAREST
            ),
        )
    )
    for mode in (DITHER_OUTLINE, DITHER_BAYER4, DITHER_FS):
        shown = unpack_ssd1306(pipe.pack(pipe.dither(gray, mode)))
        panels.append((f"OLED {DITHER_NAMES[mode]}", oled_image(shown)))
    width = sum(p.width for _, p in panels) + GAP * (len(panels) - 1)
    height = max(p.height for _, p in panels) + LABEL_H
    out = Image.new("RGB", (width, height), (24, 24, 24))
    draw = ImageDraw.Draw(out)
    x = 0
    for label, panel in panels:
        draw.text((x + 2, 1), label, fill=(220, 220, 220))
        out.paste(panel, (x, LABEL_H))
        x += panel.width + GAP
    return out


def flicker_stats(frames: np.ndarray, pipe: OledPipeline) -> dict[str, float]:
    """Mean percentage of OLED pixels that flip between consecutive frames, per dither."""
    stats: dict[str, float] = {}
    grays = [pipe.downscale(f) for f in frames]
    for mode in (DITHER_OUTLINE, DITHER_BAYER4, DITHER_FS):
        prev = None
        flips: list[float] = []
        for g in grays:
            bits = pipe.dither(g, mode)
            if prev is not None:
                flips.append(100.0 * float(np.count_nonzero(bits != prev)) / bits.size)
            prev = bits
        stats[DITHER_NAMES[mode]] = float(np.mean(flips))
    # Floor: pixels whose 8 bit gray value changed at all (no dither can do better than this).
    gray_changes = [
        100.0 * float(np.count_nonzero(a != b)) / a.size
        for a, b in zip(grays[:-1], grays[1:], strict=True)
    ]
    stats["gray_changed"] = float(np.mean(gray_changes))
    return stats


def static_flicker(
    frames: np.ndarray, pipe: OledPipeline, index: int
) -> dict[str, float]:
    """Flips when the same scene changes by +1 gray level everywhere (sensor-noise style test)."""
    base = pipe.downscale(frames[index])
    nudged = np.clip(base.astype(np.int16) + 1, 0, 255).astype(np.uint8)
    return {
        DITHER_NAMES[m]: 100.0
        * float(np.count_nonzero(pipe.dither(base, m) != pipe.dither(nudged, m)))
        / base.size
        for m in (DITHER_OUTLINE, DITHER_BAYER4, DITHER_FS)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=Path, help=".npz from capture_frames.py")
    parser.add_argument(
        "--frames", type=int, nargs="+", default=[0], help="frame indices"
    )
    parser.add_argument("--black", type=int, default=16, help="LUT black point (luma)")
    parser.add_argument("--white", type=int, default=170, help="LUT white point (luma)")
    parser.add_argument(
        "--gamma", type=int, default=100, help="gamma x100 for the gray LUT"
    )
    parser.add_argument("--outdir", type=Path, default=Path("docs/preview"))
    parser.add_argument("--stats", action="store_true", help="print flicker statistics")
    args = parser.parse_args()

    data = np.load(args.capture)
    frames, palette = data["frames"], data["palette"]
    for i in args.frames:
        if not 0 <= i < len(frames):
            parser.error(f"frame {i} out of range 0..{len(frames) - 1}")

    pipe = OledPipeline()
    pipe.set_palette(palette, args.black, args.white, args.gamma)
    args.outdir.mkdir(parents=True, exist_ok=True)
    for i in args.frames:
        path = args.outdir / f"{args.capture.stem}_f{i:03d}.png"
        sheet(frames[i], palette, pipe).save(path)
        print(f"wrote {path}")
    if args.stats:
        print(
            f"sequence flicker (% pixels flipping per frame), "
            f"levels {args.black}..{args.white}, gamma {args.gamma}:"
        )
        for k, v in flicker_stats(frames, pipe).items():
            print(f"  {k:16s} {v:6.2f}")
        print("+1 gray level on a still frame (% pixels flipping):")
        for k, v in static_flicker(frames, pipe, args.frames[0]).items():
            print(f"  {k:16s} {v:6.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
