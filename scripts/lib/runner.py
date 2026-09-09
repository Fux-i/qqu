import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from .config import ExperimentConfig
from .system import build_cpuset, setup_performance_mode


@dataclass
class BenchmarkResult:
    csv_path: Path
    meta_path: Path


def build_target(target: str) -> Path:
    root = Path(__file__).parent.parent.parent
    build_dir = root / "build"
    
    subprocess.run(
        ["cmake", "-S", str(root), "-B", str(build_dir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        check=True,
        capture_output=True,
    )
    
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", target],
        check=True,
        capture_output=True,
    )
    
    return build_dir / target


def run_benchmark(config: ExperimentConfig, producer_count: int, output_dir: Path, timestamp: str) -> tuple[Path, list[str]]:
    binary = build_target(config.build.target)
    
    producer_cpus = config.system.producer_cpus[:producer_count]
    consumer_cpu = config.system.consumer_cpu
    cpus = build_cpuset(producer_cpus + [consumer_cpu])
    
    args = [
        str(binary),
        "--cpus", cpus,
    ]
    
    if config.benchmark.queues:
        args.extend(["--only", ",".join(config.benchmark.queues)])
    
    if len(config.benchmark.capacities) == 1:
        args.extend(["--capacity", str(config.benchmark.capacities[0])])
    
    if len(config.benchmark.element_types) == 1:
        args.extend(["--payload", config.benchmark.element_types[0]])
    
    args.extend(["-n", str(config.benchmark.messages)])
    args.extend(["-r", str(config.benchmark.runs)])
    
    result = subprocess.run(
        ["taskset", "-c", cpus] + args,
        capture_output=True,
        text=True,
        check=True,
    )
    
    csv_rows = []
    for line in result.stdout.splitlines():
        if line.startswith("raw "):
            fields = line[4:].split()
            row = {}
            for field in fields:
                if "=" in field:
                    k, v = field.split("=", 1)
                    row[k] = v
            row["producer_count"] = str(producer_count)
            csv_rows.append(row)
    
    meta_path = output_dir / f"{timestamp}.{config.name}.{producer_count}p.meta.txt"
    with meta_path.open("w") as f:
        f.write(f"experiment: {config.name}\n")
        f.write(f"producer_count: {producer_count}\n")
        f.write(f"cpus: {cpus}\n")
        f.write(f"timestamp: {timestamp}\n")
    
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
        
        with csv_path.open("w") as f:
            f.write(",".join(keys) + "\n")
            for row in all_rows:
                f.write(",".join(row.get(k, "") for k in keys) + "\n")
    
    return BenchmarkResult(csv_path=csv_path, meta_path=output_dir / f"{timestamp}.{config.name}.2p.meta.txt")
