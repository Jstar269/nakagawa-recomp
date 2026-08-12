# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Headless Ghidra driver for the HST recomp pipeline.

Wraps Ghidra's analyzeHeadless so binary analysis is scriptable and
reproducible (no GUI clicks). Requires a local Ghidra 12.x install with the
ghidra-allegrex extension (see the local toolchain/setup guidance for the one-time setup); both
live under third_party/ which is gitignored -- nothing here is fetched or
committed automatically.

Commands:
  validate                    Clean throwaway import proving that exactly one
                              compatible ghidra-allegrex install supplies the
                              PSP loader and Allegrex processor.
  analyze                     One-time: import EBOOT.elf (PspElfLoader,
                              Allegrex:LE:32:default) and run auto-analysis.
                              Takes minutes; refuses to re-import if the
                              project already exists (use --force to delete
                              and redo).
  export-functions            Run ExportFunctionsCSV.java over the analyzed
                              program -> third_party/ghidra/exports/functions.csv
  decompile ADDR [ADDR ...]   Decompile functions to
                              third_party/ghidra/exports/decomp/<addr>.c.
                              Addresses are the recomp pipeline's base-0 view
                              (e.g. 0x000e1724); the stored image base is added
                              automatically. Pass --raw for Ghidra-space
                              addresses.
  info                        Show install/project/export status.

All Ghidra stdout/stderr is teed to logs/ghidra_<command>.log.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GHIDRA_HOME = os.environ.get(
    "GHIDRA_HOME", os.path.join(ROOT, "third_party", "ghidra", "ghidra_12.1_PUBLIC")
)
PROJECT_DIR = os.path.join(ROOT, "third_party", "ghidra", "projects")
PROJECT_NAME = "HST"
PROGRAM_NAME = "EBOOT.elf"
ELF = os.path.join(ROOT, "place_game_here", "EBOOT.elf")
SCRIPT_PATH = os.path.join(ROOT, "tools", "ghidra_scripts")
EXPORT_DIR = os.path.join(ROOT, "third_party", "ghidra", "exports")
FUNCTIONS_CSV = os.path.join(EXPORT_DIR, "functions.csv")
LOG_DIR = os.path.join(ROOT, "logs")

LOADER = "PspElfLoader"
PROCESSOR = "Allegrex:LE:32:default"


def headless_bat() -> str:
    bat = os.path.join(GHIDRA_HOME, "support", "analyzeHeadless.bat")
    if not os.path.isfile(bat):
        sys.exit(
            f"analyzeHeadless not found at {bat}\n"
            "Install Ghidra there or set GHIDRA_HOME before using this optional local tool."
        )
    return bat


def run_headless(tag: str, args: list[str], project_name: str = PROJECT_NAME) -> int:
    os.makedirs(LOG_DIR, exist_ok=True)
    log = os.path.join(LOG_DIR, f"ghidra_{tag}.log")
    cmd = ["cmd", "/c", headless_bat(), PROJECT_DIR, project_name] + args
    print(f"[ghidra_headless] {tag}: logging to {log}")
    with open(log, "w", encoding="utf-8", errors="replace") as fh:
        fh.write("+ " + " ".join(cmd) + "\n")
        fh.flush()
        proc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=ROOT)
    if proc.returncode != 0:
        tail = ""
        try:
            with open(log, encoding="utf-8", errors="replace") as fh:
                tail = "".join(fh.readlines()[-15:])
        except OSError:
            pass
        print(f"[ghidra_headless] {tag} FAILED (exit {proc.returncode}). Log tail:\n{tail}")
    else:
        print(f"[ghidra_headless] {tag} OK")
    return proc.returncode


def project_exists() -> bool:
    return os.path.isfile(os.path.join(PROJECT_DIR, PROJECT_NAME + ".gpr"))


def read_property(path: str, key: str) -> str | None:
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                name, sep, value = raw.strip().partition("=")
                if sep and name == key:
                    return value
    except OSError:
        pass
    return None


def ghidra_version() -> str | None:
    return read_property(
        os.path.join(GHIDRA_HOME, "Ghidra", "application.properties"),
        "application.version",
    )


def allegrex_install_dirs() -> list[str]:
    installs = [
        os.path.join(GHIDRA_HOME, "Ghidra", "Extensions", "ghidra-allegrex"),
    ]
    version = ghidra_version()
    appdata = os.environ.get("APPDATA")
    if appdata and version:
        installs.append(
            os.path.join(
                appdata,
                "ghidra",
                f"ghidra_{version}_PUBLIC",
                "Extensions",
                "ghidra-allegrex",
            )
        )
    return [path for path in installs if os.path.isdir(path)]


