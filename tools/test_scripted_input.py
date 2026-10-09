#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Scripted controller input is delivered in guest time (src/rt/scripted_input.h).

The runtime's scripted input sources used to decide the pad state from the VCOUNT seen at
each serviced vblank. On a host that falls behind, one source latch services several
elapsed periods back to back at the batch's final VCOUNT, so a press whose window the batch
stepped over was never latched. A guest that reads only the latest sample, or had not started
polling yet, never saw it, and the showcase smoke "missed the scripted Cross input sample" on
a loaded host.

The C harness below is synthetic (generated into a temp dir, never tracked). It compiles the
exact header the runtime uses and plays pad-script rows against a model of the controller
ring: one sample latched per serviced vblank, guest reads that return the latest sample. It
then simulates a starved host, where VCOUNT jumps by many periods in one batch and the guest
reads only between batches. Every press must still reach a guest read, every release must be
read before the next press, no press may start before its row, and a press nobody reads
must become a named overdue failure instead of vanishing. hle_thread_selftest drives the
same mechanism end to end through the production scheduler and sceCtrlReadBufferPositive.

A segment may also state its width in guest reads (SrInputSegment.reads, the route grammar's
WIDTHS READS / READS PRESS). Those cases hand-build the segment list, mix the two kinds of
width, and check that a read-width press lasts exactly its reads on the same starved schedule
that a vblank width does not survive.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scripted_input.h"

static SrInputPlayer P;
static SrInputSegment L[64];
static int n, pos;
static uint32_t latest_mask, latest_id, v;

static void load(const uint32_t *f, const uint32_t *m, const uint32_t *w, int rows, uint32_t budget) {
    sr_input_reset(&P);
    n = sr_input_from_rows(f, m, w, rows, budget, L, 64);
    pos = 0;
}

/* A segment list written by hand, with widths in vblanks or in guest reads. */
static void load_list(const SrInputSegment *segs, int count) {
    sr_input_reset(&P);
    for (int i = 0; i < count; i++) L[i] = segs[i];
    n = count;
    pos = 0;
}

/* One serviced vblank: advance the script, then latch one sample of the result. */
static void tick(void) {
    int r = sr_input_list_tick(&P, L, n, &pos, v);
    if (r < 0) { printf("OVERDUE %u mask=%x since=%u\n", v, P.seg.mask, P.since); sr_input_idle(&P); }
    latest_mask = sr_input_mask(&P);
    latest_id = sr_input_latch(&P);
    printf("LATCH %u %x\n", v, latest_mask);
}

/* A source latch that discovers `periods` elapsed periods: VCOUNT jumps by all of them at
 * once and every owed episode is serviced back to back at that VCOUNT. */
static void batch(uint32_t periods) {
    v += periods;
    for (uint32_t i = 0; i < periods; i++) tick();
}

