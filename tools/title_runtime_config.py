#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Emit the build-local runtime title configuration consumed by ``src/rt/title_config.c``.

The compiled runtime carries no title identity of its own. This generator turns a
*validated* public title manifest's optional ``runtime_bindings`` block into a
deterministic build-local C header under ``build/<game>/``; with no manifest it emits
the generic configuration in which every optional binding is disabled.

It deliberately depends on the manifest alone: no guest executable, no analysis
product, and no generated retail translation unit is read, so a generic
``runtime-objects`` build never needs a retail input to compile.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from typing import Any

import title_manifest

#: Bumped only when the emitted macro contract changes. ``src/rt/title_config.c``
#: refuses to compile against a different value, so a stale generated header is a
#: build failure rather than a silently wrong runtime.
GENERATED_SCHEMA_VERSION = 3

#: Emitted field -> the C validity bit that gates it. Fields sharing a bit are a
#: configured-together group; the manifest validator already enforces the pairing.
FIELD_BITS: dict[str, str] = {
    "fallback_entry": "SR_TITLE_CFG_FALLBACK_ENTRY",
    "worker_thread_entry": "SR_TITLE_CFG_WORKER_ENTRY",
    "launcher_thread_entry": "SR_TITLE_CFG_LAUNCHER_ENTRY",
    "vblank_frame_counter_addr": "SR_TITLE_CFG_VBLANK_COUNTERS",
    "vblank_vsync_counter_addr": "SR_TITLE_CFG_VBLANK_COUNTERS",
    "libfont_ready_flag_addr": "SR_TITLE_CFG_LIBFONT_READY",
    "frame_ready_latch_addr": "SR_TITLE_CFG_FRAME_LATCH",
}

#: Emitted collection -> the C validity bit that gates it. A collection counts as
#: configured exactly when the manifest supplied a non-empty one; the validator
#: rejects an empty array, so a set bit always means at least one entry.
COLLECTION_BITS: dict[str, str] = {
    "dispatch_aliases": "SR_TITLE_CFG_DISPATCH_ALIASES",
    "callback_terminators": "SR_TITLE_CFG_CALLBACK_TERMINATORS",
}

#: Emitted object -> the C validity bit that gates it. An object is atomic:
#: either fully configured or absent.
OBJECT_BITS: dict[str, str] = {
    "display_bringup": "SR_TITLE_CFG_DISPLAY_BRINGUP",
    "runtime_sync": "SR_TITLE_CFG_RUNTIME_SYNC",
}

GENERIC_SOURCE_ID = "none"

#: Suffix that continues a C macro definition onto the next line.
CONT = " " + chr(92)


class TitleRuntimeConfigError(ValueError):
    """Fail-closed generation error."""


def bindings_from_manifest(manifest: dict[str, Any] | None) -> dict[str, Any]:
    """Return the normalized binding set for a manifest (or the generic empty set)."""
    if manifest is None:
        return {"source_id": GENERIC_SOURCE_ID, "bindings": {}}
    normalized = title_manifest.validate_manifest(manifest)
    block = dict(normalized.get("runtime_bindings") or {})
    block.pop("schema_version", None)
    unknown = sorted(set(block) - set(FIELD_BITS) - set(COLLECTION_BITS) - set(OBJECT_BITS))
    if unknown:
        # Unreachable through the validator; a fail-closed guard against a future
        # manifest field silently reaching the runtime without a C binding.
        raise TitleRuntimeConfigError(
            "runtime binding(s) have no runtime representation: " + ", ".join(unknown)
        )
    return {"source_id": normalized["id"], "bindings": block}


def config_digest(config: dict[str, Any]) -> str:
    """Deterministic identity of the emitted configuration.

    The build binds this into the runtime profile hash, so changing a title binding
    invalidates every stale runtime object rather than relinking one silently.
    """
    payload = {
        "generated_schema_version": GENERATED_SCHEMA_VERSION,
        "source_id": config["source_id"],
        "bindings": config["bindings"],
    }
    rendered = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(rendered.encode("utf-8")).hexdigest()


