# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Determinism and public-export exclusion of the player package route (#508).

``tools/test_player_package_route.py`` proves the consumer's route *happens*: one
source-owned disc, one BUILD PACKAGE, one PLAY, guest-visible evidence at the
end. This module proves the properties that make that route a product rather than
a demo, plus the manifest variant issue #508 asks for.

**Determinism.** The same source-owned fixture is built twice from byte-identical
inputs into two independent per-user output roots, through the same real route
(the player's own build action -> ``tools/nk_cli.py build-package`` -> real
analysis, real AOT codegen, a real ``gcc``/``mingw32-make`` link). The promoted
package is then compared file by file. Every project-authored generated source is
required to be byte-identical with no normalization at all. The remaining files
are compared after the documented rule set in :class:`PackageNormalizer`, and a
difference that no rule explains fails the test, so the rule set cannot quietly
grow into "everything matches".

The rules exist because three things legitimately vary with *where* and *when* a
build ran, and the toolchain copies them into its own output:

``workspace-path``
    The absolute per-user output root, in its long spelling and in the Windows 8.3
    short spelling Make and ``gcc`` echo into dependency files, the link map and
    ``-DSR_BUILD_DIR``.
``scratch-name``
    The ``tempfile.mkdtemp`` scratch workspace ``tools/title_codegen_plan.py``
    creates when the output path is not Make-safe. Its random suffix is part of
    those same path echoes, so it is normalized as a path component, never alone.
``build-profile-hash``
    The content hash ``tools/build_profile.py`` derives from ``CFLAGS`` -- which
    contains that path -- and the ``.runtime-profile-<hash>`` stamp named from it.
``compile-clock``
    The ``__DATE__``/``__TIME__`` expansion ``src/rt/flight_recorder.c`` compiles
    into the evidence bundle's ``build`` block, in exactly the two shapes
    ``assets/flight_recorder_schema.json`` documents.
``input-stamp-mtime``
    The guest input's modification time in the ``.game-inputs`` freshness stamp.
    That mtime *is* the freshness mechanism, so it is normalized, not removed.
``pe-image-checksum``
    The PE32+ optional-header image checksum, which is a checksum over the image
    and therefore necessarily tracks every other difference above.
``transitive-digest``
    A ``sha256`` the package's own JSON records for a file that differs only by
    the rules above. A recorded digest is a consequence, not a new difference.

**Public-export exclusion.** The route writes nothing the public export or
``git status`` could pick up, and the public scope refuses the output root even
when someone points it there by mistake.

**Manifest variant.** The build half is re-run with a title manifest whose guest
addresses come from the fixture recipe's generated ELF metadata instead of a
hand-written literal, and the same launch is proved. That is *not* the canonical
analyzer-to-title-manifest path: no tool in this tree generates a title manifest
from an ELF, so this module composes the two source-owned artifacts that do exist
and does not pretend to be that tool.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
from ctypes import wintypes
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import publication_policy  # noqa: E402
from test_player_package_route import (  # noqa: E402
    DISC_ID,
    FRAMES,
    LAUNCH_TIMEOUT_MS,
    TITLE_ID,
    align_executable,
    create_test_iso_with_executables,
    synthetic_manifest,
)

# The artifact stem the source-owned display smoke guest is built under.
GAME = "display_smoke"
EXECUTABLE = f"{GAME}{'.exe' if os.name == 'nt' else ''}"
# The recipe that assembles the guest and, beside it, the only analyzer output in
# this tree that describes a source-owned ELF.
FIXTURE_GENERATOR = ROOT / "fixtures" / "display_smoke" / "generate.py"
# The committed, publication-admitted source-owned title manifest for that guest.
SOURCE_OWNED_TITLE = ROOT / "assets" / "titles" / "display-smoke.json"

# The documented rules. Order matters only in that a longer path spelling must be
# consumed before a prefix of it (see _root_spellings).
RULE_NAMES = (
    "workspace-path",
    "scratch-name",
    "build-profile-hash",
    "compile-clock",
    "input-stamp-mtime",
    "pe-image-checksum",
    "transitive-digest",
)

_STAMP_RE = re.compile(rb"\.(runtime-profile|codegen-profile|recomp-profile"
                       rb"|title-config)-[0-9a-f]{4,}")
_STAMP_NAME_RE = re.compile(r"\.(runtime-profile|codegen-profile|recomp-profile"
                            r"|title-config)-[0-9a-f]{4,}")
