"""Generate the OpenZip app icon.

Produces msix/Assets/source.png — a 1024x1024 RGBA icon.
Run msix/generate_assets.py afterwards to derive the MSIX visual asset set.

Style: Win11 Fluent-ish — vertical blue gradient on a rounded square,
single white stylized "Z" centered.
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


def render_icon(size, font_path):
    """Render the OpenZip icon at the given pixel size from scratch.

    Rendering at the target size (rather than scaling a 1024px master
    down to 16/24/32) produces clean strokes at small sizes, which is
    what users see in window title bars and the taskbar.
    """
    bg = vertical_gradient(size, top=(33, 150, 243), bottom=(13, 71, 161))
    mask = Image.new('L', (size, size), 0)
    # Rounded-square corner radius scales with size; clamp small icons to
    # at least 2px radius so they read as a square.
    r = max(2, size // 7)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size - 1, size - 1), radius=r, fill=255
    )
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    img.paste(bg, mask=mask)
    draw = ImageDraw.Draw(img)

    # Z size tuned per-icon-size: bigger relative weight at small sizes
    # so the glyph remains legible after antialiasing.
    if size <= 24:
        z_factor = 0.92
    elif size <= 48:
        z_factor = 0.85
    else:
        z_factor = 0.78
    font_z = ImageFont.truetype(font_path, size=int(size * z_factor))
    text = 'Z'
    bbox = draw.textbbox((0, 0), text, font=font_z)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    z_x = (size - tw) / 2 - bbox[0]
    z_y = (size - th) / 2 - bbox[1]
    draw.text((z_x, z_y), text, fill=(255, 255, 255), font=font_z)
    return img


def main():
    font_path = find_bold_font()
    if not font_path:
        raise RuntimeError('No bold system font found')

    # The 1024px master used by msix/generate_assets.py to derive Store tiles.
    master = render_icon(SIZE, font_path)
    master.save(OUT)
    print(f'Wrote {OUT}')

    # Multi-resolution .ico for the shell extension DLL + the GUI app.
    # Each size rendered natively rather than downsampled, so 16/24px
    # glyphs come out crisp instead of mush.
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    images = [render_icon(s, font_path) for s in sizes]
    ico_paths = [
        os.path.normpath(os.path.join(
            os.path.dirname(OUT), '..', '..', 'src', 'shellext', 'icon.ico')),
        os.path.normpath(os.path.join(
            os.path.dirname(OUT), '..', '..', 'src', 'shellext_classic', 'icon.ico')),
        os.path.normpath(os.path.join(
            os.path.dirname(OUT), '..', '..', 'src', 'app', 'icon.ico')),
    ]
    for ico_path in ico_paths:
        # PIL.IcoImagePlugin can append additional images via the `append_images`
        # kwarg on save(). Pass the smallest image as the primary so its
        # at-size frame is selected (otherwise PIL re-resamples from the
        # primary instead of using the appended frames).
        images[0].save(
            ico_path, format='ICO',
            sizes=[(s, s) for s in sizes],
            append_images=images[1:],
        )
        print(f'Wrote {ico_path}')


if __name__ == '__main__':
    main()
