#!/bin/bash
set -e

PASS=0
FAIL=0

for file in $(find tests -name "*.tr"); do
    if ./turf "$file" -o /tmp/turf_test_bin 2>/dev/null; then
        echo "PASS $file"
        PASS=$((PASS+1))
    else
        echo "FAIL $file"
        FAIL=$((FAIL+1))
    fi
done

echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ $FAIL -ne 0 ]; then
    exit 1
fi
