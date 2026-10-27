#!/bin/sh
# One entry point for the checks of a THPS64Recomp repository. Run it as tools/check.sh from the root of a game repository or of
# THPS64Host (that file is a short wrapper that calls this one).
#
#   tools/check.sh                contributor mode, no ROM needed: the checks CI runs on a pull request
#                                 (the host pin, the version tables, a build with stand-in game functions, and the ROM-free
#                                 checks of the built binary: --features, --check-rom of an empty file, the controls.cfg test)
#   tools/check.sh --maintainer   adds the ROM-based steps: a real build from your dumps, the game's full test suite, the
#                                 first-run test, and the pre-push guard. It refuses to start when it finds no dumps.
#   tools/check.sh --game thpsN   check one game only (where several are present)
#
# In THPS64Host it checks each game repository cloned next to it (../THPS1-64Recomp, ../THPS2-64Recomp, ../THPS3-64Recomp)
# against this host, as the host CI does. A host change should be checked in all three: it warns about each one that is missing,
# with the command to clone it, and the last line names the games it checked and the ones it didn't.
#
# Environment:
#   THPS_RUNTIME_DIR  one runtime folder (N64ModernRuntime, RT64) shared by every game's build (build_host.py --runtime):
#                     set up once, then reused. In THPS64Host the default is build/runtime, shared by the games checked.
#   THPS_ROMS    maintainer mode: the folder with your dumps (default: the nearest roms/ folder at or above the repository)
#   THPS_GUARD   maintainer mode: the pre-push guard to run (default: the one this checkout's pre-push hook calls)
#   CHECK_JOBS   parallel build jobs (default: all cores)
#
# Windows: run it in Git Bash (part of Git for Windows). The build finds Visual Studio by itself.
# Exit status: 0 all passed, 1 a check failed (each failure prints a "Fix:" line), 2 refused to start.
# MAINTAINING.md in THPS64Host lists every check, who runs it and how to fix it.
set -u

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }   # the comment block above

MAINT=
ONLY=
while [ $# -gt 0 ]; do
    case "$1" in
        --maintainer) MAINT=1; shift ;;
        --game) ONLY=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

HOSTROOT=$(cd "$(dirname "$0")/../.." && pwd)        # the THPS64Host tree this script belongs to
REPO=${CHECK_REPO:-}                                  # set by the wrapper: the repository being checked
if [ -z "$REPO" ]; then
    case "$HOSTROOT" in */decomp/common) REPO=$(cd "$HOSTROOT/../.." && pwd) ;; *) REPO=$HOSTROOT ;; esac
fi

FAILS=0
FAILED=
step() { printf '\n== %s\n' "$1"; }
ok()   { printf '   ok: %s\n' "$1"; }
note() { printf '   note: %s\n' "$1"; }
fail() {   # $1 = what failed, $2 = how to fix it
    printf '   FAILED: %s\n   Fix: %s\n' "$1" "$2"
    FAILS=$((FAILS + 1))
    FAILED="$FAILED
 - $1"
}
refuse() { printf '\ncheck.sh: %s\n' "$1" >&2; exit 2; }

# Python 3.11+: python3, else (Windows, Git Bash) the py launcher. Windows' "python3" may be the Microsoft Store stub.
if python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; then PY=python3
elif py -3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; then PY="py -3"
elif python -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; then PY=python
else refuse "Python 3.11 or newer was not found. Fix: install it (BUILDING.md, \"Dependencies\") and run again."; fi

# ------------------------------------------------------------------ which games, against which host

GAMES=           # lines "<game repository> <family>"
if [ -d "$REPO/decomp" ]; then
    for h in "$REPO"/decomp/thps*/recomp-host; do
        [ -f "$h/game/game.toml" ] || continue
        g=$(basename "$(dirname "$h")")
        GAMES="$GAMES$REPO $g
"
    done
    [ -n "$GAMES" ] || refuse "no game found under $REPO/decomp/. Run this from the root of a game repository (tools/check.sh)."
    if [ -z "${THPS_HOST_ROOT:-}" ]; then
        [ -f "$REPO/decomp/common/host/tools/hostenv.py" ] || refuse "the shared host (THPS64Host) is missing at decomp/common. Fix: git submodule update --init --recursive"
        THPS_HOST_ROOT=$REPO/decomp/common
    fi
    HOSTMODE=
