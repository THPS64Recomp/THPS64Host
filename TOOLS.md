# decomp/common: shared tools for the Edge of Reality THPS N64 ports

One set of tools and one table of per-game facts, used by the decompilation of each game and by the recomp host. Every game's
repository (THPS1, THPS2, THPS3) carries a copy of this directory. The copies are the same except that `games.toml` keeps only
that repository's own game, and the front-end overlay tools (`overlay_*.py`, used for THPS2 and THPS3) are left out of the THPS1 one.

Everything here reads a ROM you supply and writes only to a git-ignored `build/` folder. The table holds **facts** (addresses,
sizes, names, hashes), never ROM bytes.

## The table: `games.toml`

One `[row]` per ROM release. `thpsgame.py` loads it; every tool takes `--game NAME` (or `$THPS_GAME`). A row may say
`inherits = "<row>"` and list only what differs (the European releases and THPS1's USA v1.0 do). Field meanings are in the file header.
The recomp host reads the same rows through `gen_versions.py`
([versions](https://github.com/THPS64Recomp/THPS1-64Recomp/blob/main/docs/internals/versions.md)).

## The host: `host/`

The recomp host itself is here too: `host/src` (the engine), `host/cmake` (builds `<game>_recomp` from a game profile), `host/tools`
(generate, build, package, scan), `host/tests` (the regression suite) and `host/patches` (our RT64 patches). One source serves all the
games; a game keeps only its profile (`decomp/<game>/recomp-host/`: `game/game.toml`, the version tables, hook and offset tables, its
test scenarios) plus small wrappers with the old tool names. The repository's sister projects carry the same host.

## Tools

<!-- table-ok: the list of tools in this directory -->
| Tool | Purpose | Used by |
|---|---|---|
| `thpsgame.py` | loads `games.toml` | all |
| `erz.py` | ERZ (RNC ProPack, methods 1 and 2) decoder with CRC checks | unpack, repack |
| `thps_unpack.py` | inflate the main code image from the ROM, CRC-checked | `make extract`, the first-run build |
| `erz_pack.py` | ERZ packer front-end around the patched `rnc_propack` | `build_rom.py`, `repack_check.py` |
| `rnc_propack/lookahead.patch` | our patch to the packer's look-ahead buffer that makes it byte-identical to the original | Makefile |
| `build_rom.py` | cut an image into stride chunks, pack, rewrite the chunk table, fix CRCs; carry everything after the asset root verbatim | Makefile |
| `repack_check.py` | recompress every ERZ chunk in the ROM and compare (code and all asset pieces) | proof |
| `fix_asm.py` | rewrite linker-garbage padding opcodes as `.word` so GNU as reassembles one-to-one | Makefile |
| `funclist.py` | function starts and sizes from jump targets, data pointers and padding detection | `gen_syms.py` |
| `gen_syms.py` | code blob and `recomp/<game>.syms.toml` for N64Recomp | `make recomp-inputs`, `build_host.py` |
| `import_n64sym.py` | import shygoo's n64sym libultra and libmus hits | symbols |
| `sigmatch.py` | masked-signature matcher: seed names and boundaries from a solved release | new releases |
| `gen_host_syms.py` + `host_force/<game>.toml` | the host's own symbol copy (hook names, hidden starts) | recomp host |
| `gen_splat_cfg.py` | both splat configs from `games.toml` and the ROM | new game |
| `recomp_check.py` | static completeness checks on the function list | proof |
| `disfunc.py`, `strxref.py` | disassemble one function; find a string and its referencing functions | reverse engineering |
| `boot_test.sh` | headless mupen64plus boot screenshots of a rebuilt ROM | proof |

The THPS2 and THPS3 repositories also carry the front-end overlay tools: `overlay_table.py` (reads the overlay table), `overlay_relocs.py`
(decodes the relocation streams), `overlay_syms.py` and `overlay_recomp.py` (turn the screens into relocatable sections for N64Recomp).
See their `docs/internals/overlays.md`.

Run Python with `-I` (`python3 -I decomp/common/x.py --game thps1 ...`); the tools put their own directory on `sys.path`.
`funclist`, `gen_syms`, `import_n64sym` and `disfunc` need `rabbitizer` (the per-game venv has it).

## Proof the tools reproduce the original

For THPS1 USA v1.1 (the other games have their own checksums in `decomp/<game>/checksums/`): `thps_unpack.py` gives an image whose SHA-1 equals `decomp/thps1/checksums/input.main.sha1` with 14 of 14 chunks passing
both CRCs; `build_rom.py` on the rebuilt ROM and image gives the retail SHA-1; `repack_check.py` reports 1,094 identical pieces,
17 stored raw, 0 mismatches; `funclist.py` and `gen_syms.py` give 253 boot and 2,490 main functions; `gen_host_syms.py` gives the
2,778-function host list.

## Adding a release or a game

For a new release of a game, follow [the versions page](https://github.com/THPS64Recomp/THPS1-64Recomp/blob/main/docs/internals/versions.md) of that repository.

---
Unofficial. This project is not affiliated with Activision, Neversoft, Edge of Reality, Tony Hawk or Nintendo. See the top-level README.
