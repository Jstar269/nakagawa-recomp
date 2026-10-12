# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Manifest-driven production-route checks for the two profile-zero fixtures."""

from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import ge_stat_windows  # noqa: E402
import prxload  # noqa: E402
import vulkan_sdk  # noqa: E402
from test_build_truth import _scratch_build_root  # noqa: E402


MANIFEST_PATHS = (
    ROOT / "assets" / "titles" / "synthetic.json",
    ROOT / "assets" / "titles" / "synthetic-title2.json",
)
CASE_EVIDENCE = {
    "program-image": "SOURCE_SHAPE",
    "aot-generation": "SOURCE_SHAPE",
    "runtime-dispatch": "PRODUCTION_DISPATCH",
    "scheduler-vblank-input": "PRODUCTION_DISPATCH",
    "ge-presentation": "PRODUCTION_DISPATCH",
    "audio-helper": "PRODUCTION_HELPER",
    "savedata-roundtrip": "PRODUCTION_DISPATCH",
}
SAVE_BYTES = b"PROFILE_ZERO_SAVE_V1\n"
VBLANK_SCRIPT = "12 4000 4\n240 0008 4\n"
# Committed PSPDEV build output of fixtures/profile_zero/{main.c,Makefile}.
# The route consumes these bytes on hosts without PSPDEV/PSPSDK (hosted CI);
# where the toolchain exists, the same build is reproduced and compared.
# The bytes are reproducible only with the distribution pinned by
# assets/upstream/pspdev.lock.json (#708).
PREBUILT_DIR = ROOT / "fixtures" / "profile_zero" / "prebuilt"
PREBUILT_SUMS = PREBUILT_DIR / "SHA256SUMS"
PREBUILT_EBOOT = PREBUILT_DIR / "EBOOT.PBP"
PREBUILT_GUEST = PREBUILT_DIR / "profile_zero_guest.prx"
PSPDEV_LOCK_PATH = ROOT / "assets" / "upstream" / "pspdev.lock.json"
PSPDEV_EVIDENCE_PATH = ROOT / "assets" / "upstream" / "pspdev.evidence.json"
PSPDEV_README_PATH = ROOT / "fixtures" / "profile_zero" / "README.md"
PSPDEV_REQUIRED_TOOLS = ("psp-config", "psp-gcc", "psp-prxgen", "pack-pbp")


