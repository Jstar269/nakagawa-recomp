# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Prove the FSR 3 host cells are load-bearing.

Each mutant below is one semantic defect in a copy of the FSR 3 host code (src/rt/fsr3_host.c,
src/rt/fsr3_jitter.c), never in the tree itself. The driver builds tests/native/fsr3_host_selftest.c
against the copy, runs it on the AMD record, and requires the run to fail for the stated reason:

* A mutant must build. A compile error proves the compiler noticed, not the cell, so it is
  INVALID and fails the campaign.
* A mutant must fail the cell, and the failure output must contain the mutant's `expect` text,
  which names the record line or field the defect changes. A run that fails for another reason is
  reported as WRONG_REASON, not counted as a kill.
* The unmutated sources must pass first, in the same build directory and with the same flags.

usage: python tools/fsr3_mutants.py --build-dir build/fsr3-mutants [--cc gcc]
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HOST = "src/rt/fsr3_host.c"
JITTER = "src/rt/fsr3_jitter.c"
SELFTEST = "tests/native/fsr3_host_selftest.c"
RECORD = "tests/native/fsr3_host_oracle.txt"
CFLAGS = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
INCLUDES = ["-Isrc/rt", "-Ithird_party/fidelityfx_sdk/upscalers/fsr3/include/gpu"]

# (name, file, old text, new text, text the failure output must contain)
MUTANTS: list[tuple[str, str, str, str, str]] = [
    ("ping-pong parity swapped", HOST,
     "const int odd = (h->resource_frame_index & 1u) != 0;",
     "const int odd = (h->resource_frame_index & 1u) == 0;",
     "  srv r_previous_luma"),
    ("reset keeps the SPD mips", HOST,
     "        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS], 0, 0, 0, 0);\n",
     "",
     "AMD:  clear 27"),
    ("no per-frame SPD mip clear", HOST,
     "    job_clear(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS], 0, 0, 0, 0);\n",
     "",
     "AMD:  clear 27"),
    ("dilated reactive masks kept", HOST,
     "    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS]);\n",
     "",
     "barrier 26 not before a discard"),
    ("reconstructed depth cleared to the far value of forward depth", HOST,
     "(h->desc.flags & FSR3_HOST_ENABLE_DEPTH_INVERTED) ? 0.0f : 1.0f;",
     "(h->desc.flags & FSR3_HOST_ENABLE_DEPTH_INVERTED) ? 1.0f : 0.0f;",
     "AMD:  clear 8"),
    ("RCAS tiles of 8", HOST,
     "const int32_t rcas_region = 16;",
     "const int32_t rcas_region = 8;",
     "AMD:  dispatch FSR3-RCAS"),
    ("sharpness not remapped", HOST,
     "fsr3_rcas_con(h->rcas.rcasConfig, (-2.0f * d->sharpness) + 2.0f);",
     "fsr3_rcas_con(h->rcas.rcasConfig, d->sharpness);",
     "AMD:  stage 2"),
    ("frame index kept on a reset", HOST,
     "c->frameIndex = reset ? 0.0f : c->frameIndex + 1.0f;",
     "c->frameIndex = c->frameIndex + 1.0f;",
     "AMD:  stage 0"),
    ("frame time not clamped to 1 s", HOST,
     "const float lo = 1.0f < v ? 1.0f : v;",
     "const float lo = v;",
     "AMD:  stage 0"),
    ("tanHalfFOV in double", HOST,
     "const float camera_angle_horizontal = atanf(tanf(d->camera_fov_angle_vertical / 2.0f) * aspect_ratio) * 2.0f;",
     "const float camera_angle_horizontal = (float)(atan(tan((double)(d->camera_fov_angle_vertical / 2)) * "
     "(double)aspect_ratio) * 2);",
     "AMD:  stage 0"),
    ("jitter cancellation sign", HOST,
     "(h->previous_jitter_offset[0] - c->jitterOffset[0]) / (float)mv_target[0];",
     "(c->jitterOffset[0] - h->previous_jitter_offset[0]) / (float)mv_target[0];",
     "AMD:  stage 0"),
    ("SPD mip level dropped", HOST,
     "*mip = b->id - R(SPD_MIPS_LEVEL_0);",
     "*mip = 0;",
     "  uav rw_spd_mip"),
    ("accumulate reads dilated depth for motion", HOST,
     '{ 10, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), "r_dilated_motion_vectors" },\n'
     '    { 11, FSR3_BIND_SRV, R(INTERNAL_UPSCALED_COLOR)',
     '{ 10, FSR3_BIND_SRV, R(DILATED_DEPTH), "r_dilated_motion_vectors" },\n'
     '    { 11, FSR3_BIND_SRV, R(INTERNAL_UPSCALED_COLOR)',
     "  srv r_dilated_motion_vectors"),
    ("f16 keeps exponent 15 out of the normal range", HOST,
     "else if (e <= 15) {",
     "else if (e < 15) {",
     "f16 of"),
    ("phase count ramps by 2", JITTER,
     "if (delta > 0) return current + 1.0f;",
     "if (delta > 0) return current + 2.0f;",
     "AMD:  stage 0"),
    ("halton base 3 read as 2", JITTER,
     "*y = fsr3_halton((index % phase_count) + 1, 3) - 0.5f;",
     "*y = fsr3_halton((index % phase_count) + 1, 2) - 0.5f;",
     "jitter "),
]


def build_and_run(cc: str, work: Path, host: Path, jitter: Path, tag: str) -> tuple[str, int, str]:
    exe = work / f"fsr3_host_selftest_{tag}{'.exe' if os.name == 'nt' else ''}"
    cmd = [cc, *CFLAGS, *INCLUDES, "-o", str(exe), SELFTEST, str(host), str(jitter), "-lm"]
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if built.returncode != 0:
        return "BUILD", built.returncode, built.stderr
    ran = subprocess.run([str(exe), RECORD], cwd=ROOT, capture_output=True, text=True, timeout=300)
    return "RAN", ran.returncode, ran.stdout + ran.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    work = args.build_dir.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    status, rc, out = build_and_run(args.cc, work, ROOT / HOST, ROOT / JITTER, "base")
    if status != "RAN" or rc != 0:
        print(f"FAIL fsr3-mutants: the unmutated cell does not pass ({status}, exit {rc})\n{out}")
        return 1

    failures = 0
    for index, (name, path, old, new, expect) in enumerate(MUTANTS):
        source = (ROOT / path).read_text(encoding="utf-8")
        if source.count(old) != 1:
            print(f"INVALID {name}: the text to change appears {source.count(old)} times in {path}")
            failures += 1
            continue
        lane = work / f"m{index:02d}"
        lane.mkdir()
        mutated = lane / Path(path).name
        mutated.write_text(source.replace(old, new), encoding="utf-8", newline="\n")
        host = mutated if path == HOST else ROOT / HOST
        jitter = mutated if path == JITTER else ROOT / JITTER
        status, rc, out = build_and_run(args.cc, lane, host, jitter, f"m{index:02d}")
        if status == "BUILD":
            print(f"INVALID {name}: the mutant does not build\n{out}")
            failures += 1
        elif rc == 0:
            print(f"SURVIVED {name}")
            failures += 1
        elif expect not in out:
            first = next((line for line in out.splitlines() if "FAIL" in line or "AMD:" in line), out[:200])
            print(f"WRONG_REASON {name}: expected {expect!r}, first failure: {first}")
            failures += 1
        else:
            print(f"KILLED {name}")
    print(f"fsr3-mutants: {len(MUTANTS) - failures} of {len(MUTANTS)} killed for the stated reason")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
