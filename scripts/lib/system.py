import subprocess
from pathlib import Path


def setup_performance_mode() -> None:
    system_sh = Path(__file__).parent / "system.sh"
    subprocess.run(["bash", "-c", f"source {system_sh} && qqu_perf_init"], check=True)


def build_cpuset(cpus: list[int]) -> str:
    return ",".join(str(c) for c in cpus)
