# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Prove the FSR 3 cells are load-bearing.

Each mutant below is one semantic defect in a copy of the FSR 3 code, never in the tree itself.

* Host mutants (the default) change the host (src/rt/fsr3_host.c, src/rt/fsr3_jitter.c). The
  driver builds tests/native/fsr3_host_selftest.c against the copy and runs it on the AMD record.
* GPU mutants (--pass) change the pass (src/rt/gpu_sdl3vk/fsr3_pass.c), the jitter, or the scenes
  in src/rt/gpu_sdl3vk/fsr3_pass_selftest.c, which is built against the copy and run on a Vulkan
  device for the one cell the mutant targets. Recorded mutants are run and their measurements
  printed, but not judged: they show a behaviour, such as a value moved to another input.

Every judged mutant must fail for the stated reason:

* A mutant must build. A compile error proves the compiler noticed, not the cell, so it is
  INVALID and fails the campaign.
* A mutant must fail the cell, and the failure output must contain the mutant's `expect` text,
  which names the record line, field or measurement the defect changes. A run that fails for
  another reason is reported as WRONG_REASON, not counted as a kill.
* The unmutated sources must pass first, in the same build directory and with the same flags.

--calibrate (with --pass) runs every mutant and prints its measurements even when the unmutated
cell fails, for choosing bars on a new device; it always exits 1. --cell limits --pass to one cell.

usage: python tools/fsr3_mutants.py --build-dir build/fsr3-mutants [--cc gcc] [--pass [--calibrate] [--cell F-R]]
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
     '{ 10, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },\n'
     '    { 11, FSR3_BIND_SRV, R(INTERNAL_UPSCALED_COLOR)',
     '{ 10, FSR3_BIND_SRV, R(DILATED_DEPTH), 0, "r_dilated_motion_vectors" },\n'
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
    ("the halton index without + 1", JITTER,
     "*x = fsr3_halton((index % phase_count) + 1, 2) - 0.5f;\n"
     "    *y = fsr3_halton((index % phase_count) + 1, 3) - 0.5f;",
     "*x = fsr3_halton(index % phase_count, 2) - 0.5f;\n"
     "    *y = fsr3_halton(index % phase_count, 3) - 0.5f;",
     "J: offset"),
    ("the phase count rounded up", JITTER,
     "return (int32_t)(base_phase_count * powf((float)display_width / (float)render_width, 2.0f));",
     "return (int32_t)ceilf(base_phase_count * powf((float)display_width / (float)render_width, 2.0f));",
     "J: phase count"),
    ("the clip offset's x sign", JITTER,
     "out[0] = 2.0f * jx / viewport_w;",
     "out[0] = -2.0f * jx / viewport_w;",
     "J: clip offset"),
]

