# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan
import title_manifest
import test_public_title_isolation
from test_import_name_safety import build_synthetic_import_prx
import nk_cli
from nk_core import package_cache


class TitleCodegenPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.hst_path = ROOT / "assets" / "titles" / "hst-ucus98701.json"
        self.synthetic_path = ROOT / "assets" / "titles" / "synthetic.json"
        self.synthetic = title_manifest.validate_manifest(
            title_manifest.load_manifest(self.synthetic_path)
        )
        # Opt-in by *tracked bytes*, never by directory presence (#335): a
        # developer's gitignored lookalike must not silently stand in for the
        # private fixture, and validating an arbitrary private file in setUp
        # must not error unrelated tests.
        self.hst = (
            title_manifest.validate_manifest(title_manifest.load_manifest(self.hst_path))
            if test_public_title_isolation.is_tracked("assets/titles/hst-ucus98701.json")
            else None
        )

    def plan(self, manifest=None, **overrides):
        values = {
            "game_name": "synthetic",
            "game_elf": Path("fixtures/synthetic.elf"),
            "build_dir": Path("build/synthetic"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": None,
            "include_optional_modules": set(),
            "funcs_per_chunk": 2000,
        }
        values.update(overrides)
        selected = self.synthetic if manifest is None else manifest
        return title_codegen_plan.build_plan(selected, **values)

    def test_package_build_command_runs_the_chosen_job_count(self) -> None:
        self.assertEqual(
            title_codegen_plan._make_build_command("mingw32-make", "compile", jobs=6),
            ["mingw32-make", "--no-print-directory", "-j6", "compile"],
        )
        self.assertEqual(
            title_codegen_plan._make_build_command("make", "compile", jobs=3, dry_run=True),
            ["make", "--no-print-directory", "-j3", "-n", "compile"],
        )

    def test_package_build_targets_split_generation_from_compilation(self) -> None:
        targets = title_codegen_plan._make_build_targets
        self.assertEqual(targets(native_only=False), ("pipeline", "compile"))
        self.assertEqual(targets(native_only=True), ("compile",))
        self.assertEqual(targets(native_only=False, aot_stage=True), ("pipeline",))
        self.assertEqual(targets(native_only=True, aot_stage=True), ("pipeline",))

    def test_package_build_jobs_follow_memory_and_cpus(self) -> None:
        gib = 1024**3
        jobs = title_codegen_plan.package_build_jobs
        cases = (
            # (logical CPUs, available bytes, expected jobs)
            (24, 22 * gib, 6),          # the measured PC: memory bound
            (24, 28 * gib, 8),          # never above eight jobs
            (24, 64 * gib, 8),
            (4, 64 * gib, 4),           # CPU bound
            (24, 10 * gib, 2),
            (24, 10 * gib - 1, 1),      # floor((10 GiB - 1 - 4 GiB) / 3 GiB) = 1
            (24, 6 * gib, 1),           # never below one job
            (24, 3 * gib, 1),           # less than the reserve
            (1, 64 * gib, 1),
            (0, 64 * gib, 1),           # an unknown CPU count is one CPU
            (None, 64 * gib, 1),
        )
        for cpus, available, expected in cases:
            with self.subTest(cpus=cpus, available=available):
                chosen, reason = jobs(cpu_count=cpus, available_bytes=available)
                self.assertEqual(chosen, expected)
                self.assertIn("GB memory available", reason)

    def test_package_build_jobs_without_a_memory_reading_keep_two(self) -> None:
        jobs = title_codegen_plan.package_build_jobs
        self.assertEqual(jobs(cpu_count=24, available_bytes=None)[0], 2)
        self.assertEqual(jobs(cpu_count=1, available_bytes=None)[0], 1)
        self.assertIn("unknown", jobs(cpu_count=24, available_bytes=None)[1])

    def test_package_build_jobs_override_is_clamped(self) -> None:
        jobs = title_codegen_plan.package_build_jobs
        gib = 1024**3
        for override, expected in (("3", 3), (" 5 ", 5), ("8", 8), ("32", 8), ("0", 1), ("-4", 1)):
            with self.subTest(override=override):
                chosen, reason = jobs(cpu_count=2, available_bytes=5 * gib, override=override)
                self.assertEqual(chosen, expected)
                self.assertIn("NK_PACKAGE_JOBS=", reason)
        # An empty or unreadable override is ignored, and the log says so.
        self.assertEqual(jobs(cpu_count=24, available_bytes=22 * gib, override="")[0], 6)
        chosen, reason = jobs(cpu_count=24, available_bytes=22 * gib, override="many")
        self.assertEqual(chosen, 6)
        self.assertIn("ignored NK_PACKAGE_JOBS='many'", reason)

    def test_host_package_build_jobs_reads_the_environment(self) -> None:
        with mock.patch.dict(os.environ, {"NK_PACKAGE_JOBS": "3"}):
            self.assertEqual(title_codegen_plan._host_package_build_jobs()[0], 3)
        available = title_codegen_plan.available_physical_memory()
        if available is not None:
            self.assertGreater(available, 0)
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("NK_PACKAGE_JOBS", None)
            chosen, _reason = title_codegen_plan._host_package_build_jobs()
        self.assertGreaterEqual(chosen, 1)
        self.assertLessEqual(chosen, title_codegen_plan.PACKAGE_BUILD_MAX_JOBS)

    def test_planned_compile_objects_reads_a_make_dry_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            absolute = (cwd / "abs").as_posix()
            listing = "\n".join((
                '"python" tools/shader_embed.py verify',
                "gcc -O2 -MMD -MP -c src/rt/hle.c -o build/x/hle.o",
                f"gcc -O2 -I{absolute} -c {absolute}/game_recomp_0.c -o {absolute}/game_recomp_0.o",
                "gcc -O2 -c src/rt/hle.c -o build/x/hle.o",  # repeated: counted once
                "gcc -O2 -o build/x/game.exe @build/x/link-objects.rsp -lm",  # the link
                "gcc -c src/rt/ge.c -o \"build/x/ge.o\"",
            ))
            objects = title_codegen_plan.planned_compile_objects(listing, cwd)
        self.assertEqual(
            objects,
            [cwd / "build/x/hle.o", Path(absolute) / "game_recomp_0.o", cwd / "build/x/ge.o"],
        )
        self.assertEqual(title_codegen_plan.planned_compile_objects("make: Nothing to be done", cwd), [])

    def test_compile_progress_counter_counts_written_objects(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fresh = root / "fresh.o"
            in_flight = root / "in_flight.o"
            stale = root / "stale.o"
            stale.write_bytes(b"old object")
            os.utime(stale, ns=(1_000_000_000, 1_000_000_000))
            counter = title_codegen_plan.CompileProgressCounter([fresh, in_flight, stale])
            self.assertEqual(counter.total, 3)
            self.assertEqual(counter.completed(), 0)
            in_flight.write_bytes(b"")  # created by the assembler, not yet filled
            self.assertEqual(counter.completed(), 0)
            fresh.write_bytes(b"\x7fELF")
            self.assertEqual(counter.completed(), 1)
            stale.write_bytes(b"new object")
            self.assertEqual(counter.completed(), 2)
            in_flight.write_bytes(b"\x7fELF")
            self.assertEqual(counter.completed(), 3)

    def test_make_with_progress_appends_each_new_count(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            objects = [root / f"part{index}.o" for index in range(3)]
            progress = root / "progress.jsonl"
            # A stand-in for Make that writes the objects one at a time.
            script = (
                "import sys, time\n"
                "for path in sys.argv[1:]:\n"
                "    time.sleep(0.2)\n"
                "    open(path, 'wb').write(b'obj')\n"
                "time.sleep(0.2)\n"
            )
            returncode = title_codegen_plan._run_make_with_progress(
                [sys.executable, "-c", script, *map(str, objects)],
                cwd=root,
                env=os.environ.copy(),
                counter=title_codegen_plan.CompileProgressCounter(objects),
                progress_file=progress,
                poll_seconds=0.05,
            )
            rows = [json.loads(line) for line in progress.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(returncode, 0)
        self.assertEqual(rows[0], {"completed": 0, "total": 3})
        self.assertEqual(rows[-1], {"completed": 3, "total": 3})
        completed = [row["completed"] for row in rows]
        self.assertEqual(completed, sorted(set(completed)), "each count is written once, in order")
        self.assertTrue(all(row["total"] == 3 for row in rows))

    def test_unconfigured_data_root_has_no_invalid_local_asset_record(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["filesystem"]["data_root"] = ""
        manifest["runtime_contract"]["resources"] = {
            "mode": "none", "locators": [],
        }

        assets = title_codegen_plan._required_local_assets(
            manifest, set(), {}, Path("build/synthetic"),
        )
        kinds = [asset["kind"] for asset in assets]
        self.assertNotIn("title-data-root", kinds)
        self.assertNotIn("runtime-resource-locator", kinds)

    def test_hst_plan_matches_current_makefile_contract(self) -> None:
        if self.hst is None:
            self.skipTest("PRIVATE_INPUT: private HST title manifest is unavailable in the sanitized public tree")
        plan = self.plan(
            self.hst,
            game_name="hst",
            game_elf=Path("place_game_here/EBOOT.elf"),
            build_dir=Path("build/hst"),
            module_dir=Path("place_game_here/EXTRACTED/decrypted"),
            psp_header=Path("place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN"),
            codegen_profile="hst",
        )
        self.assertEqual(plan["game_base"], 0)
        self.assertEqual(plan["game_entry"], 0)
        self.assertEqual(plan["codegen_profile"], "hst")
        # TITLE_EXTRA_SPANS is the sole host-portable planner projection.
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x00000000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x00000000")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "0x00303194,0x00306e24")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["commands"]["prxload"], [
            "python", "tools/prxload.py", "place_game_here/EBOOT.elf", "0x00000000",
            "--psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
            "--out=build/hst/hst_image.bin",
        ])
        self.assertEqual(plan["commands"]["codegen"], [
            "python", "tools/codegen.py", "place_game_here/EBOOT.elf",
            "build/hst/hst_recomp.c", "--base=0x00000000", "--profile=hst",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/libfont.prx@0x09ebfc00",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/scePsmf_library.prx@0x09ed6000",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx@0x09ec7f00",
            "--funcs-per-chunk=2000",
        ])
        self.assertEqual(plan["commands"]["imports"], [
            "python", "tools/imports.py", "place_game_here/EBOOT.elf", "0x00000000",
            "--toml=build/hst/hst_imports.toml",
        ])

    def test_generic_plan_explicitly_disables_hst_default_span(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["id"] = "synthetic-test-v1"
        manifest["executable"] = {
            "base": 0x08804000,
            "entry": 0x08804128,
            "bss_metadata_source": "elf",
            "extra_executable_spans": [],
        }
        manifest["modules"] = []
        manifest["codegen_profile"] = "none"
        plan = self.plan(
            manifest,
            game_name="synthetic",
            module_dir=None,
            psp_header=None,
            codegen_profile="none",
        )
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x08804000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x08804128")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertNotIn("--profile=hst", plan["commands"]["codegen"])

    def test_psp_header_is_required_only_by_manifest_policy(self) -> None:
        psp_manifest = copy.deepcopy(self.synthetic)
        psp_manifest["executable"] = dict(psp_manifest["executable"])
        psp_manifest["executable"]["bss_metadata_source"] = "psp-header"
        with self.assertRaisesRegex(ValueError, "psp_header is required"):
            self.plan(psp_manifest, psp_header=None)
        generic = copy.deepcopy(self.synthetic)
        generic["executable"] = dict(generic["executable"])
        generic["executable"]["bss_metadata_source"] = "elf"
        with self.assertRaisesRegex(ValueError, "incompatible"):
            self.plan(generic, psp_header=Path("unexpected.bin"))

    def test_module_directory_is_required_for_guest_modules(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 0x08C00000,
            "required": True,
            "role": "guest-prx",
        }]
        with self.assertRaisesRegex(ValueError, "module_dir is required"):
            self.plan(manifest, module_dir=None)

    def test_runtime_placed_modules_translate_position_independently(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [
            {"name": "fixed.prx", "load_address": 0x08C00000, "required": True,
             "role": "guest-prx"},
            {"name": "level01.prx", "required": True, "role": "guest-prx",
             "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/level01.prx"},
        ]
        plan = self.plan(manifest, module_dir=Path("modules"))
        extra = [arg for arg in plan["commands"]["codegen"] if arg.startswith("--extra-elf=")]
        self.assertEqual(extra, [
            "--extra-elf=modules/fixed.prx@0x08c00000",
            "--extra-elf=modules/level01.prx@runtime",
        ])
        manager = title_codegen_plan.build_manager_plan(
            manifest, game_name="synthetic", game_elf=Path("fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"), module_dir=Path("modules"))
        self.assertEqual(manager["required_guest_modules"], [
            {"name": "fixed.prx", "load_address": 0x08C00000},
            {"name": "level01.prx", "placement": "runtime"},
        ])

    def test_runtime_placed_module_is_analyzed_in_its_link_space(self) -> None:
        with tempfile.TemporaryDirectory(prefix="title_codegen_link_space_") as temp_dir:
            prx = Path(temp_dir) / "level01.prx"
            prx.write_bytes(b"\x7fELF\x01\x01\x01" + b"\0" * 9 + (0xFFA0).to_bytes(2, "little"))
            image = Path(temp_dir) / "fixed.elf"
            image.write_bytes(b"\x7fELF\x01\x01\x01" + b"\0" * 9 + (2).to_bytes(2, "little"))
            runtime = {"name": "level01.prx", "placement": "runtime"}
            self.assertEqual(title_codegen_plan._analysis_base(runtime, prx), 0)
            self.assertIsNone(title_codegen_plan._analysis_base(runtime, image))
            self.assertEqual(title_codegen_plan._analysis_base(
                {"name": "fixed.prx", "load_address": 0x08C00000}, prx), 0x08C00000)

    def test_runtime_module_fallbacks_are_attributed_by_name(self) -> None:
        class FakeElf:
            def read_at_vaddr(self, address, size):
                return (0x24020000 | address).to_bytes(4, "little")

        sources = [
            {"name": "executable", "ranges": [(0x0, 0x1000)], "elf": FakeElf()},
            {"name": "level01.prx", "ranges": [(0x0, 0x800)], "elf": FakeElf()},
        ]
        with tempfile.TemporaryDirectory(prefix="title_codegen_fallbacks_") as temp_dir:
            report = Path(temp_dir) / "g_recomp_stubs.txt"
            report.write_text(
                "0x00000100 opcode 0x13 at 0x00000104\n"
                "level01.prx@0x00000100 opcode 0x13 at 0x00000108\n",
                encoding="ascii")
            regions, instructions = title_codegen_plan._read_codegen_fallbacks(report, sources)
            self.assertEqual(sorted((row["module"], row["entry_address"]) for row in regions), [
                ("executable", "0x00000100"), ("level01.prx", "0x00000100")])
            self.assertEqual(sorted((row["module"], row["address"]) for row in instructions), [
                ("executable", "0x00000104"), ("level01.prx", "0x00000108")])
            report.write_text("absent.prx@0x00000100 opcode 0x13\n", encoding="ascii")
            with self.assertRaises(title_codegen_plan.PackageRouteError):
                title_codegen_plan._read_codegen_fallbacks(report, sources)

    def test_hle_capabilities_are_not_passed_as_guest_elf_files(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [
            {
                "name": "kernel-capability",
                "load_address": 0x08C00000,
                "required": True,
                "role": "hle-capability",
            }
        ]
        plan = self.plan(manifest, module_dir=None)
        self.assertFalse(any(arg.startswith("--extra-elf=") for arg in plan["commands"]["codegen"]))

    def test_missing_import_boundary_points_to_open_product_issue(self) -> None:
        base = self.synthetic["executable"]["base"]
        elf_bytes, _stub = build_synthetic_import_prx(b"sceUntrackedSynthetic", base)
        with tempfile.TemporaryDirectory(prefix="title_codegen_missing_import_") as temp_dir:
            elf_path = Path(temp_dir) / "synthetic-prx.elf"
            elf_path.write_bytes(elf_bytes)
            _sources, _summary, unsupported_imports, _diagnostics = (
                title_codegen_plan._make_input_images(
                    self.synthetic, elf_path, None, None, set()
                )
            )
        self.assertEqual(unsupported_imports, [{
            "boundary": "PSP import sceUntrackedSynthetic:0x12345678",
            "module": "executable",
            "library": "sceUntrackedSynthetic",
            "nid": "0x12345678",
            "name": None,
            "classification": "missing",
            "status": "in the works",
            "tracking_issue": 308,
        }])

    def test_player_and_planner_cache_keys_include_planner_source(self) -> None:
        captured = []

        def capture_cache_key(**kwargs):
            captured.append(kwargs)
            return {}

        with mock.patch.object(
            package_cache, "build_cache_key", side_effect=capture_cache_key
        ), mock.patch.object(
            package_cache, "compiler_identity", return_value="synthetic-compiler"
        ), mock.patch.object(
            package_cache, "compiler_target", return_value="synthetic-target"
        ):
            identity = package_cache.build_title_input_identity(
                manifest={"id": "synthetic-cache-test", "schema_version": 1},
                executable_name="EBOOT.BIN",
                executable_sha256="2" * 64,
                modules=[],
            )
            title_codegen_plan._cache_key_for_build(
                input_hashes={}, title_input_identity=identity, plan=self.plan(),
                selected_optional=set(), funcs_per_chunk=64, public_safe=True,
                compiler_name="gcc",
            )
            nk_cli._current_package_cache_key(
                self.synthetic,
                self.synthetic_path,
                "synthetic-executable-hash",
                None,
                None,
                identity,
                public_safe=True,
            )

        expected = package_cache.sha256_file(ROOT / "tools" / "title_codegen_plan.py")
        self.assertEqual(len(captured), 2)
        for call in captured:
            self.assertEqual(call["codegen_options"].get("planner_sha256"), expected)

    def test_player_and_planner_hash_a_runtime_placed_module_alike(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        module = {"name": "level01.prx", "required": True, "role": "guest-prx",
                  "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/level01.prx"}
        manifest["modules"] = [module]
        captured = []

        def capture_cache_key(**kwargs):
            captured.append(kwargs)
            return {}

        with tempfile.TemporaryDirectory(prefix="title_codegen_runtime_hash_") as temp_dir:
            module_dir = Path(temp_dir)
            (module_dir / "level01.prx").write_bytes(b"synthetic runtime-placed module")
            game_elf = module_dir / "game.elf"
            game_elf.write_bytes(b"synthetic executable")
            with mock.patch.object(
                package_cache, "build_cache_key", side_effect=capture_cache_key
            ), mock.patch.object(
                package_cache, "compiler_identity", return_value="synthetic-compiler"
            ), mock.patch.object(
                package_cache, "compiler_target", return_value="synthetic-target"
            ):
                nk_cli._current_package_cache_key(
                    manifest, self.synthetic_path, "synthetic-executable-hash",
                    module_dir, None, {}, public_safe=True,
                )
            planned = title_codegen_plan._hash_package_inputs(
                self.synthetic_path, game_elf, [module],
                {"level01.prx": module_dir / "level01.prx"}, None,
            )
        self.assertEqual(len(captured), 1)
        player_modules = captured[0]["input_hashes"]["modules"]
        self.assertEqual(player_modules, planned["modules"])
        self.assertEqual(player_modules[0]["load_address"], "runtime")

    def test_optional_guest_modules_require_explicit_selection(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [
            {
                "name": "required.prx",
                "load_address": 0x08C00000,
                "required": True,
                "role": "guest-prx",
            },
            {
                "name": "optional.prx",
                "load_address": 0x08C10000,
                "required": False,
                "role": "optional-guest-prx",
            },
        ]
        default = self.plan(manifest, module_dir=Path("modules"))
        self.assertTrue(any("required.prx@" in arg for arg in default["commands"]["codegen"]))
        self.assertFalse(any("optional.prx@" in arg for arg in default["commands"]["codegen"]))
        selected = self.plan(
            manifest,
            module_dir=Path("modules"),
            include_optional_modules={"optional.prx"},
        )
        self.assertTrue(any("optional.prx@" in arg for arg in selected["commands"]["codegen"]))
        with self.assertRaisesRegex(ValueError, "unknown optional"):
            self.plan(
                manifest,
                module_dir=Path("modules"),
                include_optional_modules={"missing.prx"},
            )

    def test_module_binding_rejects_ambiguous_separator(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 0x08C00000,
            "required": True,
            "role": "guest-prx",
        }]
        with self.assertRaisesRegex(ValueError, "must not contain '@'"):
            self.plan(manifest, module_dir=Path("private@modules"))

    def test_current_analyzer_limit_fails_closed(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["executable"] = dict(manifest["executable"])
        manifest["executable"]["extra_executable_spans"] = [
            {"start": 1, "end": 2}, {"start": 3, "end": 4}
        ]
        with self.assertRaisesRegex(ValueError, "at most one"):
            self.plan(manifest)
        manifest["executable"]["extra_executable_spans"] = [{"start": 1, "end": 2}]
        manifest["executable"]["base"] = 0x08804000
        manifest["codegen_profile"] = "none"
        with self.assertRaisesRegex(ValueError, "nonzero base"):
            self.plan(manifest, codegen_profile="none")
        manifest["executable"]["extra_executable_spans"] = []
        manifest["codegen_profile"] = "hst"
        with self.assertRaisesRegex(ValueError, "hst codegen profile requires"):
            self.plan(manifest)

    def test_profiles_and_chunk_bounds_fail_closed(self) -> None:
        for profile in ("unknown", " HST", "hst/unsafe", ""):
            with self.subTest(profile=profile):
                with self.assertRaises(ValueError):
                    self.plan(codegen_profile=profile)
        for count in (0, -1, 100001, True):
            with self.subTest(count=count):
                with self.assertRaises(ValueError):
                    self.plan(funcs_per_chunk=count)

    def test_unused_module_binding_and_bad_paths_fail_closed(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = []
        with self.assertRaisesRegex(ValueError, "no guest modules"):
            self.plan(manifest, module_dir=Path("unused"))
        with self.assertRaisesRegex(ValueError, "control characters"):
            self.plan(game_elf=Path("bad\nelf"))
        with self.assertRaisesRegex(ValueError, "identify a file"):
            self.plan(build_dir=Path("."))
        with self.assertRaisesRegex(ValueError, "must be strings"):
            self.plan(include_optional_modules={1})

    def test_windows_style_bindings_are_rendered_with_forward_slashes(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 0x08C00000,
            "required": True,
            "role": "guest-prx",
        }]
        plan = self.plan(
            manifest,
            game_elf=Path(r"C:\private\EBOOT.elf"),
            build_dir=Path(r"C:\repo\build\synthetic"),
            module_dir=Path(r"C:\private\modules"),
        )
        rendered = json.dumps(plan)
        self.assertNotIn("\\\\", rendered)
        self.assertIn("C:/private/EBOOT.elf", rendered)
        self.assertIn("C:/repo/build/synthetic/synthetic_recomp.c", rendered)

    def test_plan_json_is_deterministic(self) -> None:
        first = title_codegen_plan.canonical_json(self.plan())
        second = title_codegen_plan.canonical_json(self.plan())
        self.assertEqual(first, second)
        self.assertEqual(json.loads(first)["title_manifest_id"], "synthetic-allegrex-v1")

    def test_manifest_profile_is_authoritative_when_cli_profile_is_omitted(self) -> None:
        plan = title_codegen_plan.build_plan(
            self.synthetic,
            game_name="synthetic",
            game_elf=Path("fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            module_dir=None,
            psp_header=None,
            codegen_profile=None,
        )
        self.assertEqual(plan["codegen_profile"], "none")
        with self.assertRaisesRegex(ValueError, "conflicts with the manifest"):
            self.plan(codegen_profile="hst")

    def test_cli_emits_json_and_never_executes_codegen(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out_c = Path(temp) / "build" / "synthetic" / "synthetic_recomp.c"
            proc = subprocess.run([
                sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"),
                str(self.synthetic_path), "--game-name=synthetic",
                "--game-elf=fixtures/synthetic.elf", "--build-dir=build/synthetic",
                "--profile=none",
            ], cwd=ROOT, capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["game_name"], "synthetic")
            self.assertFalse(out_c.exists())

    def test_resolve_make_safe_build_root_priority(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            spaced_dest = temp_path / "spaced folder" / "output_package"
            spaced_dest.parent.mkdir(parents=True)
            safe_env_root = temp_path / "safe_env_root"
            safe_env_root.mkdir()
            short_mock = temp_path / "SHORTP~1"
            short_mock.mkdir()

            # Priority 1: NK_BUILD_ROOT wins when set and safe
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": str(safe_env_root)}):
                with mock.patch("title_codegen_plan._windows_short_path", return_value=short_mock):
                    res = title_codegen_plan._resolve_make_safe_build_root(spaced_dest)
                    self.assertEqual(res, safe_env_root.resolve())

            # Priority 2: When NK_BUILD_ROOT contains spaces, it is skipped and short_path wins
            unsafe_env_root = temp_path / "unsafe env root"
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": str(unsafe_env_root)}):
                with mock.patch("title_codegen_plan._windows_short_path", return_value=short_mock):
                    res = title_codegen_plan._resolve_make_safe_build_root(spaced_dest)
                    self.assertEqual(res, short_mock.resolve())

            # Priority 3: When NK_BUILD_ROOT is unset, short_path wins
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("NK_BUILD_ROOT", None)
                with mock.patch("title_codegen_plan._windows_short_path", return_value=short_mock):
                    res = title_codegen_plan._resolve_make_safe_build_root(spaced_dest)
                    self.assertEqual(res, short_mock.resolve())

            # Priority 4: When neither is available, fails with actionable PACKAGE_UNSUPPORTED_PATH
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("NK_BUILD_ROOT", None)
                with mock.patch("title_codegen_plan._windows_short_path", return_value=None):
                    with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
                        title_codegen_plan._resolve_make_safe_build_root(spaced_dest)
                    self.assertEqual(caught.exception.code, "PACKAGE_UNSUPPORTED_PATH")
                    self.assertIn("contains spaces", str(caught.exception))
                    self.assertIn("8.3 short names are unavailable on this volume", str(caught.exception))
                    self.assertIn("set NK_BUILD_ROOT to a folder without spaces", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
