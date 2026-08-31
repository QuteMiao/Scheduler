#!/usr/bin/env bash
#
# build_all.sh — build one binary per workload graph in cases/.
#
# For every cases/*.h header, this script compiles src/scheduler.c (plus its
# dependencies: dispatch.c, painter.c, log.c) with
#   -DSCHEDULER_CASE=cases/<name>.h
# and the painter thread count extracted from that header's
# "PAINTER_THREAD_CNT=N" comment, producing bin/<name>.
#
# Usage:
#   ./build_all.sh              # build every case found in cases/
#   ./build_all.sh <case...>    # build only the given case basename(s)
#   ./build_all.sh clean        # remove the bin/ output directory
#   CLEAN=1 ./build_all.sh      # clean, then rebuild everything
#
# Environment overrides: CC, CFLAGS, CPPFLAGS, JOBS (parallel builds).

set -euo pipefail

# Resolve paths relative to this script so it can be run from any directory.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CASE_DIR="$ROOT_DIR/cases"
BIN_DIR="$ROOT_DIR/bin"
cd "$ROOT_DIR"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=gnu11 -O2 -Wall -Wextra -pthread}"
CPPFLAGS="${CPPFLAGS:--Iinclude -Isrc -I.}"
JOBS="${JOBS:-1}"

SRCS="src/scheduler.c src/dispatch.c src/painter.c src/log.c"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '%s\n' "$*"; }

# Extract the painter thread count from a case header comment, e.g.:
#   * 2 interleaved subgraphs (...); PAINTER_THREAD_CNT=2.
#   * Single combined graph; one painter thread (PAINTER_THREAD_CNT=1).
painter_thread_cnt() {
    local f="$1" n
    n="$(grep -m1 -oE 'PAINTER_THREAD_CNT=[0-9]+' "$f" | head -n1 | cut -d= -f2)"
    if [[ -z "$n" ]]; then
        printf 'warning: no PAINTER_THREAD_CNT comment in %s, defaulting to 1\n' "$f" >&2
        n=1
    fi
    printf '%s' "$n"
}

build_case() {
    local header="$1"                # absolute path: cases/<name>.h
    local name cnt out
    name="$(basename "$header" .h)"
    cnt="$(painter_thread_cnt "$header")"
    out="$BIN_DIR/$name"

    log "==> building $name (PAINTER_THREAD_CNT=$cnt)"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $CPPFLAGS \
        -DSCHEDULER_CASE="cases/$name.h" \
        -DPAINTER_THREAD_CNT="$cnt" \
        -o "$out" $SRCS
    log "    -> $out"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "clean" ]]; then
    log "removing $BIN_DIR"
    rm -rf "$BIN_DIR"
    exit 0
fi
if [[ "${CLEAN:-0}" == "1" ]]; then
    rm -rf "$BIN_DIR"
fi

mkdir -p "$BIN_DIR"

if [[ $# -gt 0 ]]; then
    # Build only the explicitly named cases.
    for arg in "$@"; do
        header="$CASE_DIR/$arg.h"
        [[ -f "$header" ]] || die "case '$arg' not found: $header"
        build_case "$header"
    done
else
    # Build every cases/*.h header.
    shopt -s nullglob
    headers=("$CASE_DIR"/*.h)
    [[ ${#headers[@]} -gt 0 ]] || die "no case headers found in $CASE_DIR"

    if [[ "$JOBS" -gt 1 ]]; then
        pids=()
        for header in "${headers[@]}"; do
            build_case "$header" &
            pids+=("$!")
            # Cap concurrency at $JOBS in-flight builds.
            if [[ ${#pids[@]} -ge "$JOBS" ]]; then
                wait "${pids[0]}"
                pids=("${pids[@]:1}")
            fi
        done
        wait
    else
        for header in "${headers[@]}"; do
            build_case "$header"
        done
    fi
fi

log "done."
