#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build source-owned PSP showcase images and validated runtime packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from prxload import Prx

BUILD_ROOT = ROOT / "build" / "showcase"
DEMO_ROOT = ROOT / "build" / "demos"
SCREENSHOT_ROOT = BUILD_ROOT / "screenshots"
SECTOR = 2048
PSP_PRX_ELF_TYPE = 0xFFA0

DEMOS = (
    {
        "id": "showcase-scene-v1",
        "disc_id": "TEST00007",
        "display_name": "Nakagawa 3D Showcase",
        "folder": "ge_scene",
        "target": "showcase_scene_app",
        "manifest": "assets/titles/showcase-scene.json",
        "palette": ((23, 54, 81), (78, 204, 190)),
    },
    {
        "id": "showcase-breakout-v1",
        "disc_id": "TEST00008",
        "display_name": "Nakagawa Breakout Showcase",
        "folder": "breakout",
        "target": "showcase_breakout_app",
        "manifest": "assets/titles/showcase-breakout.json",
        "palette": ((49, 35, 78), (252, 200, 87)),
    },
)


class ShowcaseError(RuntimeError):
    """An explicit failure in the optional showcase build route."""


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def make_icon_png(palette: tuple[tuple[int, int, int], tuple[int, int, int]]) -> bytes:
    """Generate fixed-size source-owned ICON0 art using only the standard library."""
    width, height = 144, 80
    first, second = palette
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            stripe = ((x // 12) + (y // 10)) % 2
            color = second if stripe else first
            cx, cy = x - width // 2, y - height // 2
            if -22 <= cx <= 22 and -22 <= cy <= 22 and (abs(cx) < 5 or abs(cy) < 5):
                color = (245, 248, 250)
            rows.extend((*color, 255))
    ihdr = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) + \
        _png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + _png_chunk(b"IEND", b"")


def _both_endian32(value: int) -> bytes:
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _both_endian16(value: int) -> bytes:
    return value.to_bytes(2, "little") + value.to_bytes(2, "big")


def _dir_record(name: bytes, lba: int, length: int, directory: bool) -> bytes:
    record = bytearray(33 + len(name))
    record[2:10] = _both_endian32(lba)
    record[10:18] = _both_endian32(length)
    record[25] = 0x02 if directory else 0
    record[28:32] = _both_endian16(1)
    record[32] = len(name)
    record[33:] = name
    if len(record) & 1:
        record.append(0)
    record[0] = len(record)
    return bytes(record)


def make_param_sfo(disc_id: str, title: str) -> bytes:
    entries = [
        ("DISC_ID", disc_id.encode("ascii") + b"\0"),
        ("DISC_VERSION", b"1.00\0"),
        ("TITLE", title.encode("utf-8") + b"\0"),
    ]
    entries.sort(key=lambda item: item[0])
    keys = bytearray()
    values = bytearray()
    table = bytearray()
    for key, value in entries:
        key_offset = len(keys)
        keys.extend(key.encode("ascii") + b"\0")
        data_offset = len(values)
        values.extend(value)
        while len(values) & 3:
            values.append(0)
        table.extend(struct.pack("<HHIII", key_offset, 0x0204, len(value), len(value), data_offset))
    key_start = 20 + len(table)
    data_start = key_start + len(keys)
    while data_start & 3:
        keys.append(0)
        data_start += 1
    header = struct.pack("<4s4sIII", b"\0PSF", b"\1\1\0\0", key_start, data_start, len(entries))
    return header + table + keys + values


def write_iso(path: Path, disc_id: str, title: str, icon: bytes,
              eboot: bytes, license_notice: bytes) -> None:
    """Write a minimal deterministic ISO9660 tree for one source-owned PSP app."""
    if len(disc_id) != 9 or not eboot.startswith(b"\x7fELF"):
        raise ShowcaseError("showcase disc ID or plaintext EBOOT.BIN is invalid")
    blobs = {
        "PARAM.SFO;1": make_param_sfo(disc_id, title),
        "ICON0.PNG;1": icon,
        "EBOOT.BIN;1": eboot,
        "PSPSDK.LICENSE;1": license_notice,
    }
    lbas: dict[str, int] = {}
    lba = 21
    for name, content in blobs.items():
        lbas[name] = lba
        lba += (len(content) + SECTOR - 1) // SECTOR
    total_sectors = max(lba, 1024)
    image = bytearray(total_sectors * SECTOR)

    pvd = memoryview(image)[16 * SECTOR:17 * SECTOR]
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"NAKAGAWA SHOWCASE".ljust(32, b" ")
    pvd[40:72] = b"NKSHOWCASE".ljust(32, b" ")
    pvd[80:88] = _both_endian32(total_sectors)
    pvd[120:124] = _both_endian16(1)
    pvd[124:128] = _both_endian16(1)
    pvd[128:132] = _both_endian16(SECTOR)
    pvd[813:830] = b"2026092400000000\0"
    pvd[881] = 1
    pvd[156:190] = _dir_record(b"\0", 18, SECTOR, True)
    image[17 * SECTOR] = 0xFF
    image[17 * SECTOR + 1:17 * SECTOR + 7] = b"CD001\x01"

    root_entries = (
        _dir_record(b"\0", 18, SECTOR, True)
        + _dir_record(b"\1", 18, SECTOR, True)
        + _dir_record(b"PSP_GAME", 19, SECTOR, True)
    )
    game_entries = (
        _dir_record(b"\0", 19, SECTOR, True)
        + _dir_record(b"\1", 18, SECTOR, True)
        + _dir_record(b"PARAM.SFO;1", lbas["PARAM.SFO;1"], len(blobs["PARAM.SFO;1"]), False)
        + _dir_record(b"ICON0.PNG;1", lbas["ICON0.PNG;1"], len(icon), False)
        + _dir_record(b"SYSDIR", 20, SECTOR, True)
        + _dir_record(b"PSPSDK.LICENSE;1", lbas["PSPSDK.LICENSE;1"], len(license_notice), False)
    )
    sysdir_entries = (
        _dir_record(b"\0", 20, SECTOR, True)
        + _dir_record(b"\1", 19, SECTOR, True)
        + _dir_record(b"EBOOT.BIN;1", lbas["EBOOT.BIN;1"], len(eboot), False)
    )
    for target, content in ((18, root_entries), (19, game_entries), (20, sysdir_entries)):
        if len(content) > SECTOR:
            raise ShowcaseError("showcase ISO directory exceeds one sector")
        start = target * SECTOR
        image[start:start + len(content)] = content
    for name, content in blobs.items():
        start = lbas[name] * SECTOR
        image[start:start + len(content)] = content
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def _wsl_path(path: Path) -> str:
    result = subprocess.run(["wsl", "-e", "wslpath", "-u", str(path)],
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise ShowcaseError("WSL path conversion failed: " + result.stderr.strip())
    return result.stdout.strip()


def _check_pspdev() -> None:
    command = "test -x /usr/local/pspdev/bin/psp-gcc && test -d /usr/local/pspdev/psp/sdk"
    result = subprocess.run(["wsl", "-e", "bash", "-lc", command],
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise ShowcaseError(
            "PSPDEV/PSPSDK was not found in WSL at /usr/local/pspdev. "
            "Install PSPDEV locally, then rerun `mingw32-make showcase`."
        )


def _build_prx(demo: dict[str, object], out_dir: Path) -> Path:
    source_dir = ROOT / "fixtures" / "showcase" / str(demo["folder"])
    out_dir.mkdir(parents=True, exist_ok=True)
    makefile = _wsl_path(ROOT / "fixtures" / "showcase" / "Makefile")
    wsl_out = _wsl_path(out_dir)
    wsl_source = _wsl_path(source_dir)
    command = (
        "export PSPDEV=/usr/local/pspdev; export PATH=\"$PSPDEV/bin:$PATH\"; "
        f"make -B -f '{makefile}' -C '{wsl_out}' VPATH='{wsl_source}' "
        f"TARGET='{demo['target']}'"
    )
    result = subprocess.run(["wsl", "-e", "bash", "-lc", command],
                            cwd=ROOT, text=True, check=False)
    if result.returncode:
        raise ShowcaseError(f"PSPDEV build failed for {demo['id']} (exit {result.returncode})")
    prx = out_dir / f"{demo['target']}.prx"
    if not prx.is_file():
        raise ShowcaseError(f"PSPDEV did not produce the expected PRX: {prx}")
    return prx


def validate_psp_prx(path: Path, base: int) -> int:
    """Require the rebased, relocation-bearing PRX emitted by psp-prxgen."""
    header = path.read_bytes()[:52]
    if len(header) < 52 or header[:4] != b"\x7fELF" or \
            struct.unpack_from("<H", header, 16)[0] != PSP_PRX_ELF_TYPE:
        raise ShowcaseError(
            f"PSPDEV output is not a final relocatable PSP PRX: {path}"
        )
    try:
        image = Prx(str(path), base)
        relocations = image.relocate()
    except (OSError, ValueError) as exc:
        raise ShowcaseError(f"PSPDEV PRX cannot be loaded by Nakagawa: {exc}") from exc
    if relocations == 0:
        raise ShowcaseError(
            f"PSPDEV PRX has no MIPS relocations; use psp-prxgen output: {path}"
        )
    return base + image.entry


def _clear_owned(path: Path, parent: Path) -> None:
    resolved_parent = parent.resolve()
    resolved_path = path.resolve()
    if resolved_path == resolved_parent or resolved_parent not in resolved_path.parents:
        raise ShowcaseError(f"refusing to replace output outside build tree: {path}")
    if path.exists():
        if path.is_symlink():
            raise ShowcaseError(f"refusing to replace symlink output: {path}")
        shutil.rmtree(path)


def _pspsdk_license() -> bytes:
    command = (
        "export PSPDEV=/usr/local/pspdev; "
        "cat \"$PSPDEV/psp/share/licenses/pspsdk/LICENSE\""
    )
    result = subprocess.run(["wsl", "-e", "bash", "-lc", command],
                            capture_output=True, check=False)
    if result.returncode or not result.stdout:
        raise ShowcaseError("Could not read the installed PSPSDK license notice.")
    return result.stdout


def _package_fingerprint(demo: dict[str, object], prx: Path) -> str:
    digest = hashlib.sha256()
    inputs = [
        ROOT / "Makefile", ROOT / "copy_build_assets.ps1",
        ROOT / "tools" / "analyze.py", ROOT / "tools" / "codegen.py",
        ROOT / "tools" / "imports.py", ROOT / "tools" / "title_codegen_plan.py",
        ROOT / "tools" / "title_runtime_config.py", ROOT / "tools" / "build_profile.py",
        ROOT / "tools" / "shader_embed.py", ROOT / str(demo["manifest"]), prx,
    ]
    for tree in (ROOT / "src" / "rt", ROOT / "src" / "core", ROOT / "mk"):
        inputs.extend(path for path in tree.rglob("*") if path.is_file())
    for path in sorted(set(inputs), key=lambda item: item.as_posix()):
        try:
            name = path.resolve().relative_to(ROOT.resolve()).as_posix()
        except ValueError:
            name = path.name
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def _build_one(demo: dict[str, object], license_notice: bytes) -> None:
    title_id = str(demo["id"])
    stage = BUILD_ROOT / title_id
    prx = _build_prx(demo, stage / "psp-build")
    manifest_path = ROOT / str(demo["manifest"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    loaded_entry = validate_psp_prx(prx, int(manifest["executable"]["base"]))
    if loaded_entry != int(manifest["executable"]["entry"]):
        raise ShowcaseError(
            f"{title_id} manifest entry 0x{manifest['executable']['entry']:08x} "
            f"does not match the PSPDEV PRX entry 0x{loaded_entry:08x}"
        )
    icon = make_icon_png(demo["palette"])
    (stage / "ICON0.PNG").write_bytes(icon)
    iso_path = stage / f"{demo['disc_id']}.iso"
    write_iso(iso_path, str(demo["disc_id"]), str(demo["display_name"]), icon,
              prx.read_bytes(), license_notice)

    package_stage = stage / f"package-{_package_fingerprint(demo, prx)}"
    if package_stage.is_symlink():
        raise ShowcaseError(f"refusing to use symlink package output: {package_stage}")
    package_stage.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"),
        str(ROOT / str(demo["manifest"])), "--package", "--public-safe",
        "--game-elf", str(prx), "--output-dir", str(package_stage),
    ]
    result = subprocess.run(command, cwd=ROOT, check=False)
    if result.returncode:
        raise ShowcaseError(f"analyze/codegen/package failed for {title_id} (exit {result.returncode})")
    package_meta = json.loads((package_stage / "package.json").read_text(encoding="utf-8"))
    if package_meta.get("format") != "nakagawa-aot-package":
        raise ShowcaseError(f"{title_id} did not produce nakagawa-aot-package v1")
    exe = package_stage / package_meta["executable"]["path"]
    if not exe.is_file():
        raise ShowcaseError(f"validated package executable is missing for {title_id}")

    release_dir = DEMO_ROOT / "packages" / str(demo["disc_id"])
    _clear_owned(release_dir, DEMO_ROOT / "packages")
    release_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(package_stage, release_dir)
    image = DEMO_ROOT / "images" / f"{demo['disc_id']}.iso"
    image.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(iso_path, image)
    license_dir = DEMO_ROOT / "THIRD_PARTY_NOTICES"
    license_dir.mkdir(parents=True, exist_ok=True)
    (license_dir / "PSPSDK-LICENSE.txt").write_bytes(license_notice)
    (DEMO_ROOT / "data" / title_id).mkdir(parents=True, exist_ok=True)
    print(f"SHOWCASE_PACKAGE: {demo['disc_id']} {release_dir}")


def build_all() -> None:
    _check_pspdev()
    license_notice = _pspsdk_license()
    for demo in DEMOS:
        _build_one(demo, license_notice)


def _runtime_command(demo: dict[str, object], padscript: Path) -> tuple[list[str], dict[str, str], Path]:
    disc_id = str(demo["disc_id"])
    package_dir = DEMO_ROOT / "packages" / disc_id
    meta = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / str(demo["manifest"])).read_text(encoding="utf-8"))
    image = package_dir / f"{str(demo['id'])}_image.bin"
    exe = package_dir / meta["executable"]["path"]
    base = int(manifest["executable"]["base"])
    command = [str(exe), "--image", str(image), f"0x{base:08x}",
               meta["runtime"]["run_entry"], "none", "none", "--sched"]
    env = os.environ.copy()
    env.update({
        "SR_AUDIOSTAT": "1",
        "SR_AUDIO_BACKEND": "sdl",
        "SR_GESTAT": "1",
        "SR_FBSNAP": "1",
        "SR_INLOG": "1",
        "SR_NOINPUT": "1",
        "SR_EXIT_AT_VBLANK": "180",
        "SR_PADSCRIPT": str(padscript),
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "SR_MEMSTICK": str(BUILD_ROOT / "smoke" / disc_id / "memstick"),
    })
    return command, env, package_dir


def _ppm_metrics(path: Path, background: bytes) -> tuple[int, int]:
    parts = path.read_bytes().split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or len(background) != 3:
        raise ShowcaseError(f"invalid P6 framebuffer capture: {path}")
    width, height = (int(value) for value in parts[1].split())
    pixels = parts[3]
    if len(pixels) != width * height * 3:
        raise ShowcaseError(f"truncated P6 framebuffer capture: {path}")
    colors: set[bytes] = set()
    foreground = 0
    for offset in range(0, len(pixels), 3):
        pixel = pixels[offset:offset + 3]
        colors.add(pixel)
        foreground += pixel != background
    return len(colors), foreground


def smoke_all() -> None:
    SCREENSHOT_ROOT.mkdir(parents=True, exist_ok=True)
    padscript_root = BUILD_ROOT / "smoke"
    padscript_root.mkdir(parents=True, exist_ok=True)
    ppm_converter = ROOT / "tools" / "ppm2png.py"
    for demo in DEMOS:
        disc_id = str(demo["disc_id"])
        smoke_dir = padscript_root / disc_id
        smoke_dir.mkdir(parents=True, exist_ok=True)
        (smoke_dir / "padscript.txt").write_text("12 4000 4\n240 0008 4\n", encoding="ascii")
        command, env, package_dir = _runtime_command(demo, smoke_dir / "padscript.txt")
        try:
            result = subprocess.run(command, cwd=package_dir, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=15, check=False)
            output = result.stdout
        except subprocess.TimeoutExpired as exc:
            output = (exc.stdout or "") + "\nSHOWCASE_SMOKE_TIMEOUT\n"
            raise ShowcaseError(f"{disc_id} exceeded the 15 second scripted smoke window") from exc
        log_path = smoke_dir / "runtime.log"
        log_path.write_text(output, encoding="utf-8", errors="replace")
        if result.returncode != 0:
            raise ShowcaseError(f"{disc_id} runtime exited {result.returncode}; see {log_path}")
        if "GESTAT f=60 " not in output:
            raise ShowcaseError(f"{disc_id} did not reach its first frame checkpoint; see {log_path}")
        if "-> 0x4000" not in output:
            raise ShowcaseError(f"{disc_id} missed the scripted Cross input sample; see {log_path}")
        if "AUDIOSTAT_HOST:" not in output or "pushed=0" in output:
            raise ShowcaseError(f"{disc_id} missed audio-submission telemetry; see {log_path}")
        if disc_id == "TEST00007":
            scene_frame = re.search(r"GESTAT f=60 .*?tri3d=(\d+).*?px3d=(\d+)", output)
            if not scene_frame or not int(scene_frame.group(1)) or not int(scene_frame.group(2)):
                raise ShowcaseError(f"{disc_id} produced no visible 3D geometry at frame 60; see {log_path}")
        ppm = package_dir / "snap_f00060.ppm"
        if not ppm.is_file():
            snapshots = sorted(package_dir.glob("snap_f*.ppm"))
            if not snapshots:
                raise ShowcaseError(f"{disc_id} produced no GE framebuffer capture; see {log_path}")
            ppm = snapshots[0]
        color_count, foreground_pixels = _ppm_metrics(ppm, bytes((16, 24, 32)))
        # A flat-colour 2D frame legitimately has only a handful of colours; this
        # catches an empty or single-colour frame, and the screenshots are reviewed.
        if color_count < 4 or foreground_pixels < 1000:
            raise ShowcaseError(
                f"{disc_id} framebuffer is visually empty ({color_count} colors, "
                f"{foreground_pixels} non-background pixels); see {ppm}"
            )
        screenshot = SCREENSHOT_ROOT / f"{disc_id}.png"
        converted = subprocess.run([sys.executable, str(ppm_converter), str(ppm), str(screenshot)],
                                   cwd=ROOT, text=True, capture_output=True, check=False)
        if converted.returncode:
            raise ShowcaseError(f"could not convert {ppm} to PNG: {converted.stderr.strip()}")
        if disc_id == "TEST00008":
            save_padscript = smoke_dir / "padscript-save.txt"
            save_padscript.write_text("12 4000 4\n120 0008 4\n", encoding="ascii")
            save_command, save_env, save_package_dir = _runtime_command(demo, save_padscript)
            save_env.pop("SR_EXIT_AT_VBLANK", None)
            try:
                save_result = subprocess.run(save_command, cwd=save_package_dir, env=save_env,
                                             text=True, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, timeout=15, check=False)
            except subprocess.TimeoutExpired as exc:
                raise ShowcaseError(f"{disc_id} savedata smoke exceeded the 15 second window") from exc
            save_log_path = smoke_dir / "runtime-save.log"
            save_log_path.write_text(save_result.stdout, encoding="utf-8", errors="replace")
            if save_result.returncode != 0:
                raise ShowcaseError(f"{disc_id} savedata smoke exited {save_result.returncode}; see {save_log_path}")
            save_root = Path(env["SR_MEMSTICK"])
            if not save_root.exists() or not any(path.is_file() for path in save_root.rglob("*")):
                raise ShowcaseError(f"Breakout did not create savedata under {save_root}")
        print(f"SHOWCASE_SMOKE: PASS {disc_id} frame/input/audio; screenshot={screenshot}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "smoke"))
    args = parser.parse_args()
    try:
        if args.action == "build":
            build_all()
        else:
            smoke_all()
    except (ShowcaseError, OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        print(f"SHOWCASE_ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
