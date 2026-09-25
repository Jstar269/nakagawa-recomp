# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Static contracts and small independent arithmetic fixtures for issues #170 and #302.

The MPEG ring code is part of the native runtime and the Media Foundation backend is Windows-
only.  These tests therefore keep the production source shape fail-closed and exercise the
overflow model independently; native compilation and the private HST route remain separate
evidence labels.
"""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

MPEG = (ROOT / "src" / "rt" / "mpeg.c").read_text(encoding="utf-8")
H264 = (ROOT / "src" / "rt" / "h264_mf.c").read_text(encoding="utf-8")
PSMF_MEDIA = (ROOT / "src" / "rt" / "psmf_media_selftest.c").read_text(encoding="utf-8")


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

    def test_atrac_au_uses_the_psmf_presentation_origin(self) -> None:
        """The public sceMpeg clock is not the raw private-PES diagnostic clock.

        PPSSPP's public sceMpeg model defines audioFirstTimestamp as 90000.  The
        raw first private-stream PTS is useful for demux diagnostics, but using it
        as the AU origin makes A/V sync title-dependent and can make a player
        reject the first video AU as too early.
        """
        atrac = MPEG[MPEG.index("uint32_t mpeg_get_atrac_au") : MPEG.index("static uint32_t video_buffer_bytes")]
        self.assertIn("int64_t pts = ctx->audioPts + ctx->firstTimestamp;", atrac)
        self.assertNotIn("sr_h264_first_audio_pts", atrac)

    def test_h264_conversion_preflights_source_and_all_destination_rows(self) -> None:
        convert = H264[H264.index("static int convert_frame") : H264.index("/* Try to pull one decoded frame")]
        self.assertIn("uint64_t srcNeed", convert)
        self.assertIn("srcNeed > srcLen", convert)
        self.assertIn("uint64_t rowAddr", convert)
        self.assertIn("sr_guest_span_writable((uint32_t)rowAddr, (uint32_t)rowBytes64)", convert)
        self.assertIn("uint64_t fullBytes", convert)
        self.assertNotIn("stride * h + stride * h / 2", convert)

    def test_h264_conversion_reports_delivery_rather_than_production(self) -> None:
        """A converted picture is only claimed when it was actually written.

        The converter returns 1 for a delivered picture and 0 for every refusal
        (malformed NV12 extent, unusable destination row, out-of-range geometry),
        and the pump distinguishes "produced but not delivered" (2) from a real
        decoder failure -- so an unwritable destination can never be reported as a
        successful frame, and it must not poison the decoder either.
        """
        convert = H264[H264.index("static int convert_frame") : H264.index("/* Try to pull one decoded frame")]
        # Every refusal class returns 0 explicitly, and the only success return is
        # the one after the whole picture has been converted and marked dirty.
        for refusal in (
            "if (!buffer || !src || frameWidth <= 0 || pixelMode < 0 || pixelMode > 3) return 0;",
            "if (stride <= 0 || w <= 0 || h <= 0 || w > stride) return 0;",
            "if (srcNeed < yBytes || srcNeed > srcLen) return 0;",
            "!sr_guest_span_writable((uint32_t)rowAddr, (uint32_t)rowBytes64)) return 0;",
        ):
            self.assertIn(refusal, convert)
        self.assertEqual(convert.count("return 1;"), 1)
        pump = H264[H264.index("static int pump_out") : H264.index("static int pull_frame")]
        self.assertIn("return delivered ? 1 : 2;", pump)
        pull = H264[H264.index("static int pull_frame") : H264.index("int sr_h264_frame(")]
        self.assertIn("if (r > 0) return r == 1 ? 1 : -1;", pull)
        self.assertIn("if (r < 0) { d->failed = 1; return -1; }", pull)


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


class TestMpegYcbcrContracts(unittest.TestCase):
    def test_ycbcr_geometry_is_guest_or_stream_supplied(self) -> None:
        block = MPEG[MPEG.index("/* ---- YCbCr decode path") : MPEG.index("/* LPCM ES")]
        self.assertNotIn("#define YCBCR_W", block)
        self.assertNotIn("#define YCBCR_H\n", block)
        self.assertNotIn("frameWidth ? 512", MPEG)
        self.assertNotIn("frameWidth ? (uint32_t)ctx->defaultFrameWidth : 512u", MPEG)
        self.assertNotIn("272u", MPEG)
        self.assertIn("streamHeight", MPEG)
        self.assertIn("b->width", block)
        self.assertIn("b->height", block)

    def test_ycbcr_guest_state_is_checked_before_hidden_picture_use(self) -> None:
        block = MPEG[MPEG.index("/* ---- YCbCr decode path") : MPEG.index("/* LPCM ES")]
        for contract in (
            "ycbcr_fingerprint",
            "guest_tag",
            "ycbcr_state_usable",
            "mpeg_contract_error",
            "sr_guest_span_writable(addr, size)",
            "if (!from || !to ||",
            "ycbcr_state_usable(b, 1)",
        ):
            self.assertIn(contract, block)
        guard = block[block.index("static int ycbcr_state_usable") : block.index("static YcbcrBuf *ycbcr_slot_for_init")]
        self.assertIn("ycbcr_fingerprint(b->buf, b->size) != b->guest_tag", guard)
        self.assertIn("modified guest YCbCr state", block)
        self.assertIn("test_mpeg_ycbcr_guest_contract", PSMF_MEDIA)
        self.assertIn("guest mutation cannot reuse hidden RGBA state", PSMF_MEDIA)

    def test_ycbcr_operations_preflight_ranges_and_modes(self) -> None:
        block = MPEG[MPEG.index("/* ---- YCbCr decode path") : MPEG.index("/* LPCM ES")]
        for contract in (
            "sr_guest_rect_writable",
            "rangeAddr & 3u",
            "frameWidth == 0",
            "ctx->pixelMode < 0 || ctx->pixelMode > 3",
            "(uint32_t)w > b->width",
            "(uint32_t)h > b->height",
        ):
            self.assertIn(contract, block)

    def test_mpeg_nid_statuses_are_explicit(self) -> None:
        import hle_registry_meta as meta

        expected = {
            "h_MpegAvcQueryYCbCrSize": "partial",
            "h_MpegAvcInitYCbCr": "partial",
            "h_MpegAvcDecodeYCbCr": "partial",
            "h_MpegAvcDecodeStopYCbCr": "partial",
            "h_MpegAvcCopyYCbCr": "partial",
            "h_MpegAvcCsc": "partial",
            "h_MpegQueryPcmEsSize": "partial",
            "h_MpegGetPcmAu": "controlled_unsupported",
            "h_MpegChangeGetAuMode": "partial",
        }
        self.assertEqual({k: meta.HANDLER_STATUS.get(k) for k in expected}, expected)


if __name__ == "__main__":
    unittest.main()
