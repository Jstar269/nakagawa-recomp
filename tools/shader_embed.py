#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regenerate and verify the checked-in Vulkan shader embeddings."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
GPU_DIR = ROOT / "src" / "rt" / "gpu_sdl3vk"
MANIFEST = GPU_DIR / "shader_manifest.json"
UI_DIR = ROOT / "src" / "ui"
UI_MANIFEST = ROOT / Path("src/ui/shaders/shader_manifest.json")
TARGET_ENV = "vulkan1.1"
SHADERS = (
    ("vert", Path("shaders/psp.vert"), Path("psp_vert.inc")),
    ("frag", Path("shaders/psp.frag"), Path("psp_frag.inc")),
    ("vert", Path("shaders/psp_tnl.vert"), Path("psp_tnl_vert.inc")),
    ("comp", Path("shaders/ace_match.comp"), Path("ace_match_comp.inc")),
    ("comp", Path("shaders/ace_compose.comp"), Path("ace_compose_comp.inc")),
    ("comp", Path("shaders/fsr1_easu.comp"), Path("fsr1_easu_comp.inc")),
    ("comp", Path("shaders/fsr1_rcas.comp"), Path("fsr1_rcas_comp.inc")),
)
UI_SHADERS = (
    ("vert", Path("shaders/ui.vert"), Path("shaders/ui_vert.inc")),
    ("frag", Path("shaders/ui.frag"), Path("shaders/ui_frag.inc")),
    ("frag", Path("shaders/waves.frag"), Path("shaders/waves_frag.inc")),
)
WORD_RE = re.compile(r"0x([0-9a-fA-F]{8})")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_embedding(data: str) -> bytes:
    remainder = WORD_RE.sub("", data)
    if remainder.translate(str.maketrans("", "", "{}, \t\r\n")):
        raise ValueError("embedded shader contains text outside uint32 literals")
    words = WORD_RE.findall(data)
    if not words:
        raise ValueError("embedded shader has no uint32 literals")
    return b"".join(int(word, 16).to_bytes(4, "little") for word in words)


def format_embedding(data: bytes) -> str:
    if len(data) % 4:
        raise ValueError("SPIR-V byte count is not a multiple of four")
    words = [f"0x{int.from_bytes(data[i:i + 4], 'little'):08x}" for i in range(0, len(data), 4)]
    return "{" + ",".join(words) + "}\n"


def glslc_version(glslc: str) -> str:
    proc = subprocess.run(
        [glslc, "--version"], check=True, capture_output=True, text=True, timeout=30
    )
    return (proc.stdout + proc.stderr).strip()


def compile_shader(glslc: str, source: Path, output: Path, gpu_dir: Path) -> bytes:
    relative_source = source.relative_to(gpu_dir)
    subprocess.run(
        [glslc, f"--target-env={TARGET_ENV}", str(relative_source), "-o", str(output)],
        cwd=gpu_dir,
        check=True,
        timeout=120,
    )
    return output.read_bytes()


def regenerate(
    gpu_dir: Path, manifest_path: Path, glslc: str,
    shaders: tuple[tuple[str, Path, Path], ...] = SHADERS,
) -> None:
    compiler_version = glslc_version(glslc)
    entries = []
    staged: list[tuple[Path, bytes]] = []
    with tempfile.TemporaryDirectory(prefix="nakagawa-shaders-") as temp_name:
        temp = Path(temp_name)
        for stage, source_rel, embedded_rel in shaders:
            source = gpu_dir / source_rel
            spirv = compile_shader(glslc, source, temp / f"{source.stem}.{stage}.spv", gpu_dir)
            embedded = format_embedding(spirv).encode("ascii")
            entries.append(
                {
                    "stage": stage,
                    "source": source_rel.as_posix(),
                    "embedded": embedded_rel.as_posix(),
                    "source_sha256": sha256(source.read_bytes()),
                    "spirv_sha256": sha256(spirv),
                    "spirv_bytes": len(spirv),
                    "embedded_sha256": sha256(embedded),
                }
            )
            staged.append((gpu_dir / embedded_rel, embedded))

    manifest = {
        "schema_version": 1,
        "compiler": {
            "name": Path(glslc).name,
            "version": compiler_version,
            "target_environment": TARGET_ENV,
            "options": [f"--target-env={TARGET_ENV}"],
        },
        "shaders": entries,
    }
    for output, data in staged:
        output.write_bytes(data)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("shaders"), list):
        raise ValueError("unsupported or malformed shader manifest")
    compiler = manifest.get("compiler", {})
    if compiler.get("target_environment") != TARGET_ENV:
        raise ValueError("shader manifest target environment is not vulkan1.1")
    if not compiler.get("version"):
        raise ValueError("shader manifest does not record compiler identity/version")
    returned: dict[str, Any] = manifest
    return returned


