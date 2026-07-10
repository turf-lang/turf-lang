#!/usr/bin/env bash
# Mini test runner for tests/10_default_init/ — mirrors runner.sh conventions
cd "$(dirname "$0")/.." || exit 1

PASS=0; FAIL=0
COMPILER="./build/turf"

for f in tests/10_default_init/*.tr; do
    name=$(basename "$f")
    out="${f%.tr}.out"

    if [[ "$name" == fail_* ]]; then
        # Negative test — must fail to compile
        "$COMPILER" "$f" -o "$out" > /dev/null 2>&1
        status=$?
        rm -f "$out" "${out%.out}.o"
        if [ $status -ne 0 ]; then
            echo "[PASS] (failed as expected): $name"
            PASS=$((PASS+1))
        else
            echo "[FAIL] expected compile error but succeeded: $name"
            FAIL=$((FAIL+1))
        fi
    else
        # Positive test — must compile and exit 0
        compile_out=$("$COMPILER" "$f" -o "$out" 2>&1)
        if [ $? -ne 0 ]; then
            echo "[FAIL] compile error: $name"
            echo "$compile_out" | sed 's/^/  /'
            FAIL=$((FAIL+1))
            continue
        fi
        run_out=$("./$out" 2>&1)
        run_status=$?
        rm -f "$out"
        if [ $run_status -eq 0 ]; then
            echo "[PASS]: $name"
            PASS=$((PASS+1))
        else
            echo "[FAIL] runtime (exit $run_status): $name"
            echo "$run_out" | sed 's/^/  /'
            FAIL=$((FAIL+1))
        fi
    fi
done

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] && exit 0 || exit 1
