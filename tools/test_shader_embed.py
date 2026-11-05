# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for deterministic checked-in Vulkan shader provenance."""

from __future__ import annotations

from decimal import Decimal, localcontext
from fractions import Fraction
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parent))

import shader_embed
from test_ge_nonfinite_vertex import CC, GE_HARNESS_PRELUDE_C, compile_ge_harness  # noqa: E402


class ShaderEmbedTests(unittest.TestCase):
    def test_checked_in_sources_and_embeddings_match_manifest(self) -> None:
        self.assertEqual(shader_embed.verify(shader_embed.GPU_DIR, shader_embed.MANIFEST), [])
        manifest = json.loads(shader_embed.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["compiler"]["target_environment"], "vulkan1.1")
        self.assertTrue(manifest["compiler"]["version"])

    def test_source_or_embedding_change_is_rejected_without_glslc(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nakagawa-shader-test-") as temp_name:
            gpu_dir = Path(temp_name)
            (gpu_dir / "shaders").mkdir()
            shutil.copy2(shader_embed.MANIFEST, gpu_dir / "shader_manifest.json")
            for _, source, embedded in shader_embed.SHADERS:
                shutil.copy2(shader_embed.GPU_DIR / source, gpu_dir / source)
                shutil.copy2(shader_embed.GPU_DIR / embedded, gpu_dir / embedded)

            source = gpu_dir / "shaders" / "psp.frag"
            source.write_text(source.read_text(encoding="utf-8") + "\n// stale test\n")
            errors = shader_embed.verify(gpu_dir, gpu_dir / "shader_manifest.json")
            self.assertTrue(any("source SHA-256 mismatch" in error for error in errors), errors)

            shutil.copy2(shader_embed.GPU_DIR / "shaders" / "psp.frag", source)
            embedded = gpu_dir / "psp_vert.inc"
            embedded.write_text(embedded.read_text(encoding="ascii").replace("0x", "0X", 1))
            errors = shader_embed.verify(gpu_dir, gpu_dir / "shader_manifest.json")
            self.assertTrue(errors)

    def test_embedding_format_round_trips_spirv_bytes(self) -> None:
        data = bytes.fromhex("0302230700000100")
        self.assertEqual(shader_embed.parse_embedding(shader_embed.format_embedding(data)), data)

    def test_ui_shader_sources_and_embeddings_match_manifest(self) -> None:
        self.assertEqual(
            shader_embed.verify(
                shader_embed.UI_DIR, shader_embed.UI_MANIFEST,
                shaders=shader_embed.UI_SHADERS,
            ),
            [],
        )
        manifest = json.loads(shader_embed.UI_MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["compiler"]["target_environment"], "vulkan1.1")
        self.assertTrue(manifest["compiler"]["version"])

    def test_ui_shader_embeddings_recompile_with_pinned_tool(self) -> None:
        glslc = shutil.which("glslc")
        if not glslc:
            self.skipTest("glslc is unavailable")
        self.assertEqual(
            shader_embed.verify(
                shader_embed.UI_DIR, shader_embed.UI_MANIFEST, glslc,
                shader_embed.UI_SHADERS,
            ),
            [],
        )

    def test_ui_shader_set_rejects_stale_source_or_embedding(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nakagawa-ui-shader-test-") as temp_name:
            shader_dir = Path(temp_name)
            (shader_dir / "shaders").mkdir()
            shutil.copy2(shader_embed.UI_MANIFEST, shader_dir / "shaders" / "shader_manifest.json")
            for _, source, embedded in shader_embed.UI_SHADERS:
                shutil.copy2(shader_embed.UI_DIR / source, shader_dir / source)
                shutil.copy2(shader_embed.UI_DIR / embedded, shader_dir / embedded)

            source = shader_dir / "shaders" / "waves.frag"
            source.write_text(source.read_text(encoding="utf-8") + "\n// stale test\n", encoding="utf-8")
            errors = shader_embed.verify(
                shader_dir, shader_dir / "shaders" / "shader_manifest.json",
                shaders=shader_embed.UI_SHADERS,
            )
            self.assertTrue(any("source SHA-256 mismatch" in error for error in errors), errors)

            shutil.copy2(shader_embed.UI_DIR / "shaders" / "waves.frag", source)
            embedded = shader_dir / "shaders" / "ui_vert.inc"
            embedded.write_text(embedded.read_text(encoding="ascii").replace("0x", "0X", 1), encoding="ascii")
            errors = shader_embed.verify(
                shader_dir, shader_dir / "shaders" / "shader_manifest.json",
                shaders=shader_embed.UI_SHADERS,
            )
            self.assertTrue(errors)


# ---- schema 2: the FSR 3 set, compiled by the pinned DXC (third_party/fidelityfx_sdk) ------------

FSR3_MANIFEST_REL = shader_embed.FSR3_MANIFEST.relative_to(shader_embed.ROOT)


def _fsr3_copy(root: Path) -> Path:
    """The FSR 3 manifest and every file it names, under a scratch root; the manifest's new path."""
    manifest = json.loads(shader_embed.FSR3_MANIFEST.read_text(encoding="utf-8"))
    paths = {FSR3_MANIFEST_REL.as_posix()}
    for entry in manifest["shaders"]:
        paths |= {entry["source"], entry["embedded"], *(inc["path"] for inc in entry["includes"])}
    for rel in paths:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(shader_embed.ROOT / rel, root / rel)
    return root / FSR3_MANIFEST_REL


class Fsr3ShaderSetTests(unittest.TestCase):
    def test_checked_in_set_matches_its_manifest(self) -> None:
        errors, skipped = shader_embed.verify_dxc(shader_embed.ROOT, shader_embed.FSR3_MANIFEST)
        self.assertEqual(errors, [])
        self.assertIsNone(skipped)
        manifest = json.loads(shader_embed.FSR3_MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual(manifest["compiler"]["release"], shader_embed.DXC_RELEASE)
        self.assertTrue(shader_embed.dxc_is_pinned(manifest["compiler"]["version"]))
        for entry in manifest["shaders"]:
            self.assertTrue(entry["includes"], entry["source"])
            for inc in entry["includes"]:
                self.assertTrue(inc["path"].startswith("third_party/fidelityfx_sdk/"), inc["path"])

    def test_a_changed_include_source_or_embedding_is_rejected(self) -> None:
        manifest = json.loads(shader_embed.FSR3_MANIFEST.read_text(encoding="utf-8"))
        first = manifest["shaders"][0]
        cases = (
            (first["includes"][0]["path"], "include", f"include {first['includes'][0]['path']} SHA-256 mismatch"),
            (first["source"], "source", "source SHA-256 mismatch"),
            (first["embedded"], "embedded", "embedded SHA-256 mismatch"),
        )
        for rel, what, expect in cases:
            with self.subTest(changed=what), tempfile.TemporaryDirectory(prefix="nakagawa-fsr3-test-") as temp:
                root = Path(temp)
                manifest_path = _fsr3_copy(root)
                target = root / rel
                if what == "embedded":  # one word changed, still a valid embedding
                    text = target.read_text(encoding="ascii")
                    self.assertIn("0x07230203", text)
                    target.write_text(text.replace("0x07230203", "0x07230202", 1), encoding="ascii")
                else:
                    target.write_bytes(target.read_bytes() + b"\n// changed\n")
                errors, _ = shader_embed.verify_dxc(root, manifest_path)
                self.assertTrue(any(expect in error for error in errors), errors)

    def test_a_changed_build_contract_is_rejected(self) -> None:
        for key, value in (("defines", ["FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0"]), ("options", ["-O0"]),
                           ("release", {"tag": "v1.8.2505"})):
            with self.subTest(field=key), tempfile.TemporaryDirectory(prefix="nakagawa-fsr3-test-") as temp:
                manifest_path = _fsr3_copy(Path(temp))
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest["compiler"][key] = value
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                errors, _ = shader_embed.verify_dxc(Path(temp), manifest_path)
                self.assertIn(f"compiler {key} does not match the build contract", errors)

    def test_a_missing_include_closure_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nakagawa-fsr3-test-") as temp:
            manifest_path = _fsr3_copy(Path(temp))
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["shaders"][0]["includes"] = []
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            errors, _ = shader_embed.verify_dxc(Path(temp), manifest_path)
            self.assertTrue(any("no include closure recorded" in error for error in errors), errors)

    def test_only_the_pinned_dxc_regenerates_or_reproduces(self) -> None:
        self.assertTrue(shader_embed.dxc_is_pinned("dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.2609.5) - 1.9.2609.5 (01b62ad47)"))
        self.assertTrue(shader_embed.dxc_is_pinned("libdxcompiler.so: 1.9(5512-01b62ad4)(1.9.2609.5)"))
        self.assertFalse(shader_embed.dxc_is_pinned("dxcompiler.dll: 1.8 - 1.8.2505.1 (b9b7f4e3c)"))
        self.assertFalse(shader_embed.dxc_is_pinned("libdxcompiler.so: 1.9(5512-01b62ad4)(1.9.2610.1)"))
        self.assertFalse(shader_embed.dxc_is_pinned("libdxcompiler.so: 1.9(5512-0badc0de)(1.9.2609.5)"))
        real = shader_embed.dxc_version
        shader_embed.dxc_version = lambda dxc: "dxcompiler.dll: 1.8 - 1.8.2505.1 (b9b7f4e3c)"
        try:
            with tempfile.TemporaryDirectory(prefix="nakagawa-fsr3-test-") as temp:
                manifest_path = _fsr3_copy(Path(temp))
                with self.assertRaises(ValueError):
                    shader_embed.regenerate_dxc(Path(temp), manifest_path, "dxc")
                errors, skipped = shader_embed.verify_dxc(Path(temp), manifest_path, "dxc")
                self.assertEqual(errors, [])
                self.assertIn("not the pinned", skipped or "")
        finally:
            shader_embed.dxc_version = real

    def test_embeddings_recompile_with_the_pinned_dxc(self) -> None:
        dxc = os.environ.get("NK_DXC") or shutil.which("dxc")
        if not dxc:
            self.skipTest("no DXC (set NK_DXC to the pinned release)")
        if not shader_embed.dxc_is_pinned(shader_embed.dxc_version(dxc)):
            self.skipTest(f"{dxc} is not the pinned {shader_embed.DXC_RELEASE['tag']}")
        errors, skipped = shader_embed.verify_dxc(shader_embed.ROOT, shader_embed.FSR3_MANIFEST, dxc)
        self.assertEqual(errors, [])
        self.assertIsNone(skipped)


# ---- ge_powf: one text in src/rt/ge.c and shaders/psp_tnl.vert (#318, #1732) ----------------

GE_C = shader_embed.ROOT / "src" / "rt" / "ge.c"
TNL_VERT = shader_embed.GPU_DIR / "shaders" / "psp_tnl.vert"
POW_BLOCKS = (
    ("/* ---- BEGIN ge_pow helpers", "/* ---- END ge_pow helpers ---- */"),
    ("/* ---- BEGIN ge_powf (shared text) ---- */", "/* ---- END ge_powf ---- */"),
)
POW_TABLES = {"R": 92, "TH": 92, "TL": 92, "EH": 64, "EL": 64}


def _pow_blocks(text: str) -> list[str]:
    blocks = []
    for begin, end in POW_BLOCKS:
        start = text.find(begin)
        stop = text.find(end, start)
        blocks.append(text[start:stop + len(end)] if start >= 0 and stop >= 0 else "")
    return blocks


def _pow_tables(text: str) -> dict[str, list[int]]:
    tables = {}
    for name, body in re.findall(r"GE_POW_(\w+)\[\d+\]\s*=\s*(?:uint\[\d+\]\(|\{)(.*?)(?:\);|\};)",
                                 text, re.S):
        tables[name] = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]+)u", body)]
    return tables


def _f32(bits: int) -> float:
    value: float = struct.unpack("<f", struct.pack("<I", bits))[0]
    return value


def _f32_bits_nearest(value: Fraction) -> int:
    """The float32 nearest a positive value, ties to even, subnormals included."""
    if value == 0:
        return 0
    e = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** e > value:
        e -= 1
    while Fraction(2) ** (e + 1) <= value:
        e += 1
    unit = Fraction(2) ** (max(e, -126) - 23)
    q = value / unit
    n = q.numerator // q.denominator
    rest = q - n
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and n & 1):
        n += 1
    if e < -126:
        return n                      # subnormal; 1 << 23 is the smallest normal
    if n == 1 << 24:
        n >>= 1
        e += 1
    if e > 127:
        return 0x7F800000
    return ((e + 127) << 23) | (n & 0x7FFFFF)


