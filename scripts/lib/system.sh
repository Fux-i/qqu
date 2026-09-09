#!/usr/bin/env bash

qqu_perf_init() {
  local -a sudo=()
  ((EUID == 0)) || sudo=(sudo)

  if command -v powerprofilesctl >/dev/null 2>&1; then
    "${sudo[@]}" powerprofilesctl set performance
  else
    echo "warning: powerprofilesctl not found" >&2
  fi

  if [[ -w /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo 0 >/sys/devices/system/cpu/cpufreq/boost
  elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
    "${sudo[@]}" sh -c 'echo 0 > /sys/devices/system/cpu/cpufreq/boost' 2>/dev/null || true
  fi
}
