#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Benchmark isolated generated-code chunk sizes and optimization levels."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

try:
    import psutil
except ImportError:  # Peak RSS remains optional evidence.
    psutil = None


ROOT = Path(__file__).resolve().parents[1]
FUNCTION_RE = re.compile(r"(?m)^void ([fr]_[0-9a-f]{8})\(CpuState \*s\) \{")
REGISTER_RE = re.compile(r"sr_register\((0x[0-9a-f]{8}u), ([fr]_[0-9a-f]{8})\);")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def generated_catalog(directory: Path) -> tuple[dict[str, str], list[tuple[str, str]]]:
    functions: dict[str, str] = {}
    registrations: list[tuple[str, str]] = []
    for path in sorted(directory.glob("hst_recomp_*.c")):
        text = path.read_text(encoding="ascii")
        matches = list(FUNCTION_RE.finditer(text))
        wrapper = text.find("\n\nvoid sr_register_chunk_")
        for index, match in enumerate(matches):
            end = matches[index + 1].start() if index + 1 < len(matches) else wrapper
            if end < 0:
                raise ValueError(f"registration wrapper missing from {path}")
            body = text[match.start() : end].strip().encode("ascii")
            name = match.group(1)
            if name in functions:
                raise ValueError(f"duplicate generated function {name}")
            functions[name] = sha256(body)
        registrations.extend(REGISTER_RE.findall(text))
    return functions, sorted(registrations)


def terminate_tree(proc: subprocess.Popen) -> None:
    if psutil is not None:
        try:
            root = psutil.Process(proc.pid)
            children = root.children(recursive=True)
            for child in reversed(children):
                child.kill()
            root.kill()
        except psutil.Error:
            pass
    else:
        proc.kill()