def _pow_exact_bits(x_bits: int, y_bits: int) -> int:
    """Correctly rounded x^y for finite x > 0, y > 0: exact for integer y, else to 10^-70."""
    x, y = Fraction(_f32(x_bits)), Fraction(_f32(y_bits))
    if y.denominator == 1 and y <= 4096:
        return _f32_bits_nearest(x ** y.numerator)
    with localcontext() as ctx:
        ctx.prec = 90
        v = (Decimal(y.numerator) / Decimal(y.denominator) * (Decimal(x.numerator) / Decimal(x.denominator)).ln()).exp()
    return _f32_bits_nearest(Fraction(v))


# Inputs with known correctly rounded results: the edges of each range, exact powers, results
# near and below the smallest normal, the GE's float24 coefficients, and seeded random pairs.
POW_CASES = [
    (0x3F000000, 0x3F800000), (0x3F000000, 0x40000000), (0x3E800000, 0x3F000000),
    (0x3F000000, 0x43150000), (0x3F000000, 0x43160000), (0x3F000000, 0x42FD0000),
    (0x00000001, 0x3F800000), (0x0DA24260, 0x3E800000), (0x3F7FFFEF, 0x43800000),
    (0x3F800001, 0x41000000), (0x40000000, 0x40400000), (0x3F7FFFFF, 0x4B7FFFFF),
    (0x3F3504F3, 0x41200000), (0x3F3504F4, 0x41200000), (0x3F7F4FAE, 0x42F44700),
    (0x3F6E147B, 0x41000000), (0x3F733333, 0x42C80000), (0x3C23D70A, 0x40600000),
]
_seed = 0x1732
for _ in range(400):
    _seed = (_seed * 1103515245 + 12345) & 0x7FFFFFFF
    x_bits = 0x30000000 + _seed % 0x0F800000
    _seed = (_seed * 1103515245 + 12345) & 0x7FFFFFFF
    y_bits = (0x3E800000 + _seed % 0x05000000) & 0xFFFFFF00
    POW_CASES.append((x_bits, y_bits))