def cmd_validate() -> int:
    if not os.path.isfile(ELF):
        print(f"[ghidra_headless] validation FAILED: decrypted EBOOT not found: {ELF}")
        return 1
    version = ghidra_version()
    if not version:
        print("[ghidra_headless] validation FAILED: cannot read Ghidra version")
        return 1
    installs = allegrex_install_dirs()
    if not installs:
        print("[ghidra_headless] validation FAILED: ghidra-allegrex is not installed")
        return 1
    if len(installs) != 1:
        print("[ghidra_headless] validation FAILED: ghidra-allegrex is installed more than once:")
        for path in installs:
            print(f"  {path}")
        print("Remove one copy; duplicate modules make Ghidra startup ambiguous.")
        return 1
    extension = installs[0]
    target = read_property(os.path.join(extension, "extension.properties"), "version")
    if target != version:
        print(
            "[ghidra_headless] validation FAILED: extension targets Ghidra "
            f"{target or 'unknown'}, but GHIDRA_HOME is {version}"
        )
        return 1

    os.makedirs(PROJECT_DIR, exist_ok=True)
    validation_project = f"HST_AllegrexValidation_{uuid.uuid4().hex}"
    rc = run_headless(
        "validate",
        [
            "-import", ELF,
            "-loader", LOADER,
            "-processor", PROCESSOR,
            "-noanalysis",
            "-deleteProject",
        ],
        validation_project,
    )
    if rc != 0:
        return rc

    log = os.path.join(LOG_DIR, "ghidra_validate.log")
    try:
        with open(log, encoding="utf-8", errors="replace") as fh:
            output = fh.read()
    except OSError as exc:
        print(f"[ghidra_headless] validation FAILED: cannot read {log}: {exc}")
        return 1
    required = (
        "Using Loader: PSP Executable (ELF)",
        "Using Language/Compiler: Allegrex:LE:32:default:default",
        "REPORT: Import succeeded",
    )
    missing = [marker for marker in required if marker not in output]
    if "Multiple modules collided" in output or missing:
        if missing:
            print("[ghidra_headless] validation FAILED: missing import evidence:")
            for marker in missing:
                print(f"  {marker}")
        else:
            print("[ghidra_headless] validation FAILED: duplicate ghidra-allegrex modules collided")
        return 1
    print(f"[ghidra_headless] Ghidra {version} / ghidra-allegrex verified at {extension}")
    print(f"[ghidra_headless] loader={LOADER} processor={PROCESSOR}")
    return 0


def cmd_analyze(force: bool) -> int:
    if not os.path.isfile(ELF):
        sys.exit(f"decrypted EBOOT not found: {ELF}")
    if project_exists():
        if not force:
            print(
                f"[ghidra_headless] project {PROJECT_NAME} already exists in "
                f"{PROJECT_DIR}; use --force to delete and re-analyze."
            )
            return 0
        print("[ghidra_headless] --force: deleting existing project")
        for suffix in (".gpr", ".lock", ".lock~", ".rep"):
            p = os.path.join(PROJECT_DIR, PROJECT_NAME + suffix)
            if os.path.isdir(p):
                shutil.rmtree(p)
            elif os.path.exists(p):
                os.remove(p)
    os.makedirs(PROJECT_DIR, exist_ok=True)
    return run_headless(
        "analyze",
        [
            "-import", ELF,
            "-loader", LOADER,
            "-processor", PROCESSOR,
            "-analysisTimeoutPerFile", "3600",
        ],
    )


def cmd_export_functions() -> int:
    if not project_exists():
        sys.exit("no analyzed project; run: python tools/ghidra_headless.py analyze")
    os.makedirs(EXPORT_DIR, exist_ok=True)
    rc = run_headless(
        "export_functions",
        [
            "-process", PROGRAM_NAME,
            "-noanalysis",
            "-scriptPath", SCRIPT_PATH,
            "-postScript", "ExportFunctionsCSV.java", FUNCTIONS_CSV,
        ],
    )
    if rc == 0 and os.path.isfile(FUNCTIONS_CSV):
        with open(FUNCTIONS_CSV, encoding="utf-8") as fh:
            n = sum(1 for line in fh if line[:1] == "0")
        print(f"[ghidra_headless] {FUNCTIONS_CSV}: {n} functions")
    return rc


