"""Draws the XMB pictures packed into EBOOT.PBP: ICON0.PNG (144x80, the icon
in the game list) and PIC1.PNG (480x272, the background behind it).

They are drawn from code with the app's own fonts (M PLUS Rounded 1c and
Lucide, fetched by ui_assets.py) and colors, so they can be changed here and
regenerated instead of edited by hand:

    python scripts/eboot_art.py

Needs Pillow. The output goes to assets/ and is committed, so the PSP build
itself does not need Pillow.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / ".deps" / "fonts"
OUT = ROOT / "assets"

# The Midnight theme (themes/Midnight/theme.ini).
BG = (14, 15, 19)
SURFACE = (27, 29, 36)
TEXT = (242, 242, 242)
ACCENT = (30, 215, 96)

MUSIC = ""
AUDIO_LINES = ""

SS = 4  # drawn at 4x and scaled down, for smooth edges


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size * SS)


def centered(draw, box, text, fnt, fill):
    x0, y0, x1, y1 = box
    l, t, r, b = draw.textbbox((0, 0), text, font=fnt)
    draw.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), text, font=fnt, fill=fill)


def glow(size, ellipse, color, radius, strength):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).ellipse(ellipse, fill=strength)
    mask = mask.filter(ImageFilter.GaussianBlur(radius))
    layer = Image.new("RGBA", size, color + (0,))
    layer.putalpha(mask)
    return layer


def icon0():
    w, h = 144 * SS, 80 * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Card with a soft green light from the left.
    card = Image.new("RGBA", (w, h), BG + (255,))
    card.alpha_composite(glow((w, h), (-40 * SS, -20 * SS, 90 * SS, 100 * SS), ACCENT, 30 * SS, 70))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), 14 * SS, fill=255)
    img.paste(card, (0, 0), mask)
    d.rounded_rectangle((SS, SS, w - 1 - SS, h - 1 - SS), 13 * SS, outline=(255, 255, 255, 28), width=SS)

    # Green disc with a music note.
    cx, cy, r = 32 * SS, 40 * SS, 22 * SS
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ACCENT)
    centered(d, (cx - r, cy - r + SS, cx + r, cy + r + SS), MUSIC, font("lucide.ttf", 24), BG)

    # Name.
    d.text((61 * SS, 19 * SS), "PSPotify", font=font("MPLUSRounded1c-ExtraBold.ttf", 16), fill=TEXT)
    me = font("MPLUSRounded1c-ExtraBold.ttf", 13)
    l, t, r2, b = d.textbbox((0, 0), "ME", font=me)
    px, py = 62 * SS, 44 * SS
    d.rounded_rectangle((px, py, px + (r2 - l) + 14 * SS, py + 18 * SS), 9 * SS, fill=ACCENT)
    centered(d, (px, py, px + (r2 - l) + 14 * SS, py + 18 * SS), "ME", me, BG)
    return img.resize((144, 80), Image.LANCZOS)


def pic1():
    w, h = 480 * SS, 272 * SS
    img = Image.new("RGBA", (w, h), BG + (255,))
    # Vertical shade, a little lighter at the top.
    shade = Image.linear_gradient("L").resize((w, h))
    top = Image.new("RGBA", (w, h), SURFACE + (255,))
    top.putalpha(shade.point(lambda v: 255 - v))
    img.alpha_composite(top)
    img.alpha_composite(glow((w, h), (220 * SS, 120 * SS, 620 * SS, 420 * SS), ACCENT, 70 * SS, 90))
    # A large, faint sound wave on the right, away from the XMB's icon column.
    wave = Image.new("RGBA", (w, h), ACCENT + (0,))
    centered(ImageDraw.Draw(wave), (250 * SS, 40 * SS, 470 * SS, 260 * SS), AUDIO_LINES,
             font("lucide.ttf", 190), ACCENT + (40,))
    img.alpha_composite(wave)
    return img.convert("RGB").resize((480, 272), Image.LANCZOS)


def main():
    OUT.mkdir(exist_ok=True)
    icon0().save(OUT / "ICON0.PNG", optimize=True)
    pic1().save(OUT / "PIC1.PNG", optimize=True)
    for name in ("ICON0.PNG", "PIC1.PNG"):
        print(OUT / name, (OUT / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
