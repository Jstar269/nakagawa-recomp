#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Strict parsers for aggregate GE replay CPU-profile summaries."""

CPU_PHASES = (
    "state_prep", "state_key", "pipeline_lookup", "pipeline_create",
    "descriptor_alloc", "descriptor_update", "bind_record", "object_lookup",
    "texture_decode", "texture_shadow", "vertex_prep", "snapshot_target",
    "snapshot_decision", "snapshot_region", "snapshot_metadata", "command_record",
    "memcpy", "heap",
)

HIERARCHY_FIELDS = {
    "list_ns", "command_ns", "primitive_ns", "primitive_frontend_ns", "gpu_hook_ns",
    "block_ns", "clut_ns", "flush_ns", "list_residual_ns", "hook_renderer_ns",
    "hook_submit_ns", "hook_wait_ns", "hook_residual_ns",
}

WALL_PHASES = {
    "replay_reset": "HARNESS-ONLY",
    "fixture_apply": "HARNESS-ONLY",
    "ge_restore": "HARNESS-ONLY",
    "ge_lists": "PRODUCTION-RELEVANT",
    "loop_other": "HARNESS-ONLY",
    "materialize": "PRODUCTION-RELEVANT",
}

GE_CPU_PHASES = (
    "list_total", "command_dispatch", "primitive", "gpu_hook", "block_transfer", "clut_load", "flush",
)

PRIMITIVE_PROFILE_PHASES = (
    "vertex_fetch_decode", "transform", "lighting",
    "clipping_acceptance", "primitive_assembly",
)
# Short alias used by focused replay tests and callers that already use the
# GE_CPU_PHASES naming convention.
GE_PRIM_PROFILE_PHASES = PRIMITIVE_PROFILE_PHASES


def _fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for field in line.split()[1:]:
        if "=" not in field:
            continue
        key, value = field.split("=", 1)
        if key in result:
            raise ValueError(f"duplicate field {key}")
        result[key] = value
    return result


def parse_cpu_profile(text: str) -> tuple[dict[str, dict[str, int]], dict[str, int]]:
    """Parse exactly one complete CPU profile, rejecting merged/duplicate phases."""
    phases: dict[str, dict[str, int]] = {}
    counts: dict[str, int] | None = None
    for line in text.splitlines():
        if line.startswith("GE_REPLAY_CPU phase="):
            fields = _fields(line)
            name = fields.pop("phase")
            if name in phases:
                raise ValueError(f"duplicate CPU phase {name}")
            if set(fields) != {"calls", "ns", "ms"}:
                raise ValueError(f"unexpected CPU phase fields for {name}")
            phases[name] = {"calls": int(fields["calls"]), "ns": int(fields["ns"])}
        elif line.startswith("GE_REPLAY_CPU_COUNTS "):
            if counts is not None:
                raise ValueError("duplicate CPU count summary")
            counts = {key: int(value) for key, value in _fields(line).items()}
    missing = set(CPU_PHASES) - set(phases)
    extra = set(phases) - set(CPU_PHASES)
    if missing or extra:
        raise ValueError(f"CPU phase mismatch missing={sorted(missing)} extra={sorted(extra)}")
    if counts is None:
        raise ValueError("missing CPU count summary")
    return phases, counts


def parse_wall_profile(text: str) -> dict[str, dict[str, int | str]]:
    """Parse one complete, non-merged end-to-end replay wall profile."""
    phases: dict[str, dict[str, int | str]] = {}
    for line in text.splitlines():
        if not line.startswith("GE_REPLAY_WALL_PHASE phase="):
            continue
        fields = _fields(line)
        name = fields.pop("phase")
        if name in phases:
            raise ValueError(f"duplicate wall phase {name}")
        if set(fields) != {"classification", "calls", "ns", "ms"}:
            raise ValueError(f"unexpected wall phase fields for {name}")
        classification = fields["classification"]
        expected = WALL_PHASES.get(name)
        if expected is not None and classification != expected:
            raise ValueError(
                f"wall phase classification mismatch for {name}: {classification} != {expected}"
            )
        phases[name] = {
            "classification": classification,
            "calls": int(fields["calls"]),
            "ns": int(fields["ns"]),
        }
    missing = set(WALL_PHASES) - set(phases)
    extra = set(phases) - set(WALL_PHASES)
    if missing or extra:
        raise ValueError(f"wall phase mismatch missing={sorted(missing)} extra={sorted(extra)}")
    return phases


