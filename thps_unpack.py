"""Inflate a THPS N64 port's main code image from the ROM, exactly as boot main does.

Boot main reads `count` + `count+1` table-relative offsets at the chunk table
(root_ptr + 4). For each chunk it PIO-copies to a staging buffer, inflates ERZ
(= RNC ProPack, magic renamed) method 1/2 (or copies raw), writing to
main_vram + i*stride. Each chunk's two CRC-16s are checked here.

usage: python -I decomp/common/thps_unpack.py --game thps2 ROM.z64 OUT.bin [-v]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import erz  # noqa: E402
import thpsgame  # noqa: E402


def chunk_spans(rom, table):
    n = struct.unpack_from(">I", rom, table)[0]
    offs = struct.unpack_from(f">{n + 1}I", rom, table + 4)
    return [(table + offs[i], offs[i + 1] - offs[i]) for i in range(n)]


def unpack(rom, g, verbose=False):
    img = bytearray()
    for i, (a, span) in enumerate(chunk_spans(rom, g.table)):
        if rom[a:a + 3] == erz.MAGIC:
            data, info = erz.decompress(rom, a)
            ok = info["pcrc_ok"] and info["ucrc_ok"]
            if verbose:
                print(f"chunk {i:2d} @ROM {a:#08x} span {span:#6x} method {info['method']} "
                      f"usize {info['usize']:#x} blocks {info['blocks']}/{info['blocks_hdr']} "
                      f"pcrc/ucrc {'OK' if ok else 'BAD'} -> {g.main_vram + i * g.stride:08X}")
            if not ok:
                raise SystemExit(f"chunk {i}: CRC mismatch")
            if info["method"] != g.code_method:
                raise SystemExit(f"chunk {i}: method {info['method']} != table's {g.code_method}")
        else:
            data = rom[a:a + span]
        if len(img) != i * g.stride:
            raise SystemExit(f"chunk {i}: previous chunk shorter than 0x{g.stride:X}")
        img += data
    return bytes(img)


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    verbose = "-v" in args
    args = [a for a in args if a != "-v"]
    rom = open(args[0], "rb").read()
    n = struct.unpack_from(">I", rom, g.table)[0]
    if n != g.n_chunks:
        raise SystemExit(f"{g.name}: table says {n} chunks, games.toml says {g.n_chunks} (wrong ROM?)")
    img = unpack(rom, g, verbose)
    if len(img) != g.main_size:
        raise SystemExit(f"image is {len(img):#x} bytes, games.toml says {g.main_size:#x}")
    open(args[1], "wb").write(img)
    print(f"{args[1]}: {g.main_vram:08X}-{g.main_vram + len(img):08X} ({len(img):#x} bytes), "
          f"{n}/{n} chunks CRC OK")