# The shader's copy compiled as C with the shader's own helper definitions (its 64-bit product
# from 32-bit limbs, as GLSL's umulExtended gives them), linked with the production ge.c.
POW_HARNESS_C = r"""
#define uint uint32_t
#define uvec2 ShV2
#define GE_POW_FN static
typedef struct { uint32_t x, y; } ShV2;
static ShV2 ge_v2(uint32_t h, uint32_t l) { ShV2 v; v.x = h; v.y = l; return v; }
static uint32_t floatBitsToUint(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uintBitsToFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int findMSB(uint32_t v) { int n = -1; while (v) { n++; v >>= 1; } return n; }
static int ge_i(uint32_t v) { return (int)v; }
static uint32_t ge_u(int v) { return (uint32_t)v; }
static uint32_t ge_pow_mulhi(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }
@TABLES@
@HELPERS@
static ShV2 ge_pow_mul(ShV2 a, ShV2 b) { return ge_pow_mul_limbs(a, b); }
@POWF@
#undef uvec2
#undef uint

static uint32_t s_seed = 0x1732u;
static uint32_t rnd(void) { s_seed = s_seed * 1664525u + 1013904223u; return s_seed; }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "cases") == 0) {
        unsigned xb, yb;
        while (scanf("%x %x", &xb, &yb) == 2) {
            float x = uintBitsToFloat(xb), y = uintBitsToFloat(yb);
            printf("%08x %08x %08x %08x\n", xb, yb, floatBitsToUint(ge_pow_selftest_powf(x, y)),
                   floatBitsToUint(ge_powf(x, y)));
        }
        return 0;
    }
    unsigned long n = 2000000ul, differ = 0, mul_differ = 0;
    for (unsigned long k = 0; k < n; k++) {
        uint32_t xb, yb;
        switch (k % 5u) {
        case 0: xb = 0x3F000000u + (rnd() & 0x7FFFFFu); break;          /* [0.5, 1) */
        case 1: xb = 0x3F7F0000u + (rnd() & 0xFFFFu); break;            /* just below 1 */
        case 2: xb = rnd() % 0x3F800000u; break;                        /* (0, 1), subnormals too */
        case 3: xb = 0x3F800000u + (rnd() & 0x00FFFFFFu); break;        /* [1, 4) */
        default: xb = rnd(); break;                                     /* any bits */
        }
        yb = (k & 1u) ? ((0x3E800000u + rnd() % 0x05000000u) & 0xFFFFFF00u) : rnd();
        float x = uintBitsToFloat(xb), y = uintBitsToFloat(yb);
        uint32_t a = floatBitsToUint(ge_pow_selftest_powf(x, y)), b = floatBitsToUint(ge_powf(x, y));
        if (a != b && differ++ == 0)
            printf("FIRST x=%08x y=%08x ge.c=%08x shader=%08x\n", (unsigned)xb, (unsigned)yb,
                   (unsigned)a, (unsigned)b);
        uint32_t ma[2] = { rnd(), rnd() }, mb[2] = { rnd(), rnd() }, out[4];
        if ((k & 7u) == 0u) { ma[0] = 0xFFFFFFFFu; ma[1] = 0xFFFFFFFFu; }
        ge_pow_selftest_mul(ma, mb, out);
        if (out[0] != out[2] || out[1] != out[3]) mul_differ++;
    }
    printf("RANDOM n=%lu differ=%lu mul_differ=%lu\n", n, differ, mul_differ);
    return 0;
}
"""


