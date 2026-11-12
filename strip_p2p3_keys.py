#!/usr/bin/env python3
"""Clear the 2P and 3P keyboard assignments in RBO KeyConfig files (cdt/v2/v3/v4).

File layout (328 bytes, little-endian dwords, DirectInput scan codes):
  0x4C  P1 keys[9]   (up,down,left,right, A,B,C,D, extra)
  0xB8  P2 keys[9]
  0x124 P3 keys[9]
A key code of 0 means "unassigned". P1 is left untouched. Originals are kept as *.bak.
usage: strip_p2p3_keys.py <DATA/SaveData dir or KeyConfig file> ...
"""
import os, shutil, struct, sys

OFFS = {"P2": 0xB8, "P3": 0x124}

def patch(path):
    d = bytearray(open(path, "rb").read())
    if len(d) != 328:
        print(f"skip {path}: unexpected size {len(d)}"); return
    for name, off in OFFS.items():
        old = struct.unpack_from("<9I", d, off)
        struct.pack_into("<9I", d, off, *([0] * 9))
        print(f"  {os.path.basename(path)} {name}: {[hex(x) for x in old]} -> cleared")
    if not os.path.exists(path + ".bak"):
        shutil.copy2(path, path + ".bak")
    open(path, "wb").write(d)

def main(args):
    if not args:
        print(__doc__); sys.exit(1)
    for a in args:
        if os.path.isdir(a):
            for n in ("KeyConfig.cdt", "KeyConfig.v2", "KeyConfig.v3", "KeyConfig.v4"):
                p = os.path.join(a, n)
                if os.path.exists(p): patch(p)
        else:
            patch(a)

if __name__ == "__main__":
    main(sys.argv[1:])
