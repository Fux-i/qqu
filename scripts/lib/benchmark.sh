#!/usr/bin/env bash

qqu_bench_command() {
  run=("$1")
  ((full)) && run+=(--full)
  [[ -n $scenario ]] && run+=(--scenario "$scenario")
  [[ -n $cpus ]] && run+=(--cpus "$cpus")
  [[ -n $only ]] && run+=(--only "$only")
  run+=("${bench_args[@]}")
}

qqu_run_benchmark() {
  local target=$1
  local type=${target#qqu_bench_}
  local cpus=${cpus:-}
  if [[ -z $cpus ]]; then
    case "$type:${scenario:-cross}" in
      spsc:cross) cpus=6,8 ;;
      spsc:smt) cpus=6,7 ;;
      mpsc:cross) cpus=6,8,10 ;;
      mpsc:smt) cpus=6,8,7 ;;
    esac
  elif [[ $suite == all && $type == spsc ]]; then
    cpus="${cpus%%,*},${cpus##*,}"
  fi

  local out_dir="$results_dir/raw/$date_stamp.$type"
  mkdir -p "$out_dir"
  local stem="$out_dir/$time_stamp.$comment"
  echo "==> $target  (build=$build cpus=$cpus) -> $stem"
  qqu_metadata "$target" >"$stem.meta.txt" || return 1

  if ((do_perf)); then
    qqu_run_perf "$build/$target" "$stem.perf.csv" "$only"
    return
  fi

  local -a run
  qqu_bench_command "$build/$target"
  local csv_tmp="$stem.bench.csv.tmp"
  if ! qqu_with_pin "${run[@]}" | qqu_bench_to_csv >"$csv_tmp"; then
    rm -f "$csv_tmp"
    return 1
  fi
  mv "$csv_tmp" "$stem.bench.csv"
}