"""Build the small test ZIP archives for OpenZipCore validation.

Generates files into tests/test_archives/. The two large cases (many_files,
zip64_big) and the encrypted cases that need external libraries are skipped
unless `pyzipper` is installed; see tests/test_archives/README.md for those.

Run:
    python tools/build_test_archives.py [--encrypted]
"""
import argparse
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, 'tests', 'test_archives')


class Cp949ZipInfo(zipfile.ZipInfo):
    """ZipInfo that encodes its filename as CP949 and does NOT set the UTF-8 flag.

    Python's zipfile auto-sets the UTF-8 general-purpose flag (bit 11) whenever
    the filename contains non-ASCII characters. We override the encoder to
    simulate legacy Korean Windows tooling that emits raw CP949 bytes.
    """

    def _encodeFilenameFlags(self):
        return self.filename.encode('cp949'), self.flag_bits


def write(path, files, info_cls=zipfile.ZipInfo, mode='w'):
    """Write a zip with `files` = list of (filename, bytes_or_str)."""
    with zipfile.ZipFile(path, mode, zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            info = info_cls(name) if info_cls is not zipfile.ZipInfo else zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED
            if isinstance(data, str):
                data = data.encode('utf-8')
            z.writestr(info, data)


def build_utf8_basic():
    """UTF-8 zip with mixed ASCII + Korean filenames. Flag bit 11 set."""
    write(
        os.path.join(OUT_DIR, 'utf8_basic.zip'),
        [
            ('hello.txt', 'hello world\n'),
            ('한글파일.txt', '한국어 텍스트입니다\n'),
            ('subdir/한글이름.md', '# 마크다운\n'),
        ],
    )


def build_cp949_legacy():
    """CP949-encoded filenames without the UTF-8 flag (legacy Korean Windows zip)."""
    write(
        os.path.join(OUT_DIR, 'cp949_legacy.zip'),
        [
            ('hello.txt', 'ascii\n'),
            ('한글파일.txt', '레거시 CP949\n'),
            ('서울지점/매출.csv', 'a,b,c\n1,2,3\n'),
        ],
        info_cls=Cp949ZipInfo,
    )


def build_traversal():
    """Entry whose path escapes the target directory via .. components."""
    write(
        os.path.join(OUT_DIR, 'traversal.zip'),
        [
            ('safe.txt', 'ok\n'),
            ('../../etc/passwd', 'pwn:x:0:0::/:/bin/sh\n'),
            ('..\\..\\Windows\\System32\\evil.dll', b'\x00\x00'),
        ],
    )


def build_reserved():
    """Windows reserved device names. CON, PRN, COM1, LPT3, plus trailing dot."""
    write(
        os.path.join(OUT_DIR, 'reserved.zip'),
        [
            ('CON.txt', 'oops\n'),
            ('PRN', b'\x00'),
            ('foo/COM1.dat', b'\x00'),
            ('trailing_dot.', 'bad name\n'),
        ],
    )


def build_empty():
    """Zip with zero entries."""
    with zipfile.ZipFile(os.path.join(OUT_DIR, 'empty.zip'), 'w'):
        pass


def build_pkcrypt(password):
    """ZipCrypto (PKZIP traditional) encryption. Pure-Python writer.

    Implements PKZIP "Standard ZIP 2.0" encryption manually since neither stdlib
    `zipfile` nor `pyzipper` support writing it. Algorithm per APPNOTE.TXT §6.0.
    """
    import struct
    import zlib
    path = os.path.join(OUT_DIR, 'pkcrypt.zip')

    def crc32_update(c, b):
        return (zlib.crc32(bytes([b]), c ^ 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF

    class PkCrypt:
        def __init__(self, password):
            self.k0, self.k1, self.k2 = 0x12345678, 0x23456789, 0x34567890
            for ch in password:
                self.update(ch)

        def update(self, c):
            self.k0 = crc32_update(self.k0, c)
            self.k1 = (self.k1 + (self.k0 & 0xFF)) & 0xFFFFFFFF
            self.k1 = (self.k1 * 134775813 + 1) & 0xFFFFFFFF
            self.k2 = crc32_update(self.k2, (self.k1 >> 24) & 0xFF)

        def stream_byte(self):
            t = (self.k2 | 2) & 0xFFFF
            return ((t * (t ^ 1)) >> 8) & 0xFF

        def encrypt(self, data):
            out = bytearray()
            for b in data:
                k = self.stream_byte()
                out.append(b ^ k)
                self.update(b)
            return bytes(out)

    entries = [
        ('secret.txt', '비밀\n'.encode('utf-8')),
        ('plain.txt', 'plain\n'.encode('utf-8')),
    ]
    pwd_bytes = password.encode('utf-8')

    with open(path, 'wb') as f:
        cd = []  # central directory entries
        for name, data in entries:
            crc = zlib.crc32(data) & 0xFFFFFFFF
            comp = zlib.compress(data, level=6)[2:-4]  # raw deflate
            # 12-byte encryption header. Last byte must equal high byte of CRC for ZIP 2.0.
            import os as _os
            header = bytearray(_os.urandom(11)) + bytes([(crc >> 24) & 0xFF])
            cipher = PkCrypt(pwd_bytes)
            enc = cipher.encrypt(bytes(header) + comp)
            comp_size = len(enc)

            local_offset = f.tell()
            name_b = name.encode('ascii')
            # Local file header
            f.write(struct.pack('<IHHHHHIIIHH',
                                0x04034b50, 20, 0x0001, 8, 0, 0,
                                crc, comp_size, len(data), len(name_b), 0))
            f.write(name_b)
            f.write(enc)

            # Stash for central directory
            cd.append((name_b, crc, comp_size, len(data), local_offset))

        cd_offset = f.tell()
        for name_b, crc, comp_size, uncomp, local_offset in cd:
            f.write(struct.pack('<IHHHHHHIIIHHHHHII',
                                0x02014b50, 20, 20, 0x0001, 8, 0, 0,
                                crc, comp_size, uncomp, len(name_b), 0, 0, 0, 0,
                                0, local_offset))
            f.write(name_b)
        cd_size = f.tell() - cd_offset
        f.write(struct.pack('<IHHHHIIH',
                            0x06054b50, 0, 0, len(cd), len(cd),
                            cd_size, cd_offset, 0))


def build_aes256(password):
    """AES-256 (WinZip-compatible) encryption. Requires `pyzipper`."""
    try:
        import pyzipper
    except ImportError:
        print('  [skip] pyzipper not installed; pip install pyzipper to build aes256.zip')
        return
    path = os.path.join(OUT_DIR, 'aes256.zip')
    with pyzipper.AESZipFile(
        path, 'w', compression=pyzipper.ZIP_DEFLATED, encryption=pyzipper.WZ_AES
    ) as z:
        z.setpassword(password.encode('utf-8'))
        z.writestr('secret.txt', '비밀 AES\n'.encode('utf-8'))
        z.writestr('plain.txt', 'plain\n'.encode('utf-8'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--encrypted', action='store_true',
                    help='Also build pkcrypt + aes256 zips (needs pyzipper).')
    ap.add_argument('--password', default='password',
                    help='Password used for the encrypted archives (default: password).')
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)

    print(f'Writing test archives to {OUT_DIR}')
    print('  utf8_basic.zip')
    build_utf8_basic()
    print('  cp949_legacy.zip')
    build_cp949_legacy()
    print('  traversal.zip')
    build_traversal()
    print('  reserved.zip')
    build_reserved()
    print('  empty.zip')
    build_empty()
    if args.encrypted:
        print(f'  pkcrypt.zip (pwd={args.password!r})')
        build_pkcrypt(args.password)
        print(f'  aes256.zip  (pwd={args.password!r})')
        build_aes256(args.password)

    print('Done. Skipped: many_files.zip, zip64_big.zip — see README.')


if __name__ == '__main__':
    main()
