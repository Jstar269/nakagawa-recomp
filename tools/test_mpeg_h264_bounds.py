# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Static contracts and small independent arithmetic fixtures for issue #170.

The MPEG ring code is part of the native runtime and the Media Foundation backend is Windows-
only.  These tests therefore keep the production source shape fail-closed and exercise the
overflow model independently; native compilation and the private HST route remain separate
evidence labels.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent
MPEG = (ROOT / "src" / "rt" / "mpeg.c").read_text(encoding="utf-8")
H264 = (ROOT / "src" / "rt" / "h264_mf.c").read_text(encoding="utf-8")


def checked_mul_u32(a: int, b: int) -> int | None:
    value = a * b
    return value if 0 <= value <= 0xFFFFFFFF else None


def checked_add_u32(a: int, b: int) -> int | None:
    value = a + b
    return value if 0 <= value <= 0xFFFFFFFF else None


class TestMpegH264Bounds(unittest.TestCase):
    def test_ring_snapshot_validates_metadata_before_callback(self) -> None:
        self.assertIn("#define RB_BYTES", MPEG)
        self.assertIn("u32_mul_checked", MPEG)
        self.assertIn("u32_add_checked", MPEG)
        put = MPEG[MPEG.index("uint32_t mpeg_ringbuffer_put") : MPEG.index("static Mpeg *au_stream")]
        self.assertLess(put.index("rb_read_valid"), put.index("call_guest3"))
        self.assertIn("r.avail > r.packets", MPEG)
        self.assertIn("r.writePos >= r.packets", MPEG)
        self.assertIn("r.packetSize != MPEG_PACKET_SIZE", MPEG)
        self.assertIn("expectedUpper != r.dataUpper", MPEG)
        self.assertIn("sr_guest_span_writable(r.data, dataBytes)", MPEG)

    def test_ring_feed_span_is_preflighted_before_h264(self) -> None:
        put = MPEG[MPEG.index("uint32_t mpeg_ringbuffer_put") : MPEG.index("static Mpeg *au_stream")]
        self.assertIn("sr_guest_span_writable(dst, chunkBytes)", put)
        self.assertIn("sr_guest_span_readable(dst, gotBytes)", put)
        self.assertIn("sr_h264_feed(ctx->h264, (const uint8_t *)SR_HOST(dst), gotBytes)", put)
        self.assertNotIn("got * packetSize);", put)

    def test_construct_rejects_packet_and_data_extent_overflow(self) -> None:
        construct = MPEG[MPEG.index("uint32_t mpeg_ringbuffer_construct") : MPEG.index("uint32_t mpeg_create")]
        self.assertIn("!u32_mul_checked(numPackets, MPEG_PACKET_SIZE", construct)
        self.assertIn("!u32_add_checked(data, dataBytes", construct)
        self.assertIn("requiredBytes > size", construct)
        self.assertNotIn("data + numPackets * 2048u", construct)

    def test_h264_accumulators_have_caps_and_checked_growth(self) -> None:
        for cap in ("H264_MAX_PS_BYTES", "H264_MAX_ES_BYTES", "H264_MAX_ES_CHUNKS", "H264_MAX_OUTPUT_BYTES"):
            self.assertIn(cap, H264)
        self.assertIn("d->failed = 1", H264)
        self.assertIn("n > H264_MAX_ES_BYTES - d->esLen", H264)
        self.assertIn("len > H264_MAX_PS_BYTES - d->psLen", H264)
        self.assertIn("c->off > d->esLen || c->len > d->esLen - c->off", H264)

    def test_h264_conversion_preflights_source_and_all_destination_rows(self) -> None:
        convert = H264[H264.index("static void convert_frame") : H264.index("/* Try to pull one decoded frame")]
        self.assertIn("uint64_t srcNeed", convert)
        self.assertIn("srcNeed > srcLen", convert)
        self.assertIn("uint64_t rowAddr", convert)
        self.assertIn("sr_guest_span_writable((uint32_t)rowAddr, (uint32_t)rowBytes64)", convert)
        self.assertIn("uint64_t fullBytes", convert)
        self.assertNotIn("stride * h + stride * h / 2", convert)


class TestMpegArithmeticFixtures(unittest.TestCase):
    def test_packet_count_and_data_end_must_fit_u32(self) -> None:
        self.assertEqual(checked_mul_u32(1024, 2048), 2 * 1024 * 1024)
        self.assertIsNone(checked_mul_u32(0xFFFFFFFF, 2048))
        self.assertEqual(checked_add_u32(0x08000000, 2 * 1024 * 1024), 0x08200000)
        self.assertIsNone(checked_add_u32(0xFFFFFFF0, 0x40))

    def test_ring_put_capacity_subtraction_avoids_add_wrap(self) -> None:
        total, avail = 0x100, 0xF0
        requested = min(0xFFFFFFFF, total - avail)
        self.assertEqual(requested, 0x10)
        self.assertLessEqual(avail + requested, total)


if __name__ == "__main__":
    unittest.main()