class GePowfSharedTextTests(unittest.TestCase):
    def test_ge_c_and_the_tnl_shader_carry_one_text(self) -> None:
        ge_blocks = _pow_blocks(GE_C.read_text(encoding="utf-8"))
        vert_blocks = _pow_blocks(TNL_VERT.read_text(encoding="utf-8"))
        for ge_block, vert_block in zip(ge_blocks, vert_blocks, strict=True):
            self.assertTrue(ge_block, "ge.c lacks a ge_powf shared-text block")
            self.assertTrue(vert_block, "psp_tnl.vert lacks a ge_powf shared-text block")
            self.assertEqual(ge_block, vert_block)
        # Integer arithmetic only: no float operation the two languages could round differently.
        for block in ge_blocks:
            code = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
            self.assertIsNone(re.search(r"\b(?:powf?|expf?|logf?|exp2f?|log2f?|fmaf?|sqrtf?)\s*\(", code))
            self.assertIsNone(re.search(r"\b(?:float|double)\s+[a-z_]+\s*=", code))

    def test_both_tables_hold_the_same_numbers(self) -> None:
        ge_tables = _pow_tables(GE_C.read_text(encoding="utf-8"))
        vert_tables = _pow_tables(TNL_VERT.read_text(encoding="utf-8"))
        for name, count in POW_TABLES.items():
            self.assertEqual(len(ge_tables.get(name, [])), count, name)
            self.assertEqual(ge_tables[name], vert_tables.get(name), name)

    @unittest.skipUnless(CC, "no C compiler on PATH")
    def test_shader_copy_gives_ge_c_bits_and_rounds_correctly(self) -> None:
        vert = TNL_VERT.read_text(encoding="utf-8")
        helpers, powf = _pow_blocks(vert)
        tables = "".join(
            f"static const uint32_t GE_POW_{name}[{len(values)}] = {{"
            + ", ".join(f"0x{v:08X}u" for v in values) + "};\n"
            for name, values in _pow_tables(vert).items()
        )
        source = GE_HARNESS_PRELUDE_C + POW_HARNESS_C.replace("@TABLES@", tables) \
            .replace("@HELPERS@", helpers).replace("@POWF@", powf)
        with tempfile.TemporaryDirectory(prefix="nakagawa-ge-powf-") as temp_name:
            exe = compile_ge_harness(Path(temp_name), source, "ge_powf_harness")
            random_run = subprocess.run([os.fspath(exe)], capture_output=True, text=True, check=True)
            cases = "".join(f"{x:08x} {y:08x}\n" for x, y in POW_CASES)
            case_run = subprocess.run([os.fspath(exe), "cases"], input=cases, capture_output=True,
                                      text=True, check=True)
        self.assertIn("RANDOM n=2000000 differ=0 mul_differ=0", random_run.stdout, random_run.stdout)
        lines = case_run.stdout.split()
        self.assertEqual(len(lines), 4 * len(POW_CASES))
        for i, (x_bits, y_bits) in enumerate(POW_CASES):
            ge_bits, shader_bits = int(lines[4 * i + 2], 16), int(lines[4 * i + 3], 16)
            want = _pow_exact_bits(x_bits, y_bits)
            self.assertEqual((ge_bits, shader_bits), (want, want),
                             f"pow({x_bits:08x}, {y_bits:08x}): ge.c {ge_bits:08x} shader {shader_bits:08x}"
                             f" correctly rounded {want:08x}")


if __name__ == "__main__":
    unittest.main()
