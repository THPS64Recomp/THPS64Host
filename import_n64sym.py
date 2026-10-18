"""Turn n64sym's splat-format output into symbols/{boot,main}.txt entries.

Only names whose address is a discovered function start (tools/funclist.py)
become `type:func`; data/rodata names are kept for main/boot data; generic
placeholders (`*_rodata_XXXX`, `*_bss_XXXX`, `*_data_XXXX`) are dropped.
Duplicate names get an `_<ADDR>` suffix. Run after n64sym; review the diff.

usage: python decomp/common/import_n64sym.py --game thps2 n64sym.txt baserom.z64 baserom.main.bin [--force-main]

--force-main: main-image hits that fall inside a larger discovered function are emitted as
`type:func` anyway (they become starts). Without it the result depends on which splat labels the
current asm/ happens to contain, i.e. on the previous run (circular).
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import funclist  # noqa: E402
import thpsgame  # noqa: E402

# Signature hits inside the main image that are not plausible for any THPS port
# (checked by hand: wrong library or tiny generic bodies).
REJECT = {"osBbUsbDevGetHandle", "osVoiceInit", "osVoiceCheckWord", "osVoiceControlGain",
          "ptstart", "MusPtrBankGetCurrent", "n_aspMainTextStart", "rspbootTextStart",
          "gspF3DEX2_NoN_fifoTextStart", "gspF3DEX2_NoN_xbusTextStart"}


def main(g, sym_path, rom, img, force_main=False):
    boot, main_, fb, fm, _ = funclist.load_all(rom, img, sym_paths=(), g=g)
    ents = []
    for line in open(sym_path):
        m = re.match(r"(\S+) = (0x[0-9A-F]+);", line.strip())
        if m:
            ents.append((m.group(1), int(m.group(2), 16)))
    counts = collections.Counter(n for n, _ in ents)
    out = {"boot": [], "main": []}
    skipped = []
    for name, a in sorted(ents, key=lambda e: e[1]):
        if re.search(r"_(rodata|bss|data)_[0-9A-F]{4}$", name) or name in REJECT:
            continue
        if counts[name] > 1:
            name = f"{name}_{a:08X}"
        seg = "boot" if a < g.main_vram else "main"
        funcs = fb if seg == "boot" else fm
        img_ = boot if seg == "boot" else main_
        if a in funcs:
            out[seg].append(f"{name} = 0x{a:08X}; // type:func size:0x{funcs[a]:X} n64sym")
        elif img_.in_text(a) and force_main and seg == "main" and a % 4 == 0:
            # exact-byte signature hit inside a larger discovered function (libmus F-commands:
            # leaf commands that a data table points to). Force it to be a start.
            out[seg].append(f"{name} = 0x{a:08X}; // type:func n64sym forced-start")
        elif img_.in_text(a):
            skipped.append((name, a))
        else:
            out[seg].append(f"{name} = 0x{a:08X}; // n64sym")
    for seg, lines in out.items():
        print(f"## {seg}: {len(lines)}")
        print("\n".join(lines))
    print("## in text but not a discovered start (not imported):", [(n, hex(a)) for n, a in skipped])


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    main(g, *args[:3], force_main="--force-main" in args)
