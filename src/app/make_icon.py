# Draws the app icon: brushed-silver disc with a purple-glow microphone on dark linen.
from PIL import Image, ImageDraw, ImageFilter

for size, name in [(57, "Icon.png"), (114, "Icon@2x.png")]:
    S = size * 4
    im = Image.new("RGB", (S, S), (24, 24, 30))
    d = ImageDraw.Draw(im)
    for y in range(0, S, 3):
        d.line([(0, y), (S, y)], fill=(34, 34, 42))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    g.ellipse([S * 0.14, S * 0.14, S * 0.86, S * 0.86], outline=(160, 110, 255, 255), width=int(S * 0.06))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.03))
    im.paste(glow, (0, 0), glow)
    for i in range(int(S * 0.32), 0, -1):
        c = int(150 + 90 * (1 - i / (S * 0.32)))
        d.ellipse(
            [S / 2 - i, S / 2 - i - (S * 0.32 - i) * 0.15, S / 2 + i, S / 2 + i - (S * 0.32 - i) * 0.15],
            fill=(c, c, c + 4),
        )
    cx, cy, k = S / 2, S / 2, S / 100
    d.rounded_rectangle([cx - 7 * k, cy - 17 * k, cx + 7 * k, cy + 5 * k], radius=7 * k, fill=(50, 50, 58))
    d.arc([cx - 12 * k, cy - 12 * k, cx + 12 * k, cy + 9 * k], 0, 180, fill=(50, 50, 58), width=int(3 * k))
    d.line([cx, cy + 9 * k, cx, cy + 15 * k], fill=(50, 50, 58), width=int(3 * k))
    d.line([cx - 7 * k, cy + 15 * k, cx + 7 * k, cy + 15 * k], fill=(50, 50, 58), width=int(3 * k))
    im.resize((size, size), Image.LANCZOS).save(name)
