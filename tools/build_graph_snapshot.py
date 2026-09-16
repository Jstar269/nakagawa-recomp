#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Read-only snapshot of the current Make build graph (baseline data).

Observational tool: it never mutates build state and never drives a build.
It records, for an existing build directory:

  - toolchain/config profile hashes (build/<game>/*_profile.json)
  - every object unit with its compiler-discovered prerequisites (.d files),
    categorized into source / runtime header / generated header / profile
    stamp / other
  - the generated-unit set (chunk discovery via $(wildcard) semantics)
  - output sizes (exe, objects, generated C) and TU counts

This is the measured baseline the future compiler-neutral build manifest
specification (see the wave-1 build/HLE portability handoff) will be judged
against. The snapshot is deliberately not a build driver.

CLI:
    python tools/build_graph_snapshot.py --build-dir build/hst
        [--out build/build_graph_snapshot.json] [--game hst]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1

_PROFILE_RE = re.compile(r"^(.+?)_profile\.json$")
_D_TARGET_RE = re.compile(r"^(.*?):\s*(.*)$")


def _join_continuations(text: str) -> list[str]:
    """Join backslash-continued Make dependency lines into logical rules."""
    logical: list[str] = []
    current = ""
    for line in text.splitlines():
        continued = line.endswith("\\")
        current += line[:-1] if continued else line
        current += " "
        if not continued:
            logical.append(current)
            current = ""
    if current.strip():
        logical.append(current)
    return logical


def _parse_d(path: Path) -> dict[str, list[str]]:
    """Parse a Make -MMD dependency file: target -> prerequisites."""
    out: dict[str, list[str]] = {}
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for line in _join_continuations(text):
        if not line.strip():
            continue
        m = _D_TARGET_RE.match(line)
        if not m:
            continue
        target = m.group(1).strip()
        # -MP emits empty phony rules for headers (deleted-header safety);
        # they are not build units, only real object rules are.
        if not target.endswith(".o"):
            continue
        prereqs = [tok for tok in m.group(2).split() if tok.strip()]
        if target in out:
            # gcc can emit the same object rule more than once (e.g. when the
            # include closure grows between -MMD runs); merge, never drop.
            out[target] = list(dict.fromkeys(out[target] + prereqs))
        else:
            out[target] = prereqs
    return out


def _categorize(prereq: str, build_dir: str, game: str) -> str:
    if prereq.startswith(build_dir):
        if "atrac3p" in prereq or "portable-core" in prereq:
            return "build_generated_or_objdir"
        if game in prereq and ("_recomp" in prereq or prereq.endswith(".c")):
            return "generated_source_or_header"
        return "build_artifact"
    if prereq.endswith(".h") or prereq.endswith(".inc"):
        if prereq.startswith("src/"):
            return "runtime_header"
        return "other_header"
    if prereq.startswith("src/") and prereq.endswith(".c"):
        return "runtime_source"
    if prereq.startswith("fixtures/") or prereq.startswith("assets/"):
        return "data_or_fixture"
    if prereq.endswith("_profile-") or "-profile-" in prereq:
        return "profile_stamp"
    return "other"


def snapshot(build_dir: Path, game: str) -> dict:
    if not build_dir.is_dir():
        raise SystemExit(f"build dir not found: {build_dir}")
    profile_hashes: dict[str, str] = {}
    for p in sorted(build_dir.glob("*_profile.json")):
        m = _PROFILE_RE.match(p.name)
        if not m:
            continue
        try:
            doc = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        section = next(iter(doc.get("sections", {}).values()), {})
        profile_hashes[m.group(1)] = section.get("profile_hash", "")

    try:
        bd_norm = str(build_dir.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        bd_norm = str(build_dir).replace("\\", "/")

    units: list[dict] = []
    seen_objs: set[str] = set()
    for d_file in sorted(build_dir.rglob("*.d")):
        parsed = _parse_d(d_file)
        for target, prereqs in parsed.items():
            target_rel = target.replace("\\", "/")
            if target_rel in seen_objs:
                continue
            seen_objs.add(target_rel)
            obj = ROOT / target_rel
            units.append(
                {
                    "object": target_rel,
                    "object_exists": obj.exists(),
                    "prereq_count": len(prereqs),
                    "prereqs": {
                        cat: sorted({p.replace("\\", "/") for p in prereqs if _categorize(p, bd_norm, game) == cat})
                        for cat in ("runtime_source", "runtime_header", "generated_source_or_header",
                                    "build_artifact", "profile_stamp", "data_or_fixture", "other", "other_header")
                    },
                }
            )

    gen_sources = sorted(build_dir.glob(f"{game}_recomp*.c"))
    gen_objs = sorted(build_dir.glob(f"{game}_recomp*.o"))
    exe = build_dir / f"{game}.exe"
    obj_bytes = sum(p.stat().st_size for p in build_dir.rglob("*.o") if p.is_file())
    gen_bytes = sum(p.stat().st_size for p in gen_sources)

    return {
        "schema": SCHEMA,
        "game": game,
        "build_dir": str(build_dir).replace("\\", "/"),
        "profile_hashes": profile_hashes,
        "generated_units": {
            "source_files": [str(p.relative_to(build_dir)).replace("\\", "/") for p in gen_sources],
            "object_files": [str(p.relative_to(build_dir)).replace("\\", "/") for p in gen_objs],
            "generated_c_bytes": gen_bytes,
            "chunk_count": len(gen_sources) - 1 if gen_sources else 0,
        },
        "outputs": {
            "exe": str(exe.relative_to(build_dir)).replace("\\", "/") if exe.exists() else None,
            "exe_bytes": exe.stat().st_size if exe.exists() else None,
            "object_bytes": obj_bytes,
            "object_count": sum(1 for _ in build_dir.rglob("*.o")),
        },
        "units": units,
        "method": "read-only scan of build artifacts (.d dependency files, profile JSONs, objects); no build state mutated",
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build" / "hst")
    ap.add_argument("--game", default="hst")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)
    snap = snapshot(args.build_dir, args.game)
    out = args.out or (ROOT / "build" / f"build_graph_snapshot_{args.game}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        json.dump(snap, f, indent=2, sort_keys=True)
        f.write("\n")
    print(
        f"build_graph_snapshot: {len(snap['units'])} units, "
        f"{snap['generated_units']['chunk_count']} generated chunks, "
        f"{snap['outputs']['object_count']} objects -> {out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
