import argparse
import re
from collections import defaultdict
from pathlib import Path
from statistics import mean

import matplotlib.pyplot as plt


QUEUES = ["qqu::spsc", "rigtorp::SPSCQueue", "atomic_queue::AtomicQueue2"]
PAYLOADS = ["u32", "u64", "p16", "p64"]
LABELS = {"u32": "uint32_t", "u64": "uint64_t", "p16": "16-byte", "p64": "64-byte"}
COLORS = ["#0072B2", "#D55E00", "#009E73"]


def records(paths):
    for path in paths:
        for line in path.read_text().splitlines():
            if not line.startswith("raw "):
                continue
            yield dict(re.findall(r"(\w+)=([^ ]+)", line))


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
    capacity_labels = {8: "8", 64: "64", 1024: "1K", 65536: "64K"}

    fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True, constrained_layout=True)
    fig.suptitle(title, fontsize=16)
    for payload, ax in zip(PAYLOADS, axes.flat):
        positions = range(len(capacities))
        width = 0.24
        for i, (queue, color) in enumerate(zip(QUEUES, COLORS)):
            x = [position + (i - 1) * width for position in positions]
            y = [mean(values[payload, capacity, queue]) for capacity in capacities]
            ax.bar(x, y, width=width, color=color, label=queue)
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
    match = re.fullmatch(r"spsc\.([^.]+)\.txt", args.results[-1].name)
    if not match:
        parser.error("result filename must be spsc.{time}.txt")
    prefix = f"spsc.{match.group(1)}"
    rows = list(records(args.results))
    if not rows:
        parser.error("no raw benchmark records found")
    required = {"metric", "queue", "payload", "capacity"}
    if any(not required <= row.keys() for row in rows):
        parser.error("result uses the legacy format; run the benchmark again")
    plot(
        rows,
        "throughput",
        "msgs_per_s",
        "SPSC Throughput",
        "Messages / second",
        output_dir / f"{prefix}.throughput.svg",
    )
    plot(
        rows,
        "latency",
        "p50_ns",
        "SPSC Ping-Pong Latency (p50)",
        "p50 RTT (ns)",
        output_dir / f"{prefix}.latency-p50.svg",
    )
    plot(
        rows,
        "latency",
        "p99_ns",
        "SPSC Ping-Pong Latency (p99)",
        "p99 RTT (ns)",
        output_dir / f"{prefix}.latency-p99.svg",
    )


if __name__ == "__main__":
    main()
