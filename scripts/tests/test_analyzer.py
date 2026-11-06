import math
import unittest

import pandas as pd

from lib.analyzer import compute_summary, format_summary


def _row(queue, metric, run, value):
    row = {
        "queue": queue,
        "metric": metric,
        "producer_count": 2,
        "payload": "u32",
        "capacity": 1024,
        "producer_cpus": "2,4",
        "consumer_cpus": "10",
        "run": run,
        "msgs_per_s": float("nan"),
        "p50_ns": float("nan"),
        "p99_ns": float("nan"),
    }
    if metric == "throughput":
        row["msgs_per_s"] = value
    elif metric == "latency":
        row["p50_ns"] = value
        row["p99_ns"] = value + 40.0
    return row


class AnalyzerSummaryTests(unittest.TestCase):
    def setUp(self):
        rows = []
        for run, value in enumerate([10e6, 20e6, 30e6], start=1):
            rows.append(_row("qqu::mpsc", "throughput", run, value))
            rows.append(_row("rigtorp::MPMCQueue", "throughput", run, value * 2))
            rows.append(_row("qqu::mpsc", "latency", run, 100.0 + 10.0 * run))
            rows.append(_row("rigtorp::MPMCQueue", "latency", run, 120.0 + 10.0 * run))
        self.summary = compute_summary(pd.DataFrame(rows))
        self.ci = 4.303 * 10.0 / math.sqrt(3)

    def test_throughput_stats(self):
        mean, ci, low, high = self.summary[(2, "u32", 1024)]["throughput"]["qqu::mpsc"]
        self.assertAlmostEqual(mean, 20e6)
        self.assertAlmostEqual(ci, self.ci * 1e6, places=0)
        self.assertEqual((low, high), (10e6, 30e6))

    def test_latency_stats(self):
        mean, ci, low, high = self.summary[(2, "u32", 1024)]["latency_p50"]["qqu::mpsc"]
        self.assertAlmostEqual(mean, 120.0)
        self.assertAlmostEqual(ci, self.ci, places=3)
        self.assertEqual((low, high), (110.0, 130.0))

    def test_format_shows_error(self):
        text = format_summary(self.summary, "synthetic", "20260101")
        self.assertIn("±", text)
        self.assertIn("[10.0, 30.0]", text)


if __name__ == "__main__":
    unittest.main()