def read_image_base() -> int:
    try:
        with open(FUNCTIONS_CSV, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("# imageBase="):
                    return int(line.split("=", 1)[1], 16)
    except OSError:
        pass
    sys.exit(
        f"image base unknown ({FUNCTIONS_CSV} missing); run: "
        "python tools/ghidra_headless.py export-functions"
    )


def cmd_decompile(addrs: list[str], raw: bool) -> int:
    if not project_exists():
        sys.exit("no analyzed project; run: python tools/ghidra_headless.py analyze")
    base = 0 if raw else read_image_base()
    out_dir = os.path.join(EXPORT_DIR, "decomp")
    ghidra_addrs = ["0x%08x" % ((int(a, 16) + base) & 0xFFFFFFFF) for a in addrs]
    return run_headless(
        "decompile",
        [
            "-process", PROGRAM_NAME,
            "-noanalysis",
            "-scriptPath", SCRIPT_PATH,
            "-postScript", "DecompileList.java", out_dir, *ghidra_addrs,
        ],
    )


def cmd_refs(addrs: list[str], raw: bool) -> int:
    if not project_exists():
        sys.exit("no analyzed project; run: python tools/ghidra_headless.py analyze")
    base = 0 if raw else read_image_base()
    ghidra_addrs = ["0x%08x" % ((int(a, 16) + base) & 0xFFFFFFFF) for a in addrs]
    rc = run_headless(
        "refs",
        [
            "-process", PROGRAM_NAME,
            "-noanalysis",
            "-scriptPath", SCRIPT_PATH,
            "-postScript", "ListRefsTo.java", *ghidra_addrs,
        ],
    )
    if rc == 0:
        # The interesting lines are buried in Ghidra's log chatter; surface them.
        with open(os.path.join(LOG_DIR, "ghidra_refs.log"),
                  encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "TARGET 0x" in line or "REF from=" in line or "(no references)" in line:
                    print(line.rstrip().split("ListRefsTo.java> ")[-1])
    return rc


def cmd_info() -> int:
    print(f"GHIDRA_HOME      {GHIDRA_HOME}  "
          f"({'ok' if os.path.isdir(GHIDRA_HOME) else 'MISSING'})")
    version = ghidra_version()
    print(f"Ghidra version   {version or 'UNKNOWN'}")
    installs = allegrex_install_dirs()
    if not installs:
        print("ghidra-allegrex MISSING")
    else:
        for i, path in enumerate(installs):
            target = read_property(os.path.join(path, "extension.properties"), "version")
            label = "ok" if len(installs) == 1 and target == version else "CHECK"
            prefix = "ghidra-allegrex" if i == 0 else "                 "
            print(f"{prefix} {path}  (target {target or 'unknown'}; {label})")
    print(f"loader           {LOADER}")
    print(f"processor        {PROCESSOR}")
    print(f"project          {os.path.join(PROJECT_DIR, PROJECT_NAME + '.gpr')}  "
          f"({'ok' if project_exists() else 'not analyzed yet'})")
    print(f"elf              {ELF}  ({'ok' if os.path.isfile(ELF) else 'MISSING'})")
    have_csv = os.path.isfile(FUNCTIONS_CSV)
    print(f"functions.csv    {FUNCTIONS_CSV}  ({'ok' if have_csv else 'not exported yet'})")
    if have_csv:
        print(f"image base       0x{read_image_base():08x}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("validate", help="verify extension + clean PSP/Allegrex import")
    p_an = sub.add_parser("analyze", help="import + auto-analyze EBOOT.elf (one-time)")
    p_an.add_argument("--force", action="store_true",
                      help="delete the existing project and re-analyze")
    sub.add_parser("export-functions", help="export all functions to functions.csv")
    p_de = sub.add_parser("decompile", help="decompile addresses to .c files")
    p_de.add_argument("addrs", nargs="+", metavar="ADDR",
                      help="hex address (pipeline base-0 view unless --raw)")
    p_de.add_argument("--raw", action="store_true",
                      help="addresses are already in Ghidra's address space")
    p_rf = sub.add_parser("refs", help="list references to addresses")
    p_rf.add_argument("addrs", nargs="+", metavar="ADDR",
                      help="hex address (pipeline base-0 view unless --raw)")
    p_rf.add_argument("--raw", action="store_true",
                      help="addresses are already in Ghidra's address space")
    sub.add_parser("info", help="show setup/project status")
    ns = ap.parse_args(argv[1:])
    if ns.cmd == "validate":
        return cmd_validate()
    if ns.cmd == "analyze":
        return cmd_analyze(ns.force)
    if ns.cmd == "export-functions":
        return cmd_export_functions()
    if ns.cmd == "decompile":
        return cmd_decompile(ns.addrs, ns.raw)
    if ns.cmd == "refs":
        return cmd_refs(ns.addrs, ns.raw)
    return cmd_info()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