/* sceCtrlReadBufferPositive(&pad, 1): the guest is handed the latest sample. */
static void guest_read(void) {
    sr_input_read(&P, latest_id);
    printf("READ %u %x\n", v, latest_mask);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    /* CROSS twice with a four-sample release between, then START. */
    const uint32_t f[] = { 12u, 20u, 100u }, m[] = { 0x4000u, 0x4000u, 0x0008u }, w[] = { 4u, 4u, 8u };
    if (strcmp(mode, "steady") == 0) {            /* the host keeps up, the guest reads every vblank */
        load(f, m, w, 3, 1800u);
        for (int i = 0; i < 160; i++) { batch(1); guest_read(); }
    } else if (strcmp(mode, "starved") == 0) {    /* one batch steps over both CROSS rows */
        load(f, m, w, 3, 1800u);
        const uint32_t batches[] = { 1u, 30u, 1u, 1u, 1u, 1u, 90u, 1u, 1u, 80u, 1u };
        for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) { batch(batches[i]); guest_read(); }
    } else if (strcmp(mode, "late") == 0) {       /* the guest starts polling at vblank 55 */
        load(f, m, w, 3, 1800u);
        for (int i = 0; i < 160; i++) { batch(1); if (v >= 55u) guest_read(); }
    } else if (strcmp(mode, "overdue") == 0) {    /* the guest never reads the pad */
        load(f, m, w, 3, 30u);
        batch(1); guest_read();                   /* it read once, before any press */
        for (int i = 0; i < 60; i++) batch(1);
    } else if (strcmp(mode, "reset") == 0) {      /* a stale sample may not vouch for a new segment */
        load(f, m, w, 3, 1800u);
        batch(1);                                 /* latched with the leading gap's id */
        uint32_t stale = latest_id;
        load(f, m, w, 3, 1800u);
        batch(1);                                 /* the reloaded script's leading gap */
        printf("FRESH %d\n", latest_id > stale);
        sr_input_read(&P, stale);                 /* the guest is handed the stale sample */
        printf("STALE_VOUCHES %d\n", sr_input_was_read(&P));
    } else if (strcmp(mode, "rows") == 0) {       /* overlapping rows hold together */
        const uint32_t cf[] = { 10u, 20u }, cm[] = { 0x0010u, 0x4000u }, cw[] = { 50u, 4u };
        load(cf, cm, cw, 2, 77u);
        for (int i = 0; i < n; i++)
            printf("SEG %x %u %u %u\n", L[i].mask, L[i].samples, L[i].not_before, L[i].budget);
    } else if (strcmp(mode, "reads_starved") == 0) {  /* widths in guest reads, starved host */
        const SrInputSegment s[] = {
            { 0x4000u, 1u, 0u, 1800u, 3u },           /* CROSS until three guest reads saw it */
            { 0x0000u, 1u, 0u, 0u,    2u },           /* released until two guest reads saw it */
            { 0x4000u, 1u, 0u, 1800u, 1u },           /* CROSS until one guest read saw it */
        };
        const uint32_t batches[] = { 1u, 30u, 30u, 30u, 30u, 30u };
        load_list(s, 3);
        for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) { batch(batches[i]); guest_read(); }
    } else if (strcmp(mode, "vblank_starved") == 0) { /* the same schedule with vblank widths */
        const SrInputSegment s[] = {
            { 0x4000u, 4u, 0u, 1800u, 0u },
            { 0x0000u, 2u, 0u, 0u,    0u },
            { 0x4000u, 4u, 0u, 1800u, 0u },
        };
        const uint32_t batches[] = { 1u, 30u, 30u, 30u, 30u, 30u };
        load_list(s, 3);
        for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) { batch(batches[i]); guest_read(); }
    } else if (strcmp(mode, "reads_mixed") == 0) {    /* vblank and read widths in one script */
        const SrInputSegment s[] = {
            { 0x4000u, 2u, 0u, 1800u, 0u },           /* CROSS for two samples */
            { 0x0000u, 1u, 0u, 0u,    1u },           /* released for one guest read */
            { 0x4000u, 1u, 0u, 1800u, 2u },           /* CROSS for two guest reads */
            { 0x0000u, 3u, 0u, 0u,    0u },           /* released for three samples */
        };
        const uint32_t batches[] = { 1u, 30u, 30u, 30u, 30u, 30u };
        load_list(s, 4);
        for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) { batch(batches[i]); guest_read(); }
    } else if (strcmp(mode, "reads_overdue") == 0) {  /* one read of three, then the guest stops */
        const SrInputSegment s[] = { { 0x4000u, 1u, 0u, 30u, 3u } };
        load_list(s, 1);
        batch(1); guest_read();
        for (int i = 0; i < 60; i++) batch(1);
        printf("READS_SEEN %u\n", P.reads);
    } else if (strcmp(mode, "reads_stale") == 0) {   /* which reads count toward a read width */
        const SrInputSegment s[] = { { 0x4000u, 1u, 0u, 1800u, 1u } };
        load_list(s, 1);
        batch(1);
        uint32_t stale = latest_id;
        load_list(s, 1);                             /* a new segment, with a newer delivery id */
        batch(1);
        sr_input_read(&P, 0u);                       /* a read that hands over no sample */
        printf("NO_SAMPLE_READS %u DELIVERED %d\n", P.reads, sr_input_delivered(&P));
        sr_input_read(&P, stale);                    /* a sample from the previous segment */
        printf("STALE_READS %u DELIVERED %d\n", P.reads, sr_input_delivered(&P));
        batch(3);                                    /* three samples of this segment ... */
        sr_input_read(&P, latest_id);                /* ... handed over by one read */
        printf("MULTI_READS %u DELIVERED %d\n", P.reads, sr_input_delivered(&P));
    } else {
        return 2;
    }
    return 0;
}
"""

LINE = re.compile(r"^(LATCH|READ) (\d+) ([0-9a-f]+)$")


def presses_seen(reads: list[tuple[int, int]], mask: int) -> list[int]:
    """The vblanks at which the guest's reads show `mask` newly pressed (rising edges)."""
    edges, before = [], False
    for vblank, pad in reads:
        now = bool(pad & mask)
        if now and not before:
            edges.append(vblank)
        before = now
    return edges


