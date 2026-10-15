"""Standard-library tests for the measurement tool; C++ CTest count stays eight."""
from __future__ import annotations

import math
import io
from contextlib import redirect_stdout, redirect_stderr
import struct
import sys
import tempfile
import argparse
import subprocess
from unittest import mock
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark_taskforge as bench


def cli_text(fixtures, mode="serial", workers=1):
    lines = [f"{bench.VERSION} mode={mode} workers={workers} files={len(fixtures)}"]
    for fixture in fixtures:
        v = fixture["expected"]
        lines.extend([
            fixture["path"],
            f"  sample_rate_hz={v['sample_rate_hz']} channels={v['channels']} frames={v['frames']}",
            f"  duration_s={v['duration_s']:.6f} peak={v['peak']:.6f} "
            f"rms={v['rms']:.6f} energy={v['energy']:.6f}",
        ])
    return "\n".join(lines) + "\n"


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="taskforge-tool-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "含空格 中文"
        self.root.mkdir()
        self.case = bench.Case("fixture", 2, 8000, 833, 2)
        self.fixtures = [bench.make_fixture(self.root, self.case, i) for i in range(2)]

    def test_generated_wav_matches_independent_full_sample_decode(self):
        for item in self.fixtures:
            with wave.open(item["path"], "rb") as source:
                self.assertEqual(source.getnframes(), 833)
                self.assertEqual(source.getnchannels(), 2)
                self.assertEqual(source.getframerate(), 8000)
                data = source.readframes(source.getnframes())
            samples = [value[0] / 32768.0 for value in struct.iter_unpack("<h", data)]
            energy = math.fsum(value * value for value in samples)
            self.assertAlmostEqual(energy, item["expected"]["energy"], places=9)
            self.assertAlmostEqual(max(map(abs, samples)), item["expected"]["peak"], places=9)
            self.assertAlmostEqual(math.sqrt(energy / len(samples)), item["expected"]["rms"], places=9)
            self.assertEqual(item["size_bytes"], 44 + 833 * 2 * 2)

    def test_generation_is_repeatable_and_inputs_are_distinct(self):
        again = bench.make_fixture(self.root, self.case, 0)
        self.assertEqual(again["sha256"], self.fixtures[0]["sha256"])
        self.assertNotEqual(self.fixtures[0]["sha256"], self.fixtures[1]["sha256"])

    def test_parse_unicode_paths_and_mode_equivalence(self):
        left = bench.validate_output(cli_text(self.fixtures), self.fixtures, "serial", 1)
        right = bench.validate_output(cli_text(self.fixtures, "parallel", 4),
                                      self.fixtures, "parallel", 4)
        self.assertEqual(left, right)

    def test_reject_wrong_metrics_header_order_count_and_nonfinite(self):
        original = cli_text(self.fixtures)
        wrong = original.replace(
            f"rms={self.fixtures[0]['expected']['rms']:.6f}", "rms=9.000000", 1)
        reversed_files = cli_text(list(reversed(self.fixtures)))
        missing = "\n".join(original.splitlines()[:-1])
        bad_version = original.replace("v0.3.0", "v9.9.9", 1)
        nan = original.replace(
            f"rms={self.fixtures[0]['expected']['rms']:.6f}", "rms=nan", 1)
        for value in (wrong, reversed_files, missing, bad_version, nan):
            with self.subTest(value=value[:80]):
                with self.assertRaises(bench.BenchmarkError):
                    bench.validate_output(value, self.fixtures, "serial", 1)

    def test_reject_duplicate_missing_extra_fields(self):
        for line in ("frames=1 frames=1 channels=2", "frames=1",
                     "sample_rate_hz=1 frames=1 channels=2 extra=3"):
            with self.assertRaises(bench.BenchmarkError):
                bench.parse_fields(line, ("sample_rate_hz", "channels", "frames"))

    def test_summary_excludes_warmups_and_uses_median(self):
        rows = [
            {"phase": "warmup", "mode": "serial", "workers": 1, "elapsed_ns": 999e9},
            *[{"phase": "measured", "mode": "serial", "workers": 1, "elapsed_ns": n * 1e9}
              for n in (1, 2, 100)],
            *[{"phase": "measured", "mode": "parallel", "workers": 2, "elapsed_ns": n * 1e9}
              for n in (0.25, 0.5, 50)],
        ]
        summary = bench.summarize(rows, self.fixtures, [("serial", 1), ("parallel", 2)])
        self.assertEqual(summary[0]["median_ms"], 2000)
        self.assertEqual(summary[1]["median_ms"], 500)
        self.assertEqual(summary[1]["speedup_vs_serial"], 4)
        self.assertEqual(summary[1]["repetitions"], 3)
        self.assertAlmostEqual(summary[1]["throughput_mib_s"],
                               sum(v["size_bytes"] for v in self.fixtures) / 1048576 / 0.5)

    def test_rotations_balance_every_position(self):
        modes = [("serial", 1), ("parallel", 1), ("parallel", 2), ("parallel", 4)]
        orders = [bench.measurement_order(modes, i, 0) for i in range(4)]
        for mode in modes:
            self.assertEqual(sorted(order.index(mode) for order in orders), [0, 1, 2, 3])

    def test_configuration_multiconfig_and_singleconfig(self):
        build = self.root / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_GENERATOR:INTERNAL=Unix Makefiles\n",
            encoding="utf-8")
        self.assertEqual(bench.configuration_info(build / "taskforge")["configuration"], "Release")
        self.assertEqual(bench.configuration_info(build / "Debug" / "taskforge.exe")["configuration"], "Debug")

    def test_invalid_worker_lists_rejected(self):
        for value in ("0", "1,1", "abc", "65", "1,,2"):
            with self.assertRaises(argparse.ArgumentTypeError):
                bench.workers_argument(value)
        self.assertEqual(bench.workers_argument("1,2,4"), [1, 2, 4])

    def test_nonzero_exit_never_accepted_as_success(self):
        completed = subprocess.CompletedProcess(
            args=[], returncode=1, stdout=cli_text(self.fixtures).encode(), stderr=b"failed")
        with mock.patch.object(bench.subprocess, "run", return_value=completed):
            with self.assertRaises(bench.BenchmarkError):
                bench.execute_one(self.root / "fake.exe", self.fixtures, "serial", 1,
                                  1, self.root / "failed_process")

    def test_failed_measurement_has_evidence_and_no_summary_csv(self):
        project = self.root / "project"
        for name in bench.CORE_FILES:
            path = project / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("test fixture", encoding="utf-8")
        exe = project / "build" / "Release" / "taskforge.exe"
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"not executed")
        args = argparse.Namespace(
            exe=exe, suite="smoke", workers=[1], repeats=1, warmups=1,
            timeout_seconds=1, output_root=project / "build" / "verification")
        version = subprocess.CompletedProcess([], 0, (bench.VERSION + chr(10)).encode(), b"")
        with (
            mock.patch.object(bench, "ROOT", project),
            mock.patch.object(bench.subprocess, "run", return_value=version),
            mock.patch.object(bench, "environment", return_value={}),
            mock.patch.object(bench, "execute_one", side_effect=bench.BenchmarkError("injected failure")),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            with self.assertRaises(bench.BenchmarkError):
                bench.run(args)
        run = next(args.output_root.glob("run-*"))
        import json
        self.assertEqual(json.loads((run / "results.json").read_text(encoding="utf-8"))["status"], "failed")
        self.assertFalse((run / "summary.csv").exists())

    def test_standard_suite_bounded_and_smoke_small(self):
        standard = bench.cases_for("standard")
        self.assertEqual(len(standard), 3)
        self.assertLess(sum(case.size_bytes for case in standard), 110 * 1024 * 1024)
        self.assertLess(sum(case.size_bytes for case in bench.cases_for("smoke")), 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