def parse_ge_cpu_profile(text: str) -> tuple[dict[str, dict[str, int]], dict[str, int]]:
    """Parse one complete coarse ge.c CPU profile; list_total is inclusive."""
    phases: dict[str, dict[str, int]] = {}
    counts: dict[str, int] | None = None
    for line in text.splitlines():
        if line.startswith("GE_REPLAY_GE_CPU phase="):
            fields = _fields(line)
            name = fields.pop("phase")
            if name in phases:
                raise ValueError(f"duplicate GE CPU phase {name}")
            if set(fields) != {"calls", "ns", "ms"}:
                raise ValueError(f"unexpected GE CPU phase fields for {name}")
            phases[name] = {"calls": int(fields["calls"]), "ns": int(fields["ns"])}
        elif line.startswith("GE_REPLAY_GE_CPU_COUNTS "):
            if counts is not None:
                raise ValueError("duplicate GE CPU count summary")
            counts = {key: int(value) for key, value in _fields(line).items()}
    missing = set(GE_CPU_PHASES) - set(phases)
    extra = set(phases) - set(GE_CPU_PHASES)
    if missing or extra:
        raise ValueError(f"GE CPU phase mismatch missing={sorted(missing)} extra={sorted(extra)}")
    if counts is None:
        raise ValueError("missing GE CPU count summary")
    return phases, counts


