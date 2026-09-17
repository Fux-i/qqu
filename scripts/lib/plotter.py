from pathlib import Path
from typing import Optional

import matplotlib.pyplot as plt
import pandas as pd


PAYLOADS = ["u32", "u64", "p16", "p64"]
LABELS = {"u32": "uint32_t", "u64": "uint64_t", "p16": "16-byte", "p64": "64-byte"}
COLORS = ["#0072B2", "#D55E00", "#009E73", "#CC79A7", "#F0E442", "#56B4E9"]


def _placement_label(df: pd.DataFrame) -> str:
    producer_cpus = df["producer_cpus"].iloc[0] if "producer_cpus" in df.columns else "unknown"
    consumer_cpus = df["consumer_cpus"].iloc[0] if "consumer_cpus" in df.columns else (
        df["consumer_cpu"].iloc[0] if "consumer_cpu" in df.columns else "unknown"
    )
    return f"P: {producer_cpus} | C: {consumer_cpus}"


def plot_capacity_comparison(df: pd.DataFrame, output_dir: Path, prefix: str):
    producer_counts = sorted(df["producer_count"].unique())
    
    for pc in producer_counts:
        df_pc = df[df["producer_count"] == pc]
        placement = _placement_label(df_pc)
        
        values_tp = {}
        values_p50 = {}
        values_p99 = {}
        
        for _, row in df_pc.iterrows():
            key = (row["payload"], int(row["capacity"]), row["queue"])
            if row["metric"] == "throughput":
                values_tp[key] = float(row["msgs_per_s"])
            elif row["metric"] == "latency":
                values_p50[key] = float(row["p50_ns"])
                values_p99[key] = float(row["p99_ns"])
        
        if not values_tp:
            continue
        
        capacities = sorted(set(k[1] for k in values_tp.keys()))
        queues = sorted(set(k[2] for k in values_tp.keys()))
        
        capacity_labels = {64: "64", 1024: "1K", 2048: "2K", 65536: "64K"}
        
        fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True, constrained_layout=True)
        fig.suptitle(f"MPSC ({pc}P/1C) Throughput | {placement}", fontsize=16)
        
        for payload, ax in zip(PAYLOADS, axes.flat):
            positions = list(range(len(capacities)))
            width = min(0.24, 0.8 / max(1, len(queues)))
            
            for i, queue in enumerate(queues):
                color = COLORS[i % len(COLORS)]
                center = (len(queues) - 1) / 2
                x = [pos + (i - center) * width for pos in positions]
                y = [values_tp.get((payload, cap, queue), float("nan")) / 1e6 for cap in capacities]
                ax.bar(x, y, width=width, color=color, label=queue)
            
            ax.set_title(LABELS[payload])
            ax.set_xticks(positions, [capacity_labels.get(c, str(c)) for c in capacities])
            ax.grid(axis="y", alpha=0.25)
            ax.set_ylabel("Mops/s")
        
        for ax in axes[-1]:
            ax.set_xlabel("Capacity")
        
        handles, labels = axes[0, 0].get_legend_handles_labels()
        fig.legend(handles, labels, loc="outside lower center", ncols=3, frameon=False)
        
        output_path = output_dir / f"{prefix}.{pc}p.throughput.svg"
        fig.savefig(output_path, dpi=180)
        plt.close(fig)
        
        for percentile, values in [("p50", values_p50), ("p99", values_p99)]:
            fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True, constrained_layout=True)
            fig.suptitle(f"MPSC ({pc}P/1C) Latency ({percentile}) | {placement}", fontsize=16)
            
            for payload, ax in zip(PAYLOADS, axes.flat):
                positions = list(range(len(capacities)))
                width = min(0.24, 0.8 / max(1, len(queues)))
                
                for i, queue in enumerate(queues):
                    color = COLORS[i % len(COLORS)]
                    center = (len(queues) - 1) / 2
                    x = [pos + (i - center) * width for pos in positions]
                    y = [values.get((payload, cap, queue), float("nan")) for cap in capacities]
                    ax.bar(x, y, width=width, color=color, label=queue)
                
                ax.set_title(LABELS[payload])
                ax.set_xticks(positions, [capacity_labels.get(c, str(c)) for c in capacities])
                ax.grid(axis="y", alpha=0.25)
                ax.set_ylabel(f"{percentile} RTT (ns)")
            
            for ax in axes[-1]:
                ax.set_xlabel("Capacity")
            
            handles, labels = axes[0, 0].get_legend_handles_labels()
            fig.legend(handles, labels, loc="outside lower center", ncols=3, frameon=False)
            
            output_path = output_dir / f"{prefix}.{pc}p.latency-{percentile}.svg"
            fig.savefig(output_path, dpi=180)
            plt.close(fig)