else
    # THPS64Host: the game repositories cloned next to it, built against this host.
    THPS_HOST_ROOT=$REPO
    HOSTMODE=1
    MISSING=     # the games not cloned next to this repository
    for n in 1 2 3; do
        d=$(dirname "$REPO")/THPS$n-64Recomp
        if [ -f "$d/decomp/thps$n/recomp-host/game/game.toml" ]; then GAMES="$GAMES$d thps$n
"
        else MISSING="$MISSING THPS$n"; fi
    done
    # One runtime for all the games checked here, instead of one per game (about 850 MB each).
    THPS_RUNTIME_DIR=${THPS_RUNTIME_DIR:-$REPO/build/runtime}
fi
export THPS_HOST_ROOT
[ -n "${THPS_RUNTIME_DIR:-}" ] && export THPS_RUNTIME_DIR
if [ -n "$ONLY" ]; then
    GAMES=$(printf '%s' "$GAMES" | awk -v g="$ONLY" '$2 == g')
    [ -n "$GAMES" ] || refuse "--game $ONLY: no such game here."
    GAMES="$GAMES
"
fi

echo "check.sh: $( [ -n "$MAINT" ] && echo maintainer || echo contributor ) mode, host tree $THPS_HOST_ROOT"
CHECKED=$(printf '%s' "$GAMES" | awk 'NF { printf "%s%s", sep, toupper($2); sep = ", " }')
echo "games to check: ${CHECKED:-none}"
[ -n "${THPS_RUNTIME_DIR:-}" ] && echo "shared runtime: $THPS_RUNTIME_DIR (THPS_RUNTIME_DIR)"
NOTCHECKED=
if [ -n "$HOSTMODE" ]; then
    for n in 1 2 3; do
        case ", $CHECKED," in *", THPS$n,"*) ;; *) NOTCHECKED="${NOTCHECKED:+$NOTCHECKED, }THPS$n" ;; esac
    done
    if [ -n "$MISSING" ]; then
        step "THPS64Host: games not found next to this repository"
        for g in $MISSING; do
            printf '   WARNING: %s is not checked: no clone at %s/%s-64Recomp
' "$g" "$(dirname "$REPO")" "$g"
            printf '   To check it, from %s:
      git clone --recursive https://github.com/THPS64Recomp/%s-64Recomp ../%s-64Recomp
' "$REPO" "$g" "$g"
        done
        note "a host change reaches all three games; CI builds all three against your branch."
    fi
fi

# ------------------------------------------------------------------ maintainer mode: find the dumps first, or refuse

ROMS=
if [ -n "$MAINT" ]; then
    if [ -n "${THPS_ROMS:-}" ]; then
        ROMS=$THPS_ROMS
        [ -d "$ROMS" ] || refuse "THPS_ROMS=$ROMS is not a folder. Fix: set it to the folder with your dumps."
    else
        d=$REPO
        while :; do
            if [ -d "$d/roms" ]; then ROMS=$d/roms; break; fi
            p=$(dirname "$d"); [ "$p" = "$d" ] && break; d=$p
        done
    fi
    [ -n "$ROMS" ] && [ -d "$ROMS" ] || refuse "maintainer mode needs your game dumps, and no roms/ folder was found at or above $REPO.
