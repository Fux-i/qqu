import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path
from statistics import mean

import matplotlib.pyplot as plt


PAYLOADS = ["u32", "u64", "p16", "p64"]
LABELS = {"u32": "uint32_t", "u64": "uint64_t", "p16": "16-byte", "p64": "64-byte"}
COLORS = ["#0072B2", "#D55E00", "#009E73"]


def records(paths):
    for path in paths:
        with path.open(newline="") as f:
            yield from csv.DictReader(f)


def plot(rows, metric, field, title, ylabel, output):
    values = defaultdict(list)
    for row in rows:
        if row["metric"] == metric:
            values[row["payload"], int(row["capacity"]), row["queue"]].append(
                float(row[field])
            )
    if not values:
        return
    capacities = sorted({capacity for _, capacity, _ in values})
    implementations = sorted({implementation for _, _, implementation in values})
    capacity_labels = {64: "64", 1024: "1K", 65536: "64K"}

    fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True, constrained_layout=True)
    fig.suptitle(title, fontsize=16)
    for payload, ax in zip(PAYLOADS, axes.flat):
        positions = range(len(capacities))
        width = min(0.24, 0.8 / max(1, len(implementations)))
        for i, implementation in enumerate(implementations):
            color = COLORS[i % len(COLORS)]
            center = (len(implementations) - 1) / 2
            x = [position + (i - center) * width for position in positions]
            y = [
                mean(samples)
                if (samples := values.get((payload, capacity, implementation)))
                else float("nan")
                for capacity in capacities
            ]
            ax.bar(x, y, width=width, color=color, label=implementation)
        ax.set_title(LABELS[payload])
        ax.set_xticks(
            list(positions), [capacity_labels.get(x, str(x)) for x in capacities]
        )
        ax.grid(axis="y", alpha=0.25)
        ax.set_ylabel(ylabel)
    for ax in axes[-1]:
        ax.set_xlabel("Capacity")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=3, frameon=False)
    fig.savefig(output, dpi=180)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", nargs="+", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    output_dir = args.output_dir or args.results[-1].parent
    output_dir.mkdir(parents=True, exist_ok=True)
    match = re.fullmatch(r"(\d{6})\.([^.]+)\.bench\.csv", args.results[-1].name)
    if not match:
        parser.error("result filename must be {time}.{comment}.bench.csv")
    prefix = f"{match.group(1)}.{match.group(2)}"
    rows = list(records(args.results))
    if not rows:
        parser.error("no benchmark records found")
    required = {"metric", "queue", "payload", "capacity"}
    if any(not required <= row.keys() for row in rows):
        parser.error("result is missing required columns")
    groups = defaultdict(list)
    for row in rows:
        groups[
            row.get("suite") or "spsc",
            row.get("producers") or "1",
            row.get("latency_kind") or "ping_pong_rtt",
        ].append(row)
    for (suite, producers, latency_kind), group in groups.items():
        stem = (
            prefix
            if len(groups) == 1
            else f"{prefix}.{suite}.{producers}p.{latency_kind}"
        )
        title = f"{suite.upper()} ({producers}P / 1C)"
        latency_label = (
            "Fan-In + Acknowledgement"
            if latency_kind == "fan_in_ack_rtt"
            else "Ping-Pong"
        )
        plot(
            group,
            "throughput",
            "msgs_per_s",
            f"{title} Throughput",
            "Messages / second",
            output_dir / f"{stem}.throughput.svg",
        )
        for percentile in ("p50", "p99"):
            plot(
                group,
                "latency",
                f"{percentile}_ns",
                f"{title} {latency_label} Latency ({percentile})",
                f"{percentile} RTT (ns)",
                output_dir / f"{stem}.latency-{percentile}.svg",
            )


if __name__ == "__main__":
    main()