def verify(
    gpu_dir: Path, manifest_path: Path, glslc: str | None = None,
    shaders: tuple[tuple[str, Path, Path], ...] = SHADERS,
) -> list[str]:
    errors: list[str] = []
    try:
        manifest = load_manifest(manifest_path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return [f"manifest: {exc}"]

    expected = {(stage, source.as_posix(), embedded.as_posix()) for stage, source, embedded in shaders}
    actual = {
        (entry.get("stage"), entry.get("source"), entry.get("embedded"))
        for entry in manifest["shaders"]
    }
    if actual != expected:
        errors.append("shader manifest file set does not match the build contract")

    embedded_spirv: dict[tuple[str, str], bytes] = {}
    for entry in manifest["shaders"]:
        source = gpu_dir / entry.get("source", "")
        embedded = gpu_dir / entry.get("embedded", "")
        try:
            source_data = source.read_bytes()
            embedded_data = embedded.read_bytes()
            spirv = parse_embedding(embedded_data.decode("ascii"))
        except (OSError, UnicodeError, ValueError) as exc:
            errors.append(f"{entry.get('stage', '?')}: {exc}")
            continue
        checks = (
            ("source SHA-256", sha256(source_data), entry.get("source_sha256")),
            ("embedded SHA-256", sha256(embedded_data), entry.get("embedded_sha256")),
            ("SPIR-V SHA-256", sha256(spirv), entry.get("spirv_sha256")),
            ("SPIR-V byte count", len(spirv), entry.get("spirv_bytes")),
        )
        for label, observed, recorded in checks:
            if observed != recorded:
                errors.append(
                    f"{entry.get('stage', '?')}: {label} mismatch ({observed} != {recorded})"
                )
        embedded_spirv[(entry.get("stage", ""), entry.get("source", ""))] = spirv

    if glslc and not errors:
        with tempfile.TemporaryDirectory(prefix="nakagawa-shader-verify-") as temp_name:
            temp = Path(temp_name)
            for stage, source_rel, _ in shaders:
                try:
                    rebuilt = compile_shader(
                        glslc, gpu_dir / source_rel,
                        temp / f"{source_rel.stem}.{stage}.spv", gpu_dir
                    )
                except (OSError, subprocess.SubprocessError) as exc:
                    errors.append(f"{stage}: reproducibility compile failed: {exc}")
                    continue
                key = (stage, source_rel.as_posix())
                if rebuilt != embedded_spirv.get(key):
                    errors.append(f"{stage}: current glslc output differs from embedded SPIR-V")
    return errors


# --- Schema 2: SPIR-V compiled with DXC from imported HLSL -----------------------------------
#
# The FSR 3.1.6 upscaler's compute passes are AMD's HLSL in third_party/fidelityfx_sdk/
# (PROVENANCE.md there), compiled with a pinned DXC release. Each entry records the compiler,
# its full option list, every permutation define and the SHA-256 of every file the source
# includes, so a change to any header fails verify. Paths are relative to the repository root.

FSR3_MANIFEST = GPU_DIR / "fsr3_shader_manifest.json"
FSR3_SHADER_DIR = Path("third_party/fidelityfx_sdk/upscalers/fsr3/internal/shaders")
FSR3_PASSES = (
    "prepare_inputs", "luma_pyramid", "shading_change_pyramid", "shading_change",
    "prepare_reactivity", "luma_instability", "accumulate", "rcas",
)
FSR3_SHADERS = tuple(
    ("comp", FSR3_SHADER_DIR / f"ffx_fsr3upscaler_{name}_pass.hlsl",
     Path(f"src/rt/gpu_sdl3vk/fsr3_{name}_comp.inc"))
    for name in FSR3_PASSES
)
# The reference compiler: DirectXShaderCompiler release v1.9.2609, both archives pinned by
# SHA-256. Its Windows and Linux builds and the Vulkan SDK 1.4.363.0 DXC (the same commit,
# 01b62ad47) give identical bytes for these passes; the hosted job checks the Linux build.
DXC_RELEASE = {
    "tag": "v1.9.2609",
    "commit": "01b62ad47",
    "windows_archive": "dxc_2026_09_29.zip",
    "windows_sha256": "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1",
    "linux_archive": "linux_dxc_2026_09_28.x86_x64.tar.gz",
    "linux_sha256": "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1",
}
DXC_OPTIONS = (
    "-T", "cs_6_2", "-E", "CS", "-spirv", f"-fspv-target-env={TARGET_ENV}", "-HV", "2021",
    "-fspv-use-unknown-image-format",
    "-fvk-b-shift", "0", "0", "-fvk-s-shift", "4", "0", "-fvk-t-shift", "8", "0", "-fvk-u-shift", "40", "0",
    "-I", "../../include/gpu",
)
# Every permutation define, passed explicitly so no header default decides a byte.
FSR3_DEFINES = (
    "FFX_GPU=1", "FFX_HLSL=1", "FFX_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "FFX_SPD_NO_WAVE_OPERATIONS=1",
)
# FSR 3.1.6 frame generation: ten frame-interpolation passes (all but AMD's debug view) and the
# seven optical-flow passes, from third_party/fidelityfx_sdk/framegeneration/fsr3/. The embedding
# names drop AMD's ffx_ prefix and _pass suffix and shorten frameinterpolation and opticalflow to
# fi and of; ffx_frameinterpolation_pass.hlsl, the interpolation itself, becomes fi_interpolation.
FSR3FG_MANIFEST = GPU_DIR / "fsr3fg_shader_manifest.json"
FSR3FG_SHADER_DIR = Path("third_party/fidelityfx_sdk/framegeneration/fsr3/internal/shaders")
FSR3FG_PASSES = (
    ("fi_reconstruct_and_dilate", "ffx_frameinterpolation_reconstruct_and_dilate_pass"),
    ("fi_setup", "ffx_frameinterpolation_setup_pass"),
    ("fi_reconstruct_previous_depth", "ffx_frameinterpolation_reconstruct_previous_depth_pass"),
    ("fi_game_motion_vector_field", "ffx_frameinterpolation_game_motion_vector_field_pass"),
    ("fi_optical_flow_vector_field", "ffx_frameinterpolation_optical_flow_vector_field_pass"),
    ("fi_disocclusion_mask", "ffx_frameinterpolation_disocclusion_mask_pass"),
    ("fi_interpolation", "ffx_frameinterpolation_pass"),
    ("fi_compute_inpainting_pyramid", "ffx_frameinterpolation_compute_inpainting_pyramid_pass"),
    ("fi_inpainting", "ffx_frameinterpolation_inpainting_pass"),
    ("fi_compute_game_vector_field_inpainting_pyramid",
     "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass"),
    ("of_prepare_luma", "ffx_opticalflow_prepare_luma_pass"),
    ("of_compute_luminance_pyramid", "ffx_opticalflow_compute_luminance_pyramid_pass"),
    ("of_generate_scd_histogram", "ffx_opticalflow_generate_scd_histogram_pass"),
    ("of_compute_scd_divergence", "ffx_opticalflow_compute_scd_divergence_pass"),
    ("of_compute_optical_flow_advanced_v5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5"),
    ("of_filter_optical_flow_v5", "ffx_opticalflow_filter_optical_flow_pass_v5"),
    ("of_scale_optical_flow_advanced_v5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"),
)
FSR3FG_SHADERS = tuple(
    ("comp", FSR3FG_SHADER_DIR / f"{source}.hlsl", Path(f"src/rt/gpu_sdl3vk/fsr3fg_{name}_comp.inc"))
    for name, source in FSR3FG_PASSES
)
# The shaders test FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS; AMD's permutation key sets
# a differently named ..._JITTER_MOTION_VECTORS, so the name the shaders read is the one passed.
# Optical flow has no permutation define the shaders read (its HDR key is unused), and runs with
# 32-wide subgroups: FFX_PREFER_WAVE64 stays undefined.
FSR3FG_DEFINES = (
    "FFX_GPU=1", "FFX_HLSL=1", "FFX_HALF=0",
    "FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
    "FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0",
    "FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=1",
    "FFX_SPD_NO_WAVE_OPERATIONS=1",
)
DXC_SETS = {
    "fsr3": (FSR3_MANIFEST, FSR3_SHADERS, FSR3_DEFINES),
    "fsr3fg": (FSR3FG_MANIFEST, FSR3FG_SHADERS, FSR3FG_DEFINES),
}
_LINE_MARKER = re.compile(r'^#line \d+ "([^"]+)"', re.MULTILINE)


def dxc_version(dxc: str) -> str:
    proc = subprocess.run([dxc, "--version"], check=True, capture_output=True, text=True, timeout=30)
    return (proc.stdout + proc.stderr).strip()


def dxc_is_pinned(version: str) -> bool:
    """The pinned release's compiler library reports `1.9(<build>-01b62ad4)(1.9.2609.<n>)`.

    The Windows dxc adds ` - 1.9.2609.<n> (01b62ad47)` after it; the Linux one prints the library
    part alone, so the check reads the part both share.
    """
    commit = re.escape(DXC_RELEASE["commit"][:8])
    return re.search(rf"\(\d+-{commit}\)\(1\.9\.2609\.\d+\)", version) is not None


def _dxc_args(defines: tuple[str, ...] = FSR3_DEFINES) -> list[str]:
    args = list(DXC_OPTIONS)
    for define in defines:
        args += ["-D", define]
    return args


def compile_dxc(
    dxc: str, root: Path, source_rel: Path, output: Path, defines: tuple[str, ...] = FSR3_DEFINES,
) -> bytes:
    source = root / source_rel
    subprocess.run(
        [dxc, *_dxc_args(defines), source.name, "-Fo", str(output)],
        cwd=source.parent, check=True, timeout=300, capture_output=True,
    )
    return output.read_bytes()


def include_closure(
    dxc: str, root: Path, source_rel: Path, temp: Path, defines: tuple[str, ...] = FSR3_DEFINES,
) -> list[str]:
    """Every file the source includes, from DXC's preprocessed output, repository-relative."""
    source = root / source_rel
    out = temp / f"{source.stem}.i"
    subprocess.run(
        [dxc, *_dxc_args(defines), "-P", "-Fi", str(out), source.name],
        cwd=source.parent, check=True, timeout=300, capture_output=True,
    )
    found = set()
    for name in _LINE_MARKER.findall(out.read_text(encoding="utf-8", errors="replace")):
        path = (source.parent / name.replace("\\\\", "/").replace("\\", "/")).resolve()
        rel = path.relative_to(root.resolve()).as_posix()
        if rel != source_rel.as_posix():
            found.add(rel)
    return sorted(found)


def regenerate_dxc(
    root: Path, manifest_path: Path, dxc: str,
    shaders: tuple[tuple[str, Path, Path], ...] = FSR3_SHADERS,
    defines: tuple[str, ...] = FSR3_DEFINES,
) -> None:
    version = dxc_version(dxc)
    if not dxc_is_pinned(version):
        raise ValueError(f"regenerate needs the pinned DXC {DXC_RELEASE['tag']}, not: {version}")
    entries = []
    staged: list[tuple[Path, bytes]] = []
    with tempfile.TemporaryDirectory(prefix="nakagawa-dxc-") as temp_name:
        temp = Path(temp_name)
        for stage, source_rel, embedded_rel in shaders:
            spirv = compile_dxc(dxc, root, source_rel, temp / f"{source_rel.stem}.spv", defines)
            embedded = format_embedding(spirv).encode("ascii")
            closure = include_closure(dxc, root, source_rel, temp, defines)
            entries.append({
                "stage": stage,
                "source": source_rel.as_posix(),
                "embedded": embedded_rel.as_posix(),
                "source_sha256": sha256((root / source_rel).read_bytes()),
                "includes": [{"path": p, "sha256": sha256((root / p).read_bytes())} for p in closure],
                "spirv_sha256": sha256(spirv),
                "spirv_bytes": len(spirv),
                "embedded_sha256": sha256(embedded),
            })
            staged.append((root / embedded_rel, embedded))
    manifest = {
        "schema_version": 2,
        "compiler": {
            "name": "dxc",
            "release": DXC_RELEASE,
            "version": version,
            "target_environment": TARGET_ENV,
            "options": list(DXC_OPTIONS),
            "defines": list(defines),
        },
        "shaders": entries,
    }
    for output, data in staged:
        output.write_bytes(data)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def verify_dxc(
    root: Path, manifest_path: Path, dxc: str | None = None,
    shaders: tuple[tuple[str, Path, Path], ...] = FSR3_SHADERS,
    defines: tuple[str, ...] = FSR3_DEFINES,
) -> tuple[list[str], str | None]:
    """Errors, and a SKIP reason when byte reproduction was asked for but not checked."""
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"manifest: {exc}"], None
    if manifest.get("schema_version") != 2 or not isinstance(manifest.get("shaders"), list):
        return ["manifest: unsupported or malformed schema 2 shader manifest"], None
    compiler = manifest.get("compiler", {})
    errors: list[str] = []
    expected_compiler = {
        "name": "dxc", "release": DXC_RELEASE, "target_environment": TARGET_ENV,
        "options": list(DXC_OPTIONS), "defines": list(defines),
    }
    for key, value in expected_compiler.items():
        if compiler.get(key) != value:
            errors.append(f"compiler {key} does not match the build contract")
    if not compiler.get("version"):
        errors.append("manifest does not record the compiler version")
    expected = {(s, src.as_posix(), emb.as_posix()) for s, src, emb in shaders}
    actual = {(e.get("stage"), e.get("source"), e.get("embedded")) for e in manifest["shaders"]}
    if actual != expected:
        errors.append("shader manifest file set does not match the build contract")

    embedded_spirv: dict[str, bytes] = {}
    for entry in manifest["shaders"]:
        label = entry.get("source", "?")
        try:
            source_data = (root / entry.get("source", "")).read_bytes()
            embedded_data = (root / entry.get("embedded", "")).read_bytes()
            spirv = parse_embedding(embedded_data.decode("ascii"))
        except (OSError, UnicodeError, ValueError) as exc:
            errors.append(f"{label}: {exc}")
            continue
        checks = [
            ("source SHA-256", sha256(source_data), entry.get("source_sha256")),
            ("embedded SHA-256", sha256(embedded_data), entry.get("embedded_sha256")),
            ("SPIR-V SHA-256", sha256(spirv), entry.get("spirv_sha256")),
            ("SPIR-V byte count", len(spirv), entry.get("spirv_bytes")),
        ]
        includes = entry.get("includes")
        if not isinstance(includes, list) or not includes:
            errors.append(f"{label}: no include closure recorded")
            includes = []
        for inc in includes:
            try:
                include_sha256 = sha256((root / inc["path"]).read_bytes())
            except (OSError, KeyError, TypeError) as exc:
                errors.append(f"{label}: include {exc}")
                continue
            checks.append((f"include {inc['path']} SHA-256", include_sha256, inc.get("sha256")))
        for what, observed, recorded in checks:
            if observed != recorded:
                errors.append(f"{label}: {what} mismatch ({observed} != {recorded})")
        embedded_spirv[label] = spirv

    if dxc is None or errors:
        return errors, None
    version = dxc_version(dxc)
    if not dxc_is_pinned(version):
        return errors, f"dxc is not the pinned {DXC_RELEASE['tag']} ({version}); byte reproduction not checked"
    with tempfile.TemporaryDirectory(prefix="nakagawa-dxc-verify-") as temp_name:
        temp = Path(temp_name)
        recorded = {e["source"]: sorted(i["path"] for i in e["includes"]) for e in manifest["shaders"]}
        for _, source_rel, _ in shaders:
            key = source_rel.as_posix()
            try:
                rebuilt = compile_dxc(dxc, root, source_rel, temp / f"{source_rel.stem}.spv", defines)
                closure = include_closure(dxc, root, source_rel, temp, defines)
            except (OSError, subprocess.SubprocessError) as exc:
                errors.append(f"{key}: reproducibility compile failed: {exc}")
                continue
            if rebuilt != embedded_spirv.get(key):
                errors.append(f"{key}: pinned dxc output differs from embedded SPIR-V")
            if closure != recorded.get(key):
                errors.append(f"{key}: include closure differs from the manifest")
    return errors, None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("regenerate", "verify"))
    parser.add_argument("--shader-set", choices=("psp", "ui", "fsr3", "fsr3fg"), default="psp")
    parser.add_argument("--gpu-dir", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--glslc")
    parser.add_argument("--dxc", help="DXC for the fsr3 and fsr3fg sets (the pinned release)")
    parser.add_argument("--recompile", action="store_true")
    return parser.parse_args()


def main_dxc(args: argparse.Namespace) -> int:
    name = args.shader_set
    default_manifest, shaders, defines = DXC_SETS[name]
    manifest = args.manifest or default_manifest
    if args.action == "regenerate":
        if not args.dxc:
            print(f"--dxc (the pinned release) is required to regenerate the {name} shader set", file=sys.stderr)
            return 2
        try:
            regenerate_dxc(ROOT, manifest, args.dxc, shaders, defines)
        except ValueError as exc:
            print(f"shader regeneration refused: {exc}", file=sys.stderr)
            return 2
        print(f"{name} shader embeddings regenerated with {args.dxc}")
        return 0
    if args.recompile and not args.dxc:
        print(f"--dxc is required for byte-for-byte reproduction of the {name} shader set", file=sys.stderr)
        return 2
    errors, skipped = verify_dxc(ROOT, manifest, args.dxc if args.recompile else None, shaders, defines)
    if errors:
        for error in errors:
            print(f"shader verification failed: {error}", file=sys.stderr)
        return 1
    if skipped:
        # Exit 77, the selftests' SKIP: reproduction was asked for and did not happen, so a caller
        # that needs it (the hosted job) fails, and make reports the SKIP.
        print(f"{name} shader provenance verified; SKIP byte reproduction: {skipped}")
        return 77
    suffix = " plus byte-for-byte pinned-DXC reproduction" if args.recompile else ""
    print(f"{name} shader provenance verified (sources, include closures, embeddings){suffix}")
    return 0


def main() -> int:
    args = parse_args()
    if args.shader_set in DXC_SETS:
        return main_dxc(args)
    glslc = args.glslc or shutil.which("glslc")
    gpu_dir = args.gpu_dir or (UI_DIR if args.shader_set == "ui" else GPU_DIR)
    manifest = args.manifest or (UI_MANIFEST if args.shader_set == "ui" else MANIFEST)
    shaders = UI_SHADERS if args.shader_set == "ui" else SHADERS
    if args.action == "regenerate":
        if not glslc:
            print("glslc is required to regenerate shader embeddings", file=sys.stderr)
            return 2
        regenerate(gpu_dir, manifest, glslc, shaders)
        print(f"shader embeddings regenerated with {glslc} ({TARGET_ENV})")
        return 0
    if args.recompile and not glslc:
        print("glslc is required for byte-for-byte shader reproduction", file=sys.stderr)
        return 2
    errors = verify(gpu_dir, manifest, glslc if args.recompile else None, shaders)
    if errors:
        for error in errors:
            print(f"shader verification failed: {error}", file=sys.stderr)
        return 1
    suffix = " plus byte-for-byte compiler reproduction" if args.recompile else ""
    print(f"shader provenance verified{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
