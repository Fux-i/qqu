import csv
import os
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from .config import ExperimentConfig
from .system import build_cpuset, setup_performance_mode


@dataclass
class BenchmarkResult:
    csv_path: Path
    meta_path: Path


def run_checked(command: list[str]) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(command, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stdout or "")
        sys.stderr.write(error.stderr or "")
        raise


def build_target(target: str) -> Path:
    root = Path(__file__).parent.parent.parent
    build_dir = Path(os.environ.get("QQU_BUILD_DIR", str(root / "build")))

    run_checked(
        [
            "cmake", "-S", str(root), "-B", str(build_dir), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
        ]
    )
    run_checked(["cmake", "--build", str(build_dir), "--target", target])
    return build_dir / target


def run_benchmark(
    config: ExperimentConfig,
    producer_count: int,
    output_dir: Path,
    timestamp: str,
) -> tuple[Path, list[dict]]:
    binary = build_target(config.build.target)

    producer_cpus = config.system.producer_cpus[:producer_count]
    consumer_cpus = config.system.consumer_cpus or [config.system.consumer_cpu]
    cpu_list = build_cpuset(producer_cpus + consumer_cpus)
    cpu_spec = f"{build_cpuset(producer_cpus)}|{build_cpuset(consumer_cpus)}"

    args = [
        str(binary),
        "--cpus", cpu_spec,
        "-n", str(config.benchmark.messages),
        "-r", str(config.benchmark.runs),
    ]
    if config.benchmark.queues:
        args.extend(["--only", ",".join(config.benchmark.queues)])

    csv_rows = []
    for capacity in config.benchmark.capacities:
        for payload in config.benchmark.element_types:
            result = run_checked(
                [
                    "taskset", "-c", cpu_list, *args,
                    "--payload", payload,
                    "--capacity", str(capacity),
                ]
            )
            for line in result.stdout.splitlines():
                if not line.startswith("raw "):
                    continue
                row = {}
                for field in line[4:].split():
                    if "=" in field:
                        key, value = field.split("=", 1)
                        row[key] = value
                row["producer_count"] = str(producer_count)
                row["consumer_count"] = str(len(consumer_cpus))
                row["producer_cpus"] = ",".join(map(str, producer_cpus))
                row["consumer_cpus"] = ",".join(map(str, consumer_cpus))
                csv_rows.append(row)

    meta_path = output_dir / f"{timestamp}.{config.name}.{producer_count}p.meta.txt"
    with meta_path.open("w") as file:
        file.write(f"experiment: {config.name}\n")
        file.write(f"producer_count: {producer_count}\n")
        file.write(f"cpus: {cpu_spec}\n")
        file.write(f"timestamp: {timestamp}\n")

    return meta_path, csv_rows


def run_experiment(config: ExperimentConfig) -> BenchmarkResult:
    root = Path(__file__).parent.parent.parent
    date_stamp = datetime.now().strftime("%Y%m%d")
    timestamp = datetime.now().strftime("%H%M%S")
    output_dir = root / config.output.base / "raw" / f"{date_stamp}.{config.name}"
    output_dir.mkdir(parents=True, exist_ok=True)

    if config.system.performance_mode:
        setup_performance_mode()

    all_rows = []
    meta_path = output_dir
    for producer_count in config.matrix.producer_counts:
        print(f"==> Running {config.name} with {producer_count} producers...")
        meta_path, rows = run_benchmark(config, producer_count, output_dir, timestamp)
        print(f"    Saved: {meta_path.name}")
        all_rows.extend(rows)

    csv_path = output_dir / f"{timestamp}.{config.name}.bench.csv"
    if all_rows:
        all_keys = set()
        for row in all_rows:
            all_keys.update(row.keys())
        keys = sorted(all_keys)

        with csv_path.open("w") as file:
            writer = csv.DictWriter(file, fieldnames=keys)
            writer.writeheader()
            writer.writerows(all_rows)

    return BenchmarkResult(csv_path=csv_path, meta_path=meta_path)