def plot_producer_scaling(df: pd.DataFrame, output_dir: Path, prefix: str):
    producer_counts = sorted(df["producer_count"].unique())
    if len(producer_counts) < 2:
        return
    
    capacity = df["capacity"].mode()[0]
    df_cap = df[df["capacity"] == capacity]
    
    values_tp = {}
    values_p99 = {}
    
    for _, row in df_cap.iterrows():
        key = (row["payload"], int(row["producer_count"]), row["queue"])
        if row["metric"] == "throughput":
            values_tp[key] = float(row["msgs_per_s"])
        elif row["metric"] == "latency":
            values_p99[key] = float(row["p99_ns"])
    
    queues = sorted(set(k[2] for k in values_tp.keys()))
    
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), constrained_layout=True)
    fig.suptitle(f"MPSC Throughput vs Producer Count (capacity={capacity})", fontsize=16)
    
    for payload, ax in zip(PAYLOADS, axes.flat):
        for i, queue in enumerate(queues):
            y = [values_tp.get((payload, pc, queue), float("nan")) / 1e6 for pc in producer_counts]
            ax.plot(producer_counts, y, marker="o", color=COLORS[i % len(COLORS)], label=queue, linewidth=2)
        
        ax.set_title(LABELS[payload])
        ax.set_xlabel("Producer Count")
        ax.set_ylabel("Mops/s")
        ax.grid(alpha=0.25)
        ax.set_xticks(producer_counts)
    
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=3, frameon=False)
    
    output_path = output_dir / f"{prefix}.scaling.throughput.svg"
    fig.savefig(output_path, dpi=180)
    plt.close(fig)
    
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), constrained_layout=True)
    fig.suptitle(f"MPSC Latency p99 vs Producer Count (capacity={capacity})", fontsize=16)
    
    for payload, ax in zip(PAYLOADS, axes.flat):
        for i, queue in enumerate(queues):
            y = [values_p99.get((payload, pc, queue), float("nan")) for pc in producer_counts]
            ax.plot(producer_counts, y, marker="o", color=COLORS[i % len(COLORS)], label=queue, linewidth=2)
        
        ax.set_title(LABELS[payload])
        ax.set_xlabel("Producer Count")
        ax.set_ylabel("p99 RTT (ns)")
        ax.grid(alpha=0.25)
        ax.set_xticks(producer_counts)
    
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=3, frameon=False)
    
    output_path = output_dir / f"{prefix}.scaling.latency-p99.svg"
    fig.savefig(output_path, dpi=180)
    plt.close(fig)


def generate_plots(csv_path: Path, output_dir: Optional[Path] = None):
    df = pd.read_csv(csv_path)
    
    if output_dir is None:
        date = csv_path.parent.name.split(".", 1)[0]
        experiment = csv_path.parent.name.split(".", 1)[1] if "." in csv_path.parent.name else "benchmark"
        root = csv_path.parent.parent.parent.parent
        output_dir = root / "svg" / f"{date}.{experiment}"
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    timestamp = csv_path.stem.split(".", 1)[0]
    experiment = csv_path.stem.split(".", 1)[1] if "." in csv_path.stem else "benchmark"
    prefix = f"{timestamp}.{experiment}"
    
    plot_capacity_comparison(df, output_dir, prefix)
    plot_producer_scaling(df, output_dir, prefix)
