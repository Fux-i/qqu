from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml


@dataclass
class SystemConfig:
    performance_mode: bool
    producer_cpus: list[int]
    consumer_cpu: int


@dataclass
class MatrixConfig:
    producer_counts: list[int]


@dataclass
class BenchmarkConfig:
    element_types: list[str]
    capacities: list[int]
    runs: int
    messages: int
    queues: Optional[list[str]] = None


@dataclass
class OutputConfig:
    base: str


@dataclass
class BuildConfig:
    target: str


@dataclass
class ExperimentConfig:
    name: str
    description: str
    build: BuildConfig
    system: Optional[SystemConfig] = None
    matrix: Optional[MatrixConfig] = None
    benchmark: Optional[BenchmarkConfig] = None
    output: Optional[OutputConfig] = None
    args: list[str] = field(default_factory=list)


def load_experiment(name: str) -> ExperimentConfig:
    exp_dir = Path(__file__).parent.parent.parent / "tasks"
    yaml_path = exp_dir / f"{name}.yaml"
    
    if not yaml_path.exists():
        raise FileNotFoundError(f"Task not found: {yaml_path}")
    
    with yaml_path.open() as f:
        data = yaml.safe_load(f)
    
    if data["build"]["target"].startswith("qqu_test_"):
        return ExperimentConfig(
            name=data["name"],
            description=data["description"],
            build=BuildConfig(**data["build"]),
            args=data.get("args", []),
        )

    return ExperimentConfig(
        name=data["name"],
        description=data["description"],
        system=SystemConfig(**data["system"]),
        build=BuildConfig(**data["build"]),
        matrix=MatrixConfig(**data["matrix"]),
        benchmark=BenchmarkConfig(**data["benchmark"]),
        output=OutputConfig(**data["output"]),
    )


def list_tasks() -> list[str]:
    exp_dir = Path(__file__).parent.parent.parent / "tasks"
    if not exp_dir.exists():
        return []
    return sorted(p.stem for p in exp_dir.glob("*.yaml"))


def validate_experiment(name: str) -> tuple[bool, str]:
    try:
        config = load_experiment(name)
        if config.build.target.startswith("qqu_test_"):
            return True, "valid"
        
        if not config.system.producer_cpus:
            return False, "system.producer_cpus cannot be empty"
        
        if not config.matrix.producer_counts:
            return False, "matrix.producer_counts cannot be empty"
        
        if max(config.matrix.producer_counts) > len(config.system.producer_cpus):
            return False, f"max producer_count ({max(config.matrix.producer_counts)}) exceeds available CPUs ({len(config.system.producer_cpus)})"
        
        valid_types = {"u32", "u64", "p16", "p64"}
        for et in config.benchmark.element_types:
            if et not in valid_types:
                return False, f"invalid element_type: {et}"
        
        return True, "valid"
    except Exception as e:
        return False, str(e)
