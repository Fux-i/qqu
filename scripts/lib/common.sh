#!/usr/bin/env bash

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
jobs=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
build=${BUILD_DIR:-"$root/build"}
results_dir=${RESULTS_DIR:-"$root/results"}

qqu_prefer_cxx() {
  [[ -n ${CXX+x} ]] && return
  if command -v clang++ >/dev/null 2>&1; then
    export CXX=clang++
  elif command -v g++ >/dev/null 2>&1; then
    export CXX=g++
  fi
}

qqu_refuse_sanitizer() {
  if [[ -f $build/CMakeCache.txt ]] &&
    grep -q 'QQU_SANITIZER:[^=]*=\(asan\|tsan\|ubsan\)' "$build/CMakeCache.txt" 2>/dev/null; then
    echo "benchmark: refusing sanitizer build dir: $build (use BUILD_DIR=build)" >&2
    exit 1
  fi
}

qqu_cmake_release() {
  cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build" --target "$@" -j"$jobs"
}

qqu_set_quiet() {
  if command -v powerprofilesctl >/dev/null 2>&1; then
    powerprofilesctl set performance 2>/dev/null \
      || echo "benchmark: warning: powerprofilesctl set performance failed" >&2
  fi
  if [[ -w /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo 0 >/sys/devices/system/cpu/cpufreq/boost
  elif sudo sh -c 'echo 0 > /sys/devices/system/cpu/cpufreq/boost'; then
    :
  else
    echo "benchmark: warning: cannot disable turbo (needs root)" >&2
  fi
}

qqu_with_pin() {
  if [[ -n ${cpus:-} ]] && command -v taskset >/dev/null 2>&1; then
    taskset -c "$cpus" "$@"
  else
    "$@"
  fi
}