def binding_summary(config: dict[str, Any]) -> str:
    """Human-readable count of what the effective configuration actually binds."""
    bindings: dict[str, Any] = config["bindings"]
    parts = [f"{sum(1 for name in bindings if name in FIELD_BITS)} binding(s)"]
    for name in COLLECTION_BITS:
        entries = bindings.get(name, [])
        if entries:
            parts.append(f"{len(entries)} {name.replace('_', ' ')}")
    for name in OBJECT_BITS:
        if name in bindings:
            obj = bindings[name]
            if name == "runtime_sync":
                parts.append(f"{len(obj.get('wrappers', []))} runtime sync wrappers")
            else:
                parts.append(f"1 {name.replace('_', ' ')}")
    return ", ".join(parts)


def render_header(config: dict[str, Any]) -> str:
    bindings: dict[str, Any] = config["bindings"]
    # Fail closed on a binding with no C representation rather than filtering it out: a
    # silently dropped field would reach neither the header nor an error, and the build
    # would look successful while the binding did nothing.
    unrepresentable = sorted(set(bindings) - set(FIELD_BITS) - set(COLLECTION_BITS) - set(OBJECT_BITS))
    if unrepresentable:
        raise TitleRuntimeConfigError(
            "runtime binding(s) have no runtime representation: " + ", ".join(unrepresentable)
        )
    valid = sorted({FIELD_BITS[name] for name in bindings if name in FIELD_BITS} |
                   {COLLECTION_BITS[name] for name in bindings if name in COLLECTION_BITS} |
                   {OBJECT_BITS[name] for name in bindings if name in OBJECT_BITS})
    valid_text = " | ".join(valid) if valid else "0u"
    source_id = config["source_id"]
    if '"' in source_id or "\\" in source_id:
        raise TitleRuntimeConfigError("title id is not representable as a C string literal")
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/* Copyright (C) 2026 the Nakagawa Recomp authors */",
        "/* GENERATED by tools/title_runtime_config.py -- do not edit.",
        " * Derived from validated title configuration only; contains no analysis product",
        " * and no guest bytes. A build with no title configuration emits every optional",
        " * binding disabled. */",
        "#ifndef SR_TITLE_CONFIG_GENERATED_H",
        "#define SR_TITLE_CONFIG_GENERATED_H",
        "",
        f"#define SR_TITLE_CONFIG_SCHEMA_VERSION {GENERATED_SCHEMA_VERSION}",
        f'#define SR_TITLE_CONFIG_SOURCE_ID "{source_id}"',
        f'#define SR_TITLE_CONFIG_DIGEST "{config_digest(config)}"',
        f"#define SR_TITLE_CONFIG_VALID ({valid_text})",
        "",
    ]
    for name in FIELD_BITS:
        macro = "SR_TITLE_CONFIG_" + name.upper()
        value = bindings.get(name, 0)
        lines.append(f"#define {macro} 0x{value:08x}u")

    # Collections are emitted as X-macro lists so title_config.c owns the C type and
    # the generated artifact stays a pure data statement. An unconfigured collection
    # emits a zero count and an empty list, which is what a generic build compiles.
    aliases: list[dict[str, int]] = bindings.get('dispatch_aliases', [])
    lines += ['', f'#define SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT {len(aliases)}',
              '#define SR_TITLE_CONFIG_DISPATCH_ALIAS_LIST' + (CONT if aliases else '')]
    for index, alias in enumerate(aliases):
        tail = CONT if index + 1 < len(aliases) else ''
        lines.append(
            f"    SR_TITLE_CFG_ALIAS(0x{alias['from']:08x}u, 0x{alias['to']:08x}u){tail}")

    terminators: list[dict[str, int]] = bindings.get('callback_terminators', [])
    lines += ['', f'#define SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT {len(terminators)}',
              '#define SR_TITLE_CONFIG_CALLBACK_TERMINATOR_LIST' + (CONT if terminators else '')]
    for index, entry in enumerate(terminators):
        tail = CONT if index + 1 < len(terminators) else ''
        # (sentinel, has_pc, pc, has_ra, ra): an absent constraint emits has_*=0, so
        # the runtime never compares a call site against a placeholder address.
        lines.append(
            '    SR_TITLE_CFG_TERMINATOR('
            f"0x{entry['sentinel']:08x}u, "
            f"{1 if 'pc' in entry else 0}u, 0x{entry.get('pc', 0):08x}u, "
            f"{1 if 'ra' in entry else 0}u, 0x{entry.get('ra', 0):08x}u){tail}")

    # Display bringup: atomic object, zero when unconfigured.
    bringup = bindings.get('display_bringup')
    if bringup:
        lines += ['',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_MALLOC_ENTRY 0x{bringup["malloc_entry"]:08x}u',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_VBLANK_DEVICE_INIT_ENTRY 0x{bringup["vblank_device_init_entry"]:08x}u',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_INIT_ENTRY 0x{bringup["render_context_init_entry"]:08x}u',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_MAGIC_ADDR 0x{bringup["render_context_magic_addr"]:08x}u',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_TABLE_READY_FLAG_ADDR 0x{bringup["render_table_ready_flag_addr"]:08x}u',
                  f'#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_WORD_ADDR 0x{bringup["render_context_word_addr"]:08x}u']
    else:
        lines += ['',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_MALLOC_ENTRY 0x00000000u',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_VBLANK_DEVICE_INIT_ENTRY 0x00000000u',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_INIT_ENTRY 0x00000000u',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_MAGIC_ADDR 0x00000000u',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_TABLE_READY_FLAG_ADDR 0x00000000u',
                  '#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_WORD_ADDR 0x00000000u']

    # Runtime sync: atomic object with mode-keyed wrapper pairs.
    runtime_sync = bindings.get('runtime_sync')
    if runtime_sync:
        lines += ['',
                  f'#define SR_TITLE_CONFIG_RUNTIME_SYNC_CONFIG_BASE 0x{runtime_sync["config_base"]:08x}u',
                  f'#define SR_TITLE_CONFIG_RUNTIME_SYNC_SEMA_NAME_PTR 0x{runtime_sync["sema_name_ptr"]:08x}u',
                  f'#define SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_COUNT {len(runtime_sync["wrappers"])}',
                  '#define SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_LIST' + (CONT if runtime_sync["wrappers"] else '')]
        for idx, w in enumerate(runtime_sync["wrappers"]):
            tail = CONT if idx + 1 < len(runtime_sync["wrappers"]) else ''
            lines.append(f'    SR_TITLE_CFG_RUNTIME_SYNC_WRAPPER({w["mode"]}u, 0x{w["enter"]:08x}u, 0x{w["leave"]:08x}u){tail}')
    else:
        lines += ['',
                  '#define SR_TITLE_CONFIG_RUNTIME_SYNC_CONFIG_BASE 0x00000000u',
                  '#define SR_TITLE_CONFIG_RUNTIME_SYNC_SEMA_NAME_PTR 0x00000000u',
                  '#define SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_COUNT 0',
                  '#define SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_LIST']

    lines += ["", "#endif /* SR_TITLE_CONFIG_GENERATED_H */", ""]
    return "\n".join(lines)


