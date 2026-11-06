# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pin the nkstate v1 container (#2146, #2147) in Python and against the C reader.

The executable proof is ``make nkstate-selftest`` (tests/native/nkstate_selftest.c): a
guest program saved mid-run, restored into a clobbered arena and continued, against an
uninterrupted run. These tests run anywhere and keep ``tools/nkstate.py``, ``src/rt/nkstate.c``
and the CpuState ABI in step: one golden file both writers must produce byte for byte, the
same refusal names in the same order, and every refusal the Python reader makes.
"""

from __future__ import annotations

import hashlib
import re
import struct
import unittest
from pathlib import Path

from tools import nkstate

ROOT = Path(__file__).resolve().parents[1]
RECOMP_H = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
NKSTATE_C = (ROOT / "src" / "rt" / "nkstate.c").read_text(encoding="utf-8")
NKSTATE_H = (ROOT / "src" / "rt" / "nkstate.h").read_text(encoding="utf-8")
SELFTEST_C = (ROOT / "tests" / "native" / "nkstate_selftest.c").read_text(encoding="utf-8")

#: The file tests/native/nkstate_selftest.c test_golden writes.
GOLDEN_SHA256 = "2be4fb0c0c28c07cc701b8a1ba35283fcaad29bee3fa8b590b9a8ac2f8555a2d"

SELF = nkstate.Identity(
    kind=nkstate.KIND_STATE,
    executable=bytes(0xA0 + i for i in range(32)),
    runtime="4f1d2c3b4a5968778695a4b3c2d1e0f001122334",
    disc_id="NKTEST00001",
    flags=nkstate.FLAG_DETERMINISTIC_CLOCK,
)
SEGMENTS = [nkstate.Segment("SEGA", 1, b"segment-a"), nkstate.Segment("SEGB", 2, b"segment-b-longer")]
VERSIONS = {"SEGA": 1, "SEGB": 2}

OFF_FORMAT, OFF_KIND, OFF_ABI, OFF_CPUSIZE = 8, 12, 16, 20
OFF_EXE, OFF_RUNTIME, OFF_DISC, OFF_FLAGS, OFF_COUNT, OFF_TABLE = 24, 56, 120, 136, 140, 144


def _good() -> bytes:
    return nkstate.build(SELF, SEGMENTS)


def _put32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def _rehash(data: bytearray) -> None:
    data[-32:] = hashlib.sha256(bytes(data[:-32])).digest()


class GoldenTest(unittest.TestCase):
    def test_python_writer_matches_the_c_golden(self) -> None:
        image = nkstate.build(SELF, [nkstate.Segment("TEST", 1, b"nakagawa-state")])
        self.assertEqual(hashlib.sha256(image).hexdigest(), GOLDEN_SHA256)
        self.assertIn(f'#define GOLDEN_SHA256 "{GOLDEN_SHA256}"', SELFTEST_C)

    def test_cpustate_abi_matches_recomp_h(self) -> None:
        match = re.search(r"#define SR_CPUSTATE_ABI_VERSION (\d+)u", RECOMP_H)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), nkstate.ABI_VERSION)

    def test_header_layout_matches_c(self) -> None:
        self.assertEqual(nkstate.HEADER.size, OFF_TABLE)
        self.assertIn('#define NKSTATE_MAGIC "NKSTATE1"', NKSTATE_H)
        self.assertIn("#define NKSTATE_FORMAT_VERSION 1u", NKSTATE_H)
        self.assertIn(f"#define NKSTATE_MAX_SEGMENTS {nkstate.MAX_SEGMENTS}u", NKSTATE_H)

    def test_refusal_names_match_c_in_order(self) -> None:
        names = re.findall(r'\[NKSTATE_[A-Z_]+\] = "([a-z-]+)"', NKSTATE_C)
        # The C table also has the names only a capture or a load can give.
        self.assertEqual(tuple(n for n in names if n in nkstate.REASONS), nkstate.REASONS)
        self.assertEqual(names[1:1 + len(nkstate.REASONS)], list(nkstate.REASONS))


class ParseTest(unittest.TestCase):
    def assertRefused(self, data: bytes, reason: str, **kwargs) -> None:
        kwargs.setdefault("self_identity", SELF)
        kwargs.setdefault("segment_versions", VERSIONS)
        kwargs.setdefault("require_deterministic_clock", True)
        with self.assertRaises(nkstate.StateError) as ctx:
            nkstate.parse(data, **kwargs)
        self.assertEqual(ctx.exception.reason, reason, str(ctx.exception))

    def test_round_trip(self) -> None:
        state = nkstate.parse(_good(), self_identity=SELF, segment_versions=VERSIONS,
                              require_deterministic_clock=True)
        self.assertEqual(state.identity, SELF)
        self.assertEqual(state.segments, SEGMENTS)

    def test_each_header_refusal(self) -> None:
        cases = [
            ("bad-magic", lambda d: d.__setitem__(0, ord("X"))),
            ("format-version", lambda d: _put32(d, OFF_FORMAT, 2)),
            ("kind", lambda d: _put32(d, OFF_KIND, nkstate.KIND_RECORDING)),
            ("abi-version", lambda d: _put32(d, OFF_ABI, nkstate.ABI_VERSION + 1)),
            ("cpustate-size", lambda d: _put32(d, OFF_CPUSIZE, nkstate.CPUSTATE_SIZE + 4)),
            ("executable-identity", lambda d: d.__setitem__(OFF_EXE + 31, d[OFF_EXE + 31] ^ 1)),
            ("runtime-identity", lambda d: d.__setitem__(OFF_RUNTIME, ord("f"))),
            ("disc-id", lambda d: d.__setitem__(OFF_DISC + 10, ord("2"))),
            ("flags", lambda d: _put32(d, OFF_FLAGS, 3)),
            ("clock-not-deterministic", lambda d: _put32(d, OFF_FLAGS, 0)),
            ("too-many-segments", lambda d: _put32(d, OFF_COUNT, nkstate.MAX_SEGMENTS + 1)),
            ("bad-tag", lambda d: d.__setitem__(OFF_TABLE, 1)),
            ("duplicate-segment", lambda d: d.__setitem__(slice(OFF_TABLE + 48, OFF_TABLE + 52), b"SEGA")),
            ("unknown-segment", lambda d: d.__setitem__(slice(OFF_TABLE, OFF_TABLE + 4), b"SEGZ")),
            ("segment-version", lambda d: _put32(d, OFF_TABLE + 4, 9)),
            ("segment-hash", lambda d: d.__setitem__(OFF_TABLE + 96, d[OFF_TABLE + 96] ^ 1)),
            ("file-hash", lambda d: d.__setitem__(len(d) - 1, d[-1] ^ 1)),
        ]
        for reason, edit in cases:
            with self.subTest(reason=reason):
                data = bytearray(_good())
                edit(data)
                self.assertRefused(bytes(data), reason)

    def test_a_tampered_payload_is_caught_even_after_a_rehash(self) -> None:
        data = bytearray(_good())
        data[OFF_TABLE + 96] ^= 1
        _rehash(data)
        self.assertRefused(bytes(data), "segment-hash")

    def test_flags_edit_without_the_clock_requirement_is_the_file_hash(self) -> None:
        data = bytearray(_good())
        _put32(data, OFF_FLAGS, 0)
        self.assertRefused(bytes(data), "file-hash", require_deterministic_clock=False)

    def test_trailing_bytes_and_lengths(self) -> None:
        self.assertRefused(_good() + b"x", "trailing-bytes")
        data = bytearray(_good())
        _put32(data, OFF_TABLE + 8, 0x7FFFFFFF)
        self.assertRefused(bytes(data), "truncated")
        _put32(data, OFF_TABLE + 8, 0)
        _put32(data, OFF_TABLE + 12, 0x80000000)
        self.assertRefused(bytes(data), "truncated")

    def test_every_prefix_is_truncated(self) -> None:
        good = _good()
        for n in range(len(good)):
            with self.subTest(prefix=n):
                self.assertRefused(good[:n], "truncated")

    def test_segments_this_build_requires_or_cannot_load(self) -> None:
        self.assertRefused(_good(), "missing-segment", segment_versions={**VERSIONS, "SEGC": 1})
        self.assertRefused(_good(), "unknown-segment", segment_versions={"SEGA": 1})

    def test_info_mode_checks_structure_and_hashes_only(self) -> None:
        state = nkstate.parse(_good())
        self.assertEqual([s.tag for s in state.segments], ["SEGA", "SEGB"])
        data = bytearray(_good())
        data[OFF_TABLE + 96] ^= 1
        with self.assertRaises(nkstate.StateError) as ctx:
            nkstate.parse(bytes(data))
        self.assertEqual(ctx.exception.reason, "segment-hash")


class WriterTest(unittest.TestCase):
    def test_no_runtime_identity_writes_nothing(self) -> None:
        anon = nkstate.Identity(SELF.kind, SELF.executable, "", SELF.disc_id, SELF.flags)
        with self.assertRaises(nkstate.StateError) as ctx:
            nkstate.build(anon, SEGMENTS)
        self.assertEqual(ctx.exception.reason, "no-runtime-identity")

    def test_duplicate_and_bad_tags_are_refused(self) -> None:
        for segments, reason in (
            ([SEGMENTS[0], SEGMENTS[0]], "duplicate-segment"),
            ([nkstate.Segment("AB\x01C", 1, b"")], "bad-tag"),
        ):
            with self.subTest(reason=reason), self.assertRaises(nkstate.StateError) as ctx:
                nkstate.build(SELF, segments)
            self.assertEqual(ctx.exception.reason, reason)

    def test_info_command(self) -> None:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.nkstate"
            path.write_bytes(_good())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(nkstate.main(["info", str(path)]), 0)
            self.assertIn("segment 'SEGB' v2: 16 bytes", out.getvalue())
            path.write_bytes(_good()[:-1])
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(nkstate.main(["info", str(path)]), 2)
            self.assertTrue(out.getvalue().startswith("REFUSED truncated"))


#: The line tests/native/nkstate_selftest.c writes for frame 42 with one digest "sched" of b"abc".
PINNED_LINE = ("NKCKPT v=1 frame=42 n=1 "
               "sched=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n")


def _line(frame: int, digests: list[tuple[str, bytes]]) -> str:
    body = "".join(f" {name}={hashlib.sha256(data).hexdigest()}" for name, data in digests)
    return f"NKCKPT v=1 frame={frame} n={len(digests)}{body}\n"


class CheckpointTest(unittest.TestCase):
    def test_pinned_line_matches_c(self) -> None:
        self.assertIn('"NKCKPT v=1 frame=42 n=1 "', SELFTEST_C)
        self.assertIn('"sched=' + PINNED_LINE.split("sched=")[1].rstrip("\n") + '\\n"', SELFTEST_C)
        ckpt = nkstate.parse_checkpoint(PINNED_LINE)
        self.assertEqual(ckpt.frame, 42)
        self.assertEqual(ckpt.digests, (("sched", hashlib.sha256(b"abc").hexdigest()),))
        self.assertEqual(_line(42, [("sched", b"abc")]), PINNED_LINE)

    def test_malformed_lines_are_refused(self) -> None:
        for bad in ("NKCKPT v=2 frame=0 n=0", "NKCKPT v=1 frame=0 n=1", "NKCKPT v=1 frame=0 n=0 extra",
                    "NKCKPT v=1 frame=0 n=1 a b=00", "NKCKPT v=1 frame=x n=0",
                    "NKCKPT v=1 frame=0 n=1 " + "x" * 24 + "=" + "0" * 64):
            with self.subTest(line=bad), self.assertRaises(ValueError):
                nkstate.parse_checkpoint(bad)

    def _diff(self, a: str, b: str) -> tuple[int, str]:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            pa, pb = Path(tmp) / "a.log", Path(tmp) / "b.log"
            pa.write_text(a, encoding="utf-8")
            pb.write_text(b, encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = nkstate.main(["ckpt-diff", str(pa), str(pb)])
        return rc, out.getvalue().strip()

    def test_diff_names_the_first_divergent_frame_and_digest(self) -> None:
        run = [_line(f, [("cpu.00000001", b"r%d" % f), ("ram.08800000", b"m%d" % f)]) for f in range(5)]
        noise = "HLE: something else\n"
        self.assertEqual(self._diff(noise.join(run), "".join(run)), (0, "MATCH checkpoints=5"))
        other = list(run)
        other[3] = _line(3, [("cpu.00000001", b"r3"), ("ram.08800000", b"changed")])
        other[4] = _line(4, [("cpu.00000001", b"changed"), ("ram.08800000", b"changed")])
        self.assertEqual(self._diff("".join(run), "".join(other)),
                         (1, "MISMATCH frame=3 index=3 kind=digest digest=ram.08800000"))
        other = list(run)
        other[2] = _line(2, [("cpu.00000001", b"r2"), ("cpu.00000002", b"x"), ("ram.08800000", b"m2")])
        self.assertEqual(self._diff("".join(run), "".join(other)),
                         (1, "MISMATCH frame=2 index=2 kind=names digest=ram.08800000"))

    def test_runs_with_other_settings_are_not_compared(self) -> None:
        run = "".join(_line(f, [("sched", b"%d" % f)]) for f in range(3))
        det = "NKCKPT_RUN v=1 every=60 clock=deterministic\n"
        host = "NKCKPT_RUN v=1 every=60 clock=host\n"
        self.assertEqual(self._diff(det + run, det + run), (0, "MATCH checkpoints=3"))
        rc, out = self._diff(det + run, host + run)
        self.assertEqual(rc, 3)
        self.assertTrue(out.startswith("NOT_RUN different-settings"), out)

    def test_a_recording_compares_with_a_stream(self) -> None:
        run = "".join(_line(f, [("sched", b"%d" % f)]) for f in range(3))
        stream = "NKCKPT_RUN v=1 every=60 clock=deterministic\n"
        record = ("NKREC v=1 every=60 clock=deterministic exe=" + "ab" * 32 + " disc=- abi=3 cpustate=1000\n"
                  "NKANSW v=1 call=osk request=0 pending=2 result=cancel units=-\n")
        self.assertEqual(self._diff(stream + run, record + run + "NKREC_END frame=2\n"),
                         (0, "MATCH checkpoints=3"))
        rc, out = self._diff(stream.replace("every=60", "every=4") + run, record + run)
        self.assertEqual(rc, 3)
        self.assertTrue(out.startswith("NOT_RUN different-settings"), out)

    def test_a_short_run_is_not_run(self) -> None:
        run = [_line(f, [("sched", b"%d" % f)]) for f in range(4)]
        self.assertEqual(self._diff("".join(run), "".join(run[:2])), (3, "NOT_RUN ended-early a=4 b=2"))
        self.assertEqual(self._diff("", ""), (3, "NOT_RUN no-checkpoints a=0 b=0"))
        self.assertEqual(self._diff("NKCKPT v=1 frame=0 n=1\n", "")[0], 3)


if __name__ == "__main__":
    unittest.main()
