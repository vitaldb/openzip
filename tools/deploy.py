"""One-command release for OpenZip maintainers.

Usage:
    python tools/deploy.py <version> [--dry-run] [--skip-build]

Steps (all run locally on the maintainer's machine):

    1. Verify clean git tree (no uncommitted changes on current branch).
    2. msbuild OpenZip.sln /p:Configuration=Release /p:Platform=x64
    3. Run unit tests (CoreTests.exe must be 0/0 failing).
    4. python msix/build_msix.py <version>  ->  build/msix/OpenZip_<version>.msix
    5. git tag v<version> + push tag to origin.
    6. gh release create v<version> <msix> --notes-file CHANGELOG fragment.

Why local instead of GitHub Actions:
    - The .vcxproj uses PlatformToolset=v145 (VS 2026), which the standard
      windows-latest GitHub runner does not have. Pinning to v143 in CI would
      diverge from the developer environment.
    - OpenZip is a quarterly-or-less release cycle, single maintainer. CI
      maintenance cost outweighs convenience.
    - The build is fully deterministic from the local checkout; hostility-of-
      CI-environment is the larger risk than provenance signing.

Prerequisites:
    - GitHub CLI (gh) authenticated to vitaldb/openzip
    - msbuild on PATH (run from "Developer PowerShell for VS")
    - Python 3.10+
    - Clean working tree on the branch you want to release from
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RELEASE_DIR = ROOT / 'x64' / 'Release'
BUILD_DIR = ROOT / 'build' / 'msix'


def die(msg: str, code: int = 1) -> None:
    print(f'\nERROR: {msg}', file=sys.stderr)
    sys.exit(code)


def step(num: str, msg: str) -> None:
    bar = '─' * (78 - len(num) - len(msg) - 4)
    print(f'\n[{num}] {msg} {bar}')


def run(cmd: list[str], *, cwd: Path | None = None, check: bool = True) -> int:
    print(f'  $ {" ".join(cmd)}')
    rc = subprocess.call(cmd, cwd=cwd or ROOT)
    if check and rc != 0:
        die(f'command failed (exit {rc}): {" ".join(cmd)}')
    return rc


def check_git_clean() -> None:
    out = subprocess.check_output(
        ['git', 'status', '--porcelain'], cwd=ROOT, text=True,
    )
    if out.strip():
        print(out)
        die('Working tree is dirty. Commit or stash before releasing.')


def check_branch() -> str:
    branch = subprocess.check_output(
        ['git', 'rev-parse', '--abbrev-ref', 'HEAD'], cwd=ROOT, text=True,
    ).strip()
    print(f'  branch: {branch}')
    if branch == 'HEAD':
        die('Detached HEAD — checkout a branch first.')
    return branch


def find_msbuild() -> str:
    """Locate msbuild.exe. Prefer PATH, then well-known VS install dirs."""
    found = shutil.which('msbuild') or shutil.which('msbuild.exe')
    if found:
        return found
    candidates = [
        r'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
        r'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        r'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        r'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    die('msbuild not found. Open "Developer PowerShell for VS" or install VS.')


def find_gh() -> str:
    found = shutil.which('gh') or shutil.which('gh.exe')
    if not found:
        die('GitHub CLI (gh) not found. Install from https://cli.github.com')
    return found


def build_release(msbuild: str) -> None:
    sln = ROOT / 'OpenZip.sln'
    run([
        msbuild, str(sln),
        '/p:Configuration=Release', '/p:Platform=x64',
        '/v:minimal', '/nologo', '/m',
    ])


def run_tests() -> None:
    debug_tests = ROOT / 'x64' / 'Debug' / 'CoreTests.exe'
    if not debug_tests.exists():
        # Build Debug specifically for the test binary if missing.
        msbuild = find_msbuild()
        run([
            msbuild, str(ROOT / 'OpenZip.sln'),
            '/p:Configuration=Debug', '/p:Platform=x64',
            '/t:CoreTests', '/v:minimal', '/nologo',
        ])
    run([str(debug_tests)])


def build_msix(version: str) -> Path:
    script = ROOT / 'msix' / 'build_msix.py'
    run([sys.executable, str(script), version])
    msix = BUILD_DIR / f'OpenZip_{version}.msix'
    if not msix.exists():
        die(f'Expected MSIX not found at {msix}')
    print(f'  -> {msix} ({msix.stat().st_size // 1024} KB)')
    return msix


def make_tag(version: str, dry_run: bool) -> str:
    tag = f'v{version}'
    existing = subprocess.run(
        ['git', 'tag', '-l', tag], cwd=ROOT, capture_output=True, text=True,
    ).stdout.strip()
    if existing:
        print(f'  tag {tag} already exists locally; reusing it.')
    else:
        if dry_run:
            print(f'  (dry-run) would: git tag {tag}')
        else:
            run(['git', 'tag', tag])
    if dry_run:
        print(f'  (dry-run) would: git push origin {tag}')
    else:
        run(['git', 'push', 'origin', tag])
    return tag


def github_release(gh: str, tag: str, msix: Path, dry_run: bool) -> None:
    notes_file = ROOT / 'RELEASE_NOTES.md'
    notes_arg: list[str]
    if notes_file.exists():
        notes_arg = ['--notes-file', str(notes_file)]
    else:
        notes_arg = ['--generate-notes']
    cmd = [
        gh, 'release', 'create', tag, str(msix),
        '--title', f'OpenZip {tag.lstrip("v")}',
        *notes_arg,
    ]
    if dry_run:
        print(f'  (dry-run) would: {" ".join(cmd)}')
        return
    run(cmd)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('version', help='SemVer like 0.3.0 (no leading v)')
    parser.add_argument(
        '--dry-run', action='store_true',
        help='Skip git tag, git push, gh release. Build + package run.',
    )
    parser.add_argument(
        '--skip-build', action='store_true',
        help='Skip msbuild + tests (assumes already built).',
    )
    parser.add_argument(
        '--skip-tests', action='store_true', help='Skip CoreTests.',
    )
    args = parser.parse_args()

    if not re.fullmatch(r'\d+\.\d+\.\d+', args.version):
        die(f'Version must be N.N.N (got {args.version!r})')

    step('1/6', 'Pre-flight: clean tree, branch check')
    check_git_clean()
    branch = check_branch()
    if branch == 'main':
        print('  releasing from main — proceeding.')
    else:
        ans = input(f'  Releasing from non-main branch "{branch}". Proceed? [y/N] ')
        if ans.strip().lower() not in ('y', 'yes'):
            die('Aborted by user.', code=2)

    step('2/6', f'Verify tools (msbuild, gh)')
    msbuild = find_msbuild()
    gh = find_gh()
    print(f'  msbuild: {msbuild}')
    print(f'  gh:      {gh}')

    if not args.skip_build:
        step('3/6', 'msbuild Release|x64')
        build_release(msbuild)

    if not args.skip_tests and not args.skip_build:
        step('4/6', 'Run unit tests')
        run_tests()
    else:
        print('\n[4/6] Tests skipped (--skip-tests or --skip-build).')

    step('5/6', f'Package MSIX {args.version}')
    msix = build_msix(args.version)

    step('6/6', f'Tag + GitHub release v{args.version}')
    tag = make_tag(args.version, args.dry_run)
    github_release(gh, tag, msix, args.dry_run)

    if args.dry_run:
        print('\nDry run complete. Re-run without --dry-run to publish.')
    else:
        print(f'\nReleased https://github.com/vitaldb/openzip/releases/tag/{tag}')


if __name__ == '__main__':
    main()
