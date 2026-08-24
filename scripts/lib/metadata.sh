#!/usr/bin/env bash

qqu_metadata() {
  local target=$1
  local compiler cpu_pair cpu cache_dir
  compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$build/CMakeCache.txt")
  compiler=${compiler:-${CXX:-c++}}
  cpu_pair=${cpus:-6,8}

  printf '# cpu_model=%s\n' "$(LC_ALL=C lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
  LC_ALL=C lscpu -e=CPU,CORE,SOCKET,NODE | while IFS= read -r line; do
    printf '# cpu_topology=%s\n' "$line"
  done
  LC_ALL=C lscpu -C | while IFS= read -r line; do
    printf '# cache=%s\n' "$line"
  done
  printf '# kernel=%s\n' "$(uname -srvm)"
  printf '# compiler=%s\n' "$compiler"
  "$compiler" --version | while IFS= read -r line; do
    printf '# compiler_version=%s\n' "$line"
  done

  for dep in spscqueue atomic_queue; do
    local dep_dir="$root/deps/${dep}-src"
    if [[ -d $dep_dir/.git ]]; then
      printf '# dependency=%s version=%s commit=%s\n' "$dep" \
        "$(git -C "$dep_dir" describe --tags --always --dirty)" \
        "$(git -C "$dep_dir" rev-parse HEAD)"
    else
      printf '# dependency=%s version=unavailable commit=unavailable\n' "$dep"
    fi
  done

  printf '# compile_command=%s\n' "$(ninja -C "$build" -t commands "$target" | sed -n '1p')"
  IFS=, read -ra cpu_list <<<"$cpu_pair"
  for cpu in "${cpu_list[@]}"; do
    for cache_dir in /sys/devices/system/cpu/cpu"$cpu"/cache/index*; do
      [[ -d $cache_dir ]] || continue
      printf '# cache_cpu=%s index=%s level=%s type=%s size=%s line_size=%s ways=%s sets=%s shared_cpus=%s\n' \
        "$cpu" "${cache_dir##*/index}" "$(<"$cache_dir/level")" "$(<"$cache_dir/type")" \
        "$(<"$cache_dir/size")" "$(<"$cache_dir/coherency_line_size")" \
        "$(<"$cache_dir/ways_of_associativity")" "$(<"$cache_dir/number_of_sets")" \
        "$(<"$cache_dir/shared_cpu_list")"
    done
    local governor="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
    if [[ $cpu =~ ^[0-9]+$ && -r $governor ]]; then
      printf '# cpu_governor cpu=%s value=%s\n' "$cpu" "$(<"$governor")"
    else
      printf '# cpu_governor cpu=%s value=unavailable\n' "$cpu"
    fi
  done

  if [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
    printf '# turbo interface=cpufreq/boost value=%s\n' "$(</sys/devices/system/cpu/cpufreq/boost)"
  elif [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    printf '# turbo interface=intel_pstate/no_turbo value=%s\n' "$(</sys/devices/system/cpu/intel_pstate/no_turbo)"
  else
    printf '# turbo interface=unavailable value=unavailable\n'
  fi
}
