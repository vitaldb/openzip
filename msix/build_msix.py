"""Build MSIX package for OpenZip Microsoft Store submission.

Usage: python msix/build_msix.py [version]
  version: e.g. 0.1.0 (default: read from src/core/version.h, fallback to 0.1.0)

Prerequisites:
  - Windows 10/11 SDK installed (for makeappx.exe)
  - Release|x64 build completed:
      openzip.exe + OpenZipShellExt.dll + OpenZipShellExtClassic.dll under x64/Release/
  - msix/Assets/*.png present (run generate_assets.py first)

Output:
  - build/msix/OpenZip_<version>.msix          (single-arch package)
  - msix/staging/                               (kept for sideload via Add-AppxPackage -Register)

Notes:
  - No local signing. Microsoft Store signs the package on submission.
  - Sideload during dev: `Add-AppxPackage -Register .\msix\staging\AppxManifest.xml`
"""
import os
import sys
import re
import glob
import shutil
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
STAGING_DIR = os.path.join(SCRIPT_DIR, 'staging')
ASSETS_DIR = os.path.join(SCRIPT_DIR, 'Assets')
MANIFEST_SRC = os.path.join(SCRIPT_DIR, 'AppxManifest.xml')
RELEASE_DIR = os.path.join(ROOT_DIR, 'x64', 'Release')


def find_sdk_tool(name):
    """Find a tool in the latest Windows SDK bin directory."""
    sdk_root = r'C:\Program Files (x86)\Windows Kits\10\bin'
    if not os.path.isdir(sdk_root):
        return None
    versions = sorted(glob.glob(os.path.join(sdk_root, '10.*')), reverse=True)
    for ver_dir in versions:
        tool = os.path.join(ver_dir, 'x64', name)
        if os.path.isfile(tool):
            return tool
    return None


def get_version_from_source():
    """Read version from src/core/version.h if present, else default."""
    version_h = os.path.join(ROOT_DIR, 'src', 'core', 'version.h')
    if os.path.isfile(version_h):
        with open(version_h, encoding='utf-8') as f:
            content = f.read()
        m = re.search(
            r'#define\s+OPENZIP_VERSION\s+"(\d+)\.(\d+)\.(\d+)"', content
        )
        if m:
            return f'{m.group(1)}.{m.group(2)}.{m.group(3)}'
    return '0.1.0'


def stage_files(version):
    """Populate the staging directory."""
    msix_version = version + '.0'  # MSIX requires 4-part version

    if os.path.exists(STAGING_DIR):
        shutil.rmtree(STAGING_DIR)
    os.makedirs(STAGING_DIR)
    os.makedirs(os.path.join(STAGING_DIR, 'Assets'))

    # Manifest with version substituted
    with open(MANIFEST_SRC, encoding='utf-8') as f:
        manifest = f.read()
    manifest = re.sub(
        r'(<Identity[^>]*?Version=")[\d.]+(")',
        rf'\g<1>{msix_version}\g<2>',
        manifest,
        count=1,
    )
    with open(os.path.join(STAGING_DIR, 'AppxManifest.xml'), 'w', encoding='utf-8') as f:
        f.write(manifest)

    # Assets
    pngs = glob.glob(os.path.join(ASSETS_DIR, '*.png'))
    if not pngs:
        print('WARNING: no assets found in msix/Assets/. Run generate_assets.py first.')
    for png in pngs:
        shutil.copy2(png, os.path.join(STAGING_DIR, 'Assets'))

    # Binaries
    required = ['openzip.exe', 'OpenZipShellExt.dll', 'OpenZipShellExtClassic.dll']
    missing = []
    for binname in required:
        src = os.path.join(RELEASE_DIR, binname)
        if os.path.isfile(src):
            shutil.copy2(src, STAGING_DIR)
        else:
            missing.append(src)
    if missing:
        print('ERROR: required binaries not found:')
        for m in missing:
            print(f'  {m}')
        print('Build the solution in Release|x64 first.')
        return False
    return True


def pack_msix(version):
    """Run makeappx pack on the staged files."""
    makeappx = find_sdk_tool('makeappx.exe')
    if not makeappx:
        print('ERROR: makeappx.exe not found. Install the Windows 10/11 SDK.')
        return False

    output_dir = os.path.join(ROOT_DIR, 'build', 'msix')
    os.makedirs(output_dir, exist_ok=True)
    output_msix = os.path.join(output_dir, f'OpenZip_{version}.msix')
    if os.path.isfile(output_msix):
        os.remove(output_msix)

    cmd = [makeappx, 'pack', '/d', STAGING_DIR, '/p', output_msix, '/o']
    print(f'Running: {" ".join(cmd)}')
    ret = subprocess.run(cmd, capture_output=True, text=True)
    if ret.returncode != 0:
        print(f'ERROR: makeappx failed:\n{ret.stdout}\n{ret.stderr}')
        return False

    print(f'\nMSIX package created: {output_msix}')
    print('To submit: upload this file to Microsoft Partner Center.')
    print('To sideload (Developer Mode):')
    print(f'  Add-AppxPackage -Register {os.path.join(STAGING_DIR, "AppxManifest.xml")}')
    return True


def build_msix(version=None):
    if version is None:
        version = get_version_from_source()
    print(f'Building MSIX for OpenZip {version}')
    if not stage_files(version):
        return False
    if not pack_msix(version):
        return False
    return True


if __name__ == '__main__':
    ver = sys.argv[1] if len(sys.argv) > 1 else None
    if not build_msix(ver):
        sys.exit(1)