_PROFILE_HASH_RE = re.compile(rb'"profile_hash": "[0-9a-f]{16,}"')
# A mkdtemp() name is "<prefix><8 chars from [a-z0-9_]>" and always reaches an
# output as a whole path component, so requiring a component terminator keeps this
# from matching an unrelated identifier.
_SCRATCH_RE = re.compile(rb"\.build-[A-Za-z0-9._-]+-[A-Za-z0-9_]{8}(?=[/\\\x00])")
_DATE_RE = re.compile(rb"[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4}")
_TIME_RE = re.compile(rb"[0-9]{2}:[0-9]{2}:[0-9]{2}")
_MTIME_RE = re.compile(rb"(?<= )[0-9]{16,20}(?= )")

# PE signature + COFF header, then the CheckSum field of the PE32+ optional header.
_PE_CHECKSUM_OFFSET = 24 + 64

# Artifacts this project authors for a title. None is written by the compiler or
# the Make recipes, so none may vary between two builds of the same inputs.
PROJECT_AUTHORED = (
    f"{GAME}_recomp.c",
    f"{GAME}_recomp_funcs.h",
    f"{GAME}_recomp_stubs.txt",
    f"{GAME}_image.bin",
    f"{GAME}_imports.toml",
    "sr_title_config.h",
    "codegen_profile.json",
    "recomp_profile.json",
    "build-report.json",
    "RELINK.md",
    "SOURCE.txt",
    "THIRD_PARTY_NOTICES.txt",
)
# Prefix of the staged assets and third-party notices, which the project copies.
PROJECT_AUTHORED_PREFIXES = ("assets/", "THIRD_PARTY_NOTICES/")
# Suffixes of files whose content is derived only from the guest image.
GUEST_DERIVED_SUFFIXES = (".c", ".h", ".bin", ".toml")
# The package files that record the manifest's own identity. Two routes that bind
# the same guest to two different manifests must differ in exactly these.
MANIFEST_IDENTIFIED = ("build-report.json", "package.json", "completion-manifest.json")
# The package files whose content names the title or the artifact stem. These are
# left out of the byte comparison, because a route may legitimately name a
# different title; the generated-manifest test then states which of them must still
# agree, since the stem is a property of the guest.
TITLE_IDENTIFIED = MANIFEST_IDENTIFIED + ("THIRD_PARTY_NOTICES.txt", "SOURCE.txt",
                                          "codegen_profile.json", "recomp_profile.json")


def names_the_title(relative: str) -> bool:
    """True for a package file whose content legitimately names the title."""
    return (relative in TITLE_IDENTIFIED
            or relative.startswith("THIRD_PARTY_NOTICES/"))


def is_project_authored(relative: str) -> bool:
    """True for a package file this project, not the toolchain, produces."""
    return relative in PROJECT_AUTHORED or relative.startswith(PROJECT_AUTHORED_PREFIXES)


