# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Contract test for the SR_TRACE_PC address-windowed instruction trace.

The full per-instruction trace is one line per guest instruction, which no run
of any length can afford to write.  SR_TRACE_PC narrows it to a guest-pc window
and lets the operator name the registers whose ABSOLUTE value each record must
carry, because a window cannot reconstruct a value written before it.  That is
what makes an out-of-domain VFPU argument traceable back to its inputs (#69).

These assertions pin the contract that makes the window usable and
non-invasive:

* the window gate runs BEFORE the register snapshot, so an armed window costs
  one range compare per guest instruction and the snapshot only for in-window
  instructions;
* with no window armed the trace stream is byte-identical to the un-windowed
  one (the absolute-value block and the record budget are both behind the same
  arming flag), so the oracle/diff builds that compare traces keep working;
* the stream can be armed before guest execution, while per-vblank hooks retain
  the frame markers, so the trace build needs no title-specific code;
* the parse rejects a malformed window and never arms on one.

It is a source-shape test on purpose: the hooks live in recomp.c, which the
native selftests do not link (they build from an explicit file list), and the
behavioural evidence is the traced run itself.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECOMP_C = (ROOT / "src/rt/recomp.c").read_text(encoding="utf-8")
RECOMP_H = (ROOT / "src/rt/recomp.h").read_text(encoding="utf-8")
GE_C = (ROOT / "src/rt/ge.c").read_text(encoding="utf-8")
DRIVER_C = (ROOT / "src/rt/driver.c").read_text(encoding="utf-8")
WINDOW_C = (ROOT / "src/rt/flight_recorder.c").read_text(encoding="utf-8")
WINDOW_H = (ROOT / "src/rt/flight_recorder.h").read_text(encoding="utf-8")
INTERP_C = (ROOT / "src/rt/guest_interp.c").read_text(encoding="utf-8")
CODEGEN_PY = (ROOT / "tools/codegen.py").read_text(encoding="utf-8")
DISPATCH_SELFTEST = (ROOT / "src/rt/dispatch_isolation_selftest.c").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")
NK_CLI = (ROOT / "tools/nk_cli.py").read_text(encoding="utf-8")


def body_of(source: str, signature: str) -> str:
    """Return the brace-balanced body of a C function definition."""
    start = source.index(signature)
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class TraceWindowProbeTests(unittest.TestCase):
    def test_window_is_declared_for_hosts(self):
        for signature in ("void sr_trace_window_configure(void);",
                          "void sr_trace_note_frame(uint32_t frame);",
                          "int  sr_trace_window_armed(void);",
                          "int  sr_trace_window_begin_instruction(uint32_t pc);",
                          "const SrTraceWindowIndices *sr_trace_window_indices(void);",
                          "void sr_trace_window_record(uint32_t pc, const char *text);"):
            self.assertIn(signature, WINDOW_H)
        # A host that does not link the recorder must keep compiling, so the
        # not-linked branch of the header provides inert definitions.
        stubbed = WINDOW_H[WINDOW_H.index("#else"):]
        self.assertIn("static inline int sr_trace_window_armed(void) { return 0; }", stubbed)

    def test_environment_contract(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        for name in ("SR_TRACE_PC", "SR_TRACE", "SR_TRACE_V", "SR_TRACE_F",
                     "SR_TRACE_LIMIT"):
            self.assertIn(f'getenv("{name}")', configure,
                          f"{name} must be read by the window probe")
        self.assertIn("strchr(window, ':')", configure)
        self.assertIn("strchr(window, '-')", configure)

    def test_malformed_window_never_arms(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        # A window without a separator, or an inverted one, is refused before the
        # stream is opened, so a typo cannot silently produce an empty trace.
        self.assertLess(configure.index("needs LO:HI"), configure.index("fopen"))
        self.assertLess(configure.index("empty window"), configure.index("fopen"))
        self.assertIn("if (hi < lo)", configure)

    def test_window_is_env_probed_once(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        self.assertTrue(configure.strip().startswith("void sr_trace_window_configure(void) {\n"
                                                    "    if (s_tw_configured) return;"))
        self.assertIn("s_tw_configured = 1;", configure)

    def test_gate_precedes_the_register_snapshot(self):
        begin = body_of(RECOMP_C, "void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op)")
        window_check = begin.index("sr_trace_window_armed()")
        snapshot = begin.index("memcpy(s_r, s->r")
        self.assertLess(window_check, snapshot,
                        "the window test must gate the snapshot, not follow it")
        self.assertIn("if (!s_fp) return;", begin)
        gate = body_of(WINDOW_C, "int sr_trace_window_begin_instruction(uint32_t pc)")
        self.assertIn("pc < s_tw_lo || pc > s_tw_hi", gate)

    def test_out_of_window_records_are_skipped(self):
        begin = body_of(RECOMP_C, "void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op)")
        self.assertIn("if (sr_trace_window_armed() && !sr_trace_window_begin_instruction(pc)) return;", begin)
        end = body_of(RECOMP_C, "void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size)")
        self.assertIn("if (sr_trace_window_armed() && !sr_trace_window_begin_instruction(s_pc)) return;", end)
        record = body_of(WINDOW_C, "void sr_trace_window_record(uint32_t pc, const char *text)")
        self.assertIn("if (!s_tw_armed || s_tw_skip || !s_tw_fp) return;", record)

    def test_unarmed_window_changes_no_output(self):
        end = body_of(RECOMP_C, "void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size)")
        # The canonical line still goes to the trace stream verbatim unless a
        # window is armed, so the oracle/diff traces keep their exact bytes.
        self.assertEqual(end.count("if (sr_trace_window_armed()) {"), 1)
        self.assertIn("fwrite(line, 1, n + 1, s_fp);", end)
        self.assertIn("abs v%u=0x%08x", end)
        self.assertIn("abs f%u=0x%08x", end)
        # Spending the record budget disarms the gate, so the rest of the run is
        # uninstrumented and the writer's own stream is left open.
        record = body_of(WINDOW_C, "void sr_trace_window_record(uint32_t pc, const char *text)")
        self.assertIn("if (s_tw_limit && ++s_tw_count >= s_tw_limit)", record)
        self.assertIn("s_tw_armed = 0;", record)

    def test_vblank_hooks_mark_trace_and_watch_timelines(self):
        flight_init = body_of(WINDOW_C, "void sr_flight_init(void)")
        self.assertIn("sr_watch_configure();", flight_init)

        configure = body_of(GE_C, "void ge_set_frame(uint32_t frame)")
        self.assertIn("sr_trace_window_configure();", configure)
        self.assertIn("sr_trace_note_frame(frame);", configure)
        self.assertIn("sr_watch_configure();", configure)
        self.assertIn("sr_watch_note_vblank(frame);", configure)
        # The frame marker is a no-op unless a window is armed.
        note = body_of(WINDOW_C, "void sr_trace_note_frame(uint32_t frame)")
        self.assertIn("if (!s_tw_armed || !s_tw_fp) return;", note)

    def test_window_is_armed_before_guest_execution(self):
        main = body_of(DRIVER_C, "int main(int argc, char **argv)")
        trace_open = main.index("sr_trace_open(out")
        configure = main.index("sr_trace_window_configure();")
        guest_start = main.index("sched_run(")
        self.assertLess(trace_open, configure)
        self.assertLess(configure, guest_start,
                        "a guest busy-loop before its first vblank must still be traceable")

    def test_index_list_is_bounded(self):
        listing = body_of(WINDOW_C, "static void sr_trace_window_list(")
        self.assertIn("*count < SR_TRACE_WINDOW_MAX_INDEX", listing)
        self.assertIn("if (value < 128u)", listing)

    def test_store_watch_parses_an_overflow_safe_range_and_reports_each_overlap(self):
        configure = body_of(WINDOW_C, "void sr_watch_configure(void)")
        self.assertIn('getenv("SR_WATCH")', configure)
        self.assertIn("strchr(spec, ':')", configure)
        self.assertIn("length > (UINT32_MAX + 1ULL) - start", configure)
        self.assertIn("if (s_watch_configured) return;", configure)

        store = body_of(WINDOW_C, "void sr_watch_store(")
        self.assertIn("store_start >= s_watch_end || store_end <= s_watch_start", store)
        self.assertIn(
            '"SR_WATCH: pc=0x%08x addr=0x%08x val=0x%08x width=%u vblank=%u\\n"',
            store,
        )
        self.assertIn("s_watch_matches++", store)
        flight_init = body_of(WINDOW_C, "void sr_flight_init(void)")
        self.assertIn("sr_watch_configure();", flight_init)

    def test_aot_and_interpreter_stores_share_the_watched_pc_aware_helpers(self):
        aot_recorder_flags = [
            line for line in MAKEFILE.splitlines()
            if line.startswith("override RECOMP_FLAGS +=")
        ]
        self.assertTrue(
            any("SR_FLIGHT_RECORDER_LINKED" in line for line in aot_recorder_flags),
            "generated AOT chunks must compile the active recorder hooks, not header stubs",
        )
        self.assertTrue(
            any("SR_INSTRUCTION_TRACE" in line for line in aot_recorder_flags),
            "trace-enabled AOT chunks must keep the instruction trace define",
        )
        runtime_trace_flags = [
            line for line in MAKEFILE.splitlines()
            if line.startswith("override CFLAGS +=")
        ]
        self.assertTrue(
            any("SR_INSTRUCTION_TRACE" in line for line in runtime_trace_flags),
            "trace-enabled interpreter/runtime code must keep the instruction trace define",
        )
        self.assertIn('env["TRACE"] = "1"', NK_CLI)
        self.assertIn('runtime_opt = env.get("RUNTIME_OPT", "-O0")', NK_CLI)
        self.assertIn('env["RUNTIME_OPT"] = f"{runtime_opt} -DSR_INSTRUCTION_TRACE".strip()', NK_CLI)
        self.assertIn(
            "environment = _runtime_build_environment(instruction_trace=instruction_trace)",
            NK_CLI,
        )
        self.assertIn(
            'instruction_trace=bool(getattr(args, "instruction_trace", False))',
            NK_CLI,
        )
        self.assertIn("$(CC) $(RECOMP_FLAGS)", MAKEFILE)

        for helper in ("sr_w8_pc", "sr_w16_pc", "sr_w32_pc"):
            body = body_of(RECOMP_H, f"static inline void {helper}(")
            self.assertIn("SR_WATCH_STORE_IF_ARMED(pc, a, v", body)

        self.assertIn("MEM_W32_PC(", CODEGEN_PY,
                      "generated AOT stores must keep their guest instruction PC")
        for store in ("MEM_W8_PC", "MEM_W16_PC", "MEM_W32_PC"):
            self.assertIn(store, INTERP_C,
                          "the interpreter store must reach the same watched helper")

        self.assertIn("test_valid_aot_miss_executes_guest_bytes", DISPATCH_SELFTEST)
        self.assertIn("test_aot_store_watch_synthetic_guest", DISPATCH_SELFTEST)
        self.assertIn("sr_watch_match_count()", DISPATCH_SELFTEST)
        self.assertIn("memory-watch-selftest", MAKEFILE)


if __name__ == "__main__":
    unittest.main()
