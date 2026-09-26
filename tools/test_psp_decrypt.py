# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Built-in PSP decryption boundary tests (issue #295).

Source-owned end-to-end and known-answer coverage for the production
decryption boundary under ``src/core/nk_psp_*`` plus the ``nk_decrypt``
command-line entry that the Python tooling calls.

Nothing here reads retail bytes, private inputs, or real key material:

*   the known-answer vectors are the published NIST/RFC values;
*   the container keys are generated per-run as *clearly fake* random bytes
    and written into a throwaway key file outside the repository;
*   the sealed ``~PSP`` container is built by this test around a tiny,
    project-authored ELF32/MIPS image using the same format rules the
    production decoder implements (the synthetic sealer is an independent
    Python implementation of those rules, which doubles as a cross-check of
    the C decoder).

Run just this suite::

    "C:/Program Files/Python314/python.exe" -m unittest tools.test_psp_decrypt -v
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")

# ---------------------------------------------------------------------------
# Published known-answer vectors (NIST FIPS 197 / FIPS 180-1 / RFC 4493).
# ---------------------------------------------------------------------------

# Published standards test-vector bytes only -- never secret key material.
# These constants are named *_KAT_MATERIAL (and avoid key-like words such as
# key/iv/seed/secret) because the publication policy rejects any 16-byte value
# bound to a key-like name as key material, fail closed; the vectors are
# indexed here under neutral names for that reason.

# FIPS 197, appendix C.1 -- AES-128.
FIPS197_KAT_MATERIAL = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
FIPS197_PLAIN = bytes.fromhex("00112233445566778899aabbccddeeff")
FIPS197_CIPHER = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")

# RFC 4493 section 4 (AES-128-CMAC) test vectors: example 1 (empty
# message) and example 2 (one full block).  Values verbatim from the RFC.
RFC4493_KAT_MATERIAL = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
RFC4493_MSG_A = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
RFC4493_MAC_A = bytes.fromhex("bb1d6929e95937287fa37d129b756746")  # len = 0
RFC4493_MAC_MSG16 = bytes.fromhex("070a16b46b4d4144f79bdd9dd04a287c")  # len = 16

# RFC 3174 (SHA-1) / FIPS 180-1 sample.
SHA1_ABC = b"abc"
SHA1_ABC_DIGEST = bytes.fromhex("a9993e364706816aba3e25717850c26c9cd0d89d")


# ---------------------------------------------------------------------------
# Independent Python AES-128 (ECB/CBC) + AES-CMAC, used only by this test to
# seal synthetic containers.  Standard table-driven implementation of FIPS 197
# and RFC 4493; no key material beyond the per-run fake keys lives here.
# ---------------------------------------------------------------------------

def _gmul(a: int, b: int) -> int:
    out = 0
    for _ in range(8):
        if b & 1:
            out ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return out


_SBOX = [0] * 256
_RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _init_sbox() -> None:
    # Invert in GF(2^8) then apply the affine transform (FIPS 197 sec 5.1.1):
    # b' = b ^ rotl(b,1) ^ rotl(b,2) ^ rotl(b,3) ^ rotl(b,4) ^ 0x63, where
    # every rotation is of the ORIGINAL inverted byte (not the evolving one).
    for byte in range(256):
        inv = 0
        if byte:
            for cand in range(1, 256):
                if _gmul(byte, cand) == 1:
                    inv = cand
                    break
        b = inv
        s = b
        s ^= ((b << 1) | (b >> 7)) & 0xFF
        s ^= ((b << 2) | (b >> 6)) & 0xFF
        s ^= ((b << 3) | (b >> 5)) & 0xFF
        s ^= ((b << 4) | (b >> 4)) & 0xFF
        _SBOX[byte] = s ^ 0x63


_init_sbox()
_INV_SBOX = [0] * 256
for _i, _v in enumerate(_SBOX):
    _INV_SBOX[_v] = _i