def _manifest(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _wsl_executable() -> str | None:
    return shutil.which("wsl.exe") or shutil.which("wsl")


def _pspdev_backend() -> tuple[str, str | None] | None:
    required = ("psp-config", "psp-gcc", "psp-prxgen", "pack-pbp")
    if all(shutil.which(tool) for tool in required):
        return ("host", None)
    wsl = _wsl_executable()
    if not wsl:
        return None
    check = subprocess.run(
        [
            wsl,
            "-d",
            "Ubuntu",
            "--",
            "bash",
            "-lc",
            "test -x /usr/local/pspdev/bin/psp-config && "
            "test -x /usr/local/pspdev/bin/psp-gcc && "
            "test -x /usr/local/pspdev/bin/psp-prxgen && "
            "test -x /usr/local/pspdev/bin/pack-pbp && "
            "test -f \"$(/usr/local/pspdev/bin/psp-config --pspsdk-path)/lib/build.mak\"",
        ],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    return ("wsl", wsl) if check.returncode == 0 else None


def _wsl_path(wsl: str, path: Path) -> str:
    converted = subprocess.run(
        [wsl, "-d", "Ubuntu", "--", "wslpath", "-u", path.as_posix()],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    if converted.returncode:
        raise AssertionError(f"WSL path conversion failed for {path}: {converted.stderr}")
    return converted.stdout.strip()


def _build_guest(manifest: dict, output_dir: Path, backend: tuple[str, str | None]) -> Path:
    profile = manifest["profile_zero"]
    build = profile["build"]
    source_dir = (ROOT / build["working_directory"]).resolve()
    makefile = (ROOT / build["makefile"]).resolve()
    game_name = manifest.get("game_name") or manifest["id"].replace("-", "_")
    output_dir.mkdir(parents=True, exist_ok=True)

    if backend[0] == "wsl":
        assert backend[1] is not None
        wsl = backend[1]
        completed = subprocess.run(
            [
                wsl,
                "-d",
                "Ubuntu",
                "--",
                "/usr/bin/env",
                "PSPDEV=/usr/local/pspdev",
                "PATH=/usr/local/pspdev/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                "make",
                "-j4",
                "--no-print-directory",
                "-B",
                "-f",
                _wsl_path(wsl, makefile),
                "-C",
                _wsl_path(wsl, output_dir),
                f"VPATH={_wsl_path(wsl, source_dir)}",
                f"TARGET={game_name}",
                str(build["target"]),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
    else:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if not make:
            raise AssertionError("PSPDEV is installed but GNU Make is unavailable")
        environment = os.environ.copy()
        ucrt = Path("C:/msys64/ucrt64/bin")
        if os.name == "nt" and ucrt.is_dir():
            environment["PATH"] = str(ucrt) + os.pathsep + environment.get("PATH", "")
        completed = subprocess.run(
            [
                make,
                "-j4",
                "--no-print-directory",
                "-B",
                "-f",
                str(makefile),
                "-C",
                str(output_dir),
                f"VPATH={source_dir}",
                f"TARGET={game_name}",
                str(build["target"]),
            ],
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
    if completed.returncode:
        raise AssertionError(
            f"PSPDEV fixture build failed for {manifest['id']} "
            f"(exit {completed.returncode}):\n{completed.stdout}\n{completed.stderr}"
        )
    eboot = output_dir / str(build["target"])
    guest = output_dir / f"{game_name}.prx"
    if not eboot.is_file() or not guest.is_file():
        raise AssertionError(
            f"PSPDEV did not produce both {build['target']} and {guest.name} "
            f"for {manifest['id']}"
        )
    return guest


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _verify_committed_fixture() -> dict[str, str]:
    """Fail closed unless the committed fixture matches its own digest list."""
    if not PREBUILT_SUMS.is_file():
        raise AssertionError(
            f"committed profile-zero fixture digest list is missing: {PREBUILT_SUMS} "
            "(rebuild it with PSPDEV; see fixtures/profile_zero/README.md)"
        )
    digests: dict[str, str] = {}
    for line in PREBUILT_SUMS.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]) or parts[1] in digests:
            raise AssertionError(f"malformed line in {PREBUILT_SUMS}: {line!r}")
        digests[parts[1]] = parts[0]
    for required in (PREBUILT_EBOOT.name, PREBUILT_GUEST.name):
        if required not in digests:
            raise AssertionError(f"{PREBUILT_SUMS} does not cover {required}")
    present = {
        path.name
        for path in PREBUILT_DIR.iterdir()
        if path.is_file() and path.name != PREBUILT_SUMS.name
    }
    if present != set(digests):
        raise AssertionError(
            f"committed profile-zero fixture files {sorted(present)} do not match "
            f"the digest list {sorted(digests)}"
        )
    for name, expected in sorted(digests.items()):
        actual = _sha256(PREBUILT_DIR / name)
        if actual != expected:
            raise AssertionError(
                f"committed profile-zero fixture {name} does not match SHA256SUMS: "
                f"expected {expected}, got {actual}"
            )
    return digests


def _materialize_committed_fixture(manifest: dict, output_dir: Path) -> Path:
    """Stage the committed PSPDEV build output where the route expects it."""
    digests = _verify_committed_fixture()
    build = manifest["profile_zero"]["build"]
    game_name = manifest.get("game_name") or manifest["id"].replace("-", "_")
    output_dir.mkdir(parents=True, exist_ok=True)
    eboot = output_dir / str(build["target"])
    guest = output_dir / f"{game_name}.prx"
    shutil.copyfile(PREBUILT_EBOOT, eboot)
    shutil.copyfile(PREBUILT_GUEST, guest)
    if _sha256(eboot) != digests[PREBUILT_EBOOT.name] or _sha256(guest) != digests[PREBUILT_GUEST.name]:
        raise AssertionError(f"staged profile-zero fixture drifted for {manifest['id']}")
    return guest


def _verify_rebuild_matches_committed(manifest: dict, output_dir: Path, guest: Path) -> None:
    """Fail closed when a PSPDEV rebuild differs from the committed fixture."""
    digests = _verify_committed_fixture()
    build = manifest["profile_zero"]["build"]
    for path, expected in (
        (output_dir / str(build["target"]), digests[PREBUILT_EBOOT.name]),
        (guest, digests[PREBUILT_GUEST.name]),
    ):
        if not path.is_file():
            raise AssertionError(
                f"PSPDEV rebuild for {manifest['id']} produced no {path.name}"
            )
        actual = _sha256(path)
        if actual != expected:
            raise AssertionError(
                f"PSPDEV rebuild for {manifest['id']} does not reproduce the committed "
                f"fixture {path.name}: expected sha256 {expected}, got {actual}. "
                "Regenerate and explain fixtures/profile_zero/prebuilt (see "
                "fixtures/profile_zero/README.md); drift must never be tolerated silently."
            )


def _pspdev_authority() -> dict:
    """Fail closed unless the lock pins the exact distribution and tool bytes."""
    try:
        lock = json.loads(PSPDEV_LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AssertionError(
            f"cannot read the PSPDEV lock {PSPDEV_LOCK_PATH}: {exc}"
        ) from exc
    distribution = lock.get("distribution")
    if not isinstance(distribution, dict):
        raise AssertionError(f"{PSPDEV_LOCK_PATH} has no distribution block")
    release = distribution.get("release")
    asset = distribution.get("archive_asset")
    if not release or not asset:
        raise AssertionError(
            f"{PSPDEV_LOCK_PATH} must pin an exact distribution release and "
            "archive_asset; the profile-zero rebuild contract is authoritative "
            "only for the distribution the lock names"
        )
    tool_versions = (lock.get("local_verification") or {}).get("tool_versions")
    if not isinstance(tool_versions, dict):
        raise AssertionError(f"{PSPDEV_LOCK_PATH} records no local tool evidence")
    tools: dict[str, tuple[int, str]] = {}
    for name in PSPDEV_REQUIRED_TOOLS:
        record = tool_versions.get(name)
        if not isinstance(record, str):
            raise AssertionError(
                f"{PSPDEV_LOCK_PATH} records no {name} identity in "
                "local_verification.tool_versions"
            )
        size = re.search(r"size=(\d+)", record)
        digest = re.search(r"sha256=([0-9a-f]{64})", record)
        if size is None or digest is None:
            raise AssertionError(
                f"{PSPDEV_LOCK_PATH} tool_versions.{name} must record size and sha256"
            )
        tools[name] = (int(size.group(1)), digest.group(1))
    return {"release": release, "asset": asset, "tools": tools}


def _wsl_tool_identity(wsl: str, tool: str) -> tuple[int, str]:
    probe = subprocess.run(
        [
            wsl,
            "-d",
            "Ubuntu",
            "--",
            "bash",
            "-c",
            f"stat -c %s /usr/local/pspdev/bin/{tool}; "
            f"sha256sum /usr/local/pspdev/bin/{tool}",
        ],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    lines = [line for line in probe.stdout.splitlines() if line.strip()]
    if probe.returncode or len(lines) < 2:
        raise AssertionError(
            f"cannot identify the WSL PSPDEV tool {tool} against the pinned "
            f"distribution: {probe.stderr.strip()}"
        )
    return int(lines[0]), lines[1].split()[0]


def _verify_backend_matches_authority(backend: tuple[str, str | None]) -> None:
    """Fail closed when the installed PSPDEV is not the distribution the lock pins.

    The committed fixture bytes are reproducible only from the pinned release
    asset. Debian-archive or other-release installations ship different tool
    binaries and must be rejected with an explicit identity mismatch instead of
    a confusing byte-drift error (#708).
    """
    authority = _pspdev_authority()
    mismatches = []
    for name in PSPDEV_REQUIRED_TOOLS:
        expected_size, expected_sha = authority["tools"][name]
        if backend[0] == "wsl":
            assert backend[1] is not None
            actual_size, actual_sha = _wsl_tool_identity(backend[1], name)
        else:
            located = shutil.which(name)
            if not located:
                raise AssertionError(f"PSPDEV tool {name} disappeared from PATH")
            resolved = Path(located)
            actual_size = resolved.stat().st_size
            actual_sha = _sha256(resolved)
        if (actual_size, actual_sha) != (expected_size, expected_sha):
            mismatches.append(
                f"{name}: found size={actual_size} sha256={actual_sha}, "
                f"lock records size={expected_size} sha256={expected_sha}"
            )
    if mismatches:
        raise AssertionError(
            "the installed PSPDEV is not the distribution pinned by "
            f"{PSPDEV_LOCK_PATH} ({authority['release']}, {authority['asset']}); "
            "the committed profile-zero fixture is reproducible only from that "
            "exact release asset, so the rebuild-and-compare refuses to run "
            "(see fixtures/profile_zero/README.md):\n" + "\n".join(mismatches)
        )


def _runtime_build_environment(test: unittest.TestCase) -> tuple[dict[str, str], str]:
    make = shutil.which("mingw32-make") or shutil.which("make")
    if not make:
        raise unittest.SkipTest("SKIP: GNU Make is unavailable for the package contract")
    environment = os.environ.copy()
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.is_dir():
        environment["PATH"] = str(ucrt) + os.pathsep + environment.get("PATH", "")
        sdk_value = environment.get("VULKAN_SDK")
        if sdk_value and sdk_value.rstrip("/") == "/ucrt64":
            sdk_value = "C:/msys64/ucrt64"
            environment["VULKAN_SDK"] = sdk_value
        try:
            sdk = vulkan_sdk.discover_vulkan_sdk(environment=sdk_value)
        except vulkan_sdk.VulkanSdkError as exc:
            raise unittest.SkipTest(
                f"SKIP: production package runtime requires a usable Vulkan SDK: {exc}"
            ) from exc
        environment["VULKAN_SDK"] = sdk.as_posix()
    environment["MAKEFLAGS"] = "-j4"
    # A scratch BUILD_ROOT, exported to the probe and to every Make run the package route
    # makes with this environment: parsing a Makefile writes its SDL3 discovery fragment
    # beneath BUILD_ROOT, which must not be the checkout's build/.
    build_root = _scratch_build_root(test, "profile-zero")
    environment["BUILD_ROOT"] = build_root.as_posix()
    probe = subprocess.run(
        [make, "--no-print-directory", "sdl3-check", f"BUILD_ROOT={build_root.as_posix()}"],
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    probe_text = (probe.stdout or "") + (probe.stderr or "")
    lowered = probe_text.lower()
    if probe.returncode and any(
        marker in lowered
        for marker in (
            "sdl3 dependency is missing",
            "explicit sdl3 directory does not exist",
            "no usable vulkan sdk found",
            "cannot find -lvulkan",
        )
    ):
        raise unittest.SkipTest(f"SKIP: public runtime link dependency is unavailable: {probe_text.strip()}")
    if probe.returncode:
        raise AssertionError(f"runtime dependency preflight failed:\n{probe_text}")
    return environment, make


def _read_ppm_metrics(path: Path) -> tuple[int, int]:
    raw = path.read_bytes().split(b"\n", 3)
    if len(raw) != 4 or raw[0] != b"P6":
        raise AssertionError(f"runtime framebuffer evidence is not a P6 image: {path}")
    width, height = (int(value) for value in raw[1].split())
    pixels = raw[3]
    if len(pixels) != width * height * 3:
        raise AssertionError("runtime framebuffer evidence is truncated")
    background = bytes((16, 24, 32))
    colors = {pixels[offset : offset + 3] for offset in range(0, len(pixels), 3)}
    foreground = sum(
        pixels[offset : offset + 3] != background
        for offset in range(0, len(pixels), 3)
    )
    return len(colors), foreground


def _run_package(manifest_path: Path, guest: Path, package_dir: Path,
                 build_environment: dict[str, str], make: str) -> dict[str, str]:
    manifest = _manifest(manifest_path)
    game_name = manifest.get("game_name") or manifest["id"].replace("-", "_")
    package_dir.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable,
        str(TOOLS / "title_codegen_plan.py"),
        str(manifest_path),
        "--package",
        "--public-safe",
        "--profile",
        "none",
        "--game-name",
        game_name,
        "--game-elf",
        str(guest),
        "--output-dir",
        str(package_dir),
        "--make-command",
        make,
        "--funcs-per-chunk",
        "256",
    ]
    built = subprocess.run(
        command,
        cwd=ROOT,
        env=build_environment,
        capture_output=True,
        text=True,
        timeout=600,
        check=False,
    )
    if built.returncode:
        raise AssertionError(
            f"package contract failed for {manifest['id']} "
            f"(exit {built.returncode}):\n{built.stdout}\n{built.stderr}"
        )

    package = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
    report = json.loads((package_dir / "build-report.json").read_text(encoding="utf-8"))
    if package.get("format") != "nakagawa-aot-package":
        raise AssertionError(f"{manifest['id']} did not produce the package contract")
    if package.get("title", {}).get("id") != manifest["id"]:
        raise AssertionError(f"package identity changed for {manifest['id']}")
    if report.get("artifacts", {}).get("executable") != package["executable"]["path"]:
        raise AssertionError(f"build report does not bind the package executable for {manifest['id']}")
    if (
        manifest.get("codegen_profile", "none") != "none"
        or package.get("cache", {}).get("codegen_options", {}).get("profile") != "none"
    ):
        raise AssertionError(f"{manifest['id']} selected an HST codegen profile")
    if not package.get("generated_objects"):
        raise AssertionError(f"AOT code was not generated for {manifest['id']}")

    executable = package_dir / package["executable"]["path"]
    image = package_dir / f"{game_name}_image.bin"
    if not executable.is_file() or not image.is_file():
        raise AssertionError(f"package executable or guest image is missing for {manifest['id']}")

    memstick = package_dir / "memstick"
    memstick.mkdir()
    padscript = package_dir / "padscript.txt"
    padscript.write_text(VBLANK_SCRIPT, encoding="ascii")
    boot_log = package_dir / "boot-events.log"
    perf_csv = package_dir / "perf.csv"
    environment = os.environ.copy()
    environment.update({
        "SR_DISPATCH_FATAL": "1",
        "SR_MEMSTICK": str(memstick),
        "SR_PADSCRIPT": str(padscript),
        "SR_INLOG": "1",
        "SR_NOINPUT": "1",
        "SR_GESTAT": "1",
        "SR_FBSNAP": "1",
        "SR_AUDIOSTAT": "1",
        "SR_AUDIO_BACKEND": "sdl",
        "SR_CALLCOUNT_ALL": "1",
        "SR_PERF": "1",
        "SR_PERF_CSV": str(perf_csv),
        "SR_BOOT_EVENT_FILE": str(boot_log),
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
    })
    if os.name == "nt" and Path("C:/msys64/ucrt64/bin").is_dir():
        ucrt = str(Path("C:/msys64/ucrt64/bin"))
        environment["PATH"] = ucrt + os.pathsep + environment.get("PATH", "")
    run_command = [
        str(executable),
        "--image",
        str(image),
        f"0x{int(manifest['executable']['base']):08x}",
        package["runtime"]["run_entry"],
        "none",
        "none",
        "--sched",
    ]
    run = subprocess.run(
        run_command,
        cwd=package_dir,
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    output = (run.stdout or "") + (run.stderr or "")
    if run.returncode:
        raise AssertionError(
            f"production runtime failed for {manifest['id']} (exit {run.returncode}):\n{output}"
        )
    for marker in (
        "BOOT_EVENT phase=init public_safe=1",
        "BOOT_EVENT phase=image_loaded",
        "BOOT_EVENT phase=runtime_registered",
        "BOOT_EVENT phase=guest_start mode=scheduler",
    ):
        if not boot_log.is_file() or marker not in boot_log.read_text(encoding="utf-8", errors="replace"):
            raise AssertionError(f"runtime omitted {marker!r} for {manifest['id']}")
    guest_start = (
        f"BOOT_EVENT phase=guest_start mode=scheduler "
        f"entry=0x{int(manifest['executable']['entry']):08x}"
    )
    if guest_start not in boot_log.read_text(encoding="utf-8", errors="replace"):
        raise AssertionError(f"runtime did not start the manifest entry for {manifest['id']}")
    for forbidden in ("UNKNOWN NID", "NONPLT_MISS", "INTERP_REJECT", "DISPATCH_FATAL"):
        if forbidden in output:
            raise AssertionError(f"production runtime reported {forbidden} for {manifest['id']}")

    result_path = memstick / "PROFILE_ZERO" / "RESULT.TXT"
    save_path = memstick / "PROFILE_ZERO" / "ROUNDTRIP.BIN"
    if not result_path.is_file() or not save_path.is_file():
        raise AssertionError(f"guest did not write its result and save files for {manifest['id']}")
    result_text = result_path.read_text(encoding="ascii")
    fields = dict(line.split("=", 1) for line in result_text.splitlines() if "=" in line)
    expected_guest_state = {
        "ENTRY": "1",
        "EXIT": "0",
        "SCHEDULER_VBLANKS": "2",
        "SCHEDULER_OK": "1",
        "INPUT_CROSS": "1",
        "INPUT_START": "1",
        "AUDIO_RESULT": "512",
        "SAVE_ROUNDTRIP": "1",
    }
    for key, expected in expected_guest_state.items():
        if fields.get(key) != expected:
            raise AssertionError(
                f"guest state {key}={fields.get(key)!r}, expected {expected!r} "
                f"for {manifest['id']}"
            )
    if save_path.read_bytes() != SAVE_BYTES:
        raise AssertionError(f"savedata bytes did not round-trip for {manifest['id']}")
    if not save_path.resolve().is_relative_to(memstick.resolve()):
        raise AssertionError(f"savedata escaped the declared writable root for {manifest['id']}")
    if int(fields.get("FRAMES", "0")) < 60:
        raise AssertionError(f"guest exited before its GE framebuffer checkpoint for {manifest['id']}")

    if not perf_csv.is_file():
        raise AssertionError(f"runtime wrote no VBLANK evidence for {manifest['id']}")
    with perf_csv.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.reader(stream))
    if len(rows) < 2 or rows[0][0] != "vblank_total" or int(rows[-1][0]) < 60:
        raise AssertionError(f"runtime VBLANK evidence is incomplete for {manifest['id']}")
    intervals = [dict(zip(rows[0], row, strict=True)) for row in rows[1:]]
    audio_calls = sum(int(row.get("audio_output_calls", "0")) for row in intervals)
    audio_frames = sum(int(row.get("audio_output_frames", "0")) for row in intervals)
    if audio_calls < 1 or audio_frames < 512:
        raise AssertionError(f"production audio helper submitted no sample for {manifest['id']}")
    if "-> 0x4000" not in output:
        raise AssertionError(f"controller replay was not sampled for {manifest['id']}")

    # The checkpoint is the first GE statistics window that rasterized the primitive, at
    # whatever vblank closed it: a loaded host pushes both later (tools/ge_stat_windows.py).
    window = ge_stat_windows.first_window(output, require=("tri2d", "px2d"))
    if window is None:
        raise AssertionError(f"GE did not rasterize the source-owned primitive for {manifest['id']}")
    ppm = package_dir / window.snapshot_name
    if not ppm.is_file():
        raise AssertionError(f"runtime framebuffer capture is missing for {manifest['id']}")
    colors, foreground = _read_ppm_metrics(ppm)
    if colors < 2 or foreground < 1000:
        raise AssertionError(
            f"runtime framebuffer is empty for {manifest['id']} "
            f"(colors={colors}, foreground={foreground})"
        )

    if "audio: initialized SDL3 audio output (driver: dummy," not in output:
        if "audio: host audio output unavailable" in output:
            raise unittest.SkipTest(
                f"SKIP: SDL3 dummy audio is unavailable for {manifest['id']}"
            )
        raise AssertionError(f"SDL3 dummy audio initialization is unproven for {manifest['id']}")
    return fields


class ProfileZeroManifestTests(unittest.TestCase):
    def test_manifest_build_contract_resolves_source(self) -> None:
        for path in MANIFEST_PATHS:
            with self.subTest(manifest=path.name):
                manifest = _manifest(path)
                profile = manifest["profile_zero"]
                build = profile["build"]
                self.assertTrue((ROOT / build["makefile"]).is_file(), build["makefile"])
                self.assertTrue((ROOT / build["working_directory"]).is_dir(),
                                build["working_directory"])
                for source in profile["source_program"]["source_files"]:
                    self.assertTrue((ROOT / source).is_file(), source)
                self.assertEqual(build["target"], "EBOOT.PBP")
                self.assertEqual(build["toolchain"], "PSPDEV/PSPSDK (locally installed)")

    def test_manifests_declare_the_same_fail_closed_case_matrix(self) -> None:
        for path in MANIFEST_PATHS:
            with self.subTest(manifest=path.name):
                manifest = _manifest(path)
                cases = {
                    case["id"]: case
                    for case in manifest["profile_zero"]["acceptance"]["cases"]
                }
                self.assertTrue(manifest["profile_zero"]["runnable"])
                self.assertEqual(manifest["profile_zero"]["acceptance"]["status"], "ready")
                self.assertEqual(set(cases), set(CASE_EVIDENCE))
                for case_id, evidence in CASE_EVIDENCE.items():
                    self.assertEqual(cases[case_id]["evidence_class"], evidence)
                    self.assertEqual(cases[case_id]["status"], "implemented")
                    self.assertEqual(cases[case_id]["gate"], "profile-zero-e2e")
                self.assertFalse(manifest["profile_zero"]["acceptance"]["private_inputs_allowed"])

    def test_make_and_hosted_platform_ladder_name_the_gate(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("profile-zero-e2e:", makefile)
        self.assertIn("profile-zero-e2e", workflow)

    def test_committed_fixture_matches_its_digests_and_manifest_entries(self) -> None:
        digests = _verify_committed_fixture()
        self.assertIn(PREBUILT_EBOOT.name, digests)
        self.assertIn(PREBUILT_GUEST.name, digests)
        for path in MANIFEST_PATHS:
            with self.subTest(manifest=path.name):
                manifest = _manifest(path)
                game_name = manifest.get("game_name") or manifest["id"].replace("-", "_")
                with tempfile.TemporaryDirectory(prefix="nk-profile-zero-prebuilt-") as temporary:
                    guest = _materialize_committed_fixture(manifest, Path(temporary))
                    self.assertEqual(guest.name, f"{game_name}.prx")
                    image = prxload.load_program_image(
                        guest,
                        base=int(manifest["executable"]["base"]),
                    )
                    self.assertIsInstance(image, prxload.ProgramImage)
                    self.assertGreater(len(image.executable_intervals), 0)
                    self.assertEqual(
                        image.entry_point,
                        int(manifest["executable"]["entry"]),
                        "manifest entry must match the committed PSPDEV guest module",
                    )

    def test_lock_evidence_and_readme_name_one_authoritative_distribution(self) -> None:
        """The lock, its evidence, and the fixture README pin the same asset (#708)."""
        authority = _pspdev_authority()
        evidence = json.loads(PSPDEV_EVIDENCE_PATH.read_text(encoding="utf-8"))
        self.assertEqual(evidence["release"], authority["release"])
        self.assertEqual(evidence["archive"]["asset_name"], authority["asset"])
        self.assertIn(authority["asset"], evidence["archive"]["download_url"])
        self.assertTrue(evidence["archive"]["verified"])
        readme = PSPDEV_README_PATH.read_text(encoding="utf-8")
        self.assertIn(authority["release"], readme)
        self.assertIn(authority["asset"], readme)

    def test_package_and_runtime_route_assert_all_profile_zero_cases(self) -> None:
        backend = _pspdev_backend()
        if backend is None:
            print(
                "profile-zero-e2e: PSPDEV/PSPSDK is absent; the route runs on the "
                "committed fixture in fixtures/profile_zero/prebuilt (no toolchain SKIP)",
                file=sys.stderr,
                flush=True,
            )
        else:
            print(
                "profile-zero-e2e: PSPDEV/PSPSDK is present; verifying it matches the "
                "distribution pinned by assets/upstream/pspdev.lock.json, then "
                "rebuilding the fixture and checking it byte-for-byte against the "
                "committed fixture",
                file=sys.stderr,
                flush=True,
            )
            _verify_backend_matches_authority(backend)
        build_environment, make = _runtime_build_environment(self)
        with tempfile.TemporaryDirectory(prefix="nk-profile-zero-e2e-") as temporary:
            root = Path(temporary)
            for manifest_path in MANIFEST_PATHS:
                with self.subTest(manifest=manifest_path.name):
                    manifest = _manifest(manifest_path)
                    if backend is not None:
                        rebuild_dir = root / manifest["id"] / "psp-rebuild"
                        rebuilt_guest = _build_guest(manifest, rebuild_dir, backend)
                        _verify_rebuild_matches_committed(manifest, rebuild_dir, rebuilt_guest)
                    guest = _materialize_committed_fixture(
                        manifest,
                        root / manifest["id"] / "psp-build",
                    )
                    image = prxload.load_program_image(
                        guest,
                        base=int(manifest["executable"]["base"]),
                    )
                    self.assertIsInstance(image, prxload.ProgramImage)
                    self.assertGreater(len(image.executable_intervals), 0)
                    self.assertEqual(
                        image.entry_point,
                        int(manifest["executable"]["entry"]),
                        "manifest entry must match the PSPDEV ELF module entry",
                    )
                    self.assertTrue(any(
                        span.start <= image.entry_point < span.end
                        for span in image.executable_intervals
                    ))
                    fields = _run_package(
                        manifest_path,
                        guest,
                        root / manifest["id"] / "package",
                        build_environment,
                        make,
                    )
                    self.assertEqual(fields["ENTRY"], "1")
                    self.assertEqual(fields["EXIT"], "0")


if __name__ == "__main__":
    unittest.main()
