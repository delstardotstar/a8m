"""Boot the way Rossum's MLO does: read /app from the SOM card image, check it, write the RAM image.

usage: python3 -I emu/mlo.py SD0.IMG OUT.BIN

Checks are MLO's as far as we know them (header + size, no checksum): load address 0x80000000 and a
size word equal to the file length (stock) or to the length less the 8-byte header (ours). OUT.BIN is
the payload with MLO's 1 at 0x34. FAT32 (or FAT16), first MBR partition, root directory only.
Exit 1 when there is no usable app (MLO would stop there).
"""
import struct
import sys


def fat_read(img, name):
    d = open(img, 'rb').read()
    base = 0
    if d[510:512] == b'\x55\xaa' and d[0x1c6:0x1ca] != b'\0\0\0\0' and d[0x1c2] in (0x0b, 0x0c, 0x0e, 0x06, 0x04):
        base = struct.unpack_from('<I', d, 0x1c6)[0] * 512
    bps, spc, rsvd, nfats, nroot, _, _, fatsz16 = struct.unpack_from('<HBHBHHBH', d, base + 11)
    fatsz = fatsz16 or struct.unpack_from('<I', d, base + 36)[0]
    fat = base + rsvd * bps
    root_dir = fat + nfats * fatsz * bps
    data = root_dir + (nroot * 32 + bps - 1) // bps * bps
    fat32 = nroot == 0
    csize = spc * bps

    def chain(c):
        while 2 <= c < (0x0ffffff8 if fat32 else 0xfff8):
            yield c
            c = (struct.unpack_from('<I', d, fat + 4 * c)[0] & 0x0fffffff) if fat32 else \
                struct.unpack_from('<H', d, fat + 2 * c)[0]

    if fat32:
        root = b''.join(d[data + (c - 2) * csize:data + (c - 1) * csize]
                        for c in chain(struct.unpack_from('<I', d, base + 44)[0]))
    else:
        root = d[root_dir:root_dir + nroot * 32]
    want = name.upper().ljust(11).encode()
    for i in range(0, len(root), 32):
        e = root[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xe5 or e[11] & 0x18 or e[11] == 0x0f or e[:11] != want:
            continue
        size = struct.unpack_from('<I', e, 28)[0]
        first = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16 if fat32 else 0)
        out = b''.join(d[data + (c - 2) * csize:data + (c - 1) * csize] for c in chain(first))
        return out[:size]
    return None


def main():
    app = fat_read(sys.argv[1], 'APP')
    if app is None:
        sys.exit('mlo: no /app on the SOM card')
    if len(app) < 64:
        sys.exit(f'mlo: /app too short ({len(app)} bytes)')
    size, load = struct.unpack_from('<II', app)
    if load != 0x80000000 or size not in (len(app), len(app) - 8):
        sys.exit(f'mlo: /app header rejected (size {size}, file {len(app)}, load {load:#x})')
    blob = bytearray(app[8:])
    blob[52:56] = b'\1\0\0\0'
    open(sys.argv[2], 'wb').write(blob)
    print(f'mlo: /app {len(app)} bytes', file=sys.stderr)


main()