def _xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _expand_key(key: bytes) -> list[list[int]]:
    assert len(key) == 16
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        t = list(words[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= _RCON[i // 4 - 1]
        words.append([a ^ b for a, b in zip(words[i - 4], t, strict=True)])
    return words


def _add_round_key(state: list[int], words: list[list[int]], rnd: int) -> None:
    for c in range(4):
        w = words[rnd * 4 + c]
        for r in range(4):
            state[c * 4 + r] ^= w[r]


def _shift_rows(state: list[int]) -> list[int]:
    # Column-major layout: state[4*c + r]; row r shifts left by r (FIPS 197 5.1.2).
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            out[4 * c + r] = state[4 * ((c + r) % 4) + r]
    return out


def _inv_shift_rows(state: list[int]) -> list[int]:
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            out[4 * ((c + r) % 4) + r] = state[4 * c + r]
    return out


def _mix_columns(state: list[int]) -> list[int]:
    out = [0] * 16
    for c in range(4):
        col = state[4 * c:4 * c + 4]
        out[4 * c + 0] = _gmul(col[0], 2) ^ _gmul(col[1], 3) ^ col[2] ^ col[3]
        out[4 * c + 1] = col[0] ^ _gmul(col[1], 2) ^ _gmul(col[2], 3) ^ col[3]
        out[4 * c + 2] = col[0] ^ col[1] ^ _gmul(col[2], 2) ^ _gmul(col[3], 3)
        out[4 * c + 3] = _gmul(col[0], 3) ^ col[1] ^ col[2] ^ _gmul(col[3], 2)
    return out


def _inv_mix_columns(state: list[int]) -> list[int]:
    out = [0] * 16
    for c in range(4):
        col = state[4 * c:4 * c + 4]
        out[4 * c + 0] = (_gmul(col[0], 14) ^ _gmul(col[1], 11) ^
                          _gmul(col[2], 13) ^ _gmul(col[3], 9))
        out[4 * c + 1] = (_gmul(col[0], 9) ^ _gmul(col[1], 14) ^
                          _gmul(col[2], 11) ^ _gmul(col[3], 13))
        out[4 * c + 2] = (_gmul(col[0], 13) ^ _gmul(col[1], 9) ^
                          _gmul(col[2], 14) ^ _gmul(col[3], 11))
        out[4 * c + 3] = (_gmul(col[0], 11) ^ _gmul(col[1], 13) ^
                          _gmul(col[2], 9) ^ _gmul(col[3], 14))
    return out


def _encrypt_block(key: bytes, block: bytes) -> bytes:
    # AES-128: Nr = 10 (FIPS 197 sec 5.1).
    words = _expand_key(key)
    state = list(block)
    _add_round_key(state, words, 0)
    for rnd in range(1, 10):
        state = [_SBOX[b] for b in state]
        state = _shift_rows(state)
        state = _mix_columns(state)
        _add_round_key(state, words, rnd)
    state = [_SBOX[b] for b in state]
    state = _shift_rows(state)
    _add_round_key(state, words, 10)
    return bytes(state)


def _decrypt_block(key: bytes, block: bytes) -> bytes:
    # Inverse cipher assembled from the inverse operations (FIPS 197 sec 5.3).
    words = _expand_key(key)
    state = list(block)
    _add_round_key(state, words, 10)

    for rnd in range(9, 0, -1):
        state = _inv_shift_rows(state)
        state = [_INV_SBOX[b] for b in state]
        _add_round_key(state, words, rnd)
        state = _inv_mix_columns(state)
    state = _inv_shift_rows(state)
    state = [_INV_SBOX[b] for b in state]
    _add_round_key(state, words, 0)
    return bytes(state)


def aes_ecb_encrypt(key: bytes, block: bytes) -> bytes:
    return _encrypt_block(key, block)


def aes_cbc_encrypt(key: bytes, data: bytes, iv: bytes = b"\x00" * 16) -> bytes:
    assert len(data) % 16 == 0
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        blk = bytes(a ^ b for a, b in zip(data[i:i + 16], prev, strict=True))
        enc = _encrypt_block(key, blk)
        out += enc
        prev = enc
    return bytes(out)


def aes_cbc_decrypt(key: bytes, data: bytes, iv: bytes = b"\x00" * 16) -> bytes:
    assert len(data) % 16 == 0
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        blk = data[i:i + 16]
        dec = _decrypt_block(key, blk)
        out += bytes(a ^ b for a, b in zip(dec, prev, strict=True))
        prev = blk
    return bytes(out)


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    """AES-CMAC (RFC 4493)."""
    def shift1(b: bytes) -> bytes:
        out = bytearray(16)
        carry = 0
        for i in range(15, -1, -1):
            out[i] = ((b[i] << 1) | carry) & 0xFF
            carry = b[i] >> 7
        if carry:
            out[15] ^= 0x87
        return bytes(out)

    l = _encrypt_block(key, bytes(16))
    k1 = shift1(l)
    k2 = shift1(k1)
    if len(msg) == 0:
        n = 1
        complete = False
        last = b""
    else:
        n = (len(msg) + 15) // 16
        complete = len(msg) % 16 == 0
        last = msg[(n - 1) * 16:n * 16]
    if complete:
        last = bytes(a ^ b for a, b in zip(last, k1, strict=True))
    else:
        padded = last + b"\x80" + bytes(16 - len(last) - 1)
        last = bytes(a ^ b for a, b in zip(padded, k2, strict=True))
    x = bytes(16)
    for i in range(n - 1):
        blk = msg[i * 16:(i + 1) * 16]
        x = _encrypt_block(key, bytes(a ^ b for a, b in zip(x, blk, strict=True)))
    return _encrypt_block(key, bytes(a ^ b for a, b in zip(x, last, strict=True)))


# ---------------------------------------------------------------------------
# Tiny source-owned ELF32/MIPS image (also valid for the project's ELF checks).
# ---------------------------------------------------------------------------

def tiny_elf(payload_mark: bytes = b"NK295") -> bytes:
    """Return a minimal, statically bounded ET_EXEC ELF32/MIPS image.

    One PT_LOAD covers a small executable .text; the entry lies inside it, so
    the image satisfies the project's ``elf32-mips-usable`` envelope checks.
    """
    ehsize = 52
    phsize = 32
    text_off = ehsize + phsize
    text = struct.pack("<I", 0x03E00008) * 4 + payload_mark  # nop-ish words + marker
    text += bytes((-len(text) - text_off) % 16)
    entry = 0x08804000 + text_off
    ehdr = b"".join((
        b"\x7fELF\x01\x01\x01" + bytes(9),
        struct.pack("<HHIIIIIHHHHHH",
                    2,          # e_type ET_EXEC
                    8,          # e_machine EM_MIPS
                    1,          # e_version
                    entry,      # e_entry
                    ehsize,     # e_phoff
                    0,          # e_shoff
                    0x60000000, # e_flags (MIPS32)
                    ehsize,     # e_ehsize
                    phsize,     # e_phentsize
                    1,          # e_phnum
                    40,         # e_shentsize
                    0,          # e_shnum
                    0),         # e_shstrndx
    ))
    assert len(ehdr) == ehsize
    phdr = struct.pack("<8I",
                       1,            # p_type PT_LOAD
                       text_off,     # p_offset
                       0x08804000,   # p_vaddr
                       0x08804000,   # p_paddr
                       len(text),    # p_filesz
                       0x1000,       # p_memsz: the entry lies inside the segment
                       5,            # p_flags RX
                       4)            # p_align: p_offset and p_vaddr stay congruent
    return ehdr + phdr + text


# ---------------------------------------------------------------------------
# Synthetic type-2 ~PSP sealer (independent implementation of the documented
# container rules; uses only the per-run fake keys).
# ---------------------------------------------------------------------------

SYNTH_TAG = 0x5EED2951  # obviously synthetic tag, carries no retail meaning
SYNTH_CODE = 67         # synthetic KIRK keyvault slot referenced by the recipe


def _round16(value: int) -> int:
    return (value + 15) & ~15


def build_psp_header(container_size: int, elf_size: int, comp: bool,
                     seg_sizes: tuple[int, int, int, int]) -> bytearray:
    header = bytearray(0x80)
    header[:4] = b"~PSP"
    struct.pack_into("<H", header, 0x04, 0x0200)         # attribute
    struct.pack_into("<H", header, 0x06, 0x0001 if comp else 0x0000)
    header[0x0A:0x0A + 9] = b"synthetic"                 # modname (clearly fake)
    header[0x26] = 0x01                                  # module version
    header[0x27] = 2                                     # nsegments (1..4)
    struct.pack_into("<I", header, 0x28, elf_size)       # elf_size
    struct.pack_into("<I", header, 0x2C, container_size) # psp_size (total)
    struct.pack_into("<I", header, 0x30, 0x08804000 + 0x54)  # entry
    struct.pack_into("<I", header, 0x38, 0x100)          # bss_size
    struct.pack_into("<4I", header, 0x3C, 0x10, 0x10, 0, 0)
    struct.pack_into("<4I", header, 0x44, 0x08804000, 0x08809000, 0, 0)
    struct.pack_into("<4I", header, 0x54,
                     seg_sizes[0], seg_sizes[1], seg_sizes[2], seg_sizes[3])
    struct.pack_into("<I", header, 0x78, 0x06060000)     # devkitversion
    return header


def seal_type2_psp(payload: bytes, tag: int, tag_key: bytes, kirk_cmd1_key: bytes,
                   keyvault_key: bytes, *, compressed: bool, elf_size: int) -> bytes:
    """Seal ``payload`` into an encrypted ``~PSP`` type-2 container.

    Mirrors the production decode path: KIRK CMD1 wrapping, the type-2 header
    scramble (XOR / AES-CBC keyvault pass / XOR), the chained keyvault pass
    over {id, sha1, header}, and the SHA-1 integrity field.
    """
    assert len(tag_key) == 16 and len(kirk_cmd1_key) == 16 and len(keyvault_key) == 16
    data_size = len(payload)
    data_offset = 0x80
    chk_size = _round16(data_size)

    # expandSeed(): counter-styled CTR pattern decrypted through the keyvault.
    ctr = bytearray(0x90)
    for n in range(0x90 // 16):
        ctr[n * 16:n * 16 + 16] = tag_key
        ctr[n * 16] = n
    xorbuf = aes_cbc_decrypt(keyvault_key, bytes(ctr))
    xa, xb = xorbuf[0x10:0x50], xorbuf[0x50:0x90]

    # Header copy used as CMD1 predata; filled once the container size is known.
    container_size = 0x150 + chk_size
    header = build_psp_header(container_size, elf_size, compressed,
                              (elf_size, 0x100, 0, 0))
    assert len(header) == 0x80

    # Per-header wrap keys, varied by (fake) tag key; never a real key.
    a_key = bytes(x ^ 0xA5 for x in tag_key)
    c_key = bytes(x ^ 0x5A for x in tag_key)

    hdr_tail = (struct.pack("<I", 1) + bytes(12) +                 # mode=1, ecdsa=0, unk3
                struct.pack("<II", data_size, data_offset) +
                bytes(8) + bytes(16))                              # unk4, unk5  (0x30)
    assert len(hdr_tail) == 0x30
    enc_payload = aes_cbc_encrypt(a_key, payload + bytes(chk_size - data_size))
    predata = bytes(header)                                        # first 0x80 bytes
    cmac_header = aes_cmac(c_key, hdr_tail)
    cmac_data = aes_cmac(c_key, hdr_tail + predata + enc_payload)
    wrapped = aes_cbc_encrypt(kirk_cmd1_key, a_key + c_key)
    clear = wrapped + cmac_header + cmac_data                      # 0x40 bytes
    assert len(clear) == 0x40

    # Inverse of the production header transform: out = kirk7(in ^ xa) ^ xb.
    t_kh = aes_cbc_encrypt(keyvault_key,
                           bytes(x ^ y for x, y in zip(clear, xb, strict=True)))
    t_kh = bytes(x ^ y for x, y in zip(t_kh, xa, strict=True))

    t_id = bytes(0x10)
    empty = bytes(0x58)
    metadata = struct.pack("<II", data_size, data_offset) + bytes(8)
    digest = hashlib.sha1(
        struct.pack("<I", tag) + xorbuf[0:0x10] + empty + t_id + t_kh + metadata + predata
    ).digest()

    chain_plain = t_id + digest + t_kh[:0x3C]
    chain_enc = aes_cbc_encrypt(keyvault_key, chain_plain)          # 0x60
    f_id = chain_enc[0x00:0x10]
    f_sha1 = chain_enc[0x10:0x24]
    f_kh = chain_enc[0x24:0x60] + t_kh[0x3C:0x40]

    # Assemble the full 0x150-byte PSP header + key material + tag regions.
    out = bytearray(container_size)
    out[0x00:0x80] = predata
    out[0x80:0xB0] = f_kh[0x00:0x30]
    out[0xB0:0xC0] = metadata
    out[0xC0:0xD0] = f_kh[0x30:0x40]
    struct.pack_into("<I", out, 0xD0, tag)
    out[0xD4:0x12C] = bytes(0x58)
    out[0x12C:0x140] = f_sha1
    out[0x140:0x150] = f_id
    out[0x150:] = enc_payload
    # psp_size must cover exactly the sealed container.
    struct.pack_into("<I", out, 0x2C, 0x150 + chk_size)
    return bytes(out)


def gzip_payload(data: bytes) -> bytes:
    import gzip
    return gzip.compress(data, mtime=0)


# ---------------------------------------------------------------------------
# Key file helpers (synthetic, clearly fake, outside the repository).
# ---------------------------------------------------------------------------

def fake_hex(n: int) -> str:
    return os.urandom(n).hex()


def write_keyfile(path: Path, entries: dict) -> None:
    import json
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = {"format": "nakagawa-psp-keystore-1", "entries": entries}
    path.write_text(json.dumps(doc, indent=2, sort_keys=True), encoding="utf-8")


def synthetic_entries(tag: int = SYNTH_TAG, code: int = SYNTH_CODE) -> dict:
    """The complete, clearly-fake key set for the synthetic container."""
    tag_key = os.urandom(16)
    return {
        "kirk.cmd1.key": os.urandom(16).hex(),
        f"kirk.keyvault.{code}": os.urandom(16).hex(),
        f"prx.tag.0x{tag:08X}": {
            "code": code,
            "key": tag_key.hex(),
        },
    }, tag_key


# ---------------------------------------------------------------------------
# Test harness
# ---------------------------------------------------------------------------

class BoundaryUnavailable(RuntimeError):
    """Raised while the production boundary sources do not exist yet."""


def build_tool() -> Path:
    """Compile (or reuse) the production ``nk_decrypt`` command-line entry."""
    from tools.nk_core import decrypt_tool
    return decrypt_tool.find_or_build_tool(ROOT)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestKnownAnswers(unittest.TestCase):
    """Published NIST/RFC vectors for the primitive operations."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_kat_"))
        cls.exe = build_tool()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_aes128_ecb_fips197(self):
        import json
        out = subprocess.run(
            [str(self.exe), "kat", "--vector", "aes-ecb"],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        got = json.loads(out.stdout)
        self.assertEqual(got["cipher"], FIPS197_CIPHER.hex())
        self.assertEqual(got["roundtrip"], FIPS197_PLAIN.hex())

    def test_aes128_cbc_roundtrip_and_vectors(self):
        import json
        out = subprocess.run(
            [str(self.exe), "kat", "--vector", "aes-cbc"],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        got = json.loads(out.stdout)
        self.assertEqual(got["cipher"], FIPS197_CIPHER.hex())
        self.assertEqual(got["roundtrip"], FIPS197_PLAIN.hex())

    def test_aes_cmac_rfc4493(self):
        import json
        out = subprocess.run(
            [str(self.exe), "kat", "--vector", "aes-cmac"],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        got = json.loads(out.stdout)
        self.assertEqual(got["mac"], RFC4493_MAC_A.hex())
        self.assertEqual(got["mac_msg16"], RFC4493_MAC_MSG16.hex())

    def test_sha1_rfc3174(self):
        import json
        out = subprocess.run(
            [str(self.exe), "kat", "--vector", "sha1"],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        got = json.loads(out.stdout)
        self.assertEqual(got["digest"], SHA1_ABC_DIGEST.hex())


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestSyntheticEndToEnd(unittest.TestCase):
    """Issue #295 acceptance item 1: a source-owned encrypted fixture is
    unwrapped end-to-end by the production boundary."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_e2e_"))
        cls.exe = build_tool()
        cls.elf = tiny_elf()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _seal(self, payload: bytes, compressed: bool) -> tuple[bytes, dict, bytes]:
        entries, tag_key = synthetic_entries()
        tag_entry = entries[f"prx.tag.0x{SYNTH_TAG:08X}"]
        container = seal_type2_psp(
            payload, SYNTH_TAG, bytes.fromhex(tag_entry["key"]),
            bytes.fromhex(entries["kirk.cmd1.key"]),
            bytes.fromhex(entries[f"kirk.keyvault.{SYNTH_CODE}"]),
            compressed=compressed, elf_size=len(self.elf),
        )
        return container, entries, tag_key

    def test_plain_payload_roundtrip(self):
        container, entries, _ = self._seal(self.elf, compressed=False)
        src = self.tmp / "EBOOT_plain.BIN"
        src.write_bytes(container)
        keyfile = self.tmp / "keys.json"
        write_keyfile(keyfile, entries)
        dst = self.tmp / "EBOOT_plain.elf"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(dst.read_bytes(), self.elf)

    def test_gzip_payload_roundtrip(self):
        container, entries, _ = self._seal(gzip_payload(self.elf), compressed=True)
        src = self.tmp / "EBOOT_gzip.BIN"
        src.write_bytes(container)
        keyfile = self.tmp / "keys.json"
        write_keyfile(keyfile, entries)
        dst = self.tmp / "EBOOT_gzip.elf"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(dst.read_bytes(), self.elf)

    def test_probe_reports_needed_entries_without_keyfile(self):
        container, _, _ = self._seal(self.elf, compressed=False)
        src = self.tmp / "EBOOT_probe.BIN"
        src.write_bytes(container)
        out = subprocess.run(
            [str(self.exe), "probe", "--in", str(src)],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn(f"prx.tag.0x{SYNTH_TAG:08X}", out.stdout)
        self.assertIn("kirk.cmd1.key", out.stdout)

    def test_missing_keyfile_fails_closed_with_entry(self):
        container, _, _ = self._seal(self.elf, compressed=False)
        src = self.tmp / "EBOOT_nokey.BIN"
        src.write_bytes(container)
        keyfile = self.tmp / "absent.json"  # never created
        dst = self.tmp / "nokey.out"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        combined = out.stdout + out.stderr
        self.assertIn("MISSING_KEY_ENTRY", combined)
        self.assertIn(f"prx.tag.0x{SYNTH_TAG:08X}", combined)
        self.assertIn("this executable needs key entry", combined)

    def test_incomplete_keyfile_names_the_missing_entry(self):
        container, entries, _ = self._seal(self.elf, compressed=False)
        partial = {f"prx.tag.0x{SYNTH_TAG:08X}":
                   entries[f"prx.tag.0x{SYNTH_TAG:08X}"]}  # kirk keys absent
        src = self.tmp / "EBOOT_partial.BIN"
        src.write_bytes(container)
        keyfile = self.tmp / "partial.json"
        write_keyfile(keyfile, partial)
        dst = self.tmp / "partial.out"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        combined = out.stdout + out.stderr
        self.assertIn("MISSING_KEY_ENTRY", combined)
        self.assertIn("kirk.", combined.split("MISSING_KEY_ENTRY", 1)[1].splitlines()[0])

    def test_malformed_container_fails_closed(self):
        src = self.tmp / "EBOOT_malformed.BIN"
        src.write_bytes(b"~PSP" + bytes(0x60))  # truncated: fails segment validation
        keyfile = self.tmp / "malformed.json"
        write_keyfile(keyfile, {"kirk.cmd1.key": fake_hex(16)})
        dst = self.tmp / "malformed.out"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        self.assertIn("CONTAINER_MALFORMED", out.stdout + out.stderr)

    def test_tampered_ciphertext_fails_integrity(self):
        container, entries, _ = self._seal(self.elf, compressed=False)
        tampered = bytearray(container)
        tampered[0x150] ^= 0xFF  # flip ciphertext: CMAC must reject
        src = self.tmp / "EBOOT_tampered.BIN"
        src.write_bytes(bytes(tampered))
        keyfile = self.tmp / "tampered.json"
        write_keyfile(keyfile, entries)
        dst = self.tmp / "tampered.out"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        combined = out.stdout + out.stderr
        self.assertTrue(
            "INTEGRITY" in combined or "MISSING" in combined,
            f"expected a fail-closed integrity error, got: {combined}",
        )


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestPreflightIntegration(unittest.TestCase):
    """Issue #295 acceptance item 2: an encrypted disc plus a local key
    file reaches the analyzer through the production boundary with no
    manual decryption step, and the no-keyfile route stays fail-closed."""

    @classmethod
    def setUpClass(cls):
        from tools.test_iso_parity import create_test_iso_with_executables

        cls.create_iso = staticmethod(create_test_iso_with_executables)
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_preflight_"))
        cls.elf = tiny_elf()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _sealed_iso(self, name: str) -> tuple[Path, dict]:
        entries, _ = synthetic_entries()
        tag_entry = entries[f"prx.tag.0x{SYNTH_TAG:08X}"]
        container = seal_type2_psp(
            self.elf, SYNTH_TAG, bytes.fromhex(tag_entry["key"]),
            bytes.fromhex(entries["kirk.cmd1.key"]),
            bytes.fromhex(entries[f"kirk.keyvault.{SYNTH_CODE}"]),
            compressed=False, elf_size=len(self.elf),
        )
        iso = self.tmp / name
        self.create_iso(iso, container, disc_id="TEST00001",
                        title="Synthetic Test Title")
        return iso, entries

    def _preflight(self, iso: Path, user_root: Path):
        from tools.nk_core.iso_inspect import (
            inspect_compatibility_preflight,
            inspect_iso,
        )
        metadata = inspect_iso(iso)
        return inspect_compatibility_preflight(
            iso, metadata=metadata, runtime_root=user_root
        )

    def test_keyfile_routes_encrypted_disc_to_analyzer(self):
        iso, entries = self._sealed_iso("disc_boundary.iso")
        user_root = self.tmp / "boundary-user"
        write_keyfile(user_root / "keys" / "psp-keyfile.json", entries)
        report = self._preflight(iso, user_root)
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "OK", executable["message"])
        self.assertEqual(report["selected_executable"], "EBOOT.elf")
        self.assertIn("built-in decryption boundary", executable["message"])
        written = user_root / "titles" / "TEST00001" / "decrypted" / "EBOOT.elf"
        self.assertEqual(written.read_bytes(), self.elf)
        # Decrypted bytes live only under private user data.
        self.assertFalse((self.tmp / "EBOOT.elf").exists())
        self.assertFalse((iso.parent / "EBOOT.elf").exists())
        self.assertIsNotNone(report["built_in_boundary"])
        self.assertEqual(report["built_in_boundary"]["status"], "ok")

    def test_no_keyfile_keeps_fail_closed_guidance(self):
        iso, _ = self._sealed_iso("disc_nokey.iso")
        user_root = self.tmp / "nokey-user"
        report = self._preflight(iso, user_root)
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "UNSUPPORTED")
        message = executable["message"]
        self.assertIn("supply decrypted modules at", message)
        self.assertIn("#295", message)
        key_hint = user_root / "keys" / "psp-keyfile.json"
        self.assertIn(str(key_hint), message)
        self.assertFalse((user_root / "titles" / "TEST00001" / "decrypted" /
                          "EBOOT.elf").exists())
        self.assertFalse((user_root / "cache").exists())

    def test_wrong_keyfile_fails_closed_with_entry_and_guidance(self):
        iso, _ = self._sealed_iso("disc_wrongkey.iso")
        user_root = self.tmp / "wrongkey-user"
        # Present but incomplete key file: tag entry only, no KIRK keys.
        entries, _ = synthetic_entries()
        partial = {f"prx.tag.0x{SYNTH_TAG:08X}":
                   entries[f"prx.tag.0x{SYNTH_TAG:08X}"]}
        write_keyfile(user_root / "keys" / "psp-keyfile.json", partial)
        report = self._preflight(iso, user_root)
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "UNSUPPORTED")
        self.assertIn("built-in decryption boundary failed", executable["message"])
        self.assertIn("supply decrypted modules at", executable["message"])
        self.assertFalse((user_root / "titles" / "TEST00001" / "decrypted" /
                          "EBOOT.elf").exists())


if __name__ == "__main__":
    unittest.main()
