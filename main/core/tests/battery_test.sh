#!/bin/sh
# Host test for the battery curve and filter (core/battery_calc.c).
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/battery_test.$$
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$out" "$d/battery_test.c" "$d/../battery_calc.c"
"$out"; r=$?
rm -f "$out"
exit $r
