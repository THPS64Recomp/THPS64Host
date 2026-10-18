"""Recompress every ERZ chunk in a THPS N64 ROM and compare with the retail bytes.

Walks the asset tree (root container -> packs -> 16 KiB pieces; also the code chunk
table), unpacks each ERZ chunk with erz.py, repacks with the patched rnc_propack
(erz_pack.py) using the chunk's own method, and compares the bytes.

Known miss (THPS2 and THPS3, pack 8): the pack's single 8-byte payload was stored
"packed" as 12 bytes; rnc_propack refuses to expand data (error 2). It is reported
as a known quirk, not a failure.

usage: python -I decomp/common/repack_check.py --game thps2 RNC_BIN ROM.z64
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import erz  # noqa: E402
import erz_pack  # noqa: E402
import thps_unpack  # noqa: E402
import thpsgame  # noqa: E402


def container(rom, off):
    n = struct.unpack_from(">I", rom, off)[0]
    offs = struct.unpack_from(f">{n + 1}I", rom, off + 4)
    return [(off + offs[i], offs[i + 1] - offs[i]) for i in range(n)]


def check(rnc, rom, label, a, span, res):
    if rom[a:a + 3] != erz.MAGIC:
        res["raw"] += 1
        return
    data, info = erz.decompress(rom, a)
    want = rom[a:a + 18 + info["psize"]]
    try:
        got = erz_pack.pack(rnc, data, info["method"])
    except Exception as e:  # noqa: BLE001
        res["quirk"].append((label, a, f"packer error: {str(e)[:60]}, payload {len(data)} B"))
        return
    if got == want:
        res["ok"] += 1
    else:
        res["bad"].append((label, a, f"method {info['method']} usize {len(data):#x}"))


def main(g, rnc, rom_path):
    rom = open(rom_path, "rb").read()
    res = {"ok": 0, "raw": 0, "bad": [], "quirk": []}
    for i, (a, span) in enumerate(thps_unpack.chunk_spans(rom, g.table)):
        check(rnc, rom, f"code{i}", a, span, res)
    root = struct.unpack_from(">I", rom, g.root_ptr)[0] & 0x0FFFFFFF
    for p, (poff, psz) in enumerate(container(rom, root)):
        for i, (a, span) in enumerate(container(rom, poff)):
            check(rnc, rom, f"pack{p}.{i}", a, span, res)
    print(f"{g.name}: identical {res['ok']}, raw (stored, not compressed) {res['raw']}, "
          f"known quirks {len(res['quirk'])}, MISMATCH {len(res['bad'])}")
    for x in res["quirk"]:
        print("  quirk:", x)
    for x in res["bad"][:20]:
        print("  BAD:", x)
    return 1 if res["bad"] else 0


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    sys.exit(main(g, args[0], args[1]))
