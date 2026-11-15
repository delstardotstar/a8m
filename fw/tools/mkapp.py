"""Wrap a raw firmware binary in the app image header MLO expects.

usage: python3 -I mkapp.py IN.bin OUT_APP

Header: payload size and load address, both little-endian u32 (MLO checks
the size; the payload is copied to the load address and entered there).
The image check block at the end of the payload (src/diag.c) gets the
number of 512-byte sectors before it, each sector's CRC-32, a copy of
sector 0, and last the CRC-32 of everything before that word; the firmware
checks them at boot.
"""
import struct
import sys
import zlib

LOAD_ADDRESS = 0x80000000
CHECK_MAGIC = 0x4b434d41  # "AMCK"
SECTOR = 512
MAX_SECTORS = 4096
BLOCK = 4 * (2 + MAX_SECTORS) + SECTOR + 4  # magic, count, sector CRCs, sector 0 copy, total CRC

payload = bytearray(open(sys.argv[1], 'rb').read())
block = len(payload) - BLOCK
if struct.unpack_from('<I', payload, block)[0] != CHECK_MAGIC:
    sys.exit('mkapp.py: image check block not at the end of the payload')
sectors = (block + SECTOR - 1) // SECTOR
if sectors > MAX_SECTORS:
    sys.exit(f'mkapp.py: {sectors} sectors > {MAX_SECTORS}; enlarge the check block')
struct.pack_into('<I', payload, block + 4, sectors)
for i in range(sectors):
    struct.pack_into('<I', payload, block + 8 + 4 * i, zlib.crc32(payload[i * SECTOR:min((i + 1) * SECTOR, block)]))
payload[block + 8 + 4 * MAX_SECTORS:block + 8 + 4 * MAX_SECTORS + SECTOR] = payload[:SECTOR]
struct.pack_into('<I', payload, len(payload) - 4, zlib.crc32(payload[:-4]))
open(sys.argv[2], 'wb').write(struct.pack('<II', len(payload), LOAD_ADDRESS) + payload)
print(f'{sys.argv[2]}: {len(payload)} bytes at {LOAD_ADDRESS:#x}')
