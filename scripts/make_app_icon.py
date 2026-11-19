#!/usr/bin/env python3
"""Draws the provisional Quarry application icon and writes every size the packages need.

Output (src/app/assets/app/): quarry.ico (Windows), quarry.icns (macOS), quarry-512.png and
quarry-256.png (Linux: AppImage, Flatpak). packaging/windows/msix-assets/: the logos of the MSIX
manifest (Square44x44Logo, Square150x150Logo, StoreLogo). Needs Pillow (`pip install pillow`); the generated files
are versioned, so only whoever changes the design runs this. Replace the drawing with a designed
icon when there is one: only this script and the files it writes change.
"""
from pathlib import Path

from PIL import Image, ImageDraw

ACCENT = (0x4F, 0x46, 0xE5, 255)  # Theme accent
WHITE = (255, 255, 255, 255)
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "app" / "assets" / "app"
MSIX_OUT = ROOT / "packaging" / "windows" / "msix-assets"
SCALE = 4  # supersampling for smooth edges


def draw(size: int) -> Image.Image:
    s = size * SCALE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Rounded square with the margins of a macOS icon.
    margin = round(s * 0.06)
    d.rounded_rectangle((margin, margin, s - margin - 1, s - margin - 1),
                        radius=round(s * 0.22), fill=ACCENT)
    # A "Q": a ring and a short tail towards the lower right.
    cx = cy = s / 2
    outer = s * 0.27
    stroke = s * 0.085
    d.ellipse((cx - outer, cy - outer, cx + outer, cy + outer), fill=WHITE)
    inner = outer - stroke
    d.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=ACCENT)
    d.line((cx + outer * 0.35, cy + outer * 0.35, cx + outer * 1.02, cy + outer * 1.02),
           fill=WHITE, width=round(stroke * 1.15))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    base = draw(1024)
    base.resize((512, 512), Image.LANCZOS).save(OUT / "quarry-512.png")
    base.resize((256, 256), Image.LANCZOS).save(OUT / "quarry-256.png")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    ico_frames = [draw(n) for n in sizes]
    ico_frames[-1].save(OUT / "quarry.ico", format="ICO", sizes=[(n, n) for n in sizes],
                        append_images=ico_frames[:-1])
    base.save(OUT / "quarry.icns", format="ICNS")
    MSIX_OUT.mkdir(parents=True, exist_ok=True)
    for name, size in (("Square44x44Logo", 44), ("Square150x150Logo", 150), ("StoreLogo", 50)):
        base.resize((size, size), Image.LANCZOS).save(MSIX_OUT / f"{name}.png")
    print("wrote", ", ".join(sorted(p.name for p in OUT.iterdir())))


if __name__ == "__main__":
    main()
