#!/usr/bin/env python3
"""uvstate_memory.py SNAPSHOT PHYS LENGTH OUT: copy guest physical memory out of a UVSTATE1
snapshot (core/state_image.cpp layout; the "memory" sparse field), for disassembly with
mips64-linux-gnuabi64-objdump -b binary -m mips:10000 -EB -D."""
import struct, sys
data = open(sys.argv[1], 'rb').read()
assert data[:8] == b'UVSTATE1'
pos, fields = 8, {}
while pos < len(data):
    (klen,) = struct.unpack_from('<I', data, pos); pos += 4
    key = data[pos:pos + klen].decode(); pos += klen
    (vlen,) = struct.unpack_from('<Q', data, pos); pos += 8
    fields[key] = (pos, vlen); pos += vlen
po, pl = fields['memory.pages']; co, _ = fields['memory.contents']
pages = struct.unpack_from('<%dQ' % (pl // 8), data, po)
index = {p: i for i, p in enumerate(pages)}
phys, length = int(sys.argv[2], 0), int(sys.argv[3], 0)
out = bytearray()
for a in range(phys, phys + length):
    i = index.get(a >> 16)
    out.append(0 if i is None else data[co + i * 0x10000 + (a & 0xffff)])
open(sys.argv[4], 'wb').write(out)
