#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build"}
jobs=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

usage() {
  cat <<'USAGE'
Usage: test.sh [suite] [gtest-args...]

Suites:
  spsc   qqu_test_spsc
  all    all test targets (default)

Env:
  BUILD_DIR   build directory (default: <repo>/build)
  JOBS        parallel build jobs (default: nproc)

Examples:
  ./scripts/test.sh
  ./scripts/test.sh spsc
  ./scripts/test.sh spsc --gtest_filter='*stress*'
  BUILD_DIR=build-asan ./scripts/test.sh all
USAGE
}

suite=${1:-all}
[[ $# -gt 0 ]] && shift

case "$suite" in
  -h | --help)
    usage
    exit 0
    ;;
  spsc) targets=(qqu_test_spsc) ;;
  all)  targets=(qqu_test_spsc) ;; # add qqu_test_mpsc / qqu_test_mpmc later
  *)
    echo "unknown suite: $suite" >&2
    usage >&2
    exit 1
    ;;
esac

if [[ ! -f $build/build.ninja && ! -f $build/Makefile && ! -f $build/CMakeCache.txt ]]; then
  cmake -S "$root" -B "$build" -G Ninja
fi

cmake --build "$build" --target "${targets[@]}" -j"$jobs"

status=0
for t in "${targets[@]}"; do
  echo "==> $t"
  if ! "$build/$t" "$@"; then
    status=1
  fi
done
exit "$status"
