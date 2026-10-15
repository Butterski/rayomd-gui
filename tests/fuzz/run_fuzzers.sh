#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# run_fuzzers.sh <fuzz build dir> <seconds> <work dir> [harness...]
#
# Runs each harness (all of them by default) for <seconds> with libFuzzer, starting from the
# seeds rgui-fuzz-seeds writes (made once per work dir) and the corpus a previous run left in
# <work dir>/corpus/<harness>. Crashes, timeouts and OOMs go to <work dir>/artifacts/<harness>/;
# the logs to <work dir>/logs/. Prints one line per harness: executions, coverage, corpus and
# findings. Exits 1 when any harness found something.
set -u
if [ $# -lt 3 ]; then
    echo "usage: run_fuzzers.sh <fuzz build dir> <seconds> <work dir> [harness...]" >&2
    exit 2
fi
build=$(cd "$1" && pwd)
seconds=$2
work=$3
shift 3
source_dir=$(cd "$(dirname "$0")/../.." && pwd)
harnesses=${*:-"pdf jpeg font theme fontconf settings xkb compose dbus wl_wire"}

mkdir -p "$work/logs"
work=$(cd "$work" && pwd)
if [ ! -d "$work/seeds" ]; then
    "$build/tests/fuzz/rgui-fuzz-seeds" "$source_dir" "$work/seeds" || exit 1
fi

max_len() {
    case $1 in
        pdf) echo 524288 ;;
        font) echo 262144 ;;
        jpeg | xkb) echo 131072 ;;
        settings) echo 16384 ;;
        *) echo 65536 ;;
    esac
}

status=0
for harness in $harnesses; do
    corpus="$work/corpus/$harness"
    artifacts="$work/artifacts/$harness"
    log="$work/logs/$harness.log"
    mkdir -p "$corpus" "$artifacts" "$work/seeds/$harness"
    dict=
    if [ -f "$source_dir/tests/fuzz/$harness.dict" ]; then dict="-dict=$source_dir/tests/fuzz/$harness.dict"; fi
    # shellcheck disable=SC2086 # $dict: one option or none
    "$build/tests/fuzz/rgui-fuzz-$harness" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
        -max_len="$(max_len "$harness")" -print_final_stats=1 -artifact_prefix="$artifacts/" $dict \
        "$corpus" "$work/seeds/$harness" >"$log" 2>&1
    execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$log" | tail -n 1)
    cov=$(grep -o 'cov: [0-9]*' "$log" | tail -n 1 | cut -d' ' -f2)
    ft=$(grep -o 'ft: [0-9]*' "$log" | tail -n 1 | cut -d' ' -f2)
    files=$(find "$corpus" -type f | wc -l)
    findings=$(find "$artifacts" -type f | wc -l)
    echo "$harness: ${execs:-0} execs in ${seconds} s, cov ${cov:-?}, ft ${ft:-?}, corpus $files, findings $findings"
    if [ "$findings" -ne 0 ]; then
        status=1
        find "$artifacts" -type f | sed 's/^/    /'
        grep -m 3 -E 'ERROR|runtime error|fuzz invariant|SUMMARY' "$log" | sed 's/^/    /'
    fi
done
exit $status