def parse_primitive_profile(text: str) -> dict[str, object]:
    """Parse one sparse, exclusive-enough GE primitive-frontend profile."""
    config: dict[str, int] | None = None
    phases: dict[str, dict[str, int | float]] = {}
    counts: dict[str, int] | None = None
    calibration_enabled: int | None = None
    calibration_adjustment: str | None = None
    control: dict[str, int | float] | None = None
    sampled_total: dict[str, int | float] | None = None
    sampled_total_adjusted: dict[str, int | float] | None = None
    adjusted_phases: dict[str, dict[str, int | float]] = {}
    population: dict[str, int] | None = None
    type_counts: dict[str, int] | None = None
    for line in text.splitlines():
        if line.startswith("GE_REPLAY_GE_PRIM_PROFILE_CONFIG "):
            if config is not None:
                raise ValueError("duplicate primitive profile config")
            fields = _fields(line)
            if set(fields) != {"stride", "timer_pair_ns"}:
                raise ValueError("unexpected primitive profile config fields")
            config = {key: int(value) for key, value in fields.items()}
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE phase="):
            fields = _fields(line)
            name = fields.pop("phase")
            if name in phases:
                raise ValueError(f"duplicate primitive profile phase {name}")
            expected = {"calls", "ns", "ms", "eligible", "estimated_ns", "estimated_ms"}
            if set(fields) != expected:
                raise ValueError(f"unexpected primitive profile fields for {name}")
            calls = int(fields["calls"])
            eligible = int(fields["eligible"])
            if calls > eligible:
                raise ValueError(f"primitive profile samples > eligible for {name}")
            phases[name] = {
                "calls": calls,
                "ns": int(fields["ns"]),
                "ms": float(fields["ms"]),
                "eligible": eligible,
                "estimated_ns": int(fields["estimated_ns"]),
                "estimated_ms": float(fields["estimated_ms"]),
            }
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_CALIBRATION "):
            if calibration_enabled is not None:
                raise ValueError("duplicate primitive profile calibration flag")
            fields = _fields(line)
            if set(fields) != {"enabled", "adjustment"}:
                raise ValueError("unexpected primitive profile calibration fields")
            calibration_enabled = int(fields["enabled"])
            if calibration_enabled not in (0, 1):
                raise ValueError("invalid primitive profile calibration flag")
            calibration_adjustment = fields["adjustment"]
            if calibration_adjustment != "none":
                raise ValueError("unsupported primitive profile calibration adjustment")
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_CONTROL "):
            if control is not None:
                raise ValueError("duplicate primitive profile control")
            fields = _fields(line)
            expected = {"calls", "ns", "ms", "per_call_ns"}
            if set(fields) != expected:
                raise ValueError("unexpected primitive profile control fields")
            control = {
                "calls": int(fields["calls"]),
                "ns": int(fields["ns"]),
                "ms": float(fields["ms"]),
                "per_call_ns": int(fields["per_call_ns"]),
            }
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_TOTAL_ADJUSTED "):
            if sampled_total_adjusted is not None:
                raise ValueError("duplicate primitive profile adjusted total")
            fields = _fields(line)
            expected = {"estimated_ns", "estimated_ms"}
            if set(fields) != expected:
                raise ValueError("unexpected primitive profile adjusted total fields")
            sampled_total_adjusted = {
                "estimated_ns": int(fields["estimated_ns"]),
                "estimated_ms": float(fields["estimated_ms"]),
            }
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_TOTAL "):
            if sampled_total is not None:
                raise ValueError("duplicate primitive profile sampled total")
            fields = _fields(line)
            expected = {"calls", "ns", "ms", "eligible", "estimated_ns", "estimated_ms"}
            if set(fields) != expected:
                raise ValueError("unexpected primitive profile total fields")
            calls = int(fields["calls"])
            eligible = int(fields["eligible"])
            if calls > eligible:
                raise ValueError("primitive profile total samples > eligible")
            sampled_total = {
                "calls": calls,
                "ns": int(fields["ns"]),
                "ms": float(fields["ms"]),
                "eligible": eligible,
                "estimated_ns": int(fields["estimated_ns"]),
                "estimated_ms": float(fields["estimated_ms"]),
            }
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_ADJUSTED phase="):
            fields = _fields(line)
            name = fields.pop("phase")
            if name in adjusted_phases:
                raise ValueError(f"duplicate primitive profile adjusted phase {name}")
            expected = {"estimated_ns", "estimated_ms"}
            if set(fields) != expected:
                raise ValueError(f"unexpected primitive profile adjusted fields for {name}")
            adjusted_phases[name] = {
                "estimated_ns": int(fields["estimated_ns"]),
                "estimated_ms": float(fields["estimated_ms"]),
            }
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_POPULATION "):
            if population is not None:
                raise ValueError("duplicate primitive profile population")
            fields = _fields(line)
            expected = {
                "commands", "submitted", "vertex_references", "triangle_vertex_references",
                "non_triangle_vertex_references", "vertex_uses", "triangle_vertex_uses",
                "non_triangle_vertex_uses", "through_vertex_uses", "transform_vertex_uses",
                "actual_decoded_vertices", "actual_transformed_vertices",
                "actual_through_vertices", "strip_cache_commands", "strip_cache_hits",
                "through_triangle_candidates", "transform_triangle_candidates",
                "transform_triangles_drawn", "transform_triangles_clipped",
                "transform_triangles_rejected", "non_triangle_primitives", "vertex_rejects",
                "patch_commands", "patch_control_vertices",
            }
            if set(fields) != expected:
                raise ValueError("unexpected primitive profile population fields")
            population = {key: int(value) for key, value in fields.items()}
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_TYPES "):
            if type_counts is not None:
                raise ValueError("duplicate primitive profile type counts")
            fields = _fields(line)
            expected = {f"commands_type{i}" for i in range(8)} | {f"submitted_type{i}" for i in range(8)}
            if set(fields) != expected:
                raise ValueError("unexpected primitive profile type fields")
            type_counts = {key: int(value) for key, value in fields.items()}
        elif line.startswith("GE_REPLAY_GE_PRIM_PROFILE_COUNTS "):
            if counts is not None:
                raise ValueError("duplicate primitive profile count summary")
            fields = _fields(line)
            if set(fields) != {"vertices", "transform_vertices", "triangle_candidates"}:
                raise ValueError("unexpected primitive profile count fields")
            counts = {key: int(value) for key, value in fields.items()}
    if config is None:
        raise ValueError("missing primitive profile config")
    missing = set(PRIMITIVE_PROFILE_PHASES) - set(phases)
    extra = set(phases) - set(PRIMITIVE_PROFILE_PHASES)
    if missing or extra:
        raise ValueError(f"primitive profile phase mismatch missing={sorted(missing)} extra={sorted(extra)}")
    if counts is None:
        raise ValueError("missing primitive profile count summary")
    if (population is None) != (type_counts is None):
        raise ValueError("incomplete primitive profile population")
    if population is not None and type_counts is not None:
        command_total = sum(type_counts[f"commands_type{i}"] for i in range(8))
        submitted_total = sum(type_counts[f"submitted_type{i}"] for i in range(8))
        if population["commands"] != command_total or population["submitted"] != submitted_total:
            raise ValueError("primitive profile population total mismatch")
        if population["vertex_references"] != (
                population["triangle_vertex_references"] + population["non_triangle_vertex_references"]):
            raise ValueError("primitive profile vertex reference population mismatch")
        if population["vertex_uses"] != (
                population["triangle_vertex_uses"] + population["non_triangle_vertex_uses"]):
            raise ValueError("primitive profile vertex population mismatch")
        if population["vertex_references"] < population["vertex_uses"]:
            raise ValueError("primitive profile references below decoded population")
        if population["vertex_uses"] != (
                population["through_vertex_uses"] + population["transform_vertex_uses"]):
            raise ValueError("primitive profile mode population mismatch")
        if population["actual_decoded_vertices"] != (
                population["actual_through_vertices"] + population["actual_transformed_vertices"]):
            raise ValueError("primitive profile decoded population mismatch")
    calibration = None
    calibration_parts = (control, sampled_total, sampled_total_adjusted, adjusted_phases)
    if calibration_enabled is not None or any(part is not None and part != {} for part in calibration_parts):
        if calibration_enabled != 1 or calibration_adjustment != "none" or control is None or sampled_total is None or \
                sampled_total_adjusted is None or set(adjusted_phases) != set(PRIMITIVE_PROFILE_PHASES):
            raise ValueError("incomplete primitive profile calibration")
        calibration = {
            "enabled": calibration_enabled,
            "adjustment": calibration_adjustment,
            "control": control,
            "sampled_total": sampled_total,
            "sampled_total_adjusted": sampled_total_adjusted,
            "adjusted_phases": adjusted_phases,
        }
    return {
        "config": config,
        "phases": phases,
        "counts": counts,
        "population": population,
        "type_counts": type_counts,
        "calibration": calibration,
    }