# The GPU cells (src/rt/gpu_sdl3vk/fsr3_pass_selftest.c), one cell a mutant:
# (name, file, old text, new text, cell, text the failure output must contain).
PASS = "src/rt/gpu_sdl3vk/fsr3_pass.c"
PASS_SELFTEST = "src/rt/gpu_sdl3vk/fsr3_pass_selftest.c"
PASS_MUTANTS: list[tuple[str, str, str, str, str, str]] = [
    ("P-0: a feature missing at device creation", PASS_SELFTEST,
     "features.shaderStorageImageReadWithoutFormat = VK_TRUE;",
     "features.shaderStorageImageReadWithoutFormat = VK_FALSE;",
     "P-0", "validation messages"),
    ("P-0: an SRV bound as a storage image", PASS,
     "case FSR3_BIND_SRV: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;",
     "case FSR3_BIND_SRV: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
     "P-0", "validation messages"),
    ("F-M: no barrier between passes", PASS,
     "            const Pipe *pp = &p->pipe[j->pass];\n            barrier_all(p, cmd);\n",
     "            const Pipe *pp = &p->pipe[j->pass];\n",
     "F-M", "validation messages"),
    ("F-S: the jitter dropped from the raster", JITTER,
     "out[0] = 2.0f * jx / viewport_w;\n    out[1] = 2.0f * jy / viewport_h;",
     "out[0] = 0.0f * jx / viewport_w;\n    out[1] = 0.0f * jy / viewport_h;",
     "F-S", "not below"),
    ("F-S: the jitter's x sign flipped", JITTER,
     "out[0] = 2.0f * jx / viewport_w;",
     "out[0] = -2.0f * jx / viewport_w;",
     "F-S", "not below"),
    ("F-S: the jitter's y sign flipped", JITTER,
     "out[1] = 2.0f * jy / viewport_h;",
     "out[1] = -2.0f * jy / viewport_h;",
     "F-S", "not below"),
    ("F-S: the NDC width taken from renderSize (x jitter x 512/480)", PASS_SELFTEST,
     "fsr3_jitter_clip_offset(r->jx, r->jy, (float)CANVAS_W, (float)CANVAS_H, o);",
     "fsr3_jitter_clip_offset(r->jx, r->jy, (float)RENDER_W, (float)CANVAS_H, o);",
     "F-S", "off the jitter"),
    ("F-M: the vectors' sign flipped", PASS_SELFTEST,
     "const double mvx = -q->vx, mvy = -q->vy;",
     "const double mvx = q->vx, mvy = q->vy;",
     "F-M", "trailing-edge error"),
    ("F-M: the vectors' y flipped", PASS_SELFTEST,
     "const double mvx = -q->vx, mvy = -q->vy;",
     "const double mvx = -q->vx, mvy = q->vy;",
     "F-M", "trailing-edge error"),
    ("F-M: the vectors in UV, not pixels", PASS_SELFTEST,
     "const double mvx = -q->vx, mvy = -q->vy;",
     "const double mvx = -q->vx / RENDER_W, mvy = -q->vy / RENDER_H;",
     "F-M", "trailing-edge error"),
    ("F-R: the reactive input unbound", PASS,
     "d.has_reactive = f->reactive != VK_NULL_HANDLE;",
     "d.has_reactive = 0;",
     "F-R", "a streak"),
    ("F-T: the T&C input ignored", PASS,
     "d.has_transparency_and_composition = f->transparency_and_composition != VK_NULL_HANDLE;",
     "d.has_transparency_and_composition = 0;",
     "F-T", "a ghost"),
    ("F-T: T&C over the whole frame", PASS_SELFTEST,
     "tc[i] = unorm8(p.tc);",
     "tc[i] = unorm8(0.9);",
     "F-T", "the static region changes"),
    ("F-C: the reset ignored", PASS,
     "Fsr3HostDispatch d = f->params;",
     "Fsr3HostDispatch d = f->params;\n    d.reset = 0;",
     "F-C", "the old scene's share"),
    ("F-C: a reset frame's clears skipped", PASS,
     "            barrier_all(p, cmd); /* AMD clears SPD_MIPS twice on a reset */\n"
     "            clear(p, cmd, &p->img[j->resource], j->value);",
     "            if (f->params.reset) break;\n"
     "            barrier_all(p, cmd); /* AMD clears SPD_MIPS twice on a reset */\n"
     "            clear(p, cmd, &p->img[j->resource], j->value);",
     "F-C", "F-C:"),
    ("F-D: window z in place of k/w", PASS_SELFTEST,
     "    (void)window;\n    return DEPTH_K / w;",
     "    (void)w;\n    return window / 65535.0;",
     "F-D", "the uncovered pixels"),
    ("F-D: the inverted flag dropped", PASS_SELFTEST,
     "d.flags = FSR3_HOST_ENABLE_DEPTH_INVERTED | FSR3_HOST_ENABLE_DEPTH_INFINITE;",
     "d.flags = FSR3_HOST_ENABLE_DEPTH_INFINITE;",
     "F-D", "the shaders are built for inverted depth"),
]

# Run and recorded, never counted as a kill: design 2.5 asks for the result either way.
PASS_RECORDED: list[tuple[str, str, str, str, str]] = [
    ("F-T: the T&C value moved to the reactive input", PASS_SELFTEST,
     "            o->reactive = 0.0;\n            o->tc = q->tc;",
     "            o->reactive = q->tc;\n            o->tc = s->bg_tc;",
     "F-T"),
    # On a layer that needs syncval_shader_accesses_heuristic (SDK 1.4.363), the dispatch canary goes
    # quiet and the run is a SKIP; on one that checks shader accesses by default (1.3.275) it passes.
    ("the shader-access heuristic left out of the layer settings", PASS_SELFTEST,
     "    lsci.settingCount = 2;",
     "    lsci.settingCount = 1;",
     "P-0"),
]
PASS_SOURCES = [PASS, PASS_SELFTEST, HOST, JITTER]
PASS_INCLUDES = ["-Isrc/rt", "-Isrc/rt/gpu_sdl3vk", "-Ithird_party/fidelityfx_sdk/upscalers/fsr3/include/gpu"]
SKIP = 77


def build_and_run(cc: str, work: Path, host: Path, jitter: Path, tag: str) -> tuple[str, int, str]:
    exe = work / f"fsr3_host_selftest_{tag}{'.exe' if os.name == 'nt' else ''}"
    cmd = [cc, *CFLAGS, *INCLUDES, "-o", str(exe), SELFTEST, str(host), str(jitter), "-lm"]
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if built.returncode != 0:
        return "BUILD", built.returncode, built.stderr
    ran = subprocess.run([str(exe), RECORD], cwd=ROOT, capture_output=True, text=True, timeout=300)
    return "RAN", ran.returncode, ran.stdout + ran.stderr


def vulkan_includes() -> list[str]:
    """The Vulkan SDK's include folders, as the Makefile finds them, or none (system headers)."""
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        from vulkan_sdk import VulkanSdkError, discover_vulkan_sdk
        sdk = discover_vulkan_sdk()
    except (ImportError, VulkanSdkError):
        return []
    return [f"-I{sdk / name}" for name in ("Include", "include") if (sdk / name).is_dir()]


