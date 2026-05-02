"""Generate the OpenZip app icon.

Produces msix/Assets/source.png — a 1024x1024 RGBA icon.
Run msix/generate_assets.py afterwards to derive the MSIX visual asset set.

Style: Win11 Fluent-ish — vertical blue gradient on a rounded square,
white stylized "Z" with a downward extraction arrow.
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), 'source.png')
SIZE = 1024


def vertical_gradient(size, top, bottom):
    img = Image.new('RGBA', (size, size))
    px = img.load()
    for y in range(size):
        t = y / (size - 1)
        r = int(top[0] * (1 - t) + bottom[0] * t)
        g = int(top[1] * (1 - t) + bottom[1] * t)
        b = int(top[2] * (1 - t) + bottom[2] * t)
        row = (r, g, b, 255)
        for x in range(size):
            px[x, y] = row
    return img


def find_bold_font():
    candidates = [
        r'C:\Windows\Fonts\segoeuib.ttf',     # Segoe UI Bold
        r'C:\Windows\Fonts\seguibl.ttf',      # Segoe UI Black
        r'C:\Windows\Fonts\arialbd.ttf',      # Arial Bold
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def main():
    # Material/Fluent blue gradient.
    bg = vertical_gradient(SIZE, top=(33, 150, 243), bottom=(13, 71, 161))

    # Rounded-square mask.
    mask = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, SIZE - 1, SIZE - 1), radius=SIZE // 7, fill=255
    )

    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    img.paste(bg, mask=mask)
    draw = ImageDraw.Draw(img)

    font_path = find_bold_font()
    if not font_path:
        raise RuntimeError('No bold system font found')

    # Big white "Z", slightly raised to leave room for the arrow underneath.
    font_z = ImageFont.truetype(font_path, size=int(SIZE * 0.65))
    text = 'Z'
    bbox = draw.textbbox((0, 0), text, font=font_z)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    z_x = (SIZE - tw) / 2 - bbox[0]
    z_y = (SIZE - th) / 2 - bbox[1] - SIZE * 0.06
    draw.text((z_x, z_y), text, fill=(255, 255, 255), font=font_z)

    # Downward extraction arrow under the Z.
    cx = SIZE / 2
    bar_w = SIZE * 0.045
    bar_top = SIZE * 0.74
    bar_bot = SIZE * 0.84
    head_w = SIZE * 0.16
    head_h = SIZE * 0.07
    head_tip_y = bar_bot + head_h

    # Bar.
    draw.rounded_rectangle(
        [cx - bar_w / 2, bar_top, cx + bar_w / 2, bar_bot + 4],
        radius=int(bar_w / 2), fill=(255, 255, 255),
    )
    # Arrow head (triangle).
    draw.polygon(
        [(cx - head_w / 2, bar_bot),
         (cx + head_w / 2, bar_bot),
         (cx, head_tip_y)],
        fill=(255, 255, 255),
    )

    img.save(OUT)
    print(f'Wrote {OUT}')

    # Also produce a multi-resolution .ico for the shell extension DLL.
    ico_out = os.path.normpath(os.path.join(
        os.path.dirname(OUT), '..', '..', 'src', 'shellext', 'icon.ico'))
    img.save(ico_out, format='ICO',
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                    (64, 64), (128, 128), (256, 256)])
    print(f'Wrote {ico_out}')


if __name__ == '__main__':
    main()
