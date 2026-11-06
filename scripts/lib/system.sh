#!/usr/bin/env bash

qqu_perf_init() {
  local -a sudo=()
  ((EUID == 0)) || sudo=(sudo -n)

  local ppd
  ppd="$(command -v powerprofilesctl || true)"
  if [[ -z "$ppd" ]]; then
    echo "qqu: powerprofilesctl not found" >&2
    return 1
  fi

  "${sudo[@]}" "$ppd" set performance || {
    echo "qqu: cannot set the performance power profile (needs NOPASSWD sudo)" >&2
    return 1
  }

  local profile
  profile="$("$ppd" get)" || return 1
  if [[ "$profile" != "performance" ]]; then
    echo "qqu: power profile is '$profile', expected 'performance'" >&2
    return 1
  fi

  local boost=/sys/devices/system/cpu/cpufreq/boost
  local tee=/run/current-system/sw/bin/tee
  [[ -x "$tee" ]] || tee="$(command -v tee)"

  if [[ -w "$boost" ]]; then
    echo 0 >"$boost"
  elif [[ -e "$boost" ]]; then
    echo 0 | "${sudo[@]}" "$tee" "$boost" >/dev/null || {
      echo "qqu: cannot disable CPU boost (needs NOPASSWD sudo)" >&2
      return 1
    }
  fi

  if [[ -e "$boost" && "$(cat "$boost")" != "0" ]]; then
    echo "qqu: CPU boost is '$(cat "$boost")', expected '0'" >&2
    return 1
  fi
}