def build_and_run_pass(cc: str, work: Path, sources: dict[str, Path], tag: str, cell: str,
                       includes: list[str]) -> tuple[str, int, str]:
    exe = work / f"fsr3_pass_selftest_{tag}{'.exe' if os.name == 'nt' else ''}"
    libs = [] if os.name == "nt" else ["-ldl"]
    cmd = [cc, *CFLAGS, *PASS_INCLUDES, *includes, "-o", str(exe),
           *(str(sources[path]) for path in PASS_SOURCES), "-lm", *libs]
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if built.returncode != 0:
        return "BUILD", built.returncode, built.stderr
    ran = subprocess.run([str(exe), "--cell", cell], cwd=ROOT, capture_output=True, text=True, timeout=1800)
    return "RAN", ran.returncode, ran.stdout + ran.stderr


def measurements(out: str, cell: str) -> str:
    return "\n".join(f"    {line}" for line in out.splitlines()
                     if line.startswith((cell, "FAIL", "validation:", "synchronisation", "SKIP"))
                     and not line.startswith("validation: on"))


def mutate(work: Path, index: int, path: str, old: str, new: str) -> tuple[Path | None, str]:
    source = (ROOT / path).read_text(encoding="utf-8")
    if source.count(old) != 1:
        return None, f"the text to change appears {source.count(old)} times in {path}"
    lane = work / f"m{index:02d}"
    lane.mkdir()
    mutated = lane / Path(path).name
    mutated.write_text(source.replace(old, new), encoding="utf-8", newline="\n")
    return mutated, ""


def main_pass(args: argparse.Namespace, work: Path) -> int:
    includes = vulkan_includes()
    base = {path: ROOT / path for path in PASS_SOURCES}
    mutants = [(i, m) for i, m in enumerate(PASS_MUTANTS) if not args.cell or m[4] == args.cell]
    recorded = [(i, m) for i, m in enumerate(PASS_RECORDED, start=len(PASS_MUTANTS))
                if not args.cell or m[4] == args.cell]
    cells = sorted({m[4] for _, m in mutants} | {m[4] for _, m in recorded})
    if not cells:
        print(f"FAIL fsr3-pass-mutants: no mutant targets the cell {args.cell!r}")
        return 1
    require = os.environ.get("NK_UI_VK_REQUIRE") == "1"
    for cell in cells:
        status, rc, out = build_and_run_pass(args.cc, work, base, f"base_{cell}", cell, includes)
        if status == "BUILD":
            print(f"SKIP fsr3-pass-mutants: the pass selftest does not build here (no Vulkan headers?)\n{out}")
            return 1 if require else 0
        if rc == SKIP:
            print(f"SKIP fsr3-pass-mutants: the unmutated {cell} skips\n{out}")
            return 1 if require else 0
        if rc != 0 and not args.calibrate:
            print(f"FAIL fsr3-pass-mutants: the unmutated {cell} does not pass (exit {rc})\n{out}")
            return 1
        print(f"BASE {cell} (exit {rc})\n{measurements(out, cell)}")

    failures = 0
    for index, (name, path, old, new, cell, expect) in mutants:
        mutated, why = mutate(work, index, path, old, new)
        if mutated is None:
            print(f"INVALID {name}: {why}")
            failures += 1
            continue
        status, rc, out = build_and_run_pass(args.cc, mutated.parent, {**base, path: mutated}, f"m{index:02d}", cell,
                                             includes)
        if status == "BUILD":
            print(f"INVALID {name}: the mutant does not build\n{out}")
            failures += 1
        elif rc == 0:
            print(f"SURVIVED {name}\n{measurements(out, cell)}")
            failures += 1
        elif expect not in out:
            print(f"WRONG_REASON {name}: expected {expect!r} (exit {rc})\n{measurements(out, cell)}")
            failures += 1
        else:
            print(f"KILLED {name}\n{measurements(out, cell)}")
    for index, (name, path, old, new, cell) in recorded:
        mutated, why = mutate(work, index, path, old, new)
        if mutated is None:
            print(f"INVALID {name}: {why}")
            failures += 1
            continue
        status, rc, out = build_and_run_pass(args.cc, mutated.parent, {**base, path: mutated}, f"m{index:02d}", cell,
                                             includes)
        print(f"RECORDED {name} ({'build failed' if status == 'BUILD' else f'exit {rc}'})\n{measurements(out, cell)}")
    count = len(mutants)
    scope = f" (the {args.cell} cell only)" if args.cell else ""
    print(f"fsr3-pass-mutants: {count - failures} of {count} killed for the stated reason{scope}")
    if args.calibrate:
        print("fsr3-pass-mutants: calibration run, not a result")
        return 1
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--pass", dest="gpu", action="store_true",
                        help="run the GPU cells' mutants (fsr3_pass_selftest.c) instead of the host's")
    parser.add_argument("--calibrate", action="store_true",
                        help="with --pass: run every mutant and print its measurements even when the unmutated "
                             "cell fails (for choosing bars on a new device; never a pass)")
    parser.add_argument("--cell", help="with --pass: only the mutants of this cell, for example F-R (never the "
                                       "complete result)")
    args = parser.parse_args()
    work = args.build_dir.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    if args.gpu:
        return main_pass(args, work)

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
