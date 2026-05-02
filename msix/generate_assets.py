"""Generate MSIX icon assets at all required sizes from a source icon.

Usage: python msix/generate_assets.py
  Reads:  src/app/res/icon.ico  (or fallback to msix/Assets/source.png)
  Writes: msix/Assets/*.png

Prerequisite: pip install Pillow
"""
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
OUT_DIR = os.path.join(SCRIPT_DIR, 'Assets')

ICON_CANDIDATES = [
    os.path.join(ROOT_DIR, 'src', 'app', 'res', 'icon.ico'),
    os.path.join(SCRIPT_DIR, 'Assets', 'source.png'),
]

# MSIX visual asset sizes (square unless tuple)
ASSETS = {
    'StoreLogo.png': 50,
    'Square44x44Logo.png': 44,
    'Square150x150Logo.png': 150,
    'Wide310x150Logo.png': (310, 150),
    'LargeTile.png': (310, 310),
    'SmallTile.png': 71,
    'SplashScreen.png': (620, 300),
}


def main():
    try:
        from PIL import Image
    except ImportError:
        print('Pillow is required: pip install Pillow')
        return 1

    src_path = next((p for p in ICON_CANDIDATES if os.path.isfile(p)), None)
    if src_path is None:
        print('No source icon found. Place an icon at one of:')
        for p in ICON_CANDIDATES:
            print(f'  {p}')
        return 1

    img = Image.open(src_path)
    if src_path.endswith('.ico'):
        sizes = img.info.get('sizes', [(256, 256)])
        max_size = max(sizes, key=lambda s: s[0])
        img.size  # trigger lazy load
        if img.size != max_size:
            img = img.resize(max_size, Image.LANCZOS)

    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    os.makedirs(OUT_DIR, exist_ok=True)
    for name, size in ASSETS.items():
        w, h = (size, size) if isinstance(size, int) else size
        resized = img.resize((w, h), Image.LANCZOS)
        out = os.path.join(OUT_DIR, name)
        resized.save(out)
        print(f'  {name} ({w}x{h})')
    print('Done.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