@unittest.skipUnless(CC, "no C compiler on PATH")
class ScriptedInputDeliveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = Path(tempfile.mkdtemp(prefix="scripted_input_"))
        source = cls.tmp / "scripted_input_harness.c"
        source.write_text(HARNESS_C, encoding="utf-8")
        cls.exe = cls.tmp / "scripted_input_harness.exe"
        result = subprocess.run(
            [CC, "-std=c99", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'src' / 'rt'}",
             "-o", os.fspath(cls.exe), os.fspath(source)],
            capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise AssertionError("scripted input harness did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_mode(self, mode: str) -> str:
        result = subprocess.run([os.fspath(self.exe), mode], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    @staticmethod
    def records(output: str, kind: str) -> list[tuple[int, int]]:
        out = []
        for line in output.splitlines():
            match = LINE.match(line)
            if match and match.group(1) == kind:
                out.append((int(match.group(2)), int(match.group(3), 16)))
        return out

    def test_a_host_that_keeps_up_gets_the_authored_timing_exactly(self) -> None:
        latches = self.records(self.run_mode("steady"), "LATCH")
        authored = {v: (0x4000 if 12 <= v < 16 or 20 <= v < 24 else 0) | (0x0008 if 100 <= v < 108 else 0)
                    for v, _ in latches}
        self.assertEqual([pad for _, pad in latches], [authored[v] for v, _ in latches],
                         "guest-time delivery must equal the old absolute-vblank table when "
                         "the guest reads every vblank")

    def test_a_starved_host_still_delivers_every_press_and_release(self) -> None:
        output = self.run_mode("starved")
        reads = self.records(output, "READ")
        latches = self.records(output, "LATCH")
        # The second batch moved VCOUNT 1 -> 31: both CROSS rows ([12,16) and [20,24)) lie
        # inside it, so no sample is latched at a VCOUNT inside either row and the old
        # absolute-vblank table latched no CROSS at all. Here the guest reads two separate
        # presses with a release between them.
        self.assertEqual([v for v, _ in latches if 12 <= v < 16 or 20 <= v < 24], [],
                         "the scenario must step over both rows, as the starved host did")
        self.assertEqual(len(presses_seen(reads, 0x4000)), 2, reads)
        self.assertEqual(len(presses_seen(reads, 0x0008)), 1, reads)
        cross_reads = [pad & 0x4000 for _, pad in reads]
        first = cross_reads.index(0x4000)
        self.assertIn(0, cross_reads[first:], "the release between the presses is read")
        for vblank, pad in latches:
            if pad & 0x4000:
                self.assertGreaterEqual(vblank, 12, "no press is latched before its row")
            if pad & 0x0008:
                self.assertGreaterEqual(vblank, 100, "no press is latched before its row")
        self.assertNotIn("OVERDUE", output)

    def test_each_press_lasts_at_least_its_authored_samples(self) -> None:
        latches = self.records(self.run_mode("starved"), "LATCH")
        runs, current = [], 0
        for _, pad in latches:
            if pad & 0x4000:
                current += 1
            elif current:
                runs.append(current)
                current = 0
        self.assertEqual(len(runs), 2)
        self.assertTrue(all(run >= 4 for run in runs), runs)

    def test_a_guest_that_boots_late_still_sees_the_first_press(self) -> None:
        output = self.run_mode("late")
        reads = self.records(output, "READ")
        self.assertEqual(reads[0], (55, 0), "its first read shows the pad released")
        self.assertEqual(len(presses_seen(reads, 0x4000)), 2)
        self.assertEqual(len(presses_seen(reads, 0x0008)), 1)

    def test_a_press_nobody_reads_becomes_a_named_overdue_failure(self) -> None:
        output = self.run_mode("overdue")
        overdue = re.findall(r"^OVERDUE (\d+) mask=([0-9a-f]+) since=(\d+)$", output, re.M)
        self.assertEqual(len(overdue), 1, output)
        vblank, mask, since = int(overdue[0][0]), int(overdue[0][1], 16), int(overdue[0][2])
        self.assertEqual(mask, 0x4000)
        self.assertEqual(since, 12)
        self.assertEqual(vblank - since, 30, "the run fails exactly when the read budget runs out")

    def test_a_stale_sample_cannot_vouch_for_a_new_segment(self) -> None:
        output = self.run_mode("reset")
        self.assertIn("FRESH 1", output)
        self.assertIn("STALE_VOUCHES 0", output)

    def test_overlapping_rows_are_held_together_and_the_leading_gap_is_one_sample(self) -> None:
        segs = [tuple(int(x, 16) if i == 0 else int(x) for i, x in enumerate(line.split()[1:]))
                for line in self.run_mode("rows").splitlines() if line.startswith("SEG ")]
        self.assertEqual(segs, [
            (0x0000, 1, 0, 0),         # the guest must see the pad released first
            (0x0010, 10, 10, 77),      # [10, 20)
            (0x4010, 4, 20, 77),       # [20, 24): the overlap holds both buttons
            (0x0010, 36, 24, 77),      # [24, 60)
        ])

    def test_a_guest_read_width_holds_for_exactly_that_many_reads_on_a_starved_host(self) -> None:
        # Each guest read follows a batch of several vblanks. CROSS is read three times, then
        # released for two reads, then CROSS once: the reads count, not the vblanks.
        reads = self.records(self.run_mode("reads_starved"), "READ")
        self.assertEqual([pad for _, pad in reads], [0x4000, 0x4000, 0x4000, 0, 0, 0x4000], reads)

    def test_the_same_starved_schedule_in_vblank_widths_falls_short(self) -> None:
        # The control for the test above: a four-vblank press is latched inside one batch and
        # handed to a single read, so the guest sees one CROSS read where a read width gives three.
        reads = self.records(self.run_mode("vblank_starved"), "READ")
        self.assertEqual([pad for _, pad in reads], [0x4000, 0, 0x4000, 0, 0, 0], reads)

    def test_vblank_and_read_widths_mix_in_one_script(self) -> None:
        reads = self.records(self.run_mode("reads_mixed"), "READ")
        self.assertEqual([pad for _, pad in reads], [0x4000, 0, 0x4000, 0x4000, 0, 0], reads)

    def test_a_read_width_the_guest_never_completes_is_a_named_overdue_failure(self) -> None:
        output = self.run_mode("reads_overdue")
        overdue = re.findall(r"^OVERDUE (\d+) mask=([0-9a-f]+) since=(\d+)$", output, re.M)
        self.assertEqual(len(overdue), 1, output)
        vblank, mask, since = int(overdue[0][0]), int(overdue[0][1], 16), int(overdue[0][2])
        self.assertEqual(mask, 0x4000)
        self.assertEqual(since, 1)
        self.assertEqual(vblank - since, 30, "one read of three, then the budget runs out")
        self.assertIn("READS_SEEN 1", output.splitlines())

    def test_only_a_read_that_hands_over_the_segment_counts_toward_its_width(self) -> None:
        output = self.run_mode("reads_stale")
        self.assertIn("NO_SAMPLE_READS 0 DELIVERED 0", output, "a read with no sample of its own")
        self.assertIn("STALE_READS 0 DELIVERED 0", output, "a sample from the previous segment")
        self.assertIn("MULTI_READS 1 DELIVERED 1", output, "three samples handed over by one read")


if __name__ == "__main__":
    unittest.main()
