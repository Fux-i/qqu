import unittest
from unittest.mock import patch

from lib.cli import cmd_run
from lib.config import BuildConfig, ExperimentConfig, load_experiment, validate_experiment


class TaskTests(unittest.TestCase):
    def test_test_configs(self):
        for suite in ("mpsc", "spsc"):
            name = f"{suite}_test"
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


if __name__ == "__main__":
    unittest.main()