Fix: put your dumps in a roms/ folder (the repository's roms/ is git-ignored), or set THPS_ROMS=<folder with your dumps>.
Contributors don't need this: run tools/check.sh without --maintainer."
    [ -n "$GAMES" ] || refuse "maintainer mode in THPS64Host checks the games cloned next to it, and none was found (../THPS1-64Recomp, ...).
Fix: clone them side by side (git clone --recursive), then run again."
fi

# Prints "<version> <dump path>" for each release of FAMILY found in ROMS (by SHA-1, any byte order), and "missing <version>" lines.
find_dumps() {
    $PY -I - "$THPS_HOST_ROOT" "$1" "$ROMS" <<'EOF'
import hashlib, os, sys
root, family, roms = sys.argv[1:4]
sys.path.insert(0, root)
import thpsgame
want = {v.rom_sha1: v.name for v in thpsgame.versions(family) if getattr(v, "status", "supported") == "supported"}
def z64(b):
    m = b[:4]
    if m == bytes.fromhex("80371240"): return b
    a = bytearray(b)
    if m == bytes.fromhex("40123780"):
        for i in range(0, len(a) - 3, 4): a[i:i + 4] = a[i:i + 4][::-1]
        return bytes(a)
    if m == bytes.fromhex("37804012"):
        a[0::2], a[1::2] = b[1::2], b[0::2]
        return bytes(a)
    return None
found = {}
for n in sorted(os.listdir(roms)):
    p = os.path.join(roms, n)
    if not os.path.isfile(p) or os.path.getsize(p) > 64 << 20: continue
    with open(p, "rb") as f: data = z64(f.read())
    v = data and want.get(hashlib.sha1(data).hexdigest())
    if v and v not in found: found[v] = p
for v in want.values():
    print(f"{v} {found[v]}" if v in found else f"missing {v}")
EOF
}

# ------------------------------------------------------------------ the checks of one game

check_game() {   # $1 = game repository, $2 = family
    R=$1; G=$2
    H=$R/decomp/$G/recomp-host
    UPPER=$(echo "$G" | tr a-z A-Z)
    printf '\n######## %s (%s)\n' "$UPPER" "$R"

    # The dumps (maintainer mode): refuse before building anything.
    DUMPS=
    if [ -n "$MAINT" ]; then
        step "your dumps of $UPPER in $ROMS"
        DUMPS=$(find_dumps "$G") || refuse "could not read $ROMS."
        echo "$DUMPS" | grep -v '^missing ' | sed 's/^/   found: /'
        echo "$DUMPS" | grep '^missing ' | sed 's/^missing /   not found: /'
        BASE=$(echo "$DUMPS" | awk -v g="$G" '$1 == g {sub(/^[^ ]+ /, ""); print}')
        [ -n "$BASE" ] || refuse "no dump of $UPPER's first release ($G) in $ROMS: the suite runs on it.
Fix: copy your dump into $ROMS (any file name; .z64, .n64 or .v64), or set THPS_ROMS to the folder that has it. The supported
dumps and their SHA-1s are in the game's README."
    fi

    # 1. The host pin: the committed decomp/common is the release tag the workflows call (the CI's host-pin check, done locally).
    if [ -z "$HOSTMODE" ] && [ -f "$R/.gitmodules" ] && [ -f "$R/.github/workflows/ci.yml" ]; then
        step "host pin (decomp/common and the workflows name the same THPS64Host release)"
        wtag=$(sed -n 's/^ *host-tag: *\(v[0-9][0-9.]*\).*/\1/p' "$R/.github/workflows/ci.yml" | head -1)
        pin=$(git -C "$R" ls-tree HEAD decomp/common 2>/dev/null | awk '{print $3}')
        tagc=$(git -C "$R/decomp/common" rev-parse -q --verify "refs/tags/$wtag^{commit}" 2>/dev/null || true)
        now=$(git -C "$R/decomp/common" rev-parse -q --verify HEAD 2>/dev/null || true)
        if [ -z "$pin" ]; then
            fail "no committed submodule at decomp/common" "git submodule update --init, then git -C decomp/common checkout $wtag && git add decomp/common"
        elif [ -z "$tagc" ]; then
            note "THPS64Host tag $wtag isn't in your decomp/common checkout, so this can't be checked here (CI checks it). To check it: git -C decomp/common fetch --tags"
        elif [ "$pin" != "$tagc" ]; then
            fail "decomp/common is committed at $(echo "$pin" | cut -c1-12), but the workflows call THPS64Host $wtag" \
                 "git -C decomp/common checkout $wtag && git add decomp/common. (Moving to a new host release? Change every '@v...' and 'host-tag:' in .github/workflows/ to that tag in the same commit.)"
        else
            ok "decomp/common is THPS64Host $wtag"
        fi
        if [ -n "$now" ] && [ -n "$pin" ] && [ "$now" != "$pin" ]; then
            note "your decomp/common checkout is at another commit than the committed one (fine while you test a host branch; don't commit it)."
        fi
    fi

    # 2. The version tables.
    step "version tables (gen_versions.py check)"
    tmp=${TMPDIR:-/tmp}/thps-check.$$
    mkdir -p "$tmp"
    if $PY -I "$H/tools/gen_versions.py" check; then
        $PY -I "$H/tools/gen_versions.py" header "$tmp/a.h" >/dev/null && $PY -I "$H/tools/gen_versions.py" header "$tmp/b.h" >/dev/null
        if cmp -s "$tmp/a.h" "$tmp/b.h"; then ok "the tables agree, and the generated header is the same every time"
        else fail "gen_versions.py wrote two different headers from the same tables" "make the header generation deterministic (no dates, dict order or paths in it); run it twice and diff the results"; fi
    else
        fail "the version tables don't agree" "see the problems listed above"
    fi

    # 3. The build with stand-in game functions (no ROM). Its own build dir, so a real build in build/cmake is never replaced.
    step "build with stand-in game functions (build/cmake-check; without a runtime to reuse, it first fetches the pinned one, which takes a while)"
    log=$H/build/check-build.log
    mkdir -p "$H/build"
    if $PY -I "$H/tools/build_host.py" --stub --build-type Release --build-dir "$H/build/cmake-check" ${THPS_RUNTIME_DIR:+--runtime "$THPS_RUNTIME_DIR"} ${CHECK_JOBS:+-j "$CHECK_JOBS"} >"$log" 2>&1; then
        ok "built (log: $log)"
        rt=$(grep -m1 "reusing the runtime of your last build" "$log") && echo "   note: ${rt#no --runtime given: }"
        EXE=$H/build/cmake-check/${G}_recomp
        [ -f "$EXE.exe" ] && EXE=$EXE.exe

        # 4. The ROM-free checks of the binary, as CI runs them.
        step "ROM-free checks of the binary"
        if "$EXE" --features >/dev/null 2>&1; then ok "--features"
        else fail "$EXE --features failed" "run it by hand to see the error; it must start and print its features without a ROM"; fi
        : > "$tmp/empty.rom"
        "$EXE" --config "$tmp/cfg" --check-rom "$tmp/empty.rom" >/dev/null 2>&1
        rc=$?
        if [ "$rc" = 3 ]; then ok "--check-rom on an empty file exits 3 (not a ROM)"
        else fail "--check-rom on an empty file exited $rc, 3 expected" "the ROM checks in the host's src/ must reject a file that isn't a ROM with exit code 3; run: $EXE --check-rom <empty file>"; fi
        if $PY -I "$H/tests/controls_cfg_test.py" "$EXE" >"$tmp/controls.log" 2>&1; then ok "controls.cfg test"
        else cat "$tmp/controls.log"; fail "the controls.cfg test failed" "see its output above; rerun with: $PY -I $H/tests/controls_cfg_test.py $EXE"; fi
    else
        tail -n 30 "$log"
        fail "the build failed (the last lines are above; the whole log: $log)" \
             "a missing tool or library: install it (BUILDING.md, \"Dependencies\"). A compile error in a file you changed: fix it in your change. An error in a file under build/ (old generated output from an earlier checkout): delete that folder and run again. A runtime error from setup_runtime.py: follow its Fix line."
    fi
    rm -rf "$tmp"

    [ -n "$MAINT" ] || return 0

    # 5. Maintainer: a real build from the dumps, the split engine, the full suite and the first-run test (tier 2).
    step "maintainer: real build from your dumps (build/cmake) and the split engine (build/cmake-split)"
    set --
    while IFS= read -r line; do
        case "$line" in missing\ *|'') ;; *) set -- "$@" --rom "${line#* }" ;; esac
    done <<EOF
