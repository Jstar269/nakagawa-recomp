#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""
NID Compliance Auditor.
Cross-examines hst_imports.toml against src/rt/hle.c to evaluate HLE coverage.
"""

import sys
import os
import re
import json

def find_repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def parse_imports_toml(toml_path):
    if not os.path.exists(toml_path):
        return []

    imports = []
    current = {}

    with open(toml_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("[[import]]"):
                if current:
                    imports.append(current)
                current = {}
            elif "=" in line:
                key, val = line.split("=", 1)
                key = key.strip()
                val = val.strip().strip('"')
                if key in ("stub", "nid"):
                    current[key] = int(val, 16) if val.startswith("0x") else int(val)
                elif key == "lib":
                    current[key] = val

        if current:
            imports.append(current)

    return imports

def parse_hle_c(hle_path):
    if not os.path.exists(hle_path):
        return {}, {}

    with open(hle_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Find all sr_hle_register calls: sr_hle_register(0xca04a2b9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntr);
    # Also handle things like sr_hle_register(sas_ok[i], ...) - we'll skip variables.
    register_pattern = re.compile(r'sr_hle_register\(\s*(0x[0-9a-fA-F]+u?)\s*,\s*"([^"]+)"\s*,\s*([_a-zA-Z0-9]+)\s*\)')
    registrations = {}
    for match in register_pattern.finditer(content):
        nid_str, name, handler = match.groups()
        nid = int(nid_str.rstrip('u').rstrip('U'), 16)
        registrations[nid] = {
            "nid": nid,
            "name": name,
            "handler": handler
        }

    # Dynamically derive the SAS NID set from hle.c instead of a hardcoded table.
    # 1) Explicit __sceSas* registrations captured by the main regex above.
    # 2) The sas_ok[] NID array, registered through a loop variable (not a literal
    #    hex in the sr_hle_register call) so it is NOT picked up by the main regex.
    sas_nids = []
    for nid, reg in registrations.items():
        name = reg["name"].lower()
        if name.startswith("scesas") or "sas" in name:
            sas_nids.append(nid)

    sas_ok_pattern = re.compile(r'sas_ok\[\]\s*=\s*\{([^}]*)\};', re.DOTALL)
    m = sas_ok_pattern.search(content)
    if m:
        for hexmatch in re.finditer(r'0x[0-9a-fA-F]+', m.group(1)):
            sas_nids.append(int(hexmatch.group(), 16))

    sas_nids = sorted(set(sas_nids))
    for nid in sas_nids:
        if nid not in registrations:
            registrations[nid] = {
                "nid": nid,
                "name": f"__sceSas_NID_{nid:08x}",
                "handler": "h_ok"
            }

    # The dynamic set above is the single source of truth. Retain only a
    # minimum-expected count as a sanity guardrail (no hardcoded NID table).
    EXPECTED_SAS_NID_MIN = 13
    if len(sas_nids) < EXPECTED_SAS_NID_MIN:
        print(
            f"[nid_auditor] WARNING: dynamic SAS NID count ({len(sas_nids)}) "
            f"is below expected minimum ({EXPECTED_SAS_NID_MIN}); "
            "registration scan may have missed SAS handlers."
        )

    # Extract function bodies in hle.c to look for read/write patterns
    # Handlers usually have: static uint32_t h_FunctionName(CpuState *s) { ... }
    # Or: static int h_FunctionName(CpuState *s) { ... }
    # Let's match all static functions taking CpuState *s
    func_pattern = re.compile(r'static\s+(uint32_t|int|void)\s+([a-zA-Z0-9_]+)\s*\(\s*CpuState\s*\*\s*([a-zA-Z0-9_]+)\s*\)\s*\{', re.MULTILINE)

    handlers_info = {}
    for match in func_pattern.finditer(content):
        ret_type, func_name, state_var = match.groups()

        # Find closing brace of this function
        start_idx = match.end()
        brace_count = 1
        end_idx = start_idx
        while brace_count > 0 and end_idx < len(content):
            ch = content[end_idx]
            if ch == '{':
                brace_count += 1
            elif ch == '}':
                brace_count -= 1
            end_idx += 1

        body = content[start_idx:end_idx-1]
        handlers_info[func_name] = {
            "name": func_name,
            "body": body,
            "state_var": state_var
        }

    return registrations, handlers_info

def analyze_nid_status(registrations, handlers_info, nid, name=None):
    if nid not in registrations:
        return {
            "status": "unmapped",
            "handler": None,
            "name": name or f"NID_0x{nid:08x}",
            "registers": [],
            "logging": "None",
            "flags": []
        }

    reg = registrations[nid]
    handler_name = reg["handler"]

    if handler_name in ("h_ok", "h_PsmfPlayerUnimpl") or handler_name.endswith("Unimpl"):
        status = "stubbed"
    else:
        status = "resolved"

    # Analyze registers and logs from function body
    registers = []
    logging = "Silent"
    flags = []

    if handler_name in handlers_info:
        h = handlers_info[handler_name]
        body = h["body"]
        state_var = h["state_var"]

        # Look for register access, e.g. s->r[4]
        # MIPS registers mapping
        mips_regs = {
            2: "v0 (return)", 3: "v1 (return)",
            4: "a0 (arg)", 5: "a1 (arg)", 6: "a2 (arg)", 7: "a3 (arg)",
            8: "t0", 9: "t1", 10: "t2", 11: "t3", 12: "t4", 13: "t5", 14: "t6", 15: "t7",
            16: "s0", 17: "s1", 18: "s2", 19: "s3", 20: "s4", 21: "s5", 22: "s6", 23: "s7",
            24: "t8", 25: "t9",
            28: "gp", 29: "sp", 30: "s8", 31: "ra (return address)"
        }

        # Find read registers e.g. s->r[4]
        matches = re.findall(rf'{state_var}->r\[(\d+)\]', body)
        seen_regs = set()
        for r_str in matches:
            r_idx = int(r_str)
            if r_idx in mips_regs and r_idx not in seen_regs:
                registers.append(mips_regs[r_idx])
                seen_regs.add(r_idx)

        # Logging checks
        if "fprintf(" in body or "printf(" in body or "dbg_hle(" in body:
            logging = "Logs on call"
        if "unimplemented" in body.lower() or "unimpl" in body.lower():
            status = "stubbed"
            logging = "Logs warning"

        # Flags: yields, blocking, interrupts
        if "sr_yield" in body or "SR_YIELD" in body:
            flags.append("yields")
        if "sched_wakeup" in body or "sched_sleep" in body:
            flags.append("blocking")
        if "CpuSuspendIntr" in body or "CpuResumeIntr" in body:
            flags.append("interrupt-safe")
    else:
        # Known defaults for h_ok
        if handler_name == "h_ok":
            logging = "Silent"
            registers = []
            flags = ["no-op stub"]
        elif handler_name == "h_PsmfPlayerUnimpl":
            logging = "Logs warning"
            registers = []
            flags = ["unimplemented stub"]

    return {
        "status": status,
        "handler": handler_name,
        "name": reg["name"],
        "registers": registers,
        "logging": logging,
        "flags": flags
    }

def main():
    watch_mode = "--watch" in sys.argv
    repo_root = find_repo_root()
    imports_path = os.path.join(repo_root, "build", "hst", "hst_imports.toml")
    hle_path = os.path.join(repo_root, "src", "rt", "hle.c")
    output_path = os.path.join(repo_root, "build", "hst", "nid_audit_report.json")

    def run_audit():
        imports = parse_imports_toml(imports_path)
        registrations, handlers_info = parse_hle_c(hle_path)

        nids_report = []
        module_stats = {}

        total_imports = len(imports)
        resolved_count = 0
        stubbed_count = 0
        unmapped_count = 0

        for imp in imports:
            lib = imp["lib"]
            nid = imp["nid"]
            stub_addr = imp["stub"]

            info = analyze_nid_status(registrations, handlers_info, nid)
            info["lib"] = lib
            info["stub"] = f"0x{stub_addr:08x}"
            info["nid_hex"] = f"0x{nid:08x}"

            # Stats accumulation
            if info["status"] == "resolved":
                resolved_count += 1
            elif info["status"] == "stubbed":
                stubbed_count += 1
            else:
                unmapped_count += 1

            # Module-specific accumulation
            stats = module_stats.setdefault(lib, {"resolved": 0, "stubbed": 0, "unmapped": 0, "total": 0})
            stats["total"] += 1
            stats[info["status"]] += 1

            nids_report.append(info)

        report = {
            "summary": {
                "total_imports": total_imports,
                "resolved": resolved_count,
                "stubbed": stubbed_count,
                "unmapped": unmapped_count,
                "coverage_pct": round((resolved_count / total_imports) * 100.0, 2) if total_imports > 0 else 0.0,
                "implemented_pct": round(((resolved_count + stubbed_count) / total_imports) * 100.0, 2) if total_imports > 0 else 0.0
            },
            "modules": [
                {
                    "name": lib,
                    "resolved": stats["resolved"],
                    "stubbed": stats["stubbed"],
                    "unmapped": stats["unmapped"],
                    "total": stats["total"],
                    "coverage_pct": round((stats["resolved"] / stats["total"]) * 100.0, 2) if stats["total"] > 0 else 0.0,
                    "implemented_pct": round(((stats["resolved"] + stats["stubbed"]) / stats["total"]) * 100.0, 2) if stats["total"] > 0 else 0.0
                }
                for lib, stats in sorted(module_stats.items())
            ],
            "nids": nids_report
        }

        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

        print(f"Audit completed: resolved {resolved_count}, stubbed {stubbed_count}, unmapped {unmapped_count} out of {total_imports} imports.")

    if watch_mode:
        import time
        last_mtime = 0
        while True:
            try:
                mtime = os.stat(hle_path).st_mtime
                if mtime != last_mtime:
                    run_audit()
                    last_mtime = mtime
            except Exception as e:
                print(f"Error checking {hle_path}: {e}")
            time.sleep(2.0)
    else:
        run_audit()
        return 0

if __name__ == "__main__":
    sys.exit(main())
