#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""
Interactive Memory and CpuState Query / Mutation Tool.

Attaches to hst.exe on Windows using ctypes APIs. Mock state is available only with
an explicit ``--simulate`` flag; an absent process is otherwise reported as offline.

Safety contract (issue #180):

* Read-only by default.  Mutating actions (``pause``, ``resume``, ``write_mem``,
  ``write_cpu``) require the explicit high-friction ``--mutate`` flag.  Simulation
  mode also requires ``--mutate`` for mutating actions so the contract is uniform
  and simulation can never accidentally fall through to live mutation.
* Unique process identity.  Automatic attachment requires exactly one process whose
  normalized executable path equals the expected ``build/hst/hst.exe`` path.  Name-
  only matches are never used.  An explicit ``--pid`` selects a specific process and
  its identity (path, image base, on-disk hash) is displayed for confirmation.
* PE image identity.  Before any address-space mutation the running image's first
  page is compared byte-for-byte with the on-disk executable; a stale or mismatched
  image refuses mutation.
* Fail-closed symbol resolution.  Address-space mutation is refused unless both
  ``g_mem``/``s_cpu`` RVAs were resolved from the attached executable itself
  (``nm``), never from the hard-coded fallback RVAs.  Reads remain permitted with
  fallback RVAs and the provenance is reported.
* Overflow-safe guest spans.  Every guest access validates the complete byte span
  against the arena model from ``src/rt/recomp.h`` (guest physical [0, 0x0C000000),
  RAM 64 MiB at 0x08000000, VRAM 2 MiB at 0x04000000, scratchpad 4 KiB at
  0x00010000) with subtraction-based overflow checks, then maps to a host pointer
  with ``g_mem + (SR_PHYS(a) - SR_RAM_BASE)``.
* Deterministic budgets.  Reads/writes above the configured byte budgets, zero-size
  spans, and negative sizes are rejected with explicit reasons.
"""

import sys
import os
import json
import shutil
import subprocess
import ctypes
import hashlib

# Define standard PE image base for Mingw-w64 (64-bit)
DEFAULT_IMAGE_BASE = 0x140000000

# Cache default RVAs resolved in our analysis.  These are READ-ONLY fallbacks used
# only when nm is unavailable; address-space MUTATION never uses them.
DEFAULT_RVAS = {
    "g_mem": 0xbf61400,
    "s_cpu": 0xc5dbee8
}

# --- Guest memory model (mirrors src/rt/recomp.h) ----------------------------
GUEST_PHYS_MASK = 0x1FFFFFFF
GUEST_ARENA_END = 0x0C000000
SR_RAM_BASE = 0x08000000
SR_RAM_SIZE = 0x04000000          # 64 MB user RAM at 0x08000000
SR_VRAM_BASE = 0x04000000
SR_VRAM_SIZE = 0x00200000         # 2 MiB VRAM/eDRAM at 0x04000000
SR_SCRATCHPAD_BASE = 0x00010000
SR_SCRATCHPAD_SIZE = 0x00001000   # 4 KiB scratchpad

SUPPORTED_REGIONS = ("ram", "vram", "scratchpad")

MAX_READ_BYTES = 32 * 1024 * 1024   # 32 MiB read budget
MAX_WRITE_BYTES = 1024 * 1024       # 1 MiB write budget
PE_PREFIX_BYTES = 0x1000            # first page of the PE image used for identity

EXPECTED_EXE_REL = os.path.join("build", "hst", "hst.exe")

# Windows process access rights (read-only rights used for discovery)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010


def guest_phys(addr):
    """SR_PHYS(a) == (a) & 0x1FFFFFFF, matching src/rt/recomp.h."""
    return addr & GUEST_PHYS_MASK


def guest_region(addr):
    """Classify a guest address into a named region, never applying one
    mask-and-subtract rule to every address."""
    phys = guest_phys(addr)
    if SR_RAM_BASE <= phys < SR_RAM_BASE + SR_RAM_SIZE:
        return "ram"
    if SR_VRAM_BASE <= phys < SR_VRAM_BASE + SR_VRAM_SIZE:
        return "vram"
    if SR_SCRATCHPAD_BASE <= phys < SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE:
        return "scratchpad"
    if phys < GUEST_ARENA_END:
        return "unmapped-arena"
    return "out-of-arena"


def guest_span_validate(addr, size, max_bytes):
    """Overflow-safe validation of the COMPLETE span [addr, addr+size).

    Returns (ok, reason).  Mirrors sr_inrange_n(): the arena check is done by
    subtraction so a wrapped ``phys + size`` can never alias into a small value
    that passes.  Zero-size and negative spans are rejected deterministically.
    """
    if isinstance(size, bool) or not isinstance(size, int):
        return False, "size must be an integer"
    if size < 0:
        return False, "size must not be negative"
    if size == 0:
        return False, "zero-size spans are rejected"
    if size > max_bytes:
        return False, "size %d exceeds the %d-byte budget" % (size, max_bytes)
    phys = guest_phys(addr)
    if phys >= GUEST_ARENA_END:
        return False, ("guest 0x%08x resolves to phys 0x%08x, outside the "
                       "guest arena [0, 0x%08x)") % (addr, phys, GUEST_ARENA_END)
    if phys > GUEST_ARENA_END - size:
        return False, ("span 0x%08x+0x%x crosses the arena end 0x%08x"
                       ) % (addr, size, GUEST_ARENA_END)
    start_region = guest_region(addr)
    # NOTE: this is deliberately STRICTER than the runtime's sr_guest_span_*,
    # which accepts any phys < 0x0C000000.  A debugger should only touch regions
    # it understands (RAM/VRAM/scratchpad); the rest of the arena is classified
    # 'unmapped-arena' and rejected.  Do not 'fix' this to match the runtime.
    if start_region not in SUPPORTED_REGIONS:
        return False, ("guest 0x%08x is in unsupported region '%s'"
                       ) % (addr, start_region)
    # The span must not straddle a region boundary: every byte must lie in the
    # same supported region (regions are disjoint and contiguous, so checking the
    # last byte is sufficient once the arena bounds above have passed).
    end_region = guest_region(addr + size - 1)
    if end_region != start_region:
        return False, ("span crosses region boundary ('%s' -> '%s')"
                       ) % (start_region, end_region)
    return True, start_region


def host_offset_for_guest(g_mem, guest_addr):
    """SR_HOST(a) == g_mem + (int32_t)(SR_PHYS(a) - SR_RAM_BASE)."""
    return g_mem + (guest_phys(guest_addr) - SR_RAM_BASE)


# --- PE image identity helpers -----------------------------------------------
def sha256_file(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def pe_prefix_bytes(path, n=PE_PREFIX_BYTES):
    try:
        with open(path, "rb") as f:
            return f.read(n)
    except OSError:
        return None


def image_identity_matches(disk_prefix, memory_prefix):
    """Byte-identical comparison of the PE prefix read from disk vs the prefix
    read from the attached process at its image base.  A mismatch means the
    running image is stale or a different build, and mutation is refused."""
    if not disk_prefix or not memory_prefix:
        return False
    if len(disk_prefix) != len(memory_prefix):
        return False
    return disk_prefix == memory_prefix


# --- Process discovery -------------------------------------------------------
def normalize_exe_path(path):
    """Canonical normalized executable path for comparison.  Resolves
    symlinks/junctions and normalizes case so the same file under different
    spellings is not treated as a different process image."""
    if not path:
        return ""
    try:
        return os.path.normcase(os.path.realpath(os.path.abspath(path)))
    except (OSError, ValueError):
        return os.path.normcase(os.path.abspath(path))


def select_process_candidate(candidates, expected_path):
    """Select a unique process by EXACT normalized executable path.

    candidates: iterable of {"pid", "exe_path", "base_address"}.
    Returns (chosen or None, reason).  Name-only matches are never auto-selected;
    more than one exact match is ambiguous and refused.
    """
    expected = normalize_exe_path(expected_path)
    if not expected:
        return None, "no expected executable path configured"
    exact = []
    for c in candidates:
        if normalize_exe_path(c.get("exe_path", "")) == expected:
            exact.append(c)
    if len(exact) == 1:
        return exact[0], "unique-exact-path"
    if len(exact) > 1:
        return None, ("ambiguous: %d processes match the exact expected path; "
                      "use --pid to select one explicitly" % len(exact))
    return None, ("no process matches the exact expected executable path "
                  "(name-only matches are not used for attachment)")


def query_full_image_path(kernel32, h_process):
    try:
        buf = (ctypes.c_wchar * 1024)()
        size = ctypes.c_ulong(len(buf))
        if kernel32.QueryFullProcessImageNameW(
                h_process, 0, ctypes.byref(buf), ctypes.byref(size)):
            return buf.value or ""
    except Exception:
        pass
    return ""


def query_module_base(psapi, h_process):
    try:
        h_modules = (ctypes.c_void_p * 64)()
        cb_needed = ctypes.c_ulong()
        if psapi.EnumProcessModules(h_process, ctypes.byref(h_modules),
                                    ctypes.sizeof(h_modules),
                                    ctypes.byref(cb_needed)):
            return int(h_modules[0]) if cb_needed.value >= ctypes.sizeof(ctypes.c_void_p) else 0
    except Exception:
        pass
    return 0


def enumerate_process_candidates(expected_name="hst.exe"):
    """Enumerate processes whose executable basename matches, with full image
    paths and main-module base addresses.  Read-only; never requests write or
    suspend access during discovery."""
    if sys.platform != "win32":
        return []
    try:
        kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
        psapi = ctypes.WinDLL("psapi.dll", use_last_error=True)
    except Exception:
        return []

    candidates = []
    arr = (ctypes.c_ulong * 1024)()
    cb_needed = ctypes.c_ulong()
    if not psapi.EnumProcesses(ctypes.byref(arr), ctypes.sizeof(arr),
                               ctypes.byref(cb_needed)):
        return []
    count = cb_needed.value // ctypes.sizeof(ctypes.c_ulong)
    for i in range(count):
        pid = arr[i]
        if pid == 0:
            continue
        h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        try:
            name_buf = (ctypes.c_char * 260)()
            if not psapi.GetModuleBaseNameA(h, None, ctypes.byref(name_buf), 260):
                continue
            name = name_buf.value.decode("latin1", errors="replace")
            if name.lower() != expected_name.lower():
                continue
            full_path = query_full_image_path(kernel32, h)
            # EnumProcessModules needs slightly more rights; retry if limited
            # rights were insufficient.
            h2 = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                      False, pid)
            try:
                base = query_module_base(psapi, h2) if h2 else 0
            finally:
                if h2:
                    kernel32.CloseHandle(h2)
            candidates.append({
                "pid": int(pid),
                "exe_path": full_path,
                "base_address": base,
            })
        finally:
            kernel32.CloseHandle(h)
    return candidates


def find_repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def get_symbol_rvas(exe_path):
    """Resolve g_mem/s_cpu RVAs from the attached executable via nm.

    Returns (rvas, provenance) where provenance is "nm" only when BOTH symbols
    were resolved from this exact executable, else "fallback".  Address-space
    mutation requires provenance == "nm".
    """
    rvas = dict(DEFAULT_RVAS)
    if not exe_path or not os.path.exists(exe_path):
        return rvas, "fallback"

    nm_candidate = os.environ.get("NM")
    nm_paths = [
        nm_candidate,
        shutil.which("nm"),
        shutil.which("nm.exe"),
        os.path.join(os.environ.get("MSYSTEM_PREFIX", ""), "bin", "nm.exe") if os.environ.get("MSYSTEM_PREFIX") else None,
        "C:\\msys64\\ucrt64\\bin\\nm.exe",
        "C:\\msys64\\mingw64\\bin\\nm.exe",
        "nm.exe",
        "nm",
    ]
    nm_path = None
    for p in nm_paths:
        if not p:
            continue
        try:
            subprocess.run([p, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            nm_path = p
            break
        except Exception:
            continue

    if not nm_path:
        return rvas, "fallback"

    try:
        res = subprocess.run([nm_path, exe_path], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        found = {}
        for line in res.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                addr_str, sym_type, name = parts[0], parts[1], parts[2]
                if name in ("g_mem", "s_cpu"):
                    addr = int(addr_str, 16)
                    found[name] = addr - DEFAULT_IMAGE_BASE
        for name in ("g_mem", "s_cpu"):
            if name in found:
                rvas[name] = found[name]
        if "g_mem" in found and "s_cpu" in found:
            return rvas, "nm"
        return rvas, "fallback"
    except Exception:
        return rvas, "fallback"


# --- Mock State Manager ------------------------------------------------------
def get_mock_state_path():
    repo_root = find_repo_root()
    return os.path.join(repo_root, "build", "hst", "mock_debug_state.json")


def load_mock_state():
    path = get_mock_state_path()
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            pass

    # Initialize mock state
    mock = {
        "status": "running",
        "cpu": {
            "r": [0] * 32,
            "hi": 0,
            "lo": 0,
            "pc": 0x08804000,
            "f": [0.0] * 32,
            "fcr31": 0,
            "fpcond": 0,
            "v": [0.0] * 128,
            "vfpuCtrl": [0] * 16,
            "status": 0,
            "next_pc": 0x08804004,
            "in_delay_slot": 0
        },
        "memory": {}
    }
    # Pre-populate some mock memory strings
    mock["cpu"]["r"][29] = 0x09FF0000  # SP
    mock["cpu"]["r"][31] = 0x08800100  # RA

    # Store some text in mock RAM (0x08800000)
    import_text = "Hot Shots Tennis: Get a Grip - Interactive Debugging Active."
    for idx, ch in enumerate(import_text):
        mock["memory"][str(0x08800000 + idx)] = ord(ch)

    # Also write a couple of numerical test values
    mock["memory"][str(0x08900000)] = 0x78
    mock["memory"][str(0x08900001)] = 0x56
    mock["memory"][str(0x08900002)] = 0x34
    mock["memory"][str(0x08900003)] = 0x12  # 0x12345678 in Little-Endian

    save_mock_state(mock)
    return mock


def save_mock_state(state):
    path = get_mock_state_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


# --- Active Execution Implementation ----------------------------------------
MUTATING_ACTIONS = frozenset(("pause", "resume", "write_mem", "write_cpu"))


class MemoryDebugger:
    def __init__(self, simulate=False, mutate=False, pid=None):
        self.mutate_enabled = bool(mutate)
        self.is_simulated = bool(simulate)
        self.pid = 0
        self.base_address = 0
        self.candidate = None
        self.image_verified = False
        self.image_sha256 = None
        self.identity_note = None

        if self.is_simulated:
            self.mock = load_mock_state()
            self.rvas, self.rva_provenance = DEFAULT_RVAS, "fallback"
            self.proc_info = None
            self.is_offline = False
            return

        expected_exe = os.path.join(find_repo_root(), EXPECTED_EXE_REL)
        candidates = enumerate_process_candidates()
        if pid is not None:
            chosen = next((c for c in candidates if c["pid"] == int(pid)), None)
            if chosen is None:
                self.proc_info = None
                self.is_offline = True
                self.rvas, self.rva_provenance = get_symbol_rvas(expected_exe)
                self.identity_note = "pid %s not found or not hst.exe" % pid
                return
            chosen, reason = chosen, "explicit-pid"
            self.identity_note = ("selected by --pid %s; image path %s"
                                  % (pid, chosen.get("exe_path") or "<unknown>"))
        else:
            chosen, reason = select_process_candidate(candidates, expected_exe)
            self.identity_note = reason
            if chosen is None:
                self.proc_info = None
                self.is_offline = True
                self.rvas, self.rva_provenance = get_symbol_rvas(expected_exe)
                return

        self.proc_info = chosen
        self.is_offline = False
        self.pid = chosen["pid"]
        self.base_address = chosen.get("base_address") or 0
        self.candidate = chosen

        # Resolve symbols from the ATTACHED executable path (never the fallback
        # when mutating).  For --pid the executable may live outside build/hst/,
        # so the on-disk identity source is the candidate's own path.
        disk_source = chosen.get("exe_path") or expected_exe
        self.rvas, self.rva_provenance = get_symbol_rvas(disk_source)
        self.image_sha256 = sha256_file(disk_source) if disk_source else None

        # Verify image identity: first page of the running image vs the on-disk
        # executable at the resolved path.
        try:
            disk_prefix = pe_prefix_bytes(disk_source)
            mem_prefix = self._read_process_bytes(self.base_address, PE_PREFIX_BYTES) \
                if self.base_address else None
            self.image_verified = bool(
                disk_prefix and mem_prefix and
                image_identity_matches(disk_prefix, mem_prefix))
        except Exception:
            self.image_verified = False

    # --- Mutation gate ------------------------------------------------------
    def _mutation_allowed(self, action, needs_rvas, needs_image):
        """Central fail-closed policy for every mutating action.  Returns
        (ok, reason)."""
        if action not in MUTATING_ACTIONS:
            return False, "'%s' is not a mutating action" % action
        if not self.mutate_enabled:
            return False, ("'%s' requires the explicit --mutate flag; the "
                           "debugger is read-only by default" % action)
        if self.is_simulated:
            # Simulation only touches the local mock JSON file.  It can never
            # fall through to a live process because the live branch is guarded
            # by is_simulated at every call site.
            return True, None
        if self.candidate is None or self.is_offline:
            return False, ("'%s' requires a resolved, uniquely identified hst.exe "
                           "process" % action)
        if needs_image and not self.image_verified:
            return False, ("'%s' refused: PE image identity is not verified "
                           "(stale or mismatched build)" % action)
        if needs_rvas and self.rva_provenance != "nm":
            return False, ("'%s' refused: symbol RVAs are not resolved from the "
                           "attached image (unverified fallback RVAs are "
                           "read-only)" % action)
        return True, None

    def _guest_memory_resolver(self, guest_addr, size, max_bytes):
        """Validate the full guest span, then resolve g_mem and produce the host
        pointer.  Returns (host_addr or None, error-or-None)."""
        ok, reason = guest_span_validate(guest_addr, size, max_bytes)
        if not ok:
            return None, reason
        g_mem = self._resolve_g_mem()
        if g_mem is None:
            return None, "could not resolve g_mem (process uninitialized?)"
        return host_offset_for_guest(g_mem, guest_addr), None

    def execute_command(self, action, args):
        if self.is_offline and action not in ("status", "trace_exit"):
            return {
                "success": False,
                "online": False,
                "mode": "offline",
                "error": ("hst.exe is not running (or not uniquely identified); "
                          "start the runtime before using live debug commands"),
            }
        if action == "status":
            return self.get_status()
        elif action == "pause":
            return self.pause()
        elif action == "resume":
            return self.resume()
        elif action == "read_mem":
            try:
                addr = parse_uint32_arg(args[0])
                size = int(args[1])
            except (IndexError, ValueError) as e:
                return {"error": "invalid read_mem arguments: %s" % e}
            fmt = args[2] if len(args) > 2 else "hex"
            return self.read_mem(addr, size, fmt)
        elif action == "write_mem":
            try:
                addr = parse_uint32_arg(args[0])
            except (IndexError, ValueError) as e:
                return {"error": "invalid write_mem address: %s" % e}
            val_hex = args[1] if len(args) > 1 else ""
            return self.write_mem(addr, val_hex)
        elif action == "read_cpu":
            return self.read_cpu()
        elif action == "write_cpu":
            try:
                field = args[0]
                val = parse_uint32_arg(args[1])
            except (IndexError, ValueError) as e:
                return {"error": "invalid write_cpu arguments: %s" % e}
            return self.write_cpu(field, val)
        elif action == "read_vram":
            return self.read_vram()
        elif action == "trace_exit":
            return self.trace_exit()
        else:
            return {"error": "Unknown action: %s" % action}

    def get_status(self):
        if self.is_simulated:
            assert self.mock is not None
            return {
                "online": False,
                "mode": "simulation",
                "status": self.mock["status"],
                "rvas": self.rvas,
                "rva_provenance": self.rva_provenance,
                "mutation": "enabled" if self.mutate_enabled else "disabled",
            }
        if self.is_offline:
            return {
                "online": False,
                "mode": "offline",
                "status": "not-running",
                "identity_note": self.identity_note,
                "mutation": "enabled" if self.mutate_enabled else "disabled",
                "rvas": {k: "0x%08x" % v for k, v in self.rvas.items()},
                "rva_provenance": self.rva_provenance,
            }

        kernel32 = ctypes.WinDLL('kernel32.dll', use_last_error=True)
        h_process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION, False, self.pid)
        online = h_process is not None
        if h_process:
            kernel32.CloseHandle(h_process)

        status = "running"
        status_file = os.path.join(find_repo_root(), "build", "hst", "paused.flag")
        exit_file = os.path.join(find_repo_root(), "build", "hst", "exited.flag")
        if os.path.exists(exit_file):
            status = "exited"
        elif os.path.exists(status_file):
            status = "paused"

        return {
            "online": online,
            "mode": "process",
            "pid": self.pid,
            "base_address": "0x%016x" % self.base_address if self.base_address else None,
            "status": status,
            "image_verified": self.image_verified,
            "image_sha256": self.image_sha256,
            "identity_note": self.identity_note,
            "mutation": "enabled" if self.mutate_enabled else "disabled",
            "rvas": {k: "0x%08x" % v for k, v in self.rvas.items()},
            "rva_provenance": self.rva_provenance,
        }

    def pause(self):
        ok, reason = self._mutation_allowed("pause", needs_rvas=False, needs_image=False)
        if not ok:
            return {"success": False, "error": reason}
        if self.is_simulated:
            assert self.mock is not None
            self.mock["status"] = "paused"
            save_mock_state(self.mock)
            return {"success": True, "status": "paused", "mode": "simulation"}

        kernel32 = ctypes.WinDLL('kernel32.dll', use_last_error=True)
        ntdll = ctypes.WinDLL('ntdll.dll', use_last_error=True)
        h_process = kernel32.OpenProcess(0x0800, False, self.pid)
        if h_process:
            ntdll.NtSuspendProcess(h_process)
            kernel32.CloseHandle(h_process)
            status_file = os.path.join(find_repo_root(), "build", "hst", "paused.flag")
            with open(status_file, "w") as f:
                f.write("1")
            return {"success": True, "status": "paused", "mode": "process"}
        return {"success": False, "error": "Could not open process for suspending"}

    def resume(self):
        ok, reason = self._mutation_allowed("resume", needs_rvas=False, needs_image=False)
        if not ok:
            return {"success": False, "error": reason}
        if self.is_simulated:
            assert self.mock is not None
            self.mock["status"] = "running"
            save_mock_state(self.mock)
            return {"success": True, "status": "running", "mode": "simulation"}

        kernel32 = ctypes.WinDLL('kernel32.dll', use_last_error=True)
        ntdll = ctypes.WinDLL('ntdll.dll', use_last_error=True)
        h_process = kernel32.OpenProcess(0x0800, False, self.pid)
        if h_process:
            ntdll.NtResumeProcess(h_process)
            kernel32.CloseHandle(h_process)
            status_file = os.path.join(find_repo_root(), "build", "hst", "paused.flag")
            if os.path.exists(status_file):
                os.remove(status_file)
            return {"success": True, "status": "running", "mode": "process"}
        return {"success": False, "error": "Could not open process for resuming"}

    def _read_process_bytes(self, target_addr, size):
        if self.is_simulated:
            return None
        if size <= 0 or size > MAX_READ_BYTES:
            return None
        kernel32 = ctypes.WinDLL('kernel32.dll', use_last_error=True)
        h_process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                         False, self.pid)
        if not h_process:
            return None
        try:
            buf = (ctypes.c_ubyte * size)()
            bytes_read = ctypes.c_size_t()
            ok = kernel32.ReadProcessMemory(
                h_process, ctypes.c_void_p(target_addr), ctypes.byref(buf),
                size, ctypes.byref(bytes_read))
            if ok and bytes_read.value == size:
                return bytes(buf)
            return None
        finally:
            kernel32.CloseHandle(h_process)

    def _write_process_bytes(self, target_addr, data):
        if self.is_simulated:
            return False
        if not data or len(data) > MAX_WRITE_BYTES:
            return False
        kernel32 = ctypes.WinDLL('kernel32.dll', use_last_error=True)
        h_process = kernel32.OpenProcess(0x0020 | 0x0008, False, self.pid)
        if not h_process:
            return False
        try:
            size = len(data)
            buf = (ctypes.c_ubyte * size).from_buffer_copy(data)
            bytes_written = ctypes.c_size_t()
            ok = kernel32.WriteProcessMemory(
                h_process, ctypes.c_void_p(target_addr), ctypes.byref(buf),
                size, ctypes.byref(bytes_written))
            return bool(ok and bytes_written.value == size)
        finally:
            kernel32.CloseHandle(h_process)

    def _resolve_g_mem(self):
        """Read the g_mem pointer value from the attached process (or None)."""
        if self.is_simulated:
            return 0x08800000
        if self.base_address == 0:
            return None
        g_mem_ptr_addr = self.base_address + self.rvas["g_mem"]
        g_mem_bytes = self._read_process_bytes(g_mem_ptr_addr, 8)
        if not g_mem_bytes:
            return None
        g_mem_val = int.from_bytes(g_mem_bytes, byteorder='little')
        if g_mem_val == 0:
            return None
        return g_mem_val

    def _resolve_cpu_state_addr(self):
        if self.is_simulated:
            return None
        if self.base_address == 0:
            return None
        s_cpu_ptr_addr = self.base_address + self.rvas["s_cpu"]
        s_cpu_bytes = self._read_process_bytes(s_cpu_ptr_addr, 8)
        if not s_cpu_bytes:
            return None
        s_cpu_val = int.from_bytes(s_cpu_bytes, byteorder='little')
        return s_cpu_val if s_cpu_val != 0 else None

    def read_mem(self, addr, size, fmt):
        if fmt not in ("hex", "string", "words"):
            return {"error": "unknown format '%s' (expected hex, string, words)" % fmt}
        if self.is_simulated:
            assert self.mock is not None
            ok, reason = guest_span_validate(addr, size, MAX_READ_BYTES)
            if not ok:
                return {"error": "guest span rejected: %s" % reason}
            data = bytearray(size)
            for idx in range(size):
                key = str(addr + idx)
                data[idx] = self.mock["memory"].get(key, 0)
            data = bytes(data)
        else:
            target_addr, err = self._guest_memory_resolver(addr, size, MAX_READ_BYTES)
            if target_addr is None:
                return {"error": "guest span rejected: %s" % err}
            data = self._read_process_bytes(target_addr, size)
            if not data:
                return {"error": ("failed to read process memory at host "
                                  "0x%016x") % target_addr}

        if fmt == "string":
            end = data.find(0)
            string_val = (data[:end].decode("utf-8", errors="replace")
                          if end != -1 else data.decode("utf-8", errors="replace"))
            return {"address": "0x%08x" % addr, "string": string_val}
        elif fmt == "words":
            words = []
            for idx in range(0, len(data), 4):
                chunk = data[idx:idx + 4]
                if len(chunk) == 4:
                    val = int.from_bytes(chunk, byteorder='little')
                    words.append("0x%08x" % val)
            return {"address": "0x%08x" % addr, "words": words}
        else:
            hex_str = " ".join("%02x" % b for b in data)
            return {"address": "0x%08x" % addr, "hex": hex_str, "size": len(data)}

    def write_mem(self, addr, val_hex):
        ok_gate, gate_reason = self._mutation_allowed(
            "write_mem", needs_rvas=True, needs_image=True)
        if not ok_gate:
            return {"success": False, "error": gate_reason}
        try:
            val_bytes = bytes.fromhex(val_hex.replace(" ", "").replace(",", ""))
        except Exception as e:
            return {"success": False, "error": "Invalid hex sequence: %s" % e}
        if not val_bytes:
            return {"success": False, "error": "refusing zero-byte write"}
        if len(val_bytes) > MAX_WRITE_BYTES:
            return {"success": False,
                    "error": "write of %d bytes exceeds the %d-byte budget"
                             % (len(val_bytes), MAX_WRITE_BYTES)}

        if self.is_simulated:
            assert self.mock is not None
            ok, reason = guest_span_validate(addr, len(val_bytes), MAX_WRITE_BYTES)
            if not ok:
                return {"success": False, "error": "guest span rejected: %s" % reason}
            for idx, b in enumerate(val_bytes):
                self.mock["memory"][str(addr + idx)] = b
            save_mock_state(self.mock)
            return {"success": True, "address": "0x%08x" % addr,
                    "bytes_written": len(val_bytes)}
        else:
            target_addr, err = self._guest_memory_resolver(addr, len(val_bytes),
                                                           MAX_WRITE_BYTES)
            if target_addr is None:
                return {"success": False,
                        "error": "guest span rejected: %s" % err}
            ok = self._write_process_bytes(target_addr, val_bytes)
            if ok:
                return {"success": True, "address": "0x%08x" % addr,
                        "bytes_written": len(val_bytes)}
            return {"success": False,
                    "error": "failed to write process memory at host 0x%016x"
                             % target_addr}

    def read_vram(self):
        import base64
        addr = 0x04000000
        size = 2 * 1024 * 1024
        ok, reason = guest_span_validate(addr, size, MAX_READ_BYTES)
        if not ok:
            return {"error": "guest span rejected: %s" % reason}
        if self.is_simulated:
            assert self.mock is not None
            data = bytearray(size)
            for idx in range(size):
                key = str(addr + idx)
                data[idx] = self.mock["memory"].get(key, 0)
            data = bytes(data)
        else:
            target_addr, err = self._guest_memory_resolver(addr, size, MAX_READ_BYTES)
            if target_addr is None:
                return {"error": "guest span rejected: %s" % err}
            data = self._read_process_bytes(target_addr, size)
            if not data:
                return {"error": "failed to read VRAM memory at host 0x%016x"
                                 % target_addr}

        b64 = base64.b64encode(data).decode('ascii')
        return {"address": "0x%08x" % addr, "size": size, "base64": b64}

    def read_cpu(self):
        if self.is_simulated:
            assert self.mock is not None
            return {"success": True, "cpu": self.mock["cpu"], "mode": "simulation"}

        s_cpu_val = self._resolve_cpu_state_addr()
        if not s_cpu_val:
            return {"success": False,
                    "error": "s_cpu pointer is NULL or process uninitialized"}

        cpu_bytes = self._read_process_bytes(s_cpu_val, 864)
        if not cpu_bytes:
            return {"success": False, "error": "Failed to read CpuState structure"}

        cpu = {}
        cpu["r"] = [int.from_bytes(cpu_bytes[i * 4:(i + 1) * 4], byteorder='little')
                    for i in range(32)]
        cpu["hi"] = int.from_bytes(cpu_bytes[128:132], byteorder='little')
        cpu["lo"] = int.from_bytes(cpu_bytes[132:136], byteorder='little')
        cpu["pc"] = int.from_bytes(cpu_bytes[136:140], byteorder='little')

        import struct
        cpu["f"] = [struct.unpack('<f', cpu_bytes[140 + i * 4:140 + (i + 1) * 4])[0]
                    for i in range(32)]
        cpu["fcr31"] = int.from_bytes(cpu_bytes[268:272], byteorder='little')
        cpu["fpcond"] = int.from_bytes(cpu_bytes[272:276], byteorder='little')
        cpu["v"] = [struct.unpack('<f', cpu_bytes[276 + i * 4:276 + (i + 1) * 4])[0]
                    for i in range(128)]
        cpu["vfpuCtrl"] = [int.from_bytes(cpu_bytes[788 + i * 4:788 + (i + 1) * 4],
                                          byteorder='little') for i in range(16)]
        cpu["status"] = int.from_bytes(cpu_bytes[852:856], byteorder='little')
        cpu["next_pc"] = int.from_bytes(cpu_bytes[856:860], byteorder='little')
        cpu["in_delay_slot"] = int.from_bytes(cpu_bytes[860:864], byteorder='little')

        return {"success": True, "cpu": cpu, "mode": "process"}

    def write_cpu(self, field, val):
        ok_gate, gate_reason = self._mutation_allowed(
            "write_cpu", needs_rvas=True, needs_image=True)
        if not ok_gate:
            return {"success": False, "error": gate_reason}

        offset, is_float = _cpu_field_offset(field)
        if offset is None:
            return {"success": False, "error": "Invalid CpuState field: %s" % field}

        if self.is_simulated:
            assert self.mock is not None
            if field.startswith("r") and field[1:].isdigit():
                self.mock["cpu"]["r"][int(field[1:])] = val
            elif field.startswith("f") and field[1:].isdigit():
                self.mock["cpu"]["f"][int(field[1:])] = float(val)
            elif field.startswith("v") and field[1:].isdigit():
                self.mock["cpu"]["v"][int(field[1:])] = float(val)
            else:
                self.mock["cpu"][field] = val
            save_mock_state(self.mock)
            return {"success": True, "field": field,
                    "value_written": val, "mode": "simulation"}

        s_cpu_val = self._resolve_cpu_state_addr()
        if not s_cpu_val:
            return {"success": False,
                    "error": "s_cpu pointer is NULL or process uninitialized"}

        target_addr = s_cpu_val + offset
        if is_float:
            import struct
            data = struct.pack('<f', float(val))
        else:
            data = int(val).to_bytes(4, byteorder='little', signed=False)

        ok = self._write_process_bytes(target_addr, data)
        if ok:
            return {"success": True, "field": field,
                    "value_written": val, "mode": "process"}
        return {"success": False,
                "error": "failed to write register at host 0x%016x" % target_addr}

    def trace_exit(self):
        dump_path = os.path.join(find_repo_root(), "build", "hst", "crash_dump.bin")
        if not os.path.exists(dump_path):
            return {"error": "crash_dump.bin not found. Did the game exit?"}

        with open(dump_path, "rb") as f:
            cpu_bytes = f.read(864)
            stack_bytes = f.read()

        if len(cpu_bytes) < 864:
            return {"error": "Invalid crash_dump.bin size"}

        cpu = {}
        cpu["r"] = [int.from_bytes(cpu_bytes[i * 4:(i + 1) * 4], byteorder='little')
                    for i in range(32)]
        cpu["pc"] = int.from_bytes(cpu_bytes[136:140], byteorder='little')

        pc = cpu["pc"]
        sp = cpu["r"][29]
        ra = cpu["r"][31]

        frames = []
        frames.append({"pc": "0x%08x" % pc, "ra": "0x%08x" % ra,
                       "sp": "0x%08x" % sp, "note": "Current state"})

        # Heuristic stack scan
        sp_base = sp & 0xFFFF0000
        offset = sp - sp_base
        if offset >= 0 and offset < len(stack_bytes):
            for i in range(offset, len(stack_bytes) - 3, 4):
                val = int.from_bytes(stack_bytes[i:i + 4], byteorder='little')
                # Typical PSP executable memory is 0x08800000 to 0x0C000000
                if 0x08800000 <= val < 0x0C000000:
                    frames.append({
                        "pc": "0x%08x" % val,
                        "ra": "N/A",
                        "sp": "0x%08x" % (sp_base + i),
                        "note": "Scanned from stack"
                    })
                    if len(frames) >= 10:
                        break

        return {"success": True, "frames": frames}


def parse_uint32_arg(text):
    """Exact unsigned-32-bit parse; no silent prefix or float coercion."""
    s = text.strip()
    if s.lower().startswith("0x"):
        digits = s[2:]
        if not digits or any(c not in "0123456789abcdefABCDEF" for c in digits):
            raise ValueError("not an unsigned integer: %r" % text)
        val = int(digits, 16)
    else:
        if not s or any(c not in "0123456789" for c in s):
            raise ValueError("not an unsigned integer: %r" % text)
        val = int(s, 10)
    if val > 0xFFFFFFFF:
        raise ValueError("value exceeds UINT32_MAX: %r" % text)
    return val


def _cpu_field_offset(field):
    """Map a CpuState field name to (byte offset, is_float) or (None, False)."""
    if field.startswith("r") and field[1:].isdigit():
        reg_idx = int(field[1:])
        if 0 <= reg_idx < 32:
            return reg_idx * 4, False
    if field == "hi":
        return 128, False
    if field == "lo":
        return 132, False
    if field == "pc":
        return 136, False
    if field == "fcr31":
        return 268, False
    if field == "fpcond":
        return 272, False
    if field == "status":
        return 852, False
    if field == "next_pc":
        return 856, False
    if field == "in_delay_slot":
        return 860, False
    if field.startswith("f") and field[1:].isdigit():
        f_idx = int(field[1:])
        if 0 <= f_idx < 32:
            return 140 + f_idx * 4, True
    if field.startswith("v") and field[1:].isdigit():
        v_idx = int(field[1:])
        if 0 <= v_idx < 128:
            return 276 + v_idx * 4, True
    return None, False


def main():
    argv = sys.argv[1:]
    simulate = "--simulate" in argv
    mutate = "--mutate" in argv
    pid = None
    remaining = []
    skip_next = False
    for i, arg in enumerate(argv):
        if skip_next:
            skip_next = False
            continue
        if arg == "--simulate" or arg == "--mutate":
            continue
        if arg == "--pid":
            if i + 1 < len(argv):
                try:
                    pid = int(argv[i + 1], 10)
                except ValueError:
                    print(json.dumps({"error": "invalid --pid value: %r"
                                      % argv[i + 1]}))
                    sys.exit(1)
                skip_next = True
            else:
                print(json.dumps({"error": "--pid requires a value"}))
                sys.exit(1)
            continue
        if arg.startswith("--pid="):
            try:
                pid = int(arg.split("=", 1)[1], 10)
            except ValueError:
                print(json.dumps({"error": "invalid --pid value: %r" % arg}))
                sys.exit(1)
            continue
        remaining.append(arg)

    if not remaining:
        print(json.dumps({"error": "Usage: mem_debug.py [--simulate] [--mutate] "
                                   "[--pid <pid>] <action> [args]"}))
        sys.exit(1)
    action = remaining[0]
    args = remaining[1:]

    try:
        dbg = MemoryDebugger(simulate=simulate, mutate=mutate, pid=pid)
        res = dbg.execute_command(action, args)
        print(json.dumps(res, indent=2))
    except Exception as e:
        print(json.dumps({"error": "Unhandled script error: %s" % e}))
        sys.exit(1)


if __name__ == "__main__":
    main()
