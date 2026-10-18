"""Disassemble one function (or a range) from a main image or the boot segment.

usage: python3 -I decomp/common/disfunc.py --game thps2 IMAGE|ROM ADDR [SIZE]
  IMAGE = baserom.main.bin (main image) or baserom.z64 (boot segment, addr < main_vram)
  SIZE  defaults to the size listed for ADDR in recomp/<game>.syms.toml if present, else 0x80.
Needs rabbitizer (the per-game .venv has it).
"""
import os
import re
import struct
import sys

import rabbitizer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402


def main(g, path, addr, size=None):
    data = open(path, "rb").read()
    if addr < g.main_vram:
        data, base = data[g.boot_rom:g.boot_rom + g.boot_size], g.boot_vram
    else:
        base = g.main_vram
    if size is None:
        size = 0x80
        syms = f"recomp/{g.name}.syms.toml"
        if os.path.exists(syms):
            m = re.search(r'vram = 0x%08X, size = (0x[0-9A-Fa-f]+)' % addr, open(syms).read())
            if m:
                size = int(m.group(1), 16)
    for a in range(addr, addr + size, 4):
        w = struct.unpack_from(">I", data, a - base)[0]
        ins = rabbitizer.Instruction(w, a)
        print(f"{a:08X}  {w:08X}  {ins.disassemble()}")


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    main(g, args[0], int(args[1], 0), int(args[2], 0) if len(args) > 2 else None)