$DUMPS
EOF
    if $PY -I "$H/tools/build_host.py" "$@" ${THPS_RUNTIME_DIR:+--runtime "$THPS_RUNTIME_DIR"} ${CHECK_JOBS:+-j "$CHECK_JOBS"} >"$H/build/check-rom-build.log" 2>&1 \
       && $PY -I "$H/tools/build_host.py" --split ${THPS_RUNTIME_DIR:+--runtime "$THPS_RUNTIME_DIR"} ${CHECK_JOBS:+-j "$CHECK_JOBS"} >>"$H/build/check-rom-build.log" 2>&1; then
        ok "built (log: $H/build/check-rom-build.log)"
    else
        tail -n 30 "$H/build/check-rom-build.log"
        fail "the build from your dumps failed (log: $H/build/check-rom-build.log)" "see the last lines above; the stub build passed, so look at the code generation steps (4/6 and 5/6)"
        return 0
    fi
    if [ ! -d "$H/build/tests/refs" ]; then
        fail "no reference pictures on this machine ($H/build/tests/refs)" "record them once: decomp/$G/recomp-host/tools/test.sh --record (with the game build you trust), then run again"
        return 0
    fi
    step "maintainer: the full suite ($G) and the first-run test"
    note "only one copy of the game may run at a time: close it first. Other releases: tools/test.sh --version ID."
    if env "${UPPER}_ROM=$BASE" sh "$H/tools/test.sh"; then ok "full suite"
    else fail "the $UPPER suite failed" "see the failing scenarios above; their pictures are under decomp/$G/recomp-host/build/tests/out/. A changed screen that is right: re-record that checkpoint (tools/test.sh --help)"; fi
    if env "${UPPER}_ROM=$BASE" sh "$H/tools/test.sh" --first-run; then ok "first-run test"
    else fail "the $UPPER first-run test failed" "see its output above (tests/firstrun_test.py)"; fi
}

