#!/bin/sh
# Exercise the contributor check's exact ROM-free CTest routing and failure accounting.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/thps-ctest-test.XXXXXX")
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    rm -f "$TMP/bin/ctest" "$TMP/bin/python3" "$TMP/args" "$TMP/expected" \
        "$TMP/python-calls" "$TMP/check.out" \
        "$TMP/host/host/tools/check.sh" "$TMP/host/host/tools/rom_free_ctest.sh" \
        "$TMP/host/host/tools/setup_runtime.py" "$TMP/host/build/check-runtime.log"
    rmdir "$TMP/bin" "$TMP/host/host/tools" "$TMP/host/host" "$TMP/host/build" "$TMP/host" "$TMP" 2>/dev/null || :
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir -p "$TMP/bin" "$TMP/host/host/tools"

cat > "$TMP/bin/ctest" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$CTEST_ARGS"
exit "${CTEST_EXIT:-0}"
EOF
chmod +x "$TMP/bin/ctest"

cat > "$TMP/bin/python3" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$PYTHON_CALLS"
if [ "$1" = '-c' ]; then exit 0; fi
if [ "$1" = '-I' ] && [ "$2" = "$HOST_FIXTURE/host/tools/setup_runtime.py" ] \
   && [ "$3" = "$HOST_FIXTURE/build/check-runtime" ] && [ "$4" = '--patches-only' ]; then
    exit 0
fi
echo "unexpected python3 invocation: $*" >&2
exit 91
EOF
chmod +x "$TMP/bin/python3"

ok_count=0
fail_count=0
fail_text=
ok() { ok_count=$((ok_count + 1)); }
fail() { fail_count=$((fail_count + 1)); fail_text=$1; }
export PATH="$TMP/bin:$PATH"
export CTEST_ARGS="$TMP/args"
. "$ROOT/host/tools/rom_free_ctest.sh"

run_rom_free_ctests '/tmp/build with spaces'
cat > "$TMP/expected" <<'EOF'
--test-dir
/tmp/build with spaces
-C
Release
-R
^(ride_v3_format|ride_step_state|ride_step_profile|ride_harness|ride_step_runtime|input_events)$
--no-tests=error
--output-on-failure
EOF
cmp "$TMP/expected" "$CTEST_ARGS"
[ "$ok_count" -eq 1 ] && [ "$fail_count" -eq 0 ] || { echo 'success case did not report exactly one ok' >&2; exit 1; }

CTEST_EXIT=8
export CTEST_EXIT
run_rom_free_ctests '/tmp/build with spaces'
cmp "$TMP/expected" "$CTEST_ARGS"
[ "$ok_count" -eq 1 ] && [ "$fail_count" -eq 1 ] || { echo 'ctest failure did not propagate to fail accounting' >&2; exit 1; }
case "$fail_text" in *'ROM-free input, trigger-zone and ride regression tests failed'*) ;; *) echo 'failure message was not specific' >&2; exit 1 ;; esac

# Invoke the host-only top-level check with THPS_HOST_ROOT unset. The Python stub
# permits only the version probe and patches-only setup, so no build or fetch can run.
mkdir -p "$TMP/host/build"
cp "$ROOT/host/tools/check.sh" "$TMP/host/host/tools/check.sh"
cp "$ROOT/host/tools/rom_free_ctest.sh" "$TMP/host/host/tools/rom_free_ctest.sh"
: > "$TMP/host/host/tools/setup_runtime.py"
HOST_FIXTURE=$TMP/host
PYTHON_CALLS=$TMP/python-calls
export HOST_FIXTURE PYTHON_CALLS
if ! env -u THPS_HOST_ROOT CHECK_REPO="$HOST_FIXTURE" PATH="$TMP/bin:$PATH" \
    sh "$TMP/host/host/tools/check.sh" > "$TMP/check.out" 2>&1; then
    cat "$TMP/check.out" >&2
    echo 'host-only check failed with THPS_HOST_ROOT unset' >&2
    exit 1
fi
grep -F 'Partly checked: ready for a PR' "$TMP/check.out" >/dev/null
[ "$(wc -l < "$TMP/python-calls" | tr -d ' ')" = 2 ] || { echo 'unexpected Python calls in host-only check' >&2; exit 1; }
grep -F -- '-I '"$HOST_FIXTURE"'/host/tools/setup_runtime.py ' "$TMP/python-calls" >/dev/null
grep -F 'run_rom_free_ctests "$H/build/cmake-check"' "$ROOT/host/tools/check.sh" >/dev/null
echo 'check_ctest_test: exact routing and failure accounting passed'
