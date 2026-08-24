#!/usr/bin/env bash

qqu_bench_to_csv() {
  awk '
    BEGIN {
      print "metric,queue,payload,payload_bytes,capacity,run,msgs_per_s,p50_ns,p90_ns,p99_ns,p999_ns"
    }
    /^raw / {
      split("", kv)
      for (i = 2; i <= NF; i++) {
        eq = index($i, "=")
        if (eq)
          kv[substr($i, 1, eq - 1)] = substr($i, eq + 1)
      }
      printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", \
        kv["metric"], kv["queue"], kv["payload"], kv["payload_bytes"], \
        kv["capacity"], kv["run"], kv["msgs_per_s"], \
        kv["p50_ns"], kv["p90_ns"], kv["p99_ns"], kv["p999_ns"]
      next
    }
    { print > "/dev/stderr" }
  '
}
