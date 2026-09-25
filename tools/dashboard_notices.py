#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Third-party notice generation for the dashboard's Next.js standalone output.

The standalone build traces npm packages out of ``node_modules`` into
``.next/standalone``, so the output redistributes third-party code even though
this repository carries no ``node_modules``. This generator emits the notice
bundle for that output from two tracked inputs and nothing else:

* ``assets/third_party_components.json`` — the per-license obligations and which
  packages they apply to;
* ``interface/package-lock.json`` — the exact resolved name/version/license of
  every package in the dashboard dependency graph.

It fails closed. A missing or unreadable lockfile, a package with no license
expression, or a license expression with no recorded obligation is a named
error, never a silently shortened notice.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from package_notices import (  # noqa: E402  (path bootstrap must precede the import)
    COMPONENTS_IDENTITY,
    load_component_inventory,
)

LOCKFILE_IDENTITY = "interface/package-lock.json"

#: The project's own notice for the primitives it copied into the dashboard.
SHADCN_NOTICE = "THIRD_PARTY_LICENSES/SHADCN_UI.txt"


class DashboardNoticeError(Exception):
    """A named failure that stops the dashboard notice bundle from shipping."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def read_lockfile_packages(lock_path: Path) -> list[dict[str, str]]:
    """Return ``{name, version, license}`` for every locked npm package.

    The lockfile is the inventory: a package that reached ``.next/standalone``
    came from one of these entries, so its recorded license expression is the
    exact declaration the upstream package shipped.
    """
    if not lock_path.is_file():
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_LOCKFILE_MISSING",
            f"the declared dashboard lockfile {LOCKFILE_IDENTITY} is missing from the source tree",
        )
    try:
        document = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_LOCKFILE_UNREADABLE",
            f"the declared dashboard lockfile could not be parsed: {exc}",
        ) from exc
    packages = document.get("packages")
    if not isinstance(packages, dict) or not packages:
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_LOCKFILE_INCOMPLETE",
            f"{LOCKFILE_IDENTITY} declares no packages; the dashboard inventory is unknown",
        )
    entries: list[dict[str, str]] = []
    for path, record in sorted(packages.items()):
        if not path or not isinstance(record, dict):
            continue
        name = path.rsplit("node_modules/", 1)[-1]
        version = str(record.get("version") or "")
        license_expr = str(record.get("license") or "").strip()
        entries.append({
            "name": name,
            "version": version,
            "license": license_expr,
            "lockfile_path": path,
        })
    return entries


#: SPDX license expressions join terms with AND/OR in either case and wrap choices
#: in parentheses; every token in the expression is a class the inventory can
#: record an obligation for, so a choice expression is treated conservatively as
#: "every alternative applies".
_TERM_SPLIT = re.compile(r"\s+(?:AND|OR|and|or)\s+")


def license_classes(license_expr: str) -> set[str]:
    """Return the individual license classes named by a (possibly compound) expression."""
    return {
        token.strip()
        for token in _TERM_SPLIT.split(license_expr.replace("(", " ").replace(")", " "))
        if token.strip()
    }


def license_obligation(license_expr: str, obligations: dict[str, Any]) -> dict[str, Any] | None:
    """Return the recorded obligation for a license expression, or None.

    A compound expression is attributed to every class it names, so
    ``Apache-2.0 AND LGPL-3.0-or-later`` picks up both the Apache attribution
    note and the unresolved LGPL relink decision.
    """
    classes = license_classes(license_expr)
    matched = [obligations[name] for name in sorted(classes) if name in obligations]
    if not matched:
        return None
    return {
        "classes": sorted(classes & set(obligations)),
        "unresolved": [entry for entry in matched if not entry.get("resolved", False)],
        "applies_to": sorted({pkg for entry in matched for pkg in entry.get("applies_to", [])}),
    }


def build_dashboard_notices(
    *,
    repo_root: Path = ROOT,
    lock_path: Path | None = None,
    output_dir: Path | None = None,
) -> dict[str, Any]:
    """Return the dashboard notice bundle document, validating every package.

    Raises:
        DashboardNoticeError: when the lockfile is unusable, a locked package
            declares no license, or a license expression has no recorded
            obligation in ``assets/third_party_components.json``.
    """
    inventory = load_component_inventory()
    dashboard = inventory["dashboard"]
    if dashboard.get("lockfile") != LOCKFILE_IDENTITY:
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_SOURCE_MISMATCH",
            f"{COMPONENTS_IDENTITY} declares dashboard lockfile "
            f"{dashboard.get('lockfile')!r}, but this generator reads {LOCKFILE_IDENTITY!r}",
        )
    lock = lock_path or repo_root / LOCKFILE_IDENTITY
    obligations = dashboard["license_obligations"]
    entries = read_lockfile_packages(lock)

    components: list[dict[str, Any]] = []
    missing_license: list[str] = []
    undocumented_class: set[str] = set()
    for entry in entries:
        if not entry["license"]:
            missing_license.append(f"{entry['name']}@{entry['version']}")
            continue
        obligation = license_obligation(entry["license"], obligations)
        if obligation is None:
            undocumented_class.add(entry["license"])
        component = {
            "name": entry["name"],
            "version": entry["version"],
            "spdx_id": entry["license"],
            "source_path": entry["lockfile_path"],
            "obligation_classes": obligation["classes"] if obligation else [],
            "relink_or_attribution_unresolved": bool(obligation and obligation["unresolved"]),
        }
        components.append(component)

    if missing_license:
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_LICENSE_MISSING",
            f"the dashboard lockfile records no license for {len(missing_license)} package(s): "
            f"{', '.join(sorted(missing_license)[:10])}",
        )
    if undocumented_class:
        raise DashboardNoticeError(
            "DASHBOARD_NOTICES_LICENSE_UNDOCUMENTED",
            f"{COMPONENTS_IDENTITY} records no obligation for the dashboard license "
            f"expression(s) {', '.join(sorted(undocumented_class))}; add the obligation before "
            f"shipping, do not drop the package from the notice",
        )

    return {
        "schema_version": 1,
        "bundle_type": "dashboard_standalone_third_party_notices",
        "source_of_truth": COMPONENTS_IDENTITY,
        "lockfile": LOCKFILE_IDENTITY,
        "package_count": len(components),
        "components": components,
        "license_obligations": {
            name: {
                "applies_to": entry.get("applies_to", []),
                "obligation": entry["obligation"],
                "resolved": entry["resolved"],
            }
            for name, entry in sorted(obligations.items())
        },
        "project_attribution": dashboard.get("project_attribution", []),
    }


def render_notices(bundle: dict[str, Any], repo_root: Path = ROOT) -> str:
    """Render the human-readable THIRD_PARTY_NOTICES.txt for the standalone output."""
    lines = [
        "=" * 80,
        "            THIRD-PARTY SOFTWARE NOTICES — dashboard standalone output",
        "=" * 80,
        "",
        "This standalone build traces npm packages out of node_modules, so it",
        "redistributes third-party code. Every package below is named with the exact",
        "version and license expression recorded in",
        f"{bundle['lockfile']}, which is the inventory of this dependency graph.",
        "",
        f"Packages in this bundle: {bundle['package_count']}",
        "",
        "Open items requiring a maintainer decision are listed under",
        "'UNRESOLVED OBLIGATIONS'. They are recorded, not decided: this file does",
        "not make a legal determination about any of them.",
        "",
        "-" * 80,
        "UNRESOLVED OBLIGATIONS",
        "-" * 80,
        "",
    ]
    for name, entry in bundle["license_obligations"].items():
        if entry["resolved"]:
            continue
        lines.append(f"* {name}")
        lines.append(f"    applies to: {', '.join(entry['applies_to']) or '(per-package expression)'}")
        lines.append(f"    obligation: {entry['obligation']}")
        lines.append("")
    lines.append("-" * 80)
    lines.append("SATISFIED OBLIGATIONS")
    lines.append("-" * 80)
    lines.append("")
    for name, entry in bundle["license_obligations"].items():
        if not entry["resolved"]:
            continue
        lines.append(f"* {name}")
        lines.append(f"    applies to: {', '.join(entry['applies_to']) or '(per-package expression)'}")
        lines.append(f"    obligation: {entry['obligation']}")
        lines.append("")
    lines.append("-" * 80)
    lines.append("PROJECT ATTRIBUTION")
    lines.append("-" * 80)
    lines.append("")
    for item in bundle["project_attribution"]:
        lines.append(f"* {item['file']} — {item['covers']}")
    lines.append("")
    lines.append("-" * 80)
    lines.append("PACKAGE INVENTORY")
    lines.append("-" * 80)
    lines.append("")
    lines.append(f"{'package':<52} {'version':<14} license")
    for component in sorted(bundle["components"], key=lambda item: item["name"].lower()):
        name = component["name"]
        if len(name) > 52:
            name = name[:49] + "..."
        lines.append(f"{name:<52} {component['version']:<14} {component['spdx_id']}")
    lines.append("")
    lines.append("Machine-readable form: THIRD_PARTY_NOTICES/index.json")
    lines.append("")
    return "\n".join(lines)


def write_dashboard_notices(
    output_dir: Path,
    *,
    repo_root: Path = ROOT,
    lock_path: Path | None = None,
) -> dict[str, Any]:
    """Write THIRD_PARTY_NOTICES.txt and index.json into the standalone output."""
    # Resolved so a caller that passes a relative path with a different working
    # directory cannot scatter the bundle outside the output it belongs to.
    output_dir = output_dir.resolve()
    bundle = build_dashboard_notices(repo_root=repo_root, lock_path=lock_path)
    notices_dir = output_dir / "THIRD_PARTY_NOTICES"
    notices_dir.mkdir(parents=True, exist_ok=True)
    (notices_dir / "index.json").write_text(
        json.dumps(bundle, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    (output_dir / "THIRD_PARTY_NOTICES.txt").write_text(
        render_notices(bundle, repo_root), encoding="utf-8", newline="\n"
    )
    return bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--standalone",
        type=Path,
        default=ROOT / "interface" / ".next" / "standalone",
        help="Directory holding the built Next.js standalone output",
    )
    parser.add_argument("--lockfile", type=Path, help="Override the declared npm lockfile")
    args = parser.parse_args(argv)

    try:
        bundle = write_dashboard_notices(args.standalone, lock_path=args.lockfile)
    except DashboardNoticeError as exc:
        print(exc, file=sys.stderr)
        return 2
    except Exception as exc:  # noqa: BLE001 - named boundary for an unexpected failure
        print(f"DASHBOARD_NOTICES_FAILED: {exc}", file=sys.stderr)
        return 2
    print(
        f"Generated dashboard third-party notices for {bundle['package_count']} locked "
        f"packages in {args.standalone.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