# ------------------------------------------------------------------ run

if [ -n "$HOSTMODE" ] && [ -z "$GAMES" ]; then
    note "no game was checked, so only the patches are (below). Clone the games as shown above and run this again."
    step "THPS64Host: our RT64 patches apply to the pinned upstream"
    mkdir -p "$REPO/build"
    if $PY -I "$REPO/host/tools/setup_runtime.py" "$REPO/build/check-runtime" --patches-only >"$REPO/build/check-runtime.log" 2>&1; then
        ok "patches apply"
    else
        tail -n 20 "$REPO/build/check-runtime.log"
        fail "a patch doesn't apply to the pinned RT64 (log: $REPO/build/check-runtime.log)" "follow the Fix line above; host/patches/README.md has the \"Moving a pin\" checklist"
    fi
fi

while IFS=' ' read -r r g; do
    [ -n "$r" ] && check_game "$r" "$g" </dev/null
done <<EOF
$GAMES
EOF

# Maintainer: the pre-push guard, over what pushing this branch would publish (it also runs the export's checks on the tree).
if [ -n "$MAINT" ]; then
    step "maintainer: the pre-push guard"
    GUARD=${THPS_GUARD:-}
    if [ -z "$GUARD" ]; then
        hook=$(git -C "$REPO" rev-parse --git-path hooks/pre-push 2>/dev/null || true)
        case "$hook" in /*) ;; ?*) hook=$REPO/$hook ;; esac
        [ -n "$hook" ] && [ -f "$hook" ] && GUARD=$(sed -n 's/^exec "\([^"]*\)".*/\1/p' "$hook" | head -1)
    fi
    if [ -z "$GUARD" ] || [ ! -f "$GUARD" ]; then
        fail "the pre-push guard isn't installed in this checkout" "from a maintainer's checkout: tools/public_guard.sh --install-hook $REPO (for a game also: git -C $REPO config publicguard.hostRepo <THPS64Host checkout>), or set THPS_GUARD"
    elif sh "$GUARD" "$REPO" --ref HEAD; then
        ok "the guard passed: this branch is safe to push"
    else
        fail "the guard refused this branch" "each line above says what failed and how to fix it; MAINTAINING.md, \"Common failures\""
    fi
fi

SCOPE=
[ -n "$HOSTMODE" ] && SCOPE=" (checked: ${CHECKED:-no game}; not checked: ${NOTCHECKED:-none})"
echo
if [ "$FAILS" -eq 0 ]; then
    if [ -n "$MAINT" ]; then echo "All checks passed, the ROM-based ones included$SCOPE."
    else
        if [ -n "$NOTCHECKED" ]; then
            echo "Partly checked: ready for a PR, but not every game was built$SCOPE."
            echo "Clone the missing games as shown above and run again, or say in the PR which games you checked."
        else
            echo "Ready for a PR$SCOPE."
        fi
        echo "Before you open it: say what you changed and how you tested it, and add a line under ## [Unreleased] in CHANGELOG.md."
    fi
    exit 0
fi
echo "$FAILS check(s) failed$SCOPE:$FAILED"
echo "Each one has a Fix: line above. Still stuck? Open the PR as a draft and ask; CI shows the same checks."
exit 1
