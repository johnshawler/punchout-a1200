#!/usr/bin/env python3
"""mkdisk.py out.adf boot.bin payload.bin - patch the payload length into the bootblock and build the ADF (see platform-a1200/mkdisk.py)"""
import os, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
mkadf = os.path.join(here, '..', 'tools', 'mkadf.py')
out, boot, payload = sys.argv[1:4]
bb = bytearray(open(boot, 'rb').read()); pl = open(payload, 'rb').read()
assert bb[16:20] == b'ZKLD', 'bootblock marker missing'
n = (len(pl) + 511) & ~511
assert n <= 880 * 1024 - 1024, 'payload %d bytes does not fit on an ADF' % n
struct.pack_into('>I', bb, 20, n)
with tempfile.NamedTemporaryFile(delete=False) as f: f.write(bb); tmp = f.name
try: subprocess.check_call([sys.executable, mkadf, out, tmp, payload])
finally: os.unlink(tmp)
print('%s: bootblock %d bytes, payload %d bytes (read %d)' % (out, len(bb), len(pl), n))
