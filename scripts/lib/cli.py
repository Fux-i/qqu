#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path
from typing import Optional

from .config import list_experiments, load_experiment, validate_experiment
from .runner import run_experiment


def cmd_list():
    experiments = list_experiments()
    if not experiments:
        print("No experiments found in experiments/")
        return
    print("Available experiments:")
    for exp in experiments:
        print(f"  {exp}")


def cmd_show(name: str):
    try:
        config = load_experiment(name)
        print(f"Experiment: {config.name}")
        print(f"Description: {config.description}")
        print(f"\nSystem:")
        print(f"  Performance mode: {config.system.performance_mode}")
        print(f"  Producer CPUs: {config.system.producer_cpus}")
        print(f"  Consumer CPU: {config.system.consumer_cpu}")
        print(f"\nMatrix:")
        print(f"  Producer counts: {config.matrix.producer_counts}")
        print(f"\nBenchmark:")
        print(f"  Element types: {config.benchmark.element_types}")
        print(f"  Capacities: {config.benchmark.capacities}")
        print(f"  Runs: {config.benchmark.runs}")
        print(f"  Messages: {config.benchmark.messages}")
        if config.benchmark.queues:
            print(f"  Queues: {config.benchmark.queues}")
        print(f"\nBuild:")
        print(f"  Target: {config.build.target}")
        print(f"\nOutput:")
        print(f"  Base: {config.output.base}")
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def cmd_validate(name: str):
    valid, msg = validate_experiment(name)
    if valid:
        print(f"✓ {name}: {msg}")
    else:
        print(f"✗ {name}: {msg}", file=sys.stderr)
        sys.exit(1)


def cmd_test(suite: str, args: list[str]):
    targets = {"mpsc_test": "qqu_test_mpsc", "spsc_test": "qqu_test_spsc"}
    target = targets.get(suite)
    if target is None:
        print(f"Unknown test suite: {suite}", file=sys.stderr)
        sys.exit(1)

    root = Path(__file__).parent.parent.parent
    build = root / "build"
    subprocess.run(
        ["cmake", "--build", str(build), "--target", target], check=True
    )
    subprocess.run([str(build / target), *args], check=True)


def cmd_run(name: str):
    from .analyzer import generate_summary
    from .plotter import generate_plots
    
    try:
        config = load_experiment(name)
        valid, msg = validate_experiment(name)
        if not valid:
            print(f"Error: {msg}", file=sys.stderr)
            sys.exit(1)
        
        print(f"==> Experiment: {config.name}")
        print(f"    {config.description}")
        print()
        
        result = run_experiment(config)
        
        print()
        print(f"==> Complete: benchmark saved")
        print(f"    CSV: {result.csv_path}")
        
        print()
        print("==> Generating summary...")
        root = Path(__file__).parent.parent.parent
        date = result.csv_path.parent.name.split(".", 1)[0]
        experiment = result.csv_path.parent.name.split(".", 1)[1] if "." in result.csv_path.parent.name else name
        txt_dir = root / config.output.base / "txt" / f"{date}.{experiment}"
        txt_dir.mkdir(parents=True, exist_ok=True)
        
        timestamp = result.csv_path.stem.split(".", 1)[0]
        summary_path = txt_dir / f"{timestamp}.{experiment}.summary.txt"
        summary_text = generate_summary(result.csv_path, summary_path)
        print(summary_text)
        print()
        print(f"    Summary: {summary_path}")
        
        print()
        print("==> Generating plots...")
        svg_dir = root / config.output.base / "svg" / f"{date}.{experiment}"
        generate_plots(result.csv_path, svg_dir)
        print(f"    Plots: {svg_dir}")
        
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)


def cmd_summary(name_filter: Optional[str] = None):
    from .analyzer import find_latest_result, generate_summary
    
    root = Path(__file__).parent.parent.parent
    results_base = root / "results"
    
    csv_path = find_latest_result(results_base, name_filter)
    if not csv_path:
        print("No results found", file=sys.stderr)
        sys.exit(1)
    
    print(f"==> Using: {csv_path}")
    print()
    
    summary_text = generate_summary(csv_path)
    print(summary_text)


def cmd_plot(name_filter: Optional[str] = None):
    from .plotter import generate_plots
    
    root = Path(__file__).parent.parent.parent
    results_base = root / "results"
    
    from .analyzer import find_latest_result
    csv_path = find_latest_result(results_base, name_filter)
    if not csv_path:
        print("No results found", file=sys.stderr)
        sys.exit(1)
    
    print(f"==> Plotting: {csv_path}")
    
    date = csv_path.parent.name.split(".", 1)[0]
    experiment = csv_path.parent.name.split(".", 1)[1] if "." in csv_path.parent.name else "benchmark"
    svg_dir = root / "results" / "svg" / f"{date}.{experiment}"
    
    generate_plots(csv_path, svg_dir)
    print(f"==> Saved plots to: {svg_dir}")


def main():
    if len(sys.argv) < 2:
        print("Usage: qqu <experiment> | list | show <experiment> | validate <experiment> | summary [filter] | plot [filter]", file=sys.stderr)
        sys.exit(1)
    
    cmd = sys.argv[1]
    
    if cmd == "list":
        cmd_list()
    elif cmd == "show":
        if len(sys.argv) < 3:
            print("Usage: qqu show <experiment>", file=sys.stderr)
            sys.exit(1)
        cmd_show(sys.argv[2])
    elif cmd == "validate":
        if len(sys.argv) < 3:
            print("Usage: qqu validate <experiment>", file=sys.stderr)
            sys.exit(1)
        cmd_validate(sys.argv[2])
    elif cmd == "summary":
        name_filter = sys.argv[2] if len(sys.argv) > 2 else None
        cmd_summary(name_filter)
    elif cmd == "plot":
        name_filter = sys.argv[2] if len(sys.argv) > 2 else None
        cmd_plot(name_filter)
    elif cmd in {"mpsc_test", "spsc_test"}:
        cmd_test(cmd, sys.argv[2:])
    else:
        cmd_run(cmd)


if __name__ == "__main__":
    main()
