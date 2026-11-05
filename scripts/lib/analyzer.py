from collections import defaultdict
from pathlib import Path
from typing import Optional

import pandas as pd


def parse_csv(csv_path: Path) -> pd.DataFrame:
    return pd.read_csv(csv_path)


def find_latest_result(results_base: Path, name_filter: Optional[str] = None) -> Optional[Path]:
    raw_dir = results_base / "raw"
    if not raw_dir.exists():
        return None

    candidates = []
    for date_dir in sorted(raw_dir.iterdir(), reverse=True):
        if not date_dir.is_dir():
            continue
        if name_filter and name_filter not in date_dir.name:
            continue
        csv_files = list(date_dir.glob("*.bench.csv"))
        if csv_files:
            candidates.append(max(csv_files, key=lambda p: p.name))

    return candidates[0] if candidates else None


def compute_summary(df: pd.DataFrame, baseline_queue: Optional[str] = None) -> dict:
    if baseline_queue is None:
        baseline_queue = df["queue"].iloc[0]

    summary = defaultdict(dict)

    for producer_count in sorted(df["producer_count"].unique()):
        df_pc = df[df["producer_count"] == producer_count]

        for payload in ["u32", "u64", "p16", "p64"]:
            df_p = df_pc[df_pc["payload"] == payload]
            if df_p.empty:
                continue

            for capacity in sorted(df_p["capacity"].unique()):
                df_cap = df_p[df_p["capacity"] == capacity]

                throughput_data = df_cap[df_cap["metric"] == "throughput"].groupby("queue")["msgs_per_s"].mean()
                latency_p50 = df_cap[df_cap["metric"] == "latency"].groupby("queue")["p50_ns"].mean()
                latency_p99 = df_cap[df_cap["metric"] == "latency"].groupby("queue")["p99_ns"].mean()

                baseline_tp = throughput_data.get(baseline_queue, 0)

                key = (producer_count, payload, int(capacity))
                summary[key]["capacity"] = int(capacity)
                summary[key]["throughput"] = {}
                summary[key]["latency_p50"] = {}
                summary[key]["latency_p99"] = {}
                summary[key]["baseline"] = baseline_queue
                summary[key]["producer_cpus"] = df_pc["producer_cpus"].iloc[0] if "producer_cpus" in df_pc else "unknown"
                consumer_column = "consumer_cpus" if "consumer_cpus" in df_pc else "consumer_cpu"
                summary[key]["consumer_cpu"] = (
                    df_pc[consumer_column].iloc[0] if consumer_column in df_pc else "unknown"
                )

                for queue in throughput_data.index:
                    tp = throughput_data[queue]
                    diff = ((tp - baseline_tp) / baseline_tp * 100) if baseline_tp else 0
                    summary[key]["throughput"][queue] = (tp, diff)

                for queue in latency_p50.index:
                    p50 = latency_p50[queue]
                    p99 = latency_p99.get(queue, 0)
                    summary[key]["latency_p50"][queue] = p50
                    summary[key]["latency_p99"][queue] = p99

    return dict(summary)


def _queue_table(values: dict, baseline: str, payload: str, digits: int = 1) -> list[str]:
    if baseline not in values:
        baseline = next(iter(values))
    baseline_value = values[baseline]
    lines = [f"  {payload:4s}  {baseline:30s} {baseline_value:7.1f}  [baseline]"]
    for queue, value in values.items():
        if queue == baseline:
            continue
        diff = ((value - baseline_value) / baseline_value * 100) if baseline_value else 0
        lines.append(f"        {queue:30s} {value:7.1f}  ({diff:+.1f}%)")
    return lines


def format_summary(summary: dict, experiment_name: str, date: str) -> str:
    lines = []
    lines.append(f"Benchmark Summary: {experiment_name}")
    lines.append(f"Date: {date[:4]}-{date[4:6]}-{date[6:8]}")
    lines.append("")

    producer_counts = sorted(set(k[0] for k in summary.keys()))

    for pc in producer_counts:
        keys = [k for k in summary if k[0] == pc]
        first_key = sorted(keys)[0]
        entry = summary[first_key]
        baseline = entry.get("baseline", "")

        lines.append(f"=== {pc} Producer(s) ===")
        lines.append("")
        lines.append(
            f"CPU placement: producers={entry.get('producer_cpus', 'unknown')}; "
            f"consumer={entry.get('consumer_cpu', 'unknown')}"
        )

        capacities = sorted({k[2] for k in keys})
        payloads = sorted({k[1] for k in keys})

        for capacity in capacities:
            lines.append("")
            lines.append(f"Configuration: capacity={capacity}")
            lines.append("")
            lines.append("Throughput (Mops/s):")
            for payload in payloads:
                key = (pc, payload, capacity)
                if key not in summary or not summary[key]["throughput"]:
                    continue
                throughput = {
                    queue: value[0] / 1e6
                    for queue, value in summary[key]["throughput"].items()
                }
                lines.extend(_queue_table(throughput, baseline, payload))

            lines.append("")
            lines.append("Latency p50 (ns):")
            for payload in payloads:
                key = (pc, payload, capacity)
                if key not in summary or not summary[key]["latency_p50"]:
                    continue
                lines.extend(_queue_table(summary[key]["latency_p50"], baseline, payload))

            lines.append("")
            lines.append("Latency p99 (ns):")
            for payload in payloads:
                key = (pc, payload, capacity)
                if key not in summary or not summary[key]["latency_p99"]:
                    continue
                lines.extend(_queue_table(summary[key]["latency_p99"], baseline, payload))

        lines.append("")

    return "\n".join(lines)


def generate_summary(csv_path: Path, output_path: Optional[Path] = None) -> str:
    df = parse_csv(csv_path)

    experiment_name = csv_path.stem.split(".", 1)[1] if "." in csv_path.stem else "benchmark"
    date = csv_path.parent.name.split(".", 1)[0]

    summary = compute_summary(df)
    text = format_summary(summary, experiment_name, date)

    if output_path:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(text)

    return text
