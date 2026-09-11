import io
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import yaml

from lib.cli import cmd_run, cmd_show
from lib.config import BuildConfig, ExperimentConfig, load_experiment, validate_experiment
from lib.runner import run_benchmark


class TaskTests(unittest.TestCase):
    def test_test_configs(self):
        for suite in ("mpsc", "spsc"):
            name = f"test_{suite}"
            config = load_experiment(name)
            self.assertEqual(config.build.target, f"qqu_test_{suite}")
            self.assertIsNone(config.benchmark)
            self.assertEqual(validate_experiment(name), (True, "valid"))

    def test_build_target_selects_tests(self):
        config = ExperimentConfig(
            "custom", "Test", BuildConfig("qqu_test_spsc"),
            args=["--gtest_repeat=2"],
        )
        with (
            patch("lib.cli.load_experiment", return_value=config),
            patch("lib.cli.validate_experiment", return_value=(True, "valid")),
            patch("lib.cli.build_target", return_value="/tmp/qqu_test_spsc"),
            patch("lib.cli.subprocess.run") as run,
            patch("lib.cli.run_experiment") as benchmark,
        ):
            cmd_run("custom", ["--gtest_filter=qqu_test.empty_try_pop"])
        run.assert_called_once_with(
            ["/tmp/qqu_test_spsc", "--gtest_repeat=2",
             "--gtest_filter=qqu_test.empty_try_pop"], check=True,
        )
        benchmark.assert_not_called()


class QueueSelectionTests(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        (self.root / "tasks").mkdir()
        self.data = yaml.safe_load(
            (Path(__file__).resolve().parents[2] / "tasks/spsc_baseline.yaml").read_text()
        )
        self.data["benchmark"].pop("queues", None)
        self.groups = {
            "qqu_bench_mpsc": {"default": ["qqu::mpsc"]},
            "qqu_bench_spsc": {"default": ["qqu::spsc"]},
        }
        patcher = patch("lib.config.__file__", str(self.root / "scripts/lib/config.py"))
        patcher.start()
        self.addCleanup(patcher.stop)

    def load(self):
        (self.root / "tasks/selection.yaml").write_text(yaml.safe_dump(self.data))
        (self.root / "queue_groups.yaml").write_text(yaml.safe_dump(self.groups))
        return load_experiment("selection")

    def test_empty_selection_runs_all(self):
        for selection in ({}, {"queues": None}, {"queues": []}):
            with self.subTest(selection=selection):
                self.data["benchmark"].update(selection)
                self.assertFalse(self.load().benchmark.queues)

    def test_group_is_scoped_by_target(self):
        self.data["benchmark"]["queues"] = "default"
        for target, groups in self.groups.items():
            with self.subTest(target=target):
                self.data["build"]["target"] = target
                self.assertEqual(self.load().benchmark.queues, groups["default"])

    def test_explicit_names_are_preserved(self):
        names = ["qqu::spsc", "rigtorp::SPSCQueue"]
        self.data["benchmark"]["queues"] = names
        self.assertEqual(self.load().benchmark.queues, names)

    def test_runner_receives_resolved_selection(self):
        for selection in (None, [], "default", ["qqu::spsc", "rigtorp::SPSCQueue"]):
            with self.subTest(selection=selection):
                self.data["benchmark"]["queues"] = selection
                config = self.load()
                with (
                    patch("lib.runner.build_target", return_value=self.root / config.build.target),
                    patch("lib.runner.subprocess.run") as run,
                ):
                    run.return_value.stdout = ""
                    run_benchmark(config, 1, self.root, "test")
                args = run.call_args.args[0]
                if selection:
                    self.assertEqual(args[args.index("--only") + 1], ",".join(config.benchmark.queues))
                else:
                    self.assertNotIn("--only", args)

    def test_show_displays_selection(self):
        for selection in (None, "default"):
            with self.subTest(selection=selection):
                self.data["benchmark"]["queues"] = selection
                config = self.load()
                with redirect_stdout(io.StringIO()) as output:
                    cmd_show("selection")
                self.assertIn(f"Queues: {config.benchmark.queues or 'all'}", output.getvalue())

    def test_unknown_or_invalid_group_fails(self):
        self.data["benchmark"]["queues"] = "missing"
        with self.assertRaisesRegex(ValueError, "missing.*qqu_bench_spsc"):
            self.load()
        self.assertFalse(validate_experiment("selection")[0])
        for command in (lambda: cmd_show("selection"), lambda: cmd_run("selection", [])):
            with redirect_stderr(io.StringIO()) as output, self.assertRaises(SystemExit) as error:
                command()
            self.assertEqual(error.exception.code, 1)
            self.assertIn("missing", output.getvalue())
            self.assertNotIn("Traceback", output.getvalue())
        self.data["benchmark"]["queues"] = "default"
        for queues in ([], None, "another_group", [None], [""], [["qqu::spsc"]]):
            with self.subTest(queues=queues):
                self.groups["qqu_bench_spsc"]["default"] = queues
                with self.assertRaisesRegex(ValueError, "default.*qqu_bench_spsc"):
                    self.load()


if __name__ == "__main__":
    unittest.main()
