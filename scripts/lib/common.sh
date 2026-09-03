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
  local -a sudo=()
  ((EUID == 0)) || sudo=(sudo)

  command -v powerprofilesctl >/dev/null 2>&1 || {
    echo "benchmark: powerprofilesctl not found" >&2
    return 1
  }
  "${sudo[@]}" powerprofilesctl set performance

  if [[ -w /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo 0 >/sys/devices/system/cpu/cpufreq/boost
  else
    "${sudo[@]}" sh -c 'echo 0 > /sys/devices/system/cpu/cpufreq/boost'
  fi
}

qqu_with_pin() {
  if [[ -n ${cpus:-} ]] && command -v taskset >/dev/null 2>&1; then
    taskset -c "$cpus" "$@"
  else
    "$@"
  fi
}
