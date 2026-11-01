# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static contracts and small independent arithmetic fixtures for issues #170, #283, and #302.

The common PS demux is portable and the Media Foundation decoder backend is Windows-only.
These tests pin both production boundaries and exercise overflow arithmetic independently;
native compilation and private title acceptance remain separate evidence labels.
"""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

MPEG = (ROOT / "src" / "rt" / "mpeg.c").read_text(encoding="utf-8")
H264 = (ROOT / "src" / "rt" / "h264_mf.c").read_text(encoding="utf-8")
PSMF_MEDIA = (ROOT / "tests" / "native" / "psmf_media_selftest.c").read_text(encoding="utf-8")
PSMF_PRODUCER = (ROOT / "src" / "rt" / "psmf_producer.c").read_text(encoding="utf-8")


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
        self.assertIn("sr_guest_span_readable_current(dst, gotBytes)", put)
        self.assertIn("sr_h264_feed(ctx->h264, (const uint8_t *)SR_HOST(dst), gotBytes)", put)
        self.assertNotIn("got * packetSize);", put)

    def test_construct_rejects_packet_and_data_extent_overflow(self) -> None:
        construct = MPEG[MPEG.index("uint32_t mpeg_ringbuffer_construct") : MPEG.index("uint32_t mpeg_create")]
        self.assertIn("!u32_mul_checked(numPackets, MPEG_PACKET_SIZE", construct)
        self.assertIn("!u32_add_checked(data, dataBytes", construct)
        self.assertIn("requiredBytes > size", construct)
        self.assertNotIn("data + numPackets * 2048u", construct)

    def test_common_demux_and_decoder_accumulators_have_caps_and_checked_growth(self) -> None:
        for cap in ("H264_MAX_ES_BYTES", "H264_MAX_ES_CHUNKS", "H264_MAX_OUTPUT_BYTES"):
            self.assertIn(cap, H264)
        self.assertIn("SR_PSMF_MAX_PS_BUFFER_BYTES", PSMF_PRODUCER)
        self.assertIn("p->failed = 1", PSMF_PRODUCER)
        self.assertIn("au->size > H264_MAX_ES_BYTES - d->esLen", H264)
        self.assertIn("len > SR_PSMF_MAX_PS_BUFFER_BYTES - p->ps_input_len", PSMF_PRODUCER)
        self.assertIn("c->off > d->esLen || c->len > d->esLen - c->off", H264)

    def test_decoder_timestamp_presence_is_not_inferred_from_pts_value(self) -> None:
        self.assertIn("int has_pts;", H264)
        self.assertIn("c->has_pts ? (LONGLONG)c->pts * 1000 / 9 : d->fakeTime", H264)

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
        # #1049: the destination may sit behind a swizzled VRAM mirror, so every row is
        # validated with the any-view predicate and written through the piecewise helper.
        self.assertIn("sr_guest_span_writable_any_view((uint32_t)rowAddr, (uint32_t)rowBytes64)", convert)
        self.assertIn("sr_guest_copy_out(buffer + (uint32_t)y * pitchBytes, row, rowBytes)", convert)
        self.assertNotIn("SR_HOST(", convert)
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
            "!sr_guest_span_writable_any_view((uint32_t)rowAddr, (uint32_t)rowBytes64)) return 0;",
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
    """The YCbCr buffer is guest data in the layout measured on a PSP (#302, PSP-MPEG-001)."""

    BLOCK = MPEG[MPEG.index("/* ---- YCbCr decode path") : MPEG.index("/* LPCM ES")]

    def test_ycbcr_geometry_is_guest_supplied_or_a_measured_limit(self) -> None:
        block = self.BLOCK
        self.assertNotIn("frameWidth ? 512", MPEG)
        self.assertNotIn("frameWidth ? (uint32_t)ctx->defaultFrameWidth : 512u", MPEG)
        # The only fixed picture sizes are the measured mode-1 limits.
        self.assertEqual(MPEG.count("272u"), 1)
        self.assertIn("#define YCBCR_MODE1_MAX_H 272u", block)
        self.assertIn("#define YCBCR_MODE1_MAX_W 480u", block)
        # A picture's geometry is read from the guest header's words 0 and 1.
        header = block[block.index("static int ycbcr_header_read") : block.index("static uint32_t ycbcr_plane_bytes")]
        self.assertIn("l->height = rows * 16u;", header)
        self.assertIn("l->width = cols * 16u;", header)
        self.assertIn("l->plane[i] = MEM_R32(buf + 16u + 4u * i);", header)

    def test_ycbcr_has_no_hidden_picture(self) -> None:
        block = self.BLOCK
        for hidden in ("rgba", "ycbcr_fingerprint", "guest_tag", "ycbcr_state_usable"):
            self.assertNotIn(hidden, block)
        init = block[block.index("uint32_t mpeg_avc_init_ycbcr") : block.index("uint32_t mpeg_avc_decode_mode")]
        self.assertNotIn("memset(SR_HOST", init)
        decode = block[block.index("uint32_t mpeg_avc_decode_ycbcr") : block.index("uint32_t mpeg_avc_decode_stop_ycbcr")]
        self.assertIn("SR_H264_TARGET_HOST_YUV", decode)
        self.assertIn("ycbcr_store_picture(mpegAddr, buf, &layout, info.width, info.height, &bytes)", decode)
        self.assertIn("ycbcr_picture_set(mpegAddr, bytes, au_read_pts(auAddr));", decode)
        self.assertIn("MEM_W32(initAddr, 1u);", decode)
        store = block[block.index("static uint32_t ycbcr_store_picture") : block.index("uint32_t mpeg_avc_decode_ycbcr")]
        self.assertIn("MEM_W32(buf, h / 16u);", store)
        self.assertIn("MEM_W32(buf + 4u, w / 16u);", store)
        csc = block[block.index("uint32_t mpeg_avc_csc") :]
        self.assertIn("ycbcr_header_read(buf, &l)", csc)
        self.assertIn("ycbcr_planes_in(&l, l.width, l.height)", csc)
        self.assertIn("sr_h264_psp_rgb(", csc)
        for test in ("test_mpeg_ycbcr_measured_contract", "test_mpeg_ycbcr_guest_decode",
                     "CSC reads the planes through the header: exchanging P0 and P2 exchanges what it reads",
                     "every decoded sample sits where the measured strip layout puts it",
                     "sr_syscall(&cpu, nid)"):
            self.assertIn(test, PSMF_MEDIA)

    def test_ycbcr_operations_preflight_ranges_and_modes(self) -> None:
        block = self.BLOCK
        for contract in (
            "sr_guest_rect_geometry(dest, 0u, 0u, frameWidth",
            "sr_guest_span_writable_any_view(span.first, span.total_bytes)",
            "sr_guest_copy_out(rowAddr, out, span.row_bytes)",
            "rangeAddr & 3u",
            "frameWidth == 0",
            "ctx->pixelMode < 0 || ctx->pixelMode > 3",
            "if ((x & 1) || (w & 1)) return SCE_MPEG_ERROR_INVALID_VALUE;",
            "x + w > (int64_t)l.width || y + h > (int64_t)l.height",
            "if ((uint64_t)w > frameWidth) return MPEG_ERROR_CSC_REFUSED;",
            "#define MPEG_ERROR_CSC_REFUSED 0x800001FEu",
            "ycbcr_planes_writable(l, w, h)",
        ):
            self.assertIn(contract, block)

    def test_the_psp_conversion_is_shared_by_both_backends(self) -> None:
        header = (ROOT / "src" / "rt" / "sr_h264.h").read_text(encoding="utf-8")
        ffmpeg = (ROOT / "src" / "rt" / "h264_ffmpeg.c").read_text(encoding="utf-8")
        for constant in ("149 *", "204 * dv", "50 * du", "104 * dv", "258 * du", "+ 64) >> 7"):
            self.assertIn(constant, header)
        for backend in (H264, ffmpeg):
            self.assertIn("sr_h264_psp_rgb(", backend)
            self.assertNotIn("298 *", backend)
            self.assertIn("SR_H264_TARGET_HOST_YUV", backend)

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
        self.assertIn("MEASURED mode-1 strip layout",
                      meta.HANDLER_METADATA["h_MpegAvcDecodeYCbCr"]["limitation"])
        self.assertIn("MEASURED PSP matrix",
                      meta.HANDLER_METADATA["h_MpegAvcCsc"]["limitation"])


if __name__ == "__main__":
    unittest.main()
