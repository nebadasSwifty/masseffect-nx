#!/usr/bin/env python3
"""Draws the NRO icon (extras/icon.jpg, 256x256) and the README banner (extras/banner.png).
Original artwork made with Pillow: a planet with an atmosphere, a ring of light and stars.
Run: python3 extras/make_icon.py   (needs Pillow)"""
import math, random, os
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

def scene(w, h, cx, cy, r, seed=7):
    """Space background, a planet of radius r at (cx, cy) and an orbital ring."""
    S = 2
    W, H = w * S, h * S
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            d = math.hypot((x / W - 0.5) * (w / h), y / H - 0.5)
            t = min(1.0, d * 1.1)
            px[x, y] = (int(4 + 10 * (1 - t)), int(7 + 16 * (1 - t)), int(18 + 34 * (1 - t)))
    rnd = random.Random(seed)
    stars = Image.new("RGB", (W, H))
    sd = ImageDraw.Draw(stars)
    for _ in range(int(w * h / 350)):
        x, y = rnd.randrange(W), rnd.randrange(H)
        b = rnd.randint(90, 255)
        s = rnd.choice((1, 1, 1, 2, 2, 3))
        sd.ellipse((x - s, y - s, x + s, y + s), fill=(b, b, min(255, b + 20)))
    img = Image.blend(img, Image.composite(stars, img, stars.convert("L")), 0.9)
    cx, cy, r = cx * S, cy * S, r * S

    # atmosphere glow
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - r * 1.18, cy - r * 1.18, cx + r * 1.18, cy + r * 1.18), fill=(30, 110, 200))
    glow = glow.filter(ImageFilter.GaussianBlur(r * 0.16))
    img = ImageChops.add(img, glow)

    # ring, back half (drawn before the planet)
    ring = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rx, ry = r * 1.75, r * 0.42
    def ring_arc(draw, start, end, color, width):
        draw.arc((cx - rx, cy - ry, cx + rx, cy + ry), start, end, fill=color, width=width)
    ring_arc(rd, 180, 360, (240, 170, 90, 255), int(r * 0.05))
    ring_arc(rd, 180, 360, (255, 225, 170, 255), int(r * 0.018))
    ring = ring.rotate(-18, center=(cx, cy), resample=Image.BICUBIC)
    base = img.convert("RGBA")
    base.alpha_composite(ring.filter(ImageFilter.GaussianBlur(0.8)))

    # planet: lit sphere with banding
    planet = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pp = planet.load()
    lx, ly, lz = -0.55, -0.6, 0.58
    n = math.sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / n, ly / n, lz / n
    for y in range(int(cy - r) - 1, int(cy + r) + 2):
        for x in range(int(cx - r) - 1, int(cx + r) + 2):
            nx, ny = (x - cx) / r, (y - cy) / r
            q = nx * nx + ny * ny
            if q >= 1.0:
                continue
            nz = math.sqrt(1 - q)
            dif = max(0.0, nx * lx + ny * ly + nz * lz)
            band = 0.5 + 0.5 * math.sin(ny * 9 + math.sin(nx * 5) * 0.9)
            br = 0.12 + 0.88 * dif ** 0.9
            rim = (1 - nz) ** 3
            col = (int((46 + 40 * band) * br + 70 * rim * dif),
                   int((96 + 50 * band) * br + 130 * rim * dif),
                   int((170 + 40 * band) * br + 230 * rim * dif))
            a = 255 if q < 0.985 else int(255 * (1 - q) / 0.015)
            pp[x, y] = (min(255, col[0]), min(255, col[1]), min(255, col[2]), a)
    base.alpha_composite(planet)

    # ring, front half
    front = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    fd = ImageDraw.Draw(front)
    ring_arc(fd, 0, 180, (240, 170, 90, 255), int(r * 0.05))
    ring_arc(fd, 0, 180, (255, 225, 170, 255), int(r * 0.018))
    front = front.rotate(-18, center=(cx, cy), resample=Image.BICUBIC)
    base.alpha_composite(front.filter(ImageFilter.GaussianBlur(0.8)))
    return base.convert("RGB").resize((w, h), Image.LANCZOS)

def font(size):
    for p in ("/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
              "/System/Library/Fonts/SFNS.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()

def spaced(draw, xy, text, f, fill, tracking):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += draw.textlength(ch, font=f) + tracking

icon = scene(256, 256, 118, 138, 78, seed=3)
icon.save(os.path.join(HERE, "icon.jpg"), quality=92)

banner = scene(1200, 273, 930, 150, 100, seed=11)
d = ImageDraw.Draw(banner)
spaced(d, (70, 82), "MASS EFFECT", font(84), (235, 240, 250), 10)
spaced(d, (74, 186), "NINTENDO SWITCH PORT", font(26), (240, 170, 90), 8)
banner.save(os.path.join(HERE, "banner.png"))
print("wrote icon.jpg and banner.png")
