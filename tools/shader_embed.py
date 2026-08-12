#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regenerate and verify the checked-in Vulkan shader embeddings."""

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
import tempfile


ROOT = Path(__file__).resolve().parents[1]
GPU_DIR = ROOT / "src" / "rt" / "gpu_sdl3vk"
MANIFEST = GPU_DIR / "shader_manifest.json"
TARGET_ENV = "vulkan1.1"
SHADERS = (
    ("vert", Path("shaders/psp.vert"), Path("psp_vert.inc")),
    ("frag", Path("shaders/psp.frag"), Path("psp_frag.inc")),
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


def regenerate(gpu_dir: Path, manifest_path: Path, glslc: str) -> None:
    compiler_version = glslc_version(glslc)
    entries = []
    staged: list[tuple[Path, bytes]] = []
    with tempfile.TemporaryDirectory(prefix="nakagawa-shaders-") as temp_name:
        temp = Path(temp_name)
        for stage, source_rel, embedded_rel in SHADERS:
            source = gpu_dir / source_rel
            spirv = compile_shader(glslc, source, temp / f"psp.{stage}.spv", gpu_dir)
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
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("shaders"), list):
        raise ValueError("unsupported or malformed shader manifest")
    compiler = manifest.get("compiler", {})
    if compiler.get("target_environment") != TARGET_ENV:
        raise ValueError("shader manifest target environment is not vulkan1.1")
    if not compiler.get("version"):
        raise ValueError("shader manifest does not record compiler identity/version")
    return manifest


def verify(gpu_dir: Path, manifest_path: Path, glslc: str | None = None) -> list[str]:
    errors: list[str] = []
    try:
        manifest = load_manifest(manifest_path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return [f"manifest: {exc}"]

    expected = {(stage, source.as_posix(), embedded.as_posix()) for stage, source, embedded in SHADERS}
    actual = {
        (entry.get("stage"), entry.get("source"), entry.get("embedded"))
        for entry in manifest["shaders"]
    }
    if actual != expected:
        errors.append("shader manifest file set does not match the build contract")

    embedded_spirv: dict[str, bytes] = {}
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
        embedded_spirv[entry.get("stage", "")] = spirv

    if glslc and not errors:
        with tempfile.TemporaryDirectory(prefix="nakagawa-shader-verify-") as temp_name:
            temp = Path(temp_name)
            for stage, source_rel, _ in SHADERS:
                try:
                    rebuilt = compile_shader(
                        glslc, gpu_dir / source_rel, temp / f"psp.{stage}.spv", gpu_dir
                    )
                except (OSError, subprocess.SubprocessError) as exc:
                    errors.append(f"{stage}: reproducibility compile failed: {exc}")
                    continue
                if rebuilt != embedded_spirv.get(stage):
                    errors.append(f"{stage}: current glslc output differs from embedded SPIR-V")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("regenerate", "verify"))
    parser.add_argument("--gpu-dir", type=Path, default=GPU_DIR)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--glslc")
    parser.add_argument("--recompile", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    glslc = args.glslc or shutil.which("glslc")
    if args.action == "regenerate":
        if not glslc:
            print("glslc is required to regenerate shader embeddings", file=sys.stderr)
            return 2
        regenerate(args.gpu_dir, args.manifest, glslc)
        print(f"shader embeddings regenerated with {glslc} ({TARGET_ENV})")
        return 0
    if args.recompile and not glslc:
        print("glslc is required for byte-for-byte shader reproduction", file=sys.stderr)
        return 2
    errors = verify(args.gpu_dir, args.manifest, glslc if args.recompile else None)
    if errors:
        for error in errors:
            print(f"shader verification failed: {error}", file=sys.stderr)
        return 1
    suffix = " plus byte-for-byte compiler reproduction" if args.recompile else ""
    print(f"shader provenance verified{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