def read_tree(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def digest_map(root: Path) -> dict[str, str]:
    return {
        relative: hashlib.sha256(data).hexdigest()
        for relative, data in read_tree(root).items()
    }


def describe(left: bytes, right: bytes) -> str:
    """A short, byte-accurate report of a difference no rule explained."""
    shared = min(len(left), len(right))
    offsets = [i for i in range(shared) if left[i] != right[i]][:4]
    if len(left) != len(right):
        return f"lengths {len(left)} != {len(right)}; first differing offsets {offsets}"
    return "\n".join(
        f"  @{offset} {left[offset]:#04x}!={right[offset]:#04x} "
        f"A={left[max(0, offset - 48):offset + 48]!r} "
        f"B={right[max(0, offset - 48):offset + 48]!r}"
        for offset in offsets
    ) or "no differing byte, so the raw files were already equal"


def _short_path(path: str) -> str:
    """The Windows 8.3 short spelling of an absolute path, or the path itself.

    Make and ``gcc`` echo whichever spelling the shell handed them, so a rule that
    normalized only the long form would leave a real difference unexplained.
    """
    if os.name != "nt":
        return path
    try:
        get_short = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
    except (AttributeError, OSError):
        return path
    get_short.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    get_short.restype = wintypes.DWORD
    buffer = ctypes.create_unicode_buffer(4096)
    return buffer.value if get_short(path, buffer, 4096) else path


def _root_spellings(root: Path) -> list[bytes]:
    """Every byte spelling of one output root the toolchain could echo."""
    spellings = set()
    for base in {str(root.resolve()), str(root)}:
        for variant in (base, _short_path(base)):
            spellings.add(variant.replace("\\", "/").encode("utf-8"))
            spellings.add(variant.encode("utf-8"))
    # Longest first, so a longer spelling is consumed before a prefix of it.
    return sorted(spellings, key=len, reverse=True)


def _zero_pe_checksum(data: bytes) -> tuple[bytes, bool]:
    signature = data.find(b"PE\x00\x00")
    if signature < 0 or signature + _PE_CHECKSUM_OFFSET + 4 > len(data):
        return data, False
    field = signature + _PE_CHECKSUM_OFFSET
    if data[field:field + 4] == b"\x00\x00\x00\x00":
        return data, False
    return data[:field] + b"\x00\x00\x00\x00" + data[field + 4:], True


class PackageNormalizer:
    """Map one promoted package onto the bytes a build must actually produce.

    Every rule reports whether it fired, so a test can assert both that a
    difference was explained and which rule explained it. The rules are narrow
    byte patterns rather than a scrub, so one cannot quietly widen into
    "everything matches".
    """

    def __init__(self, root: Path, explained_digests: frozenset[str] = frozenset()):
        self.spellings = _root_spellings(root)
        self.digests = [digest.encode("ascii") for digest in sorted(explained_digests)]

    def name(self, relative: str) -> str:
        """A stamp file's name carries the CFLAGS-derived hash, so normalize it."""
        return _STAMP_NAME_RE.sub(r".\1-<HASH>", relative)

    def apply(self, relative: str, data: bytes) -> tuple[bytes, tuple[str, ...]]:
        fired = set()
        for spelling in self.spellings:
            if spelling in data:
                fired.add("workspace-path")
                data = data.replace(spelling, b"<ROOT>")
        for pattern, label, replacement in (
            (_SCRATCH_RE, "scratch-name", rb".build-<SCRATCH>"),
            (_STAMP_RE, "build-profile-hash", rb".\1-<HASH>"),
            (_PROFILE_HASH_RE, "build-profile-hash", b'"profile_hash": "<HASH>"'),
            (_DATE_RE, "compile-clock", b"<DATE>"),
            (_TIME_RE, "compile-clock", b"<TIME>"),
        ):
            rewritten = pattern.sub(replacement, data)
            if rewritten != data:
                fired.add(label)
                data = rewritten
        if relative == ".game-inputs":
            rewritten = _MTIME_RE.sub(b"<MTIME_NS>", data)
            if rewritten != data:
                fired.add("input-stamp-mtime")
            data = rewritten
        data, changed = _zero_pe_checksum(data)
        if changed:
            fired.add("pe-image-checksum")
        for digest in self.digests:
            if digest in data:
                fired.add("transitive-digest")
                data = data.replace(digest, b"<DIGEST>")
        return data, tuple(sorted(fired))


class PackagePair:
    """Two promoted packages, compared under the documented rules.

    ``identity`` is the file set both packages share under normalized names.
    ``authored`` is the subset the project writes rather than the toolchain, which
    must be byte-identical with no rule applied. ``unattributed`` is every
    difference no rule explains; it must be empty. ``rules`` is the set of rules
    that fired, so a rule that has stopped describing anything is visible.
    """

    def __init__(self, left_root: Path, right_root: Path,
                 spelling_roots: tuple[Path, Path]):
        self.left_root, self.right_root = left_root, right_root
        self.left, self.right = read_tree(left_root), read_tree(right_root)
        # A digest the package records for a differing file is a consequence of an
        # already-attributed difference, never an independent one.
        digests = set()
        for tree, other in ((self.left, self.right), (self.right, self.left)):
            digests.update(hashlib.sha256(data).hexdigest()
                           for name, data in tree.items() if other.get(name) != data)
        # The rule normalizes the consumer's per-user data root, not the promoted
        # package directory: the toolchain echoes the root, because that is where
        # the package cache and the Make-safe scratch workspace live.
        self.left_rules = PackageNormalizer(spelling_roots[0], frozenset(digests))
        self.right_rules = PackageNormalizer(spelling_roots[1], frozenset(digests))
        self.left_names = {self.left_rules.name(name) for name in self.left}
        self.right_names = {self.right_rules.name(name) for name in self.right}
        self.identity = self.left_names & self.right_names
        # Byte lookup by normalized name, so a caller never has to know a stamp
        # file's raw name to compare the file.
        self.left_by_name = {self.left_rules.name(name): data
                             for name, data in self.left.items()}
        self.right_by_name = {self.right_rules.name(name): data
                              for name, data in self.right.items()}
        self.authored = {name for name in self.identity
                         if is_project_authored(name)
                         or name.endswith(GUEST_DERIVED_SUFFIXES)}
        self.identical: set[str] = set()
        self.differing: set[str] = set()
        self.unattributed: list[str] = []
        self.rules: set[str] = set()
        self.file_rules: dict[str, set[str]] = {}
        for raw_left in sorted(self.left):
            if raw_left not in self.right:
                continue
            data_left, data_right = self.left[raw_left], self.right[raw_left]
            name = self.left_rules.name(raw_left)
            if name != self.right_rules.name(raw_left):
                self.unattributed.append(
                    f"{raw_left}: the two builds name it differently after normalizing "
                    f"the build-profile hash, so the pair is not comparable")
                continue
            if data_left == data_right:
                self.identical.add(name)
                continue
            self.differing.add(name)
            left_bytes, left_fired = self.left_rules.apply(name, data_left)
            right_bytes, right_fired = self.right_rules.apply(name, data_right)
            self.rules.update(left_fired)
            self.rules.update(right_fired)
            self.file_rules[name] = set(left_fired) | set(right_fired)
            if left_bytes != right_bytes:
                self.unattributed.append(f"{name}\n{describe(left_bytes, right_bytes)}")

    def rules_for(self, name: str) -> set[str]:
        """The documented rules that had to fire for one file to match."""
        return self.file_rules.get(name, set())


class TestPlayerPackageDeterminism(unittest.TestCase):
    """Three builds, shared by every assertion in the class."""

    # -- repository and toolchain ---------------------------------------------

    @staticmethod
    def repo_status() -> str:
        """Repository status including untracked files: nothing may be added."""
        completed = subprocess.run(
            ["git", "-C", str(ROOT), "status", "--porcelain"],
            capture_output=True, text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"git status failed: {completed.stderr.strip()}")
        return completed.stdout

    @classmethod
    def probe_toolchain(cls) -> None:
        """Skip with the Makefile's own remedy when the route cannot run here."""
        missing = [name for name in ("mingw32-make", "gcc", "pwsh", "git")
                   if not shutil.which(name)]
        if missing:
            raise unittest.SkipTest(
                "the player package route requires " + ", ".join(missing) + " on PATH")
        make_name = "mingw32-make" if os.name == "nt" else "make"
        probe = subprocess.run([make_name, "--no-print-directory", "sdl3-check"],
                               cwd=ROOT, capture_output=True, text=True)
        output = probe.stdout + probe.stderr
        if probe.returncode != 0 and "sdl3 dependency is missing" in output.lower():
            reason = next(line for line in output.splitlines()
                          if "sdl3 dependency is missing" in line.lower())
            raise unittest.SkipTest(reason.strip())
        suffix = ".exe" if os.name == "nt" else ""
        required = [ROOT / "build" / name for name in
                    (f"test_package_builder{suffix}", f"test_player_state{suffix}",
                     f"nakagawa_player{suffix}")]
        cls.harness, cls.validator, cls.player = required
        if all(binary.is_file() for binary in required):
            return
        built = subprocess.run(
            [make_name, "--no-print-directory", "player", "player-state-test-bin",
             "package-builder-test-bin"],
            cwd=ROOT, capture_output=True, text=True,
        )
        if all(binary.is_file() for binary in required):
            return
        lines = (built.stdout + built.stderr).splitlines()
        reason = next((line for line in lines
                       if "sdl3 dependency is missing" in line.lower()),
                      "building the route's binaries failed: "
                      + " ".join(lines).strip()[-400:])
        raise unittest.SkipTest(reason.strip())

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="nk-player-determinism-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.workspace = Path(cls.temporary.name)
        cls.before = cls.repo_status()
        cls.probe_toolchain()
        # Roots A and B receive byte-identical inputs; root C receives the same
        # guest bound to the analyzer-generated manifest variant.
        cls.roots = {
            name: cls.build_root(name, kind)
            for name, kind in (("a", "handwritten"), ("b", "handwritten"),
                               ("c", "generated"))
        }

    @classmethod
    def tearDownClass(cls) -> None:
        if getattr(cls, "before", None) is None:
            return
        current = cls.repo_status()
        if current != cls.before:
            raise AssertionError("the package route changed the repository's own status, "
                                 f"including untracked files:\n{current}")

    @property
    def packages(self) -> dict[str, Path]:
        return {name: root / "packages" / DISC_ID for name, root in self.roots.items()}

    def package_pair(self, left: str, right: str) -> PackagePair:
        return PackagePair(self.packages[left], self.packages[right],
                           (self.roots[left], self.roots[right]))

    # -- the build half -------------------------------------------------------

    @classmethod
    def build_root(cls, name: str, manifest_kind: str) -> Path:
        """One independent per-user output root, built through the real route."""
        root = cls.workspace / name
        user_root = root / "user"
        for directory in (user_root, root / "localappdata", root / "sandbox"):
            directory.mkdir(parents=True)

        fixture = root / "fixture"
        generated = subprocess.run(
            [sys.executable, str(FIXTURE_GENERATOR), "generate",
             "--out-dir", str(fixture), "--frames", str(FRAMES)],
            cwd=ROOT, capture_output=True, text=True,
        )
        if generated.returncode != 0:
            raise AssertionError(generated.stdout + generated.stderr)
        # The generated bytes use load-relative zero addresses; only the program
        # headers change on the way into the disc image.
        executable = align_executable((fixture / "guest.prx").read_bytes())
        iso_path = root / "synthetic.iso"
        create_test_iso_with_executables(iso_path, executable, disc_id=DISC_ID)

        manifest = (synthetic_manifest() if manifest_kind == "handwritten"
                    else cls.generated_manifest(fixture, executable))
        cls.stage_library(user_root, manifest, iso_path, executable)

        built = subprocess.run(
            [str(cls.harness), "--build-package", str(ROOT), str(user_root), DISC_ID,
             str(user_root / "logs")],
            cwd=ROOT, capture_output=True, text=True, timeout=1800,
        )
        if built.returncode != 0 or "PACKAGE_BUILD_ROUTE status=PASS" not in built.stdout:
            raise AssertionError(
                f"BUILD PACKAGE failed in root {name}:\n"
                + built.stdout[-4000:] + built.stderr[-4000:])
        return user_root

    @staticmethod
    def stage_library(user_root: Path, manifest: dict, iso_path: Path,
                      executable: bytes) -> None:
        digest = hashlib.sha256(executable).hexdigest()
        profile_dir = user_root / "experimental" / DISC_ID
        profile_dir.mkdir(parents=True, exist_ok=True)
        (profile_dir / "profile.json").write_text(json.dumps({
            "schema_version": 1,
            "manifest": manifest,
            "input_identity": {
                "disc_id": DISC_ID,
                "selected_executable": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "executable_sha256": digest,
                "elf_sha256": digest,
            },
        }, sort_keys=True) + "\n", encoding="utf-8")
        (user_root / "library.json").write_text(json.dumps({
            "schema_version": 1,
            "games": [{
                "disc_id": DISC_ID,
                "title_id": manifest["id"],
                "iso_path": str(iso_path),
                "selected_executable": "EBOOT.BIN",
                "is_experimental": True,
            }],
        }), encoding="utf-8")

    @staticmethod
    def generated_manifest(fixture: Path, executable: bytes) -> dict:
        """A title manifest whose guest addresses come from the generated ELF.

        The executable block is read out of the fixture recipe's own generated
        metadata and then cross-checked against the committed, publication-admitted
        source-owned title manifest, so a drift between the assembled guest and the
        manifest this repository publishes fails here instead of reaching a build.
        Everything an ELF cannot state -- filesystem layout, HLE and codegen
        profiles -- is the committed source-owned manifest's own statement.
        """
        generated = json.loads((fixture / "manifest.json").read_text(encoding="ascii"))
        published = json.loads(SOURCE_OWNED_TITLE.read_text(encoding="utf-8"))
        guest = (fixture / "guest.prx").read_bytes()
        if generated["prx_sha256"] != hashlib.sha256(guest).hexdigest():
            raise AssertionError("the generated fixture manifest does not describe the "
                                 "guest ELF it shipped beside")
        if generated["title_id"] != published["id"]:
            raise AssertionError("the fixture recipe and the committed title manifest "
                                 f"disagree on the title identity: "
                                 f"{generated['title_id']} != {published['id']}")
        derived = dict(published["executable"],
                       base=int(generated["base"], 16),
                       entry=int(generated["entry"], 16))
        if derived != published["executable"]:
            raise AssertionError(
                "the guest the fixture recipe assembles does not match the addresses the "
                f"committed source-owned title manifest publishes: {derived} != "
                f"{published['executable']}")
        if int(generated["bss_size"]) <= 0:
            raise AssertionError("the generated recipe reports no BSS for the guest")
        if executable[:4] != b"\x7fELF":
            raise AssertionError("the guest routed into the disc is not an ELF")
        manifest = dict(published)
        manifest["id"] = TITLE_ID
        manifest["display_name"] = "Analyzer-generated player package route"
        manifest["kind"] = "retail"
        manifest["disc"] = {"id": DISC_ID, "region": "NA",
                            "revision_policy": "exact-disc-id"}
        manifest["executable"] = derived
        # The artifact stem is a property of the guest, not of the disc it is
        # imported under, so both routes build it under the same stem and the two
        # packages stay comparable byte for byte. Only the executable block above
        # comes from the generated ELF.
        manifest["game_name"] = synthetic_manifest()["game_name"]
        return manifest

    def validate(self, name: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.validator), "--validate-package", str(self.roots[name]), DISC_ID,
             TITLE_ID, "1", "EBOOT.BIN"],
            cwd=ROOT, capture_output=True, text=True,
        )

    # -- the launch half ------------------------------------------------------

    def launch(self, name: str) -> tuple[subprocess.CompletedProcess, Path, Path]:
        """PLAY NOW, headless, through the player's own launch session."""
        local_appdata = self.roots[name].parent / "localappdata"
        library_dir = local_appdata / "Nakagawa" / "data"
        library_dir.mkdir(parents=True, exist_ok=True)
        (library_dir / "library.json").write_bytes(
            (self.roots[name] / "library.json").read_bytes())
        sandbox = self.roots[name].parent / "sandbox"
        boot_log = sandbox / f"{name}-boot-events.log"
        perf_csv = sandbox / f"{name}-perf.csv"
        environment = os.environ.copy()
        environment["LOCALAPPDATA"] = str(local_appdata)
        environment["SR_BOOT_EVENT_FILE"] = str(boot_log)
        environment["SR_PERF"] = "1"
        environment["SR_PERF_CSV"] = str(perf_csv)
        ucrt_bin = Path("C:/msys64/ucrt64/bin")
        if os.name == "nt" and ucrt_bin.is_dir():
            environment["PATH"] = str(ucrt_bin) + os.pathsep + environment.get("PATH", "")
        launched = subprocess.run(
            [str(self.player), "--empty", "--launch-index=0", "--headless-launch",
             f"--runtime-root={self.roots[name]}"],
            cwd=ROOT, env=environment, capture_output=True, text=True,
            timeout=LAUNCH_TIMEOUT_MS,
        )
        return launched, boot_log, perf_csv

    def assert_launched(self, name: str) -> None:
        launched, boot_log, perf_csv = self.launch(name)
        self.assertEqual(launched.returncode, 0,
                         launched.stdout[-4000:] + launched.stderr[-4000:])
        self.assertIn("[PLAYER] Launch index 0: PLAY NOW available", launched.stdout)
        self.assertIn("[PLAYER] Headless launch child exited with code 0", launched.stdout)
        events = boot_log.read_text(encoding="utf-8", errors="replace")
        for milestone in ("BOOT_EVENT phase=image_loaded",
                          "BOOT_EVENT phase=runtime_registered",
                          "BOOT_EVENT phase=guest_start mode=scheduler"):
            self.assertIn(milestone, events)
        for forbidden in ("UNKNOWN NID", "NONPLT_MISS", "INTERP_REJECT",
                          "DRIVER_EXPECT_U32"):
            self.assertNotIn(forbidden, events)
        rows = perf_csv.read_text(encoding="utf-8", errors="replace").splitlines()
        self.assertGreaterEqual(len(rows), 2, "the perf CSV has header and summary only")
        self.assertEqual(rows[0].split(",")[0], "vblank_total")
        self.assertGreaterEqual(int(rows[-1].split(",")[0]), FRAMES)

    # -- 1. determinism -------------------------------------------------------

    def test_identical_inputs_produce_an_identical_package(self):
        """Two builds of unchanged inputs, compared file by file."""
        pair = self.package_pair("a", "b")
        self.assertEqual(pair.left_names, pair.right_names,
                         "the two builds produced different package file sets")
        self.assertTrue(pair.authored, "the package contains no project-authored source")

        # (1) Project-authored and guest-derived artifacts: byte-identical, with no
        #     rule applied at all. This is the load-bearing determinism claim.
        for name in sorted(pair.authored):
            self.assertIn(name, pair.left,
                          f"the package no longer contains {name}")
            self.assertEqual(pair.left_by_name[name], pair.right_by_name[name],
                             f"{name} is not byte-identical between two builds of the "
                             "same inputs")

        # (2) The content-addressed cache key is the designed determinism, and it
        #     carries no path and no clock.
        left_package = json.loads((self.packages["a"] / "package.json")
                                  .read_text(encoding="utf-8"))
        right_package = json.loads((self.packages["b"] / "package.json")
                                   .read_text(encoding="utf-8"))
        for side in ("aot", "native"):
            self.assertEqual(left_package["cache"]["key"][side]["digest"],
                             right_package["cache"]["key"][side]["digest"],
                             f"the {side} cache key is not reproducible from the same inputs")

        # (3) Everything else must be explained, rule by documented rule.
        self.assertEqual(
            pair.unattributed, [],
            "these differences are explained by no documented rule:\n"
            + "\n".join(pair.unattributed))
        # A rule that never fires no longer documents anything, and a rule set that
        # explains everything only means something while it is this tight.
        self.assertEqual(
            sorted(set(RULE_NAMES) - pair.rules), [],
            "these documented rules never fired, so they no longer describe a real "
            "difference between two builds of the same inputs")

    def test_unchanged_inputs_reuse_the_package_the_route_already_built(self):
        """A second BUILD PACKAGE on unchanged inputs reproduces the same package.

        The content-addressed cache is the promise a consumer feels: press BUILD
        PACKAGE again and get the same package identity back rather than a new one.
        """
        before = digest_map(self.packages["a"])
        rebuilt = subprocess.run(
            [sys.executable, str(TOOLS / "nk_cli.py"), "build-package", DISC_ID,
             "--user-data-root", str(self.roots["a"])],
            cwd=ROOT, capture_output=True, text=True, timeout=1800,
        )
        self.assertEqual(rebuilt.returncode, 0,
                         rebuilt.stdout[-4000:] + rebuilt.stderr[-4000:])
        self.assertIn("PACKAGE_REUSED", rebuilt.stdout,
                      "an unchanged input set did not reuse its verified package")
        validated = self.validate("a")
        self.assertEqual(validated.returncode, 0, validated.stdout + validated.stderr)
        self.assertIn("PACKAGE_STATUS=OK", validated.stdout)
        self.assertEqual(before, digest_map(self.packages["a"]),
                         "a second build of unchanged inputs rewrote the package")

    # -- 2. public-export exclusion ------------------------------------------

    def test_build_output_never_enters_the_public_export(self):
        """Nothing this route wrote is reachable by git or by the public scope."""
        # (1) The outputs live in the consumer's per-user data root, outside the
        #     repository, so no export or status rule can ever see them.
        repository = ROOT.resolve()
        for name, root in self.roots.items():
            self.assertFalse(root.resolve().is_relative_to(repository),
                             f"root {name} wrote inside the repository at {root}")
        self.assertEqual(self.repo_status(), self.before,
                         "the route changed the repository's own status, including "
                         "untracked files")

        # (2) The public export and git status would both have to be pointed at the
        #     output root to see it. Pointed, the public scope must fail closed: it
        #     must audit every file the package holds and refuse the tree.
        package = self.packages["a"]
        files = sorted(read_tree(package))
        audit = subprocess.run(
            [sys.executable, str(TOOLS / "publish_audit.py"), "--public-scope",
             "--candidate-root", str(package)],
            cwd=ROOT, capture_output=True, text=True, timeout=600,
        )
        self.assertNotEqual(audit.returncode, 0,
                            "the public scope accepted a package output root as a "
                            "public candidate tree")
        findings = audit.stdout + audit.stderr
        self.assertIn("POLICY_UNCLASSIFIED", findings)
        summary = re.search(r"publication audit: \w+ \(\d+ findings across (\d+) "
                            r"candidate files", findings)
        self.assertIsNotNone(summary, f"the audit reported no per-file summary:\n{findings}")
        self.assertEqual(int(summary.group(1)), len(files),
                         "the public scope did not audit every file the package holds")
        # Most files are rejected by name. The staged assets and third-party notices
        # are the exception: their package-relative paths are the repository paths
        # the policy enumerates, so the auditor classifies them from the path alone.
        # That is why the refusal has to be a whole-tree verdict, and the test
        # states the exception instead of pretending the refusal is per file.
        policy = publication_policy.load_policy(
            ROOT / "assets" / "public_source_profile.json")
        collides = [name for name in files
                    if policy.resolve(name).disposition == publication_policy.INCLUDED]
        self.assertTrue(collides, "no package file collided with an included repository "
                                  "path, so this exclusion proof is weaker than claimed")
        for name in files:
            if name in collides:
                continue
            self.assertIn(name, findings,
                          f"the public scope said nothing about {name}, so its refusal "
                          "is not a per-file decision")

        # (3) The same verdict straight from the policy, for an output root that
        #     really did sit inside the repository.
        policy = publication_policy.load_policy(
            ROOT / "assets" / "public_source_profile.json")
        for inside in (f"packages/{DISC_ID}/package.json",
                       f"user/packages/{DISC_ID}/package.json",
                       f"build/{DISC_ID}/package.json",
                       f"logs/build_{DISC_ID}.log",
                       f"cache/packages/{DISC_ID}/selected.elf"):
            resolution = policy.resolve(inside)
            self.assertEqual(
                resolution.disposition, publication_policy.UNCLASSIFIED,
                f"{inside} resolved to {resolution.disposition}, so an output root "
                "inside the repository would be publishable")
            self.assertEqual(resolution.rule, "default_disposition")

    # -- 3. the analyzer-generated manifest variant ----------------------------

    def test_analyzer_generated_manifest_builds_and_launches_the_same_guest(self):
        """The same route, with guest addresses from the generated ELF, not a literal."""
        validated = self.validate("c")
        self.assertEqual(validated.returncode, 0, validated.stdout + validated.stderr)
        self.assertIn("PACKAGE_STATUS=OK", validated.stdout)

        generated = read_tree(self.packages["c"])
        package = json.loads(generated["package.json"])
        self.assertEqual(package["title"]["id"], TITLE_ID)
        self.assertEqual(package["executable"]["guest_entry"], "0x08810000")
        reference = json.loads(read_tree(self.packages["a"])["package.json"])
        # The two routes record different manifest identities, which is what makes
        # the byte comparison below a statement about codegen rather than about two
        # identical builds.
        self.assertNotEqual(reference["inputs"]["manifest"]["sha256"],
                            package["inputs"]["manifest"]["sha256"])
        self.assertEqual(reference["inputs"]["executable"]["sha256"],
                         package["inputs"]["executable"]["sha256"])

        # Codegen output is a function of the guest, not of where the manifest's
        # addresses came from, so every project-authored and guest-derived artifact
        # must be byte-identical to the hand-written-manifest build, with no rule.
        pair = self.package_pair("a", "c")
        strict = sorted(name for name in pair.authored if not names_the_title(name))
        self.assertTrue(strict, "the package contains no project-authored source")
        for name in strict:
            self.assertIn(name, pair.identity, f"the package no longer contains {name}")
            self.assertEqual(pair.left_by_name[name], pair.right_by_name[name],
                             f"{name} differs between the hand-written and the "
                             "analyzer-generated manifest build")

        # The compiled result differs only where the compiler's own echoes are: the
        # runtime carries the build directory it was compiled with, and the flight
        # recorder carries the compile clock. Those are the same documented rules two
        # builds of *identical* inputs needed, and nothing else may explain them.
        self.assertIn(EXECUTABLE, pair.differing,
                      f"{EXECUTABLE} is byte-identical between the two manifest routes, "
                      "so this comparison says nothing about what the compiler produced")
        identical = self.package_pair("a", "b")
        self.assertTrue(pair.rules_for(EXECUTABLE),
                        f"{EXECUTABLE} differs and no documented rule explains it")
        self.assertTrue(pair.rules_for(EXECUTABLE) <= identical.rules_for(EXECUTABLE),
                        f"{EXECUTABLE} differed between the two manifest routes for a "
                        "reason two builds of identical inputs did not share: "
                        f"{sorted(pair.rules_for(EXECUTABLE))}")

        # The only artifacts allowed to differ are the ones that record a manifest's
        # own identity. Everything the toolchain derives from the *guest* must match
        # byte for byte, with no rule applied.
        self.assertEqual(
            sorted(name for name in pair.differing
                   if not name.endswith((".d", ".o", ".map"))
                   and name not in (".game-inputs", "runtime_profile.json", EXECUTABLE)),
            sorted(MANIFEST_IDENTIFIED),
            "these artifacts differ between the two manifest routes, and the difference "
            "is neither the manifest's own recorded identity nor a compiled artifact")

        # The compiled artifacts do differ, because the two manifests carry different
        # content-addressed cache keys, and every one of them is a file two builds of
        # *identical* inputs also had to differ in. A new one appearing here would
        # mean the analyzer-derived addresses changed what the compiler produced.
        identical = self.package_pair("a", "b")

        def compiled(names) -> set[str]:
            return {name for name in names
                    if name.endswith((".d", ".o", ".map")) or name == EXECUTABLE}

        self.assertEqual(compiled(pair.differing), compiled(identical.differing),
                         "the two manifest routes differ in a compiled artifact that two "
                         "builds of identical inputs did not, so the guest itself is not "
                         "what the two routes agree on")
        self.assertIn(EXECUTABLE, pair.differing,
                      f"{EXECUTABLE} is byte-identical between the two manifest routes, "
                      "so this comparison says nothing about what the compiler produced")
        self.assertTrue(pair.rules_for(EXECUTABLE),
                        f"{EXECUTABLE} differs and no documented rule explains it")
        self.assertTrue(pair.rules_for(EXECUTABLE) <= identical.rules_for(EXECUTABLE),
                        f"{EXECUTABLE} differed between the two manifest routes for a "
                        "reason two builds of identical inputs did not share: "
                        f"{sorted(pair.rules_for(EXECUTABLE))}")

        # The staged notices, the codegen profile and the artifact names carry the
        # guest's own stem and title, so the two routes must agree on all of them.
        for name in ("THIRD_PARTY_NOTICES.txt", "THIRD_PARTY_NOTICES/index.json",
                     "codegen_profile.json"):
            self.assertIn(name, pair.identical,
                          f"{name} names the title, and a title is not what this "
                          "comparison varies")

        # And the launch is the same guest-visible run on both routes.
        self.assert_launched("a")
        self.assert_launched("c")


if __name__ == "__main__":
    unittest.main()
