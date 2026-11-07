# ROM-free CTest cases required by the contributor check. Kept separate so the
# routing and failure accounting can be exercised without a build or game.
run_rom_free_ctests() {   # $1 = configured stub-build directory
    build_dir=$1
    tests='^(ride_v3_format|ride_step_state|ride_step_profile|ride_harness|ride_step_runtime|input_events)$'
    if ctest --test-dir "$build_dir" -C Release -R "$tests" --no-tests=error --output-on-failure; then
        ok "six ROM-free input and ride regression tests"
    else
        fail "one or more ROM-free input and ride regression tests failed" \
             "rerun: ctest --test-dir $build_dir -C Release -R '$tests' --no-tests=error --output-on-failure"
    fi
}