def write_if_changed(path: Path, rendered: str) -> bool:
    """Write atomically, leaving an identical file (and its mtime) untouched."""
    if path.is_symlink():
        raise TitleRuntimeConfigError(f"{path}: refusing to replace a symbolic link")
    if path.is_file() and path.read_text(encoding="utf-8") == rendered:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp_path = Path(temp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(rendered)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise
    return True


def load_config(manifest_path: Path | None) -> dict[str, Any]:
    manifest = None
    if manifest_path is not None:
        manifest = title_manifest.load_manifest(manifest_path)
    return bindings_from_manifest(manifest)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        help="validated title manifest; omit for the generic no-title configuration",
    )
    parser.add_argument("--output", type=Path, help="generated header path")
    parser.add_argument(
        "--print-digest",
        action="store_true",
        help="print the configuration digest and exit (no header is written)",
    )
    args = parser.parse_args(argv)
    try:
        config = load_config(args.manifest)
        if args.print_digest:
            print(config_digest(config))
            return 0
        if args.output is None:
            parser.error("--output is required unless --print-digest is given")
        changed = write_if_changed(args.output, render_header(config))
        print(
            f"title runtime config: {config['source_id']} "
            f"({binding_summary(config)}, {'written' if changed else 'unchanged'})"
        )
        return 0
    except (title_manifest.TitleManifestError, TitleRuntimeConfigError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
