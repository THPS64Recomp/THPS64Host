#!/bin/sh
# Regression suite for a THPS recomp host: tests/run.py (stdlib Python, no ROM data in git). Shared by THPS1-3; run it through
# the game's wrapper, decomp/thps<N>/recomp-host/tools/test.sh.
#   tools/test.sh                 run every scenario (needs references, see README "Tests")
#   tools/test.sh --record        first run on a machine: capture missing reference frames
#   tools/test.sh --only pak_save --session pak ...   (see tools/test.sh --help)
#   tools/test.sh --controls-cfg  only the controls.cfg migration/preset test (no ROM, ~15 s)
#   tools/test.sh --gfx-cfg       only the graphics.cfg default/migration test (HUD Placement default; no ROM, ~10 s)
#   tools/test.sh --first-run [--only NAME ...] [--shots DIR]   split build: the first-run flow (picker, errors, build, cancel, lock...; ~4 min)
#                                 uses build/cmake-split/<game>_recomp (the split engine) unless $THPS<N>_BINARY says otherwise
#   tools/test.sh --soak [MINUTES] [--version ID] [soak.py options]   soak test (release tier 3, never part of the normal suite): plays the game for
#                                 MINUTES (default 120) with relaunches, measures crashes / hangs / frame times / shader hitches / memory into
#                                 build/tests/soak/<stamp>-<version>/soak.jsonl and prints a table; uses the split engine like --first-run.
#                                 `tools/test.sh --soak-summary FILE.jsonl ...` prints the table of finished logs. See tests/soak.py.
# A run with no arguments also does the controls.cfg and graphics.cfg tests first (tests/controls_cfg_test.py, tests/gfx_cfg_test.py).
# ROM version: --version ID (games.toml row; default <game> = the first release); the ROM is $THPS<N>_ROM or
# roms/<its rom_names> in a parent directory. References: build/tests/refs[-ID] with refs_from fall-through.
set -eu
HERE=${THPS_HOST:?run this through decomp/thps<N>/recomp-host/tools/test.sh}
FAMILY=$(sed -n 's/^family *= *"\([a-z0-9]*\)".*/\1/p' "$HERE/game/game.toml")
UPPER=$(echo "$FAMILY" | tr a-z A-Z)
export THPS_HOST="$HERE"
# Python: python3, else (Windows, Git Bash) the py launcher. Windows' "python3" may be the Microsoft Store stub, which
# prints "Python was not found" and exits 49, so try it rather than just finding it on PATH.
if python3 -c '' >/dev/null 2>&1; then PY=python3
elif py -3 -c '' >/dev/null 2>&1; then PY="py -3"
elif python -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; then PY=python
else echo "Python 3.11 or newer was not found" >&2; exit 2; fi
# The ROM paths ($THPS<N>_ROM, $THPS<N>_ROM_EU, ...). Git Bash turns a /c/... path into C:/... in a native program's arguments, but
# not in its environment, so the game can't open a Git Bash path given this way: convert it. Then check that each file exists.
ROMVARS=$(env | sed -n "s/^\(${UPPER}_ROM[A-Z0-9_]*\)=.*/\1/p")
case "${1:-}" in --controls-cfg|--gfx-cfg|--soak-summary) ROMVARS= ;; esac   # these need no ROM
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINSH=1 ;; *) WINSH= ;; esac
for v in $ROMVARS; do
    eval "p=\$$v"
    was=
    if [ -n "$WINSH" ] && command -v cygpath >/dev/null 2>&1; then
        case "$p" in /*) was=" (from $p)"; p=$(cygpath -m "$p"); eval "export $v=\"\$p\"" ;; esac
    fi
    if [ ! -f "$p" ]; then
        echo "$v=$p$was: no such file." >&2
        case "$WINSH$p" in 1/*) echo "In Git Bash, write the Windows path: export $v=\"C:/path/to/dump.z64\"" >&2 ;; esac
        exit 2
    fi
done
# The first-run tests need the SPLIT engine (build/cmake-split: -DTHPS_SPLIT_GAME_LIB=ON); the static build (build/cmake) has no first run.
# $<GAME>_BINARY overrides; otherwise build/cmake-split when it exists.
if [ "${1:-}" = "--first-run" ]; then
    shift
    eval "BIN=\${${UPPER}_BINARY:-}"
    if [ -z "$BIN" ]; then
        if [ -x "$HERE/build/cmake-split/${FAMILY}_recomp" ]; then BIN="$HERE/build/cmake-split/${FAMILY}_recomp"
        else echo "--first-run tests the split engine, and it isn't built ($HERE/build/cmake-split)." >&2
             echo "Build it first: python3 -I decomp/$FAMILY/recomp-host/tools/build_host.py --split (on Windows: python), or set ${UPPER}_BINARY" >&2; exit 2; fi
    fi
    # Windows: name the .exe. Git Bash's `test -x` accepts the bare name, but the tests copy the app by this exact path.
    if [ -f "$BIN.exe" ]; then BIN="$BIN.exe"; fi
    exec $PY -I "$HERE/tests/firstrun_test.py" --binary "$BIN" "$@"
fi
if [ "${1:-}" = "--soak-summary" ]; then shift; exec $PY -I "$HERE/tests/soak.py" --summary "$@"; fi
if [ "${1:-}" = "--soak" ]; then
    shift
    MINUTES=120
    case "${1:-}" in ''|-*) ;; *) MINUTES=$1; shift ;; esac
    [ -f "$HERE/tests/soak.py" ] || { echo "$FAMILY has no soak test (tests/soak.py)" >&2; exit 2; }
    eval "SBIN=\${${UPPER}_BINARY:-}"
    if [ -z "$SBIN" ]; then
        for c in "$HERE/build/cmake-split/${FAMILY}_recomp" "$HERE/build/cmake-split/Release/${FAMILY}_recomp"; do
            if [ -x "$c" ] || [ -f "$c.exe" ]; then export ${UPPER}_BINARY="$c"; break; fi
        done
    fi
    eval "SBIN=\${${UPPER}_BINARY:-}"
    if [ -n "$SBIN" ] && [ -f "$SBIN.exe" ]; then export ${UPPER}_BINARY="$SBIN.exe"; fi
    exec $PY -I "$HERE/tests/soak.py" --minutes "$MINUTES" "$@"
fi
eval "BIN=\${${UPPER}_BINARY:-$HERE/build/cmake/${FAMILY}_recomp}"
if [ -f "$BIN.exe" ]; then BIN="$BIN.exe"; fi
test -x "$BIN" || { echo "no game build at $BIN. Build it first: python3 -I decomp/$FAMILY/recomp-host/tools/build_host.py --rom <your dump> (on Windows: python; BUILDING.md), or set ${UPPER}_BINARY" >&2; exit 2; }
if [ "${1:-}" = "--controls-cfg" ]; then exec $PY -I "$HERE/tests/controls_cfg_test.py" "$BIN"; fi
if [ "${1:-}" = "--gfx-cfg" ]; then exec $PY -I "$HERE/tests/gfx_cfg_test.py" "$BIN"; fi
if [ "$#" -eq 0 ]; then $PY -I "$HERE/tests/controls_cfg_test.py" "$BIN"; $PY -I "$HERE/tests/gfx_cfg_test.py" "$BIN"; fi
exec $PY -I "$HERE/tests/run.py" "$@"