def compile_one(command: list[str], timeout: float) -> dict:
    start = time.perf_counter()
    proc = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    peak_rss = 0
    timed_out = False
    while proc.poll() is None:
        elapsed = time.perf_counter() - start
        if elapsed > timeout:
            timed_out = True
            terminate_tree(proc)
            break
        if psutil is not None:
            try:
                processes = [psutil.Process(proc.pid)] + psutil.Process(proc.pid).children(recursive=True)
                peak_rss = max(peak_rss, sum(p.memory_info().rss for p in processes))
            except psutil.Error:
                pass
        time.sleep(0.05)
    _, stderr = proc.communicate()
    wall = time.perf_counter() - start
    output = Path(command[-1])
    return {
        "source": command[-3],
        "status": "timeout" if timed_out else ("ok" if proc.returncode == 0 else "failed"),
        "exit_code": None if timed_out else proc.returncode,
        "wall_seconds": round(wall, 3),
        "peak_rss_bytes": peak_rss or None,
        "object_bytes": output.stat().st_size if output.is_file() else None,
        "stderr_tail": "\n".join(stderr.splitlines()[-20:]),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output-root", type=Path, default=ROOT / "build" / "codegen-bench")
    parser.add_argument("--base", default="0")
    parser.add_argument("--profile", default="hst")
    parser.add_argument("--extra-elf", action="append", default=[])
    parser.add_argument("--chunk-sizes", type=int, nargs="+", default=[2000, 1000, 500, 250])
    parser.add_argument("--opts", nargs="+", default=["O1", "O2"])
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--compile-timeout", type=float, default=300.0)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler_path = Path(args.cc)
    if compiler_path.parent != Path("."):
        # MinGW/UCRT GCC locates cc1 and runtime DLLs through its bin directory.
        # An absolute gcc.exe alone can otherwise exit 1 before compilation with
        # no diagnostic when launched outside the manager-initialized shell.
        os.environ["PATH"] = str(compiler_path.resolve().parent) + os.pathsep + os.environ["PATH"]
    output_root = args.output_root.resolve()
    build_root = (ROOT / "build").resolve()
    if not output_root.is_relative_to(build_root):
        raise SystemExit(f"output root must stay beneath {build_root}")
    if output_root.exists():
        if args.resume:
            pass
        elif not args.overwrite:
            raise SystemExit(f"output root exists; pass --overwrite: {output_root}")
        else:
            shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    results_path = output_root / "results.json"
    if args.resume and results_path.is_file():
        results = json.loads(results_path.read_text(encoding="utf-8"))
    else:
        results = {
            "schema_version": 1,
            "compiler": args.cc,
            "compiler_version": subprocess.run(
                [args.cc, "--version"], check=False, capture_output=True, text=True
            ).stdout.strip(),
            "peak_rss_available": psutil is not None,
            "psutil_version": psutil.__version__ if psutil is not None else None,
            "chunk_sizes": args.chunk_sizes,
            "optimization_levels": args.opts,
            "runs": [],
        }
    completed_keys = {
        (run.get("chunk_size"), run.get("optimization")) for run in results["runs"]
    }
    baseline_functions = None
    baseline_registrations = None
    for existing_dir in sorted(output_root.glob("chunk_*")):
        if list(existing_dir.glob("hst_recomp_*.c")):
            baseline_functions, baseline_registrations = generated_catalog(existing_dir)
            break

    for chunk_size in args.chunk_sizes:
        source_dir = output_root / f"chunk_{chunk_size}"
        output_c = source_dir / "hst_recomp.c"
        if args.resume and output_c.is_file():
            previous = next(
                (run for run in results["runs"] if run.get("chunk_size") == chunk_size), None
            )
            generation_seconds = previous.get("generation_seconds", 0.0) if previous else 0.0
            print(f"[generate] reuse chunk_size={chunk_size}", flush=True)
        else:
            source_dir.mkdir()
            codegen_command = [
                sys.executable,
                str(ROOT / "tools" / "codegen.py"),
                str(args.elf.resolve()),
                str(output_c),
                f"--base={args.base}",
                f"--profile={args.profile}",
                f"--funcs-per-chunk={chunk_size}",
                *[f"--extra-elf={value}" for value in args.extra_elf],
            ]
            print(f"[generate] chunk_size={chunk_size}", flush=True)
            started = time.perf_counter()
            generated = subprocess.run(
                codegen_command, cwd=ROOT, check=False, capture_output=True, text=True, timeout=900
            )
            generation_seconds = round(time.perf_counter() - started, 3)
            if generated.returncode:
                results["runs"].append(
                    {
                        "chunk_size": chunk_size,
                        "generation_status": "failed",
                        "generation_seconds": generation_seconds,
                        "stderr_tail": "\n".join(generated.stderr.splitlines()[-40:]),
                    }
                )
                print(f"[generate] FAILED chunk_size={chunk_size}", flush=True)
                continue

        functions, registrations = generated_catalog(source_dir)
        if baseline_functions is None:
            baseline_functions = functions
            baseline_registrations = registrations
        partition_equivalent = (
            functions == baseline_functions and registrations == baseline_registrations
        )
        sources = [output_c, *sorted(source_dir.glob("hst_recomp_*.c"))]
        source_bytes = sum(path.stat().st_size for path in sources)
        print(
            f"[generate] chunks={len(sources) - 1} functions={len(functions)} "
            f"seconds={generation_seconds} partition_equivalent={partition_equivalent}",
            flush=True,
        )

        for opt in args.opts:
            key = (chunk_size, f"-{opt}")
            if args.resume and key in completed_keys:
                print(f"[compile] reuse chunk_size={chunk_size} opt=-{opt}", flush=True)
                continue
            object_dir = source_dir / opt.lower()
            if object_dir.exists():
                shutil.rmtree(object_dir)
            object_dir.mkdir()
            files = []
            setting_started = time.perf_counter()
            print(f"[compile] chunk_size={chunk_size} opt=-{opt}", flush=True)
            for index, source in enumerate(sources, 1):
                output = object_dir / f"{source.stem}.o"
                command = [
                    args.cc,
                    f"-{opt}",
                    "-w",
                    "-fno-strict-aliasing",
                    "-fno-var-tracking",
                    "-ftrack-macro-expansion=0",
                    f"-I{source_dir}",
                    f"-I{ROOT / 'src' / 'rt'}",
                    "-DSR_SDL3VK",
                    "-c",
                    str(source),
                    "-o",
                    str(output),
                ]
                measurement = compile_one(command, args.compile_timeout)
                files.append(measurement)
                rss = measurement["peak_rss_bytes"]
                rss_text = f"{rss / (1024 * 1024):.1f} MiB" if rss else "unavailable"
                print(
                    f"  [{index}/{len(sources)}] {source.name}: {measurement['status']} "
                    f"{measurement['wall_seconds']:.3f}s peak={rss_text}",
                    flush=True,
                )
                if measurement["status"] != "ok":
                    break
            successful = [item for item in files if item["status"] == "ok"]
            run = {
                "chunk_size": chunk_size,
                "optimization": f"-{opt}",
                "generation_status": "ok",
                "generation_seconds": generation_seconds,
                "chunk_count": len(sources) - 1,
                "function_count": len(functions),
                "registration_count": len(registrations),
                "partition_equivalent": partition_equivalent,
                "source_bytes": source_bytes,
                "status": "ok" if len(successful) == len(sources) else "failed",
                "compile_wall_seconds": round(time.perf_counter() - setting_started, 3),
                "compiler_sum_seconds": round(sum(item["wall_seconds"] for item in files), 3),
                "peak_rss_bytes": max(
                    (item["peak_rss_bytes"] or 0 for item in files), default=0
                )
                or None,
                "object_bytes": sum(item["object_bytes"] or 0 for item in successful),
                "files_completed": len(successful),
                "files_expected": len(sources),
                "files": files,
            }
            results["runs"].append(run)
            print(
                f"[result] chunk_size={chunk_size} opt=-{opt} status={run['status']} "
                f"wall={run['compile_wall_seconds']}s objects={run['object_bytes']} bytes",
                flush=True,
            )
            results_path.write_text(
                json.dumps(results, indent=2) + "\n", encoding="utf-8"
            )

    print(f"results: {results_path}", flush=True)
    return 0 if all(run.get("status") == "ok" for run in results["runs"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
