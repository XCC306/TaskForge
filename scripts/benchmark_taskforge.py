"""Reproducible end-to-end CLI verification for TaskForge v0.3.0.

Standard library only. Does not edit C++ sources, change versions, or publish Git.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
import uuid
import wave
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

VERSION = "TaskForge v0.3.0"
ROOT = Path(__file__).resolve().parents[1]
CORE_FILES = (
    "CMakeLists.txt", "app/main.cpp", "include/taskforge/task.hpp",
    "include/taskforge/thread_pool.hpp", "include/taskforge/wav_analyzer.hpp",
    "src/thread_pool.cpp", "src/wav_analyzer.cpp",
)


class BenchmarkError(RuntimeError):
    pass


@dataclass(frozen=True)
class Case:
    name: str
    count: int
    rate: int
    frames: int
    channels: int

    @property
    def size_bytes(self):
        return self.count * (44 + self.frames * self.channels * 2)


def cases_for(suite):
    if suite == "smoke":
        return [Case("smoke", 4, 8000, 800, 2)]
    return [
        Case("tiny", 32, 16000, 1600, 1),
        Case("medium", 16, 48000, 480000, 1),
        Case("large_stereo", 8, 48000, 2880000, 2),
    ]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    # Exclusive run directory prevents replacing any previous benchmark output.
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def make_fixture(folder, case, index):
    """Periodic PCM16 gives independent expected metrics without running the CLI."""
    period = case.rate // 100
    if case.rate % 100 or case.channels not in (1, 2):
        raise BenchmarkError("Fixtures require a rate divisible by 100 and mono/stereo")
    samples = []
    for frame in range(period):
        for channel in range(case.channels):
            amplitude = 9000 + index * 137 + channel * 1700
            phase = 2 * math.pi * frame / period + channel * 0.37
            sample = round(amplitude * (0.80 * math.sin(phase) + 0.15 * math.sin(3 * phase)))
            if not -32768 <= sample <= 32767:
                raise BenchmarkError("Fixture would clip PCM16")
            samples.append(sample)
    cycle = struct.pack("<" + "h" * len(samples), *samples)
    cycles, remaining = divmod(case.frames, period)
    path = folder / f"input_{index:03d}.wav"
    with wave.open(str(path), "wb") as output:
        output.setnchannels(case.channels)
        output.setsampwidth(2)
        output.setframerate(case.rate)
        chunk = cycle * 128
        whole_chunks, tail_cycles = divmod(cycles, 128)
        for _ in range(whole_chunks):
            output.writeframesraw(chunk)
        output.writeframesraw(cycle * tail_cycles)
        output.writeframesraw(cycle[:remaining * case.channels * 2])

    normalized = [sample / 32768.0 for sample in samples]
    tail = normalized[:remaining * case.channels]
    energy = math.fsum(value * value for value in normalized) * cycles
    energy += math.fsum(value * value for value in tail)
    present = normalized if cycles else tail
    expected = {
        "sample_rate_hz": case.rate, "channels": case.channels, "frames": case.frames,
        "duration_s": case.frames / case.rate,
        "peak": max(abs(value) for value in present),
        "rms": math.sqrt(energy / (case.frames * case.channels)), "energy": energy,
    }
    return {
        "path": str(path), "size_bytes": path.stat().st_size,
        "sha256": sha256(path), "expected": expected,
    }


def parse_fields(line, expected_keys):
    fields = line.strip().split()
    if len(fields) != len(expected_keys):
        raise BenchmarkError("Missing, duplicate or extra output fields")
    pairs = [field.split("=", 1) for field in fields]
    if any(len(pair) != 2 for pair in pairs):
        raise BenchmarkError("Malformed output field")
    result = dict(pairs)
    if len(result) != len(fields) or set(result) != set(expected_keys):
        raise BenchmarkError("Unexpected output field names")
    return result


def validate_output(text, fixtures, mode, workers):
    lines = text.splitlines()
    header = f"{VERSION} mode={mode} workers={workers} files={len(fixtures)}"
    if not lines or lines[0] != header or len(lines) != 1 + len(fixtures) * 3:
        raise BenchmarkError("CLI header, version or result count mismatch")
    records = []
    for index, fixture in enumerate(fixtures):
        if lines[index * 3 + 1] != fixture["path"]:
            raise BenchmarkError("File path or input-order mismatch")
        integer_fields = parse_fields(
            lines[index * 3 + 2], ("sample_rate_hz", "channels", "frames"))
        float_fields = parse_fields(
            lines[index * 3 + 3], ("duration_s", "peak", "rms", "energy"))
        try:
            actual = {key: int(value) for key, value in integer_fields.items()}
            actual.update({key: float(value) for key, value in float_fields.items()})
        except ValueError as error:
            raise BenchmarkError("Non-numeric CLI metric") from error
        for key, expected in fixture["expected"].items():
            value = actual[key]
            if key in integer_fields:
                correct = value == expected
            else:
                # CLI prints six decimals; long sequential energy sums also round.
                tolerance = max(2e-6, abs(expected) * 1e-9) if key == "energy" else 1e-6
                correct = math.isfinite(value) and math.isclose(
                    value, expected, rel_tol=0.0, abs_tol=tolerance)
            if not correct:
                raise BenchmarkError(f"Wrong {key} for {fixture['path']}: {value} vs {expected}")
        records.append(actual)
    return hashlib.sha256(
        json.dumps(records, sort_keys=True, allow_nan=False).encode("utf-8")
    ).hexdigest()


def summarize(records, fixtures, modes):
    rows = []
    measured = [row for row in records if row["phase"] == "measured"]
    serial_times = [row["elapsed_ns"] / 1e9 for row in measured if row["mode"] == "serial"]
    if not serial_times:
        raise BenchmarkError("No measured serial runs")
    serial_median = statistics.median(serial_times)
    size_mib = sum(item["size_bytes"] for item in fixtures) / (1024 * 1024)
    for mode, workers in modes:
        times = [row["elapsed_ns"] / 1e9 for row in measured
                 if row["mode"] == mode and row["workers"] == workers]
        if not times or min(times) <= 0:
            raise BenchmarkError("Missing or invalid measured timings")
        median = statistics.median(times)
        rows.append({
            "mode": mode, "workers": workers, "repetitions": len(times),
            "median_ms": median * 1000, "min_ms": min(times) * 1000,
            "max_ms": max(times) * 1000, "input_mib": size_mib,
            "throughput_mib_s": size_mib / median,
            "speedup_vs_serial": serial_median / median,
        })
    return rows


def measurement_order(modes, repetition, case_index):
    offset = (repetition + case_index) % len(modes)
    return modes[offset:] + modes[:offset]


def optional_command(command, cwd):
    try:
        result = subprocess.run(
            command, cwd=str(cwd), capture_output=True, timeout=10, check=False)
        return {
            "returncode": result.returncode,
            "stdout": result.stdout.decode("utf-8", errors="replace").strip(),
            "stderr": result.stderr.decode("utf-8", errors="replace").strip(),
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"unavailable": str(error)}


def source_hashes(root):
    return {name: sha256(root / name) for name in CORE_FILES if (root / name).is_file()}


def configuration_info(exe):
    build = exe.parent.parent if exe.parent.name.lower() in (
        "debug", "release", "relwithdebinfo", "minsizerel") else exe.parent
    info = {"build_directory": str(build), "configuration": None}
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.match(
                r"(CMAKE_BUILD_TYPE|CMAKE_GENERATOR|CMAKE_CXX_COMPILER):[^=]*=(.*)", line)
            if match:
                info[match.group(1)] = match.group(2)
        info["compiler_records"] = []
        for compiler in sorted((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
            content = compiler.read_text(encoding="utf-8", errors="replace")
            record = {"metadata_file": str(compiler)}
            for key in ("CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
                match = re.search(r'set\(' + key + r' "([^"]*)"\)', content)
                if match:
                    record[key] = match.group(1)
            info["compiler_records"].append(record)
    if exe.parent.name.lower() in ("debug", "release", "relwithdebinfo", "minsizerel"):
        info["configuration"] = exe.parent.name
    else:
        info["configuration"] = info.get("CMAKE_BUILD_TYPE")
    return info


def environment(root, exe, build):
    value = {
        "os": platform.platform(), "machine": platform.machine(),
        "processor": platform.processor(), "logical_cpu_count": os.cpu_count(),
        "python": sys.version, "executable": str(exe), "binary_sha256": sha256(exe),
        "build": build, "core_source_sha256": source_hashes(root),
        "benchmark_script_sha256": sha256(Path(__file__)),
        "clock": {
            "implementation": time.get_clock_info("perf_counter").implementation,
            "resolution_seconds": time.get_clock_info("perf_counter").resolution,
            "monotonic": time.get_clock_info("perf_counter").monotonic,
        },
        "git_head": optional_command(["git", "rev-parse", "HEAD"], root),
        "git_v030": optional_command(["git", "rev-parse", "v0.3.0^{}"], root),
        "git_status": optional_command(["git", "status", "--short"], root),
    }
    if os.name == "nt":
        command = (
            "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); "
            "$ErrorActionPreference='Stop'; "
            "$cpu=@(Get-CimInstance Win32_Processor | "
            "Select-Object Name,NumberOfCores,NumberOfLogicalProcessors); "
            "$ram=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory; "
            "@{cpu=$cpu;ram_bytes=$ram}|ConvertTo-Json -Depth 4 -Compress"
        )
        result = optional_command(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command], root)
        try:
            value["windows_hardware"] = json.loads(result["stdout"])
        except (KeyError, ValueError):
            value["windows_hardware"] = result
    return value


def execute_one(exe, fixtures, mode, workers, timeout, log_stem):
    arguments = [str(exe)]
    arguments.extend(["--serial"] if mode == "serial" else ["--threads", str(workers)])
    arguments.append("--")
    arguments.extend(item["path"] for item in fixtures)
    started = time.perf_counter_ns()
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, check=False)
    elapsed = time.perf_counter_ns() - started
    # Timing ends before Python decodes and validates any output.
    log_stem.with_suffix(".stdout.txt").write_bytes(result.stdout)
    log_stem.with_suffix(".stderr.txt").write_bytes(result.stderr)
    if result.returncode != 0:
        raise BenchmarkError(f"CLI exited with {result.returncode}; see {log_stem}.stderr.txt")
    if result.stderr:
        raise BenchmarkError(f"Unexpected CLI stderr; see {log_stem}.stderr.txt")
    text = result.stdout.decode("utf-8", errors="strict")
    fingerprint = validate_output(text, fixtures, mode, workers)
    return {"elapsed_ns": elapsed, "output_fingerprint": fingerprint}


def workers_argument(text):
    try:
        values = [int(item) for item in text.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("workers must be comma-separated integers") from error
    if not values or len(values) != len(set(values)) or any(n < 1 or n > 64 for n in values):
        raise argparse.ArgumentTypeError("workers must be distinct integers from 1 to 64")
    return values


def parser():
    value = argparse.ArgumentParser(description=__doc__)
    default_exe = ROOT / "build" / "Release" / "taskforge.exe" if os.name == "nt" else ROOT / "build" / "taskforge"
    value.add_argument("--exe", type=Path, default=default_exe)
    value.add_argument("--suite", choices=("standard", "smoke"), default="standard")
    value.add_argument("--workers", type=workers_argument, default=workers_argument("1,2,4"))
    value.add_argument("--repeats", type=int, default=5)
    value.add_argument("--warmups", type=int, default=1)
    value.add_argument("--timeout-seconds", type=int, default=30)
    value.add_argument("--output-root", type=Path, default=ROOT / "build" / "verification")
    return value


def run(args):
    exe = args.exe.resolve()
    if not exe.is_file():
        raise BenchmarkError(f"Executable not found: {exe}")
    build = configuration_info(exe)
    if str(build["configuration"]).lower() != "release":
        raise BenchmarkError("Use a CMake Release executable; could not confirm Release configuration")
    version = subprocess.run([str(exe), "--version"], capture_output=True,
                             timeout=10, check=False)
    if version.returncode or version.stdout.decode("utf-8").strip() != VERSION:
        raise BenchmarkError("Expected TaskForge v0.3.0; clean-build Release first")
    if len(source_hashes(ROOT)) != len(CORE_FILES):
        raise BenchmarkError("Copy this toolkit into the TaskForge project root before running")
    cases = cases_for(args.suite)
    required = sum(case.size_bytes for case in cases) + 20 * 1024 * 1024
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output_root).free < required:
        raise BenchmarkError(f"Not enough disk space; need at least {required} bytes")
    run_dir = output_root / (
        "run-" + datetime.now().astimezone().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    run_dir.mkdir()
    logs = run_dir / "logs"
    logs.mkdir()
    modes = [("serial", 1)] + [("parallel", workers) for workers in args.workers]
    state = {
        "status": "running", "started_utc": datetime.now(timezone.utc).isoformat(),
        "tool_schema": 1, "suite": args.suite, "warmups": args.warmups, "repeats": args.repeats,
        "modes": [{"mode": mode, "workers": n} for mode, n in modes],
        "scope": "CLI process wall time including startup, thread creation, file I/O, analysis, captured output and exit",
        "cache_policy": "fixtures just written and hashed; each mode warmed; OS cache not flushed",
        "accuracy": "independent fixture checks plus exact equivalence at CLI printed precision",
        "environment": environment(ROOT, exe, build),
        "cases": [], "summary": [],
    }
    write_json(run_dir / "results.json", state)
    print(f"Output: {run_dir}", flush=True)
    print("Scope: complete CLI batches; no CPU-only or hard-real-time claim.", flush=True)
    try:
        with (run_dir / "runs.jsonl").open("w", encoding="utf-8") as raw:
            for case_index, case in enumerate(cases):
                folder = run_dir / "fixtures" / case.name
                folder.mkdir(parents=True)
                fixtures = [make_fixture(folder, case, index) for index in range(case.count)]
                case_record = {
                    "name": case.name, "count": case.count, "sample_rate_hz": case.rate,
                    "channels": case.channels, "frames_per_file": case.frames,
                    "duration_seconds_per_file": case.frames / case.rate,
                    "files": fixtures, "runs": [],
                }
                state["cases"].append(case_record)
                baseline = None
                print(f"Case {case.name}: {case.count} files, {case.size_bytes / 1048576:.2f} MiB", flush=True)
                for phase, rounds in (("warmup", args.warmups), ("measured", args.repeats)):
                    for repetition in range(rounds):
                        # Balanced rotations reduce always-running-serial-first bias.
                        order = measurement_order(modes, repetition, case_index)
                        for mode, workers in order:
                            label = f"{case.name}_{phase}_{repetition + 1}_{mode}_{workers}"
                            row = execute_one(exe, fixtures, mode, workers, args.timeout_seconds, logs / label)
                            row.update(case=case.name, phase=phase, repetition=repetition + 1,
                                       mode=mode, workers=workers, correct=True)
                            if baseline is None:
                                baseline = row["output_fingerprint"]
                            if row["output_fingerprint"] != baseline:
                                raise BenchmarkError("Modes/repetitions disagree at printed metric precision")
                            case_record["runs"].append(row)
                            raw.write(json.dumps(row, allow_nan=False) + "\n")
                            raw.flush()
                        print(f"  {phase} {repetition + 1}/{rounds}: metrics OK", flush=True)
                for row in summarize(case_record["runs"], fixtures, modes):
                    state["summary"].append(dict(case=case.name, **row))
                write_json(run_dir / "results.json", state)
        if sha256(exe) != state["environment"]["binary_sha256"]:
            raise BenchmarkError("Executable changed during measurement")
        if source_hashes(ROOT) != state["environment"]["core_source_sha256"]:
            raise BenchmarkError("Core sources changed during measurement")
        if sha256(Path(__file__)) != state["environment"]["benchmark_script_sha256"]:
            raise BenchmarkError("Benchmark script changed during measurement")
        for case_record in state["cases"]:
            for fixture in case_record["files"]:
                if sha256(Path(fixture["path"])) != fixture["sha256"]:
                    raise BenchmarkError("Generated fixture changed during measurement")
        state["status"] = "passed"
        state["completed_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(run_dir / "results.json", state)
        with (run_dir / "summary.csv").open("w", encoding="utf-8-sig", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=list(state["summary"][0]))
            writer.writeheader()
            writer.writerows(state["summary"])
        lines = [
            "# TaskForge verification measurements",
            "",
            "These are end-to-end CLI measurements on this machine, not universal speedup claims.",
            "",
            "| Case | Mode | Workers | Median ms | Min ms | Max ms | MiB/s | Speedup |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for row in state["summary"]:
            print(f"{row['case']:14} {row['mode']:8} workers={row['workers']:2} "
                  f"median_ms={row['median_ms']:.3f} speedup={row['speedup_vs_serial']:.3f}x")
            lines.append(
                f"| {row['case']} | {row['mode']} | {row['workers']} | {row['median_ms']:.3f} | "
                f"{row['min_ms']:.3f} | {row['max_ms']:.3f} | {row['throughput_mib_s']:.3f} | "
                f"{row['speedup_vs_serial']:.3f}x |")
        lines += [
            "", "Warmups are excluded. OS caches were not flushed. Each measured result was checked.",
            "Speedup = serial median / selected-mode median; values below 1 indicate a slowdown.",
            "Scope includes startup, I/O, analysis, stdout capture and thread shutdown.",
            "See results.json, runs.jsonl, fixture hashes and per-run logs for evidence.",
        ]
        (run_dir / "SUMMARY.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("Verification benchmark: OK", flush=True)
        return run_dir
    except BaseException as error:
        state["status"] = "failed"
        state["error"] = f"{type(error).__name__}: {error}"
        write_json(run_dir / "results.json", state)
        print(f"FAILED run evidence: {run_dir}", file=sys.stderr, flush=True)
        raise


def main():
    argument_parser = parser()
    args = argument_parser.parse_args()
    if not 1 <= args.repeats <= 50 or not 1 <= args.warmups <= 10:
        argument_parser.error("repeats must be 1..50 and warmups must be 1..10")
    if not 1 <= args.timeout_seconds <= 60:
        argument_parser.error("timeout-seconds must be 1..60")
    try:
        run(args)
        return 0
    except (BenchmarkError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Verification benchmark failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Verification benchmark interrupted; see failed run evidence.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
