#!/usr/bin/env bash

qqu_ratio() {
  local num=${1:-0} den=${2:-0}
  if [[ -z $den || $den == 0 ]]; then
    printf 'nan'
  else
    awk -v n="$num" -v d="$den" 'BEGIN { printf "%.6f", n / d }'
  fi
}

qqu_payload_bytes() {
  case "$1" in
    u32) echo 4 ;;
    u64) echo 8 ;;
    p16) echo 16 ;;
    p64) echo 64 ;;
    *) echo 0 ;;
  esac
}

qqu_parse_perf_csv() {
  local file=$1
  local count event
  while IFS= read -r line; do
    [[ $line == \#* || -z $line ]] && continue
    IFS=, read -r count _ event _ <<<"$line"
    [[ $count == \<not\ counted\> || $count == \<not\ supported\> ]] && continue
    [[ $count == *not*counted* || $count == *not*supported* ]] && continue
    event=${event%%:*}
    event=${event##*/}
    event=${event%/}
    [[ -n $event && $count =~ ^[0-9]+$ ]] && ev[$event]=$count
  done <"$file"
}

qqu_run_perf_group() {
  local events=$1
  shift
  local tmp
  tmp=$(mktemp)
  if ! perf stat -x, -e "$events" -- "$@" >/dev/null 2>"$tmp"; then
    cat "$tmp" >&2
    rm -f "$tmp"
    return 1
  fi
  qqu_parse_perf_csv "$tmp"
  rm -f "$tmp"
}

qqu_emit_perf_csv() {
  local q=$1 payload=$2 cap=$3
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    perf "$q" "$payload" "$(qqu_payload_bytes "$payload")" "$cap" \
    "${ev[cycles]:-0}" "${ev[instructions]:-0}" \
    "$(qqu_ratio "${ev[instructions]:-0}" "${ev[cycles]:-0}")" \
    "${ev[branches]:-0}" "${ev[branch-misses]:-0}" \
    "$(qqu_ratio "${ev[branch-misses]:-0}" "${ev[branches]:-0}")" \
    "${ev[cache-references]:-0}" "${ev[cache-misses]:-0}" \
    "$(qqu_ratio "${ev[cache-misses]:-0}" "${ev[cache-references]:-0}")" \
    "${ev[L1-dcache-loads]:-0}" "${ev[L1-dcache-load-misses]:-0}" \
    "$(qqu_ratio "${ev[L1-dcache-load-misses]:-0}" "${ev[L1-dcache-loads]:-0}")" \
    "${ev[context-switches]:-0}" "${ev[cpu-migrations]:-0}" \
    "${ev[page-faults]:-0}"
}

qqu_run_perf() {
  local bin=$1
  local out=$2
  local only=${3:-}
  local out_tmp="$out.tmp"
  local -a queues
  local -a payloads caps
  local -a groups=(
    cycles,instructions,branches,branch-misses
    cache-references,cache-misses
    L1-dcache-loads,L1-dcache-load-misses
    context-switches,cpu-migrations,page-faults
  )
  if ((full)); then
    payloads=(u32 u64 p16 p64)
    caps=(64 1024 65536)
  else
    payloads=(u64)
    caps=(1024)
  fi

  if [[ -n $only ]]; then
    "$bin" --list --only "$only" >/dev/null
    IFS=, read -ra queues <<<"$only"
  else
    mapfile -t queues < <("$bin" --list)
  fi

  command -v perf >/dev/null 2>&1 || {
    echo "benchmark: perf not found" >&2
    return 1
  }
  if ! perf stat -e cycles -- true >/dev/null 2>&1; then
    echo "benchmark: perf stat failed (set kernel.perf_event_paranoid <= 1)" >&2
    return 1
  fi

  printf '%s\n' \
    'metric,queue,payload,payload_bytes,capacity,cycles,instructions,ipc,branches,branch_misses,branch_miss_ratio,cache_refs,cache_misses,cache_miss_ratio,l1d_loads,l1d_misses,l1d_miss_ratio,context_switches,cpu_migrations,page_faults' \
    >"$out_tmp"

  local q payload cap group
  local -a cmd
  for q in "${queues[@]}"; do
    for payload in "${payloads[@]}"; do
      for cap in "${caps[@]}"; do
        cmd=("$bin" --throughput --only "$q" --payload "$payload" --capacity "$cap")
        ((full)) && cmd+=(--full) || cmd+=(--quick)
        [[ -n $scenario ]] && cmd+=(--scenario "$scenario")
        [[ -n $cpus ]] && cmd+=(--cpus "$cpus")
        if [[ -n $cpus ]] && command -v taskset >/dev/null 2>&1; then
          cmd=(taskset -c "$cpus" "${cmd[@]}")
        fi
        echo "==> perf $q payload=$payload capacity=$cap" >&2
        declare -gA ev=()
        for group in "${groups[@]}"; do
          if ! qqu_run_perf_group "$group" "${cmd[@]}"; then
            rm -f "$out_tmp"
            return 1
          fi
        done
        qqu_emit_perf_csv "$q" "$payload" "$cap" >>"$out_tmp"
      done
    done
  done
  mv "$out_tmp" "$out"
}
