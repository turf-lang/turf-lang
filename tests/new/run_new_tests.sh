#!/bin/bash
set -e

PASS=0
FAIL=0
TMP=tests/new/_out.txt

run_and_check() {
  file="$1"
  expect="$2"
  echo "Running $file..."
  ./turf "$file" 2>$TMP || true
  if grep -q "$expect" $TMP; then
    echo "PASS $file"
    PASS=$((PASS+1))
  else
    echo "FAIL $file"
    cat $TMP
    FAIL=$((FAIL+1))
  fi
  rm -f $TMP
}

run_and_check tests/new/alias_use_after_free_new.turf "after it was freed"

echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ $FAIL -ne 0 ]; then
  exit 1
fi
