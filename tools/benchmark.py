#!/usr/bin/env python3
"""Build and measure FastPage on the verified 12-file Silesia corpus.

Each round runs in its own process pinned to one CPU. It includes one complete
corpus sample in each direction, with an untimed warmup and calibration before
each sample. Every sample is retained. Existing output directories are refused.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import sys

from fetch_corpus import EXPECTED, SIZES, URL, verify_corpus

ROOT = Path(__file__).resolve().parent.parent
BUILD_TARGETS = ("build/libfastpage.so", "build/benchmark")
INPUT_BYTES = sum(SIZES.values())
PAGE_COUNT = sum((size + 4095) // 4096 for size in SIZES.values())


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def save_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def code_hashes():
    paths = [ROOT / "Makefile"]
    for directory in ("src", "include", "bench", "tools"):
        paths.extend(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and path.suffix in (".c", ".h", ".inc", ".py"))
    return {str(path.relative_to(ROOT)): sha256(path) for path in sorted(paths)}


def binary_hashes():
    return {name: {"sha256": sha256(ROOT / name), "bytes": (ROOT / name).stat().st_size}
            for name in BUILD_TARGETS}


def corpus_fingerprint(directory):
    result = {}
    for name in EXPECTED:
        stat = (directory / name).stat()
        result[name] = [stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns]
    return result


def optional_text(path):
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def cpu_snapshot(cpu):
    base = Path("/sys/devices/system/cpu/cpu{}".format(cpu))
    return {
        "time_utc": utc_now(),
        "cpu": cpu,
        "scaling_governor": optional_text(base / "cpufreq/scaling_governor"),
        "scaling_cur_freq_khz": optional_text(base / "cpufreq/scaling_cur_freq"),
        "scaling_min_freq_khz": optional_text(base / "cpufreq/scaling_min_freq"),
        "scaling_max_freq_khz": optional_text(base / "cpufreq/scaling_max_freq"),
        "physical_package_id": optional_text(base / "topology/physical_package_id"),
        "core_id": optional_text(base / "topology/core_id"),
    }


def system_info(cpu):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    try:
        result = subprocess.run(compiler + ["--version"], capture_output=True, text=True,
                                timeout=10, check=False)
        compiler_version = result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        compiler_version = None
    info = optional_text("/proc/cpuinfo")
    return {
        "uname": platform.uname()._asdict(),
        "python": sys.version,
        "os_release": optional_text("/etc/os-release"),
        "cpuinfo_first_processor": info.split("\n\n", 1)[0] if info else None,
        "parent_allowed_cpus": sorted(os.sched_getaffinity(0)),
        "selected_cpu": cpu,
        "default_cc_command": compiler,
        "default_cc_version": compiler_version,
        "build_environment": {name: os.environ.get(name) for name in
                              ("CC", "CFLAGS", "CPPFLAGS", "ARCHFLAGS", "LDFLAGS", "MAKEFLAGS")},
        "compiler_note": "The actual compile and link commands are recorded in build.log and build-plan.txt.",
    }


def validate_round(records, round_number, order, cpu, seconds, verify_only):
    if not records or records[-1].get("type") != "validation":
        raise RuntimeError("Round {} did not finish validation".format(round_number))
    groups = {}
    for record in records:
        if record.get("round") != round_number:
            raise RuntimeError("Incorrect round number in benchmark output")
        groups.setdefault(record.get("type"), []).append(record)
    expected_types = {"environment", "ratio", "file_ratio", "validation"}
    if not verify_only:
        expected_types.add("sample")
    if set(groups) != expected_types:
        raise RuntimeError("Unexpected or missing benchmark record types")
    for kind in ("environment", "ratio", "validation"):
        if len(groups[kind]) != 1:
            raise RuntimeError("Expected one {} record".format(kind))
    env = groups["environment"][0]
    for key, value in {
        "cpu_affinity": cpu, "input_bytes": INPUT_BYTES, "blocks": PAGE_COUNT,
        "block_bytes": 4096, "compressed_slot_stride": 5120, "decoded_slot_stride": 4096,
        "order": order, "verify_only": verify_only,
    }.items():
        if env.get(key) != value:
            raise RuntimeError("Unexpected benchmark environment field: {}".format(key))
    ratio = groups["ratio"][0]
    stored = ratio.get("stored_payload_bytes", 0)
    if (ratio.get("roundtrip") != "passed" or ratio.get("input_bytes") != INPUT_BYTES
            or not isinstance(stored, int) or not 0 < stored <= INPUT_BYTES
            or not math.isclose(ratio.get("ratio", 0), INPUT_BYTES / stored, rel_tol=1e-8)):
        raise RuntimeError("Invalid size or round-trip record")
    files = groups["file_ratio"]
    if len(files) != len(EXPECTED) or [row.get("file") for row in files] != list(EXPECTED):
        raise RuntimeError("Incorrect corpus file list")
    for row in files:
        name = row["file"]
        if (row.get("input_bytes") != SIZES[name]
                or row.get("blocks") != (SIZES[name] + 4095) // 4096
                or not 0 < row.get("stored_bytes", 0) <= SIZES[name]
                or not 0 <= row.get("raw_blocks", -1) <= row["blocks"]):
            raise RuntimeError("Invalid per-file record: {}".format(name))
    if (sum(row["stored_bytes"] for row in files) != stored
            or sum(row["raw_blocks"] for row in files) != ratio.get("raw_blocks")):
        raise RuntimeError("Per-file sizes do not match the total")
    samples = groups.get("sample", [])
    operations = [] if verify_only else (["decompress", "compress"]
                                         if order == "decompress-first" else ["compress", "decompress"])
    if [row.get("operation") for row in samples] != operations:
        raise RuntimeError("Missing, duplicate or incorrectly ordered timing samples")
    for row in samples:
        if (not isinstance(row.get("loops"), int) or not 1 <= row["loops"] <= 64
                or row.get("input_bytes_per_loop") != INPUT_BYTES
                or not math.isclose(row.get("target_seconds", 0), seconds, rel_tol=1e-8, abs_tol=1e-9)):
            raise RuntimeError("Invalid sample calibration")
        for key in ("wall_seconds", "thread_cpu_seconds", "wall_MBps", "thread_cpu_MBps"):
            if not isinstance(row.get(key), (int, float)) or not math.isfinite(row[key]) or row[key] <= 0:
                raise RuntimeError("Invalid measured value: {}".format(key))
        for duration, throughput in (("wall_seconds", "wall_MBps"),
                                     ("thread_cpu_seconds", "thread_cpu_MBps")):
            computed = INPUT_BYTES * row["loops"] / row[duration] / 1e6
            if not math.isclose(row[throughput], computed, rel_tol=1e-6):
                raise RuntimeError("Measured byte count and throughput disagree")
    validation = groups["validation"][0]
    if (validation.get("result") != "passed"
            or validation.get("forward_roundtrip") != "passed"
            or validation.get("reverse_roundtrip") != "passed"
            or validation.get("timing_samples") != len(operations)):
        raise RuntimeError("Round validation did not pass")
    signature = {
        "ratio": {key: value for key, value in ratio.items() if key != "round"},
        "files": [{key: value for key, value in row.items() if key != "round"} for row in files],
    }
    return signature, samples


def make_summary(signature, samples, args):
    ratio = signature["ratio"]
    result = {
        "algorithm": "FastPage",
        "mode": "verify-only" if args.verify_only else "benchmark",
        "input_bytes": INPUT_BYTES,
        "pages": PAGE_COUNT,
        "full_pages": sum(size // 4096 for size in SIZES.values()),
        "partial_pages": sum(size % 4096 != 0 for size in SIZES.values()),
        "stored_payload_bytes": ratio["stored_payload_bytes"],
        "compression_ratio": INPUT_BYTES / ratio["stored_payload_bytes"],
        "stored_percent": 100 * ratio["stored_payload_bytes"] / INPUT_BYTES,
        "saved_percent": 100 * (1 - ratio["stored_payload_bytes"] / INPUT_BYTES),
        "raw_pages": ratio["raw_blocks"],
        "roundtrip": "passed",
        "rounds": 1 if args.verify_only else args.rounds,
        "timing_samples": len(samples),
        "cpu": args.cpu,
        "files": [{key: value for key, value in row.items() if key != "type"}
                  for row in signature["files"]],
        "measurement": {
            "page_bytes": 4096, "compressed_slot_stride": 5120, "decoded_slot_stride": 4096,
            "units": "decimal MB/s, using original input bytes for both directions",
            "all_samples_retained": True,
            "target_seconds": None if args.verify_only else args.seconds,
            "calibration": "One untimed full-corpus pass per direction per round; ceil(target/warmup), capped at 64 loops. The target is not a guaranteed minimum duration.",
            "scope": "Each page, including internal RAW pages, calls the public codec function pointer. Corpus IO, checksums and byte comparisons are outside timing.",
            "ratio_scope": "Encoded payload only; slot padding, external lengths and indexing are excluded.",
        },
    }
    if not args.verify_only:
        for operation in ("compress", "decompress"):
            rows = [row for row in samples if row["operation"] == operation]
            speeds = [row["wall_MBps"] for row in rows]
            median = statistics.median(speeds)
            result[operation] = {
                "samples": len(rows), "median_MBps": median,
                "min_MBps": min(speeds), "max_MBps": max(speeds),
                "equivalent_us_per_4096_bytes": 4096 / median,
                "median_thread_cpu_to_wall": statistics.median(
                    row["thread_cpu_seconds"] / row["wall_seconds"] for row in rows),
            }
    return result


def write_report(path, summary):
    lines = ["# FastPage corpus results", "", "- Mode: `{}`".format(summary["mode"]),
             "- CPU: {}; verified pages: {:,}".format(summary["cpu"], summary["pages"]),
             "- Original bytes: {:,}; encoded payload: {:,}".format(
                 summary["input_bytes"], summary["stored_payload_bytes"]),
             "- Compression ratio: **{:.9f}**; saved space: **{:.4f}%**".format(
                 summary["compression_ratio"], summary["saved_percent"]),
             "- Forward and reverse page-order round trips: passed", ""]
    if summary["mode"] == "benchmark":
        lines.extend(["| Operation | Samples | Median MB/s | Minimum MB/s | Maximum MB/s |",
                      "|---|---:|---:|---:|---:|"])
        for operation in ("compress", "decompress"):
            row = summary[operation]
            lines.append("| {} | {} | {:.2f} | {:.2f} | {:.2f} |".format(
                operation, row["samples"], row["median_MBps"], row["min_MBps"], row["max_MBps"]))
        lines.extend(["", "Each round retains both samples; operation order alternates between rounds.",
                      summary["measurement"]["calibration"], ""])
    else:
        lines.extend(["No timing was requested. This run reports correctness and payload sizes only.", ""])
    lines.extend(["## Measurement scope", "", summary["measurement"]["scope"], "",
                  summary["measurement"]["ratio_scope"], "",
                  "Throughput uses decimal MB/s of original input in both directions. "
                  "The corpus includes {} partial final pages; files are never joined across page boundaries.".format(
                      summary["partial_pages"]), "",
                  "CPU affinity is enforced, but frequency and other host activity are not controlled. "
                  "CPU frequency observations in environment.json are snapshots, not a continuous frequency lock.", "",
                  "## Artifacts", "", "- [Summary](summary.json)", "- [All raw records](samples.jsonl)",
                  "- [Environment](environment.json)", "- [Code and binary hashes](manifest.json)",
                  "- [Corpus checksums](corpus.json)", "- [Run status](status.json)", ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def run(args):
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        raise ValueError("Output already exists; choose a new directory: {}".format(output))
    status = {"state": "running", "mode": "verify-only" if args.verify_only else "benchmark",
              "started_utc": utc_now(), "phase": "validate-input", "completed_rounds": 0,
              "requested_rounds": 1 if args.verify_only else args.rounds, "timing_samples": 0}
    save_json(output / "status.json", status)
    try:
        if args.cpu not in os.sched_getaffinity(0):
            raise ValueError("Requested CPU {} is not in the allowed affinity set".format(args.cpu))
        corpus = args.corpus.resolve()
        print("Checking all 12 corpus files...", flush=True)
        records = verify_corpus(corpus)
        fingerprint = corpus_fingerprint(corpus)
        save_json(output / "corpus.json", {"directory": str(corpus), "official_source": URL,
                                          "files": records, "verification": "passed"})
        environment = {
            "started_utc": status["started_utc"], "command": sys.argv,
            "project_directory": str(ROOT), "corpus_directory": str(corpus),
            "system": system_info(args.cpu), "rounds": [],
            "frequency_note": "Before/after snapshots only; frequency is not continuously locked or monitored.",
        }
        save_json(output / "environment.json", environment)
        status["phase"] = "build"
        save_json(output / "status.json", status)
        sources_before = code_hashes()
        command = ["make", "-B", "BUILD=build", *BUILD_TARGETS]
        plan = ["make", "-n", "-B", "BUILD=build", *BUILD_TARGETS]
        with (output / "build-plan.txt").open("w", encoding="utf-8") as stream:
            subprocess.run(plan, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
        print("Building the standalone codec and benchmark...", flush=True)
        with (output / "build.log").open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
        sources = code_hashes()
        if sources != sources_before:
            raise RuntimeError("Source files changed during the build")
        binaries = binary_hashes()
        manifest = {"created_utc": utc_now(), "source_sha256": sources, "binaries": binaries,
                    "build_command": command, "build_plan_sha256": sha256(output / "build-plan.txt"),
                    "build_log_sha256": sha256(output / "build.log")}
        save_json(output / "manifest.json", manifest)

        def check_frozen():
            if code_hashes() != sources or binary_hashes() != binaries:
                raise RuntimeError("Source files or benchmark binaries changed during the run")
            if corpus_fingerprint(corpus) != fingerprint:
                raise RuntimeError("Corpus files changed during the run")

        signature = None
        samples = []
        rounds = 1 if args.verify_only else args.rounds
        with (output / "samples.jsonl").open("w", encoding="utf-8") as merged:
            for number in range(1, rounds + 1):
                check_frozen()
                order = "verify" if args.verify_only else (
                    "compress-first" if number % 2 else "decompress-first")
                status.update(phase="verify" if args.verify_only else "measure", current_round=number)
                save_json(output / "status.json", status)
                print("{} {}/{} ({})".format("Verification" if args.verify_only else "Round",
                                              number, rounds, order), flush=True)
                command = [str(ROOT / "build/benchmark"), str(corpus),
                           str(ROOT / "build/libfastpage.so"), str(args.cpu), str(number),
                           str(args.seconds), order]
                raw = output / "round-{:03d}.jsonl".format(number)
                log = output / "round-{:03d}.log".format(number)
                round_info = {"round": number, "order": order, "command": command,
                              "raw_records": raw.name, "stderr_log": log.name,
                              "cpu_before": cpu_snapshot(args.cpu)}
                environment["rounds"].append(round_info)
                save_json(output / "environment.json", environment)
                with raw.open("w", encoding="utf-8") as stream, log.open("w", encoding="utf-8") as errors:
                    subprocess.run(command, cwd=ROOT, stdout=stream, stderr=errors, check=True)
                round_info["cpu_after"] = cpu_snapshot(args.cpu)
                check_frozen()
                round_records = [json.loads(line) for line in raw.read_text(encoding="utf-8").splitlines()]
                current, measured = validate_round(round_records, number, order, args.cpu,
                                                   args.seconds, args.verify_only)
                if signature is None:
                    signature = current
                elif current != signature:
                    raise RuntimeError("Encoded sizes or correctness changed between rounds")
                for record in round_records:
                    merged.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")
                merged.flush()
                samples.extend(measured)
                round_info["validation"] = "passed"
                status.update(completed_rounds=number, timing_samples=len(samples))
                save_json(output / "environment.json", environment)
                save_json(output / "status.json", status)
        status["phase"] = "finalize"
        save_json(output / "status.json", status)
        check_frozen()
        if verify_corpus(corpus) != records:
            raise RuntimeError("Final corpus checksum verification differs")
        check_frozen()
        summary = make_summary(signature, samples, args)
        save_json(output / "summary.json", summary)
        write_report(output / "REPORT.md", summary)
        environment.update(completed_utc=utc_now(), code_and_binary_consistency="passed",
                           corpus_checksum_before_and_after="passed")
        save_json(output / "environment.json", environment)
        status.update(state="completed", phase="completed", completed_utc=utc_now())
        save_json(output / "status.json", status)
        print("Completed: {}".format(output), flush=True)
        return 0
    except BaseException as error:
        status.update(state="failed", phase="failed", failed_utc=utc_now(),
                      error="{}: {}".format(type(error).__name__, error))
        save_json(output / "status.json", status)
        if isinstance(error, KeyboardInterrupt):
            print("Interrupted; partial records retained in {}".format(output), file=sys.stderr)
            return 130
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("runs/benchmark"))
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=1.5)
    parser.add_argument("--verify-only", action="store_true", help="verify all pages and sizes without timing")
    args = parser.parse_args()
    if not hasattr(os, "sched_getaffinity"):
        parser.error("Linux CPU affinity support is required")
    if not 1 <= args.rounds <= 100:
        parser.error("--rounds must be in 1..100")
    if not math.isfinite(args.seconds) or not 0 < args.seconds <= 60:
        parser.error("--seconds must be finite and in (0,60]")
    if args.cpu < 0:
        parser.error("--cpu must be nonnegative")
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Benchmark failed: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