parse_ge_primitive_profile = parse_primitive_profile


def parse_hook_cpu_profile(text: str) -> tuple[dict[str, dict[str, int]], dict[str, int]]:
    """Parse renderer phases restricted to time spent under primitive GPU hooks."""
    phases: dict[str, dict[str, int]] = {}
    counts: dict[str, int] | None = None
    for line in text.splitlines():
        if line.startswith("GE_REPLAY_HOOK_CPU phase="):
            fields = _fields(line)
            name = fields.pop("phase")
            if name in phases:
                raise ValueError(f"duplicate hook CPU phase {name}")
            if set(fields) != {"calls", "ns", "ms"}:
                raise ValueError(f"unexpected hook CPU phase fields for {name}")
            phases[name] = {"calls": int(fields["calls"]), "ns": int(fields["ns"])}
        elif line.startswith("GE_REPLAY_HOOK_COUNTS "):
            if counts is not None:
                raise ValueError("duplicate hook CPU count summary")
            counts = {key: int(value) for key, value in _fields(line).items()}
    missing = set(CPU_PHASES) - set(phases)
    extra = set(phases) - set(CPU_PHASES)
    if missing or extra:
        raise ValueError(f"hook CPU phase mismatch missing={sorted(missing)} extra={sorted(extra)}")
    if counts is None:
        raise ValueError("missing hook CPU count summary")
    return phases, counts


def parse_hierarchy(text: str) -> dict[str, int]:
    """Parse the single non-overlapping list/hook reconciliation summary."""
    summaries = [line for line in text.splitlines() if line.startswith("GE_REPLAY_HIERARCHY ")]
    if len(summaries) != 1:
        raise ValueError(f"expected one hierarchy summary, found {len(summaries)}")
    fields = _fields(summaries[0])
    missing = HIERARCHY_FIELDS - set(fields)
    extra = set(fields) - HIERARCHY_FIELDS
    if missing or extra:
        raise ValueError(f"hierarchy field mismatch missing={sorted(missing)} extra={sorted(extra)}")
    values = {key: int(value) for key, value in fields.items()}
    top = (values["command_ns"] + values["primitive_ns"] + values["block_ns"] +
           values["clut_ns"] + values["flush_ns"] + values["list_residual_ns"])
    if top != values["list_ns"]:
        raise ValueError(f"list hierarchy does not reconcile: {top} != {values['list_ns']}")
    primitive = values["primitive_frontend_ns"] + values["gpu_hook_ns"]
    if primitive != values["primitive_ns"]:
        raise ValueError(
            f"primitive hierarchy does not reconcile: {primitive} != {values['primitive_ns']}"
        )
    hook = (values["hook_renderer_ns"] + values["hook_submit_ns"] +
            values["hook_wait_ns"] + values["hook_residual_ns"])
    if hook != values["gpu_hook_ns"]:
        raise ValueError(f"hook hierarchy does not reconcile: {hook} != {values['gpu_hook_ns']}")
    return values
