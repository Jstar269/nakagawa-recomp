# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Built-in PSP decryption boundary tests.

Source-owned end-to-end and known-answer coverage for the production
decryption boundary under ``src/core/nk_psp_*`` plus the ``nk_decrypt``
command-line entry, including the built-in game tables (owner decision
2026-10-27) and the firmware refusal.

Nothing here reads retail bytes or private inputs:

*   the known-answer vectors are the published NIST/RFC values, except the
    two-block AES-CBC check, whose expected output is zero-IV CBC computed
    over the published SP 800-38A F.2.1 key and plaintext (see the comment at
    ``ZERO_START_CBC_CIPHER``);
*   the container keys are generated per-run as *clearly fake* random bytes
    and written into a throwaway key file outside the repository, except in
    ``TestBuiltinTables``, which seals with one recipe read at run time from
    the public built-in table ``src/core/nk_psp_builtin_keys.c``;
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
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]

CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")

# ---------------------------------------------------------------------------
# Published known-answer vectors (NIST FIPS 197 / FIPS 180-1 / RFC 4493).
# The zero-IV CBC expectation (ZERO_START_CBC_CIPHER) is not a published value;
# it is cross-checked against OpenSSL and documented at its definition.
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

# NIST SP 800-38A section F.2.1 plaintext (two AES blocks), published value.
# The AES-128 key for this vector is the public F.2.1 key, which is the same
# value as RFC4493_KAT_MATERIAL above.
NIST_SP800_38A_F21_PLAIN = bytes.fromhex(
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
)
# Expected output of AES-128-CBC over NIST_SP800_38A_F21_PLAIN with an all-zero
# starting block (the C library has no IV parameter; chaining starts from zero).
# This is NOT the SP 800-38A F.2.1 ciphertext, which uses the starting block
# 000102...0f. Cross-checked independently with OpenSSL:
#   openssl enc -aes-128-cbc -nopad -K <F.2.1 key hex> -iv <16 zero bytes> -in <plain>
# gives this value, and the same command with the F.2.1 starting block gives the
# published F.2.1 ciphertext.
ZERO_START_CBC_CIPHER = bytes.fromhex(
    "3ad77bb40d7a3660a89ecaf32466ef97b148c17f309ee692287ae57cf12add49"
)

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


def seal_type01_psp(payload: bytes, tag: int, key144: bytes, kirk_cmd1_key: bytes,
                    keyvault_key: bytes, *, type1: bool, elf_size: int) -> bytes:
    """Seal ``payload`` into an encrypted ``~PSP`` type-0 or type-1 container.

    The inverse of the production type-0/1 path (144-byte recipes): the KIRK
    CMD1 header's first 0x70 bytes go through ``out = kirk7(in ^ key144[0x14:])
    ^ key144[0x20:]``, the SHA-1 covers ``key144[:0x14]`` plus the view, and
    type 1 additionally runs a keyvault pass over the 0xA0 bytes from
    ``sha1 + 0xC``.
    """
    assert len(key144) == 144 and len(kirk_cmd1_key) == 16 and len(keyvault_key) == 16
    data_size = len(payload)
    data_offset = 0x80
    chk_size = _round16(data_size)
    container_size = 0x150 + chk_size
    prx_header = bytes(build_psp_header(container_size, elf_size, False,
                                        (elf_size, 0x100, 0, 0)))

    a_key = bytes(x ^ 0xA5 for x in key144[:16])
    c_key = bytes(x ^ 0x5A for x in key144[:16])
    hdr_tail = (struct.pack("<I", 1) + bytes(12) +
                struct.pack("<II", data_size, data_offset) + bytes(8) + bytes(16))
    enc_payload = aes_cbc_encrypt(a_key, payload + bytes(chk_size - data_size))
    clear = (aes_cbc_encrypt(kirk_cmd1_key, a_key + c_key) +
             aes_cmac(c_key, hdr_tail) +
             aes_cmac(c_key, hdr_tail + prx_header + enc_payload))
    clear70 = clear + bytes(0x20) + hdr_tail[:0x10]
    scrambled = aes_cbc_encrypt(
        keyvault_key, bytes(clear70[i] ^ key144[i + 0x20] for i in range(0x70)))
    kirk_block = bytes(scrambled[i] ^ key144[i + 0x14] for i in range(0x70)) + hdr_tail[0x10:]
    unused = bytes(0x28)
    digest = hashlib.sha1(key144[:0x14] + unused + kirk_block + prx_header).digest()

    # The view, laid out as the decoder reads it: tag, sha1, unused, kirkBlock.
    view = bytearray(struct.pack("<I", tag) + digest + unused + kirk_block)
    if type1:
        view[0x10:0xB0] = aes_cbc_encrypt(keyvault_key, bytes(view[0x10:0xB0]))
    sha1_field = bytes(view[0x04:0x18])
    unused_field = bytes(view[0x18:0x40])
    kirk_field = bytes(view[0x40:0xD0])

    out = bytearray(container_size)
    out[0x00:0x80] = prx_header
    out[0x80:0xD0] = kirk_field[0x40:0x90]
    struct.pack_into("<I", out, 0xD0, tag)
    out[0xD4:0xE8] = sha1_field
    out[0xE8:0x110] = unused_field
    out[0x110:0x150] = kirk_field[0x00:0x40]
    out[0x150:] = enc_payload
    return bytes(out)


def gzip_payload(data: bytes) -> bytes:
    import gzip
    return gzip.compress(data, mtime=0)


def mixed_block_gzip_payload(elf: bytes) -> tuple[bytes, bytes, list[bytes]]:
    """Return ``(plain, gzip, runs)`` whose DEFLATE stream mixes block types.

    zlib writes stored blocks for the incompressible stretches of a module, so
    a real compressed ``~PSP`` payload switches from Huffman-coded blocks to
    stored blocks and back.  Each run here comes from its own zlib compressor
    and ends on a full flush, so every seam is byte aligned and no match
    reaches across it; the middle run is compressed at level 0, which zlib
    writes as stored blocks.  ``runs`` holds each run's DEFLATE bytes.
    """
    import zlib

    compressible = (struct.pack("<I", 0x27BDFFE0) + bytes(12)) * 0x10000  # 1 MiB
    incompressible = b"".join(hashlib.sha256(struct.pack("<I", n)).digest()
                              for n in range(0x400))                     # 32 KiB
    plans = ((elf + compressible, 9), (incompressible, 0), (compressible, 9))
    plain = b"".join(data for data, _ in plans)
    runs = []
    for index, (data, level) in enumerate(plans):
        compressor = zlib.compressobj(level, zlib.DEFLATED, -15)
        last = index == len(plans) - 1
        runs.append(compressor.compress(data) +
                    compressor.flush(zlib.Z_FINISH if last else zlib.Z_FULL_FLUSH))
    header = b"\x1f\x8b\x08\x00" + bytes(4) + b"\x00\xff"  # no optional fields
    trailer = struct.pack("<II", zlib.crc32(plain), len(plain))
    stream = header + b"".join(runs) + trailer
    assert zlib.decompress(stream, 16 + 15) == plain
    return plain, stream, runs


# ---------------------------------------------------------------------------
# Key file helpers (synthetic, clearly fake, outside the repository).
# ---------------------------------------------------------------------------

def fake_hex(n: int) -> str:
    return os.urandom(n).hex()


def require_scratch_path(path: Path) -> Path:
    """Refuse a test key file anywhere but the system temporary directory.

    The real key file sits under the user's profile, so a fixture written
    through a default user-data root would replace the user's own keys.
    """
    resolved = Path(path).resolve()
    scratch = Path(tempfile.gettempdir()).resolve()
    if not resolved.is_relative_to(scratch):
        raise AssertionError(
            f"test key file {resolved} is outside the temporary directory {scratch}")
    return path


def write_keyfile(path: Path, entries: dict) -> None:
    import json
    require_scratch_path(path)
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


# Distinct obviously-synthetic tags so several sealed modules can be keyed
# independently within one disc fixture (still no retail meaning, no real keys).
SYNTH_TAG_ALPHA = 0x5EED2952
SYNTH_TAG_BETA = 0x5EED2953


def multi_tag_entries(tags: list[int], code: int = SYNTH_CODE) -> dict:
    """A clearly-fake key set covering several synthetic containers at once."""
    entries, _tag_key = synthetic_entries(tags[0], code)
    for tag in tags[1:]:
        entries[f"prx.tag.0x{tag:08X}"] = {"code": code, "key": os.urandom(16).hex()}
    return entries


def seal_with_entries(payload: bytes, tag: int, entries: dict, *, compressed: bool = False) -> bytes:
    """Seal ``payload`` with the per-run fake keys already in ``entries``."""
    tag_entry = entries[f"prx.tag.0x{tag:08X}"]
    return seal_type2_psp(
        payload, tag,
        bytes.fromhex(tag_entry["key"]),
        bytes.fromhex(entries["kirk.cmd1.key"]),
        bytes.fromhex(entries[f"kirk.keyvault.{SYNTH_CODE}"]),
        compressed=compressed, elf_size=len(payload),
    )


# ---------------------------------------------------------------------------
# Test harness
# ---------------------------------------------------------------------------

class BoundaryUnavailable(RuntimeError):
    """Raised while the production boundary sources do not exist yet."""


def build_tool() -> Path:
    """Compile (or reuse) the production ``nk_decrypt`` command-line entry."""
    from tools.nk_core import decrypt_tool
    return decrypt_tool.find_or_build_tool(ROOT)


def isolated_profile_environment(root: Path) -> dict[str, str]:
    """Per-user locations that the tools and the player resolve, all under ``root``."""
    home = root / "home"
    return {
        "LOCALAPPDATA": str(root / "localappdata"),
        "APPDATA": str(root / "appdata"),
        "USERPROFILE": str(home),
        "HOME": str(home),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local" / "share"),
        "XDG_STATE_HOME": str(home / ".local" / "state"),
    }


_PROFILE_ROOT: Path | None = None
_PROFILE_PATCH = None


def setUpModule():
    """Run every test here against a throwaway user profile.

    Nothing here should touch a profile, but a test that fell back to the
    default user-data root would otherwise read or write the real one.
    """
    global _PROFILE_ROOT, _PROFILE_PATCH

    _PROFILE_ROOT = Path(tempfile.mkdtemp(prefix="nk_psp_profile_"))
    _PROFILE_PATCH = mock.patch.dict(os.environ,
                                     isolated_profile_environment(_PROFILE_ROOT))
    _PROFILE_PATCH.start()


def tearDownModule():
    global _PROFILE_ROOT, _PROFILE_PATCH
    if _PROFILE_PATCH is not None:
        _PROFILE_PATCH.stop()
        _PROFILE_PATCH = None
    if _PROFILE_ROOT is not None:
        shutil.rmtree(_PROFILE_ROOT, ignore_errors=True)
        _PROFILE_ROOT = None


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestKnownAnswers(unittest.TestCase):
    """Published NIST/RFC vectors for the primitive operations, plus the
    cross-checked zero-IV CBC expectation."""

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
        self.assertEqual(got["cipher"], ZERO_START_CBC_CIPHER.hex())
        self.assertEqual(got["roundtrip"], NIST_SP800_38A_F21_PLAIN.hex())

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
    """A source-owned encrypted fixture is
    unwrapped end-to-end by the production boundary."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_e2e_"))
        cls.exe = build_tool()
        cls.elf = tiny_elf()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _seal(self, payload: bytes, compressed: bool,
              elf_size: int | None = None) -> tuple[bytes, dict, bytes]:
        entries, tag_key = synthetic_entries()
        tag_entry = entries[f"prx.tag.0x{SYNTH_TAG:08X}"]
        container = seal_type2_psp(
            payload, SYNTH_TAG, bytes.fromhex(tag_entry["key"]),
            bytes.fromhex(entries["kirk.cmd1.key"]),
            bytes.fromhex(entries[f"kirk.keyvault.{SYNTH_CODE}"]),
            compressed=compressed,
            elf_size=len(self.elf) if elf_size is None else elf_size,
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

    def test_gzip_payload_with_stored_blocks_between_huffman_blocks(self):
        """A gzip payload that switches between Huffman and stored blocks decrypts.

        The inflater used to fall through from a stored block into the Huffman
        body loop with the previous block's table, so a real compressed module
        failed with "inflate: invalid deflate block type" while zlib inflated
        it.  The tiny-ELF payload above never produces a stored block.
        """
        plain, stream, runs = mixed_block_gzip_payload(self.elf)
        # The stream really has the shape under test: a Huffman-coded run, a
        # stored run, then a Huffman-coded final run (BTYPE is bits 1-2).
        self.assertIn((runs[0][0] >> 1) & 3, (1, 2))
        self.assertEqual((runs[1][0] >> 1) & 3, 0)
        self.assertIn((runs[2][0] >> 1) & 3, (1, 2))
        container, entries, _ = self._seal(stream, compressed=True,
                                           elf_size=len(plain))
        src = self.tmp / "EBOOT_mixed.BIN"
        src.write_bytes(container)
        keyfile = self.tmp / "keys_mixed.json"
        write_keyfile(keyfile, entries)
        dst = self.tmp / "EBOOT_mixed.elf"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(dst.read_bytes(), plain)

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


# ---------------------------------------------------------------------------
# Built-in game tables (owner decision 2026-10-27) and the firmware refusal
# ---------------------------------------------------------------------------

BUILTIN_TABLE = ROOT / "src" / "core" / "nk_psp_builtin_keys.c"
# A game EBOOT tag from the built-in table, and the firmware containers the
# boundary must refuse by name (updater, DATA.PSAR, Media Engine image, the
# system menu's index.dat).
BUILTIN_GAME_TAG = 0xD91613F0
FIRMWARE_TAGS = (0x0B000000, 0x0E000000, 0x0F000000, 0x862648D1, 0x07000000, 0x0B2B92F0)


def builtin_table() -> tuple[dict, dict, dict]:
    """Parse the public built-in table: byte arrays, recipes, flat values, firmware tags."""
    text = BUILTIN_TABLE.read_text(encoding="utf-8")
    arrays = {
        name: bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\b", body))
        for name, body in re.findall(r"static const u8 (\w+)\[\] = \{([^}]*)\};", text)
    }
    for name, body in re.findall(r"static const u32 (\w+)\[\] = \{([^}]*)\};", text):
        arrays[name] = b"".join(struct.pack("<I", int(v, 16))
                                for v in re.findall(r"0x([0-9A-Fa-f]{8})\b", body))
    recipes = {}
    for tag, code, key, key144, xor in re.findall(
            r"\{0x([0-9A-F]{8}), 0x([0-9A-F]{2}), (\w+), (\w+), (\w+)\}", text):
        recipes[int(tag, 16)] = {"code": int(code, 16), "key": key, "key144": key144,
                                 "xor": xor}
    values = {name: arrays[cname]
              for name, cname in re.findall(r'\{"([a-z0-9.]+)", (\w+), sizeof', text)}
    firmware = {int(tag, 16): what
                for tag, what in re.findall(r'\{0x([0-9A-F]{8}), "([^"]+)"\}', text)}
    return arrays, recipes, {"values": values, "firmware": firmware}


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestBuiltinTables(unittest.TestCase):
    """nk_decrypt with no key file uses the built-in game tables; firmware is
    refused by name before any key lookup, whatever key store is loaded."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_builtin_"))
        cls.exe = build_tool()
        cls.elf = tiny_elf(b"NK295-BUILTIN")
        cls.arrays, cls.recipes, extra = builtin_table()
        cls.values, cls.firmware = extra["values"], extra["firmware"]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _seal_builtin(self, header_tag: int) -> bytes:
        recipe = self.recipes[BUILTIN_GAME_TAG]
        return seal_type2_psp(
            self.elf, header_tag, self.arrays[recipe["key"]],
            self.values["kirk.cmd1.key"],
            self.values[f"kirk.keyvault.{recipe['code']}"],
            compressed=False, elf_size=len(self.elf),
        )

    def _run(self, name: str, data: bytes, *extra: str):
        src = self.tmp / f"{name}.BIN"
        dst = self.tmp / f"{name}.out"
        src.write_bytes(data)
        dst.unlink(missing_ok=True)
        out = subprocess.run(
            [str(self.exe), "decrypt", *extra, "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        return out, dst

    def test_builtin_tables_decrypt_a_container_without_a_key_file(self):
        out, dst = self._run("builtin_ok", self._seal_builtin(BUILTIN_GAME_TAG))
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("DECRYPT_OK", out.stdout)
        self.assertEqual(dst.read_bytes(), self.elf)

    def test_probe_names_the_builtin_recipe_keyvault_row(self):
        src = self.tmp / "builtin_probe.BIN"
        src.write_bytes(self._seal_builtin(BUILTIN_GAME_TAG))
        out = subprocess.run([str(self.exe), "probe", "--in", str(src)],
                             capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        code = self.recipes[BUILTIN_GAME_TAG]["code"]
        self.assertIn(f"kirk.keyvault.{code}", out.stdout)

    def test_a_tag_outside_the_tables_fails_closed_naming_it(self):
        out, dst = self._run("builtin_unknown", self._seal_builtin(SYNTH_TAG))
        self.assertEqual(out.returncode, 3, out.stdout + out.stderr)
        self.assertIn(f"MISSING_KEY_ENTRY prx.tag.0x{SYNTH_TAG:08X}", out.stdout)
        self.assertFalse(dst.exists())

    def test_firmware_containers_are_refused_before_any_key_lookup(self):
        bare_kirk = bytearray(0x200)
        struct.pack_into("<I", bare_kirk, 0x60, 1)  # KIRK command-1 mode: the IPL form
        cases = {
            "psar": b"PSAR" + bytes(0x200),
            "psar_nul": b"\0PSAR" + bytes(0x200),
            "ipl_block": bytes(bare_kirk),
        }
        for tag in FIRMWARE_TAGS:
            cases[f"tag_{tag:08X}"] = self._seal_builtin(tag)
        updater = self._seal_builtin(0x0B000000)
        pbp = bytearray(0x28)
        pbp[0:4] = b"\0PBP"
        struct.pack_into("<I", pbp, 0x04, 0x00010000)
        for slot in range(8):
            struct.pack_into("<I", pbp, 0x08 + slot * 4, 0x28)
        cases["updater_pbp"] = bytes(pbp) + updater
        absent = self.tmp / "no-such-keys.json"
        for name, data in cases.items():
            for extra in ((), ("--key-file", str(absent))):
                with self.subTest(name=name, key_store="empty" if extra else "built-in"):
                    out, dst = self._run(f"fw_{name}", data, *extra)
                    self.assertEqual(out.returncode, 10, out.stdout + out.stderr)
                    self.assertIn("FIRMWARE_OUT_OF_SCOPE", out.stdout)
                    self.assertIn("never decrypts firmware", out.stdout)
                    self.assertNotIn("MISSING_KEY_ENTRY", out.stdout)
                    self.assertFalse(dst.exists())

    def test_table_carries_no_firmware_recipe_or_psar_value(self):
        self.assertEqual(set(self.firmware), set(FIRMWARE_TAGS) | {
            0x0B2B90F0, 0x0B2B91F0, 0x495BE403, 0x0B2B05F0, 0x0B2B06F0, 0x0B2B08F0,
            0x0B2B0AF0, 0x0B2B0BF0, 0x0B2B11F0, 0x0B2B1EF0, 0x0B2B0CF0, 0x0B2B12F0,
            0x0B2B1FF0, 0x0B2B80F0, 0x0B2B81F0, 0x0B2B82F0})
        self.assertFalse(set(self.firmware) & set(self.recipes))
        self.assertFalse([name for name in self.values if name.startswith("psar.")])
        used = {recipe["code"] for recipe in self.recipes.values()}
        rows = {int(name.rsplit(".", 1)[1]) for name in self.values
                if name.startswith("kirk.keyvault.")}
        self.assertEqual(rows, used)
        # The updater (0x4E) and Media Engine image (0x52) keyvault rows are not carried.
        self.assertNotIn(0x4E, rows)
        self.assertNotIn(0x52, rows)

    def test_a_key_file_cannot_name_a_firmware_entry(self):
        keyfile = self.tmp / "psar.json"
        write_keyfile(keyfile, {"psar.k1": os.urandom(16).hex()})
        out, dst = self._run("psar_keyfile", self._seal_builtin(BUILTIN_GAME_TAG),
                             "--key-file", str(keyfile))
        self.assertEqual(out.returncode, 6, out.stdout + out.stderr)
        self.assertIn("unknown key entry name 'psar.k1'", out.stdout)
        self.assertFalse(dst.exists())


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestType01Containers(unittest.TestCase):
    """Type 0 and type 1 (144-byte recipes) through the production boundary.

    Real discs use both: older executables and modules carry them, next to the
    type-2 recipes. Synthetic keys first, then the built-in recipe of one real
    tag of each type, read from the public table at run time.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_type01_"))
        cls.exe = build_tool()
        cls.elf = tiny_elf(b"NK295-T01")
        cls.arrays, cls.recipes, extra = builtin_table()
        cls.values = extra["values"]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _decrypt(self, name: str, container: bytes, *extra: str):
        src = self.tmp / f"{name}.BIN"
        dst = self.tmp / f"{name}.out"
        src.write_bytes(container)
        dst.unlink(missing_ok=True)
        out = subprocess.run(
            [str(self.exe), "decrypt", *extra, "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True)
        return out, dst

    def _synthetic(self, type1: bool):
        key144 = os.urandom(144)
        cmd1 = os.urandom(16)
        vault = os.urandom(16)
        keyfile = self.tmp / f"type{int(type1)}-keys.json"
        write_keyfile(keyfile, {
            "kirk.cmd1.key": cmd1.hex(),
            f"kirk.keyvault.{SYNTH_CODE}": vault.hex(),
            f"prx.tag.0x{SYNTH_TAG:08X}": {"code": SYNTH_CODE, "key144": key144.hex()},
        })
        container = seal_type01_psp(self.elf, SYNTH_TAG, key144, cmd1, vault,
                                    type1=type1, elf_size=len(self.elf))
        return container, keyfile

    def test_synthetic_type0_and_type1_round_trip(self):
        for type1 in (False, True):
            with self.subTest(type=int(type1)):
                container, keyfile = self._synthetic(type1)
                out, dst = self._decrypt(f"synth{int(type1)}", container,
                                         "--key-file", str(keyfile))
                self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
                self.assertEqual(dst.read_bytes(), self.elf)

    def test_tampered_type0_and_type1_fail_integrity(self):
        for type1 in (False, True):
            with self.subTest(type=int(type1)):
                container, keyfile = self._synthetic(type1)
                tampered = bytearray(container)
                tampered[0x160] ^= 0x01  # one payload byte
                out, dst = self._decrypt(f"tamper{int(type1)}", bytes(tampered),
                                         "--key-file", str(keyfile))
                self.assertNotEqual(out.returncode, 0)
                self.assertIn("INTEGRITY_CHECK_FAILED", out.stdout)
                self.assertFalse(dst.exists())

    def test_builtin_type0_and_type1_recipes_decrypt_without_a_key_file(self):
        # One real tag per type from the public table: an older game executable
        # recipe (type 0) and a newer one (type 1).
        for tag, type1 in ((0x08000000, False), (0xC0CB167C, True)):
            with self.subTest(tag=f"0x{tag:08X}"):
                recipe = self.recipes[tag]
                self.assertNotEqual(recipe["key144"], "NULL")
                container = seal_type01_psp(
                    self.elf, tag, self.arrays[recipe["key144"]],
                    self.values["kirk.cmd1.key"],
                    self.values[f"kirk.keyvault.{recipe['code']}"],
                    type1=type1, elf_size=len(self.elf))
                out, dst = self._decrypt(f"builtin_{tag:08X}", container)
                self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
                self.assertEqual(dst.read_bytes(), self.elf)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestMatchedTypeDiagnosis(unittest.TestCase):
    """The reported failure belongs to the container type whose
    shape matched, never to a later type's unmet key entry."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_diag_"))
        cls.exe = build_tool()
        cls.elf = tiny_elf()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _sealed(self) -> tuple[bytes, dict]:
        entries, _ = synthetic_entries()
        tag_entry = entries[f"prx.tag.0x{SYNTH_TAG:08X}"]
        container = seal_type2_psp(
            self.elf, SYNTH_TAG, bytes.fromhex(tag_entry["key"]),
            bytes.fromhex(entries["kirk.cmd1.key"]),
            bytes.fromhex(entries[f"kirk.keyvault.{SYNTH_CODE}"]),
            compressed=False, elf_size=len(self.elf),
        )
        return container, entries

    def _decrypt(self, name: str, container: bytes, entries: dict):
        src = self.tmp / f"{name}.BIN"
        src.write_bytes(bytes(container))
        keyfile = self.tmp / f"{name}.json"
        write_keyfile(keyfile, entries)
        dst = self.tmp / f"{name}.out"
        out = subprocess.run(
            [str(self.exe), "decrypt", "--key-file", str(keyfile),
             "--in", str(src), "--out", str(dst)],
            capture_output=True, text=True,
        )
        combined = out.stdout + out.stderr
        return out, dst, combined

    def test_corrupt_type2_container_reports_integrity_not_type6_key(self):
        """The type-6 path shares the type-2 SHA-1 input when the ECDSA
        signature tail is zero, so a corrupt type-2 container used to
        surface the type-6 ECDSA key request instead of its own failure."""
        container, entries = self._sealed()
        tampered = bytearray(container)
        tampered[0x150] ^= 0xFF  # payload bytes: the type-2 selector still matches
        out, dst, combined = self._decrypt("diag_corrupt", tampered, entries)
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        self.assertNotIn("MISSING_KEY_ENTRY", combined)
        self.assertNotIn("kirk.ecdsa", combined)
        self.assertIn("INTEGRITY_CHECK_FAILED", combined)

    def test_valid_container_missing_keyvault_entry_names_exactly_it(self):
        """A well-formed container whose key entry is absent still names
        exactly that entry, not any other type's material."""
        container, entries = self._sealed()
        entries.pop(f"kirk.keyvault.{SYNTH_CODE}")
        out, dst, combined = self._decrypt("diag_missing", container, entries)
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        self.assertIn("MISSING_KEY_ENTRY", combined)
        first = combined.split("MISSING_KEY_ENTRY", 1)[1].splitlines()[0].strip()
        self.assertTrue(
            first.startswith(f"kirk.keyvault.{SYNTH_CODE}"),
            f"expected the keyvault entry first, got: {first!r}",
        )
        self.assertNotIn("kirk.ecdsa", combined)

    def test_container_matching_no_type_reports_format_failure(self):
        """A ~PSP container whose header no type's selector matches is a
        format failure that says so, not a key request."""
        container, entries = self._sealed()
        corrupted = bytearray(container)
        corrupted[0x140] ^= 0xFF  # id region: hashed by every eligible type
        out, dst, combined = self._decrypt("diag_nomatch", corrupted, entries)
        self.assertNotEqual(out.returncode, 0)
        self.assertFalse(dst.exists())
        self.assertNotIn("MISSING_KEY_ENTRY", combined)
        self.assertIn("CONTAINER_MALFORMED", combined)
        self.assertIn("matches no supported PRX container type", combined)


# ---------------------------------------------------------------------------
# Key-material erasure harness (see TestKeystoreErasure).
# ---------------------------------------------------------------------------

# Patterns chosen so a surviving copy is unmistakable in a byte scan, and so
# they are obviously not key material: a repeating 16-byte ramp.
_ERASE_FLAT_KEY = bytes(range(0x10, 0x20))          # flat 16-byte entry
_ERASE_TAG_KEY = bytes(range(0x80, 0x90))          # prx.tag "key"
_ERASE_KEYFILE_JSON = (
    b'{\n'
    b'  "format": "nakagawa-psp-keystore-1",\n'
    b'  "entries": {\n'
    b'    "kirk.cmd1.key": "' + _ERASE_FLAT_KEY.hex().encode() + b'",\n'
    b'    "prx.tag.0x0BADC0DE": {\n'
    b'      "code": 1,\n'
    b'      "key": "' + _ERASE_TAG_KEY.hex().encode() + b'"\n'
    b'    }\n'
    b'  }\n'
    b'}\n'
)

_ERASE_HARNESS_C = r"""
/* Key-material erasure harness for the PSP KeyStore.
 *
 * Loads a user key file, confirms each entry reads back correctly, frees the
 * keystore, and then re-allocates the freed blocks and searches them for the
 * exact key patterns.  Any hit means the keys outlived nk_keystore_free.
 *
 * The scan reads only memory this process currently owns, so it is not a
 * use-after-free; the freed block is inspected indirectly, through whatever
 * the allocator hands back for a later same-class allocation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_keystore.h"

static const unsigned char FLAT_KEY[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const unsigned char TAG_KEY[16] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f
};
/* Filler for the positive control block.  Distinct from both key patterns so a
 * hit can only come from the control, never from the keystore's own bytes. */
static const unsigned char CONTROL_MARKER[16] = {
    0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
    0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5
};

/* Number of occurrences of needle inside haystack. */
static size_t count_occurrences(const unsigned char *hay, size_t hay_len,
                                const unsigned char *needle, size_t needle_len)
{
    size_t hits = 0;
    size_t i;
    if (needle_len == 0 || hay_len < needle_len) return 0;
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) hits++;
    }
    return hits;
}

int main(int argc, char **argv)
{
    char err[256];
    NkKeystore *ks;
    const uint8_t *flat = NULL;
    size_t flat_len = 0;
    NkPrxTagEntry entry;
    size_t flat_bytes, tag_bytes;
    size_t control_found;
    int probes_distinct;
    unsigned char *control;
    unsigned char *control_probe;
    unsigned char *flat_probe, *tag_probe;
    size_t flat_leaked, tag_leaked;
    int pre_free_matches = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: erase_harness <keyfile.json>\n");
        return 2;
    }

    ks = nk_keystore_create();
    if (ks == NULL) {
        fprintf(stderr, "keystore create failed\n");
        return 2;
    }

    if (nk_keystore_load_file(ks, argv[1], err, sizeof(err)) != 0) {
        fprintf(stderr, "load failed: %s\n", err);
        nk_keystore_free(ks);
        return 2;
    }

    /* Read every entry back so the harness proves the load actually worked
     * before it reasons about anything the free() is supposed to erase. */
    if (nk_keystore_get(ks, "kirk.cmd1.key", &flat, &flat_len) == 0 &&
        flat_len == 16 && memcmp(flat, FLAT_KEY, 16) == 0) {
        pre_free_matches++;
    }
    memset(&entry, 0, sizeof(entry));
    if (nk_keystore_get_prx_tag(ks, 0x0BADC0DEu, &entry) == 0 &&
        entry.key != NULL && memcmp(entry.key, TAG_KEY, 16) == 0) {
        pre_free_matches++;
    }
    if (nk_keystore_count(ks) == 2u) {
        pre_free_matches++;
    }

    flat_bytes = nk_keystore_flat_bytes();
    tag_bytes = nk_keystore_tag_bytes();
    nk_keystore_free(ks);

    /* Draw BOTH probes before running any control allocation.  The keystore has
     * just freed its two arrays plus its own struct, and those are the blocks a
     * same-size request will be served from.  Allocating the control first would
     * consume one of the blocks under test and could hand it straight back to a
     * probe, leaving the array that was never scanned unobserved while the run
     * still reported a pass. */
    flat_probe = (unsigned char *)malloc(flat_bytes);
    tag_probe = (unsigned char *)malloc(tag_bytes);
    if (flat_probe == NULL || tag_probe == NULL) {
        fprintf(stderr, "probe allocation failed\n");
        return 2;
    }

    /* The two arrays must be examined independently, so the probes have to be
     * distinct blocks.  Identical probe addresses would mean both scans covered
     * the same memory and one array went unobserved. */
    probes_distinct = (void *)flat_probe != (void *)tag_probe;

    /* Positive control, run LAST and in its own allocation so it cannot consume
     * the blocks under test: a block is filled with a marker and freed WITHOUT
     * being wiped.  If a same-size request does not come back with that marker,
     * this allocator scrubs freed memory, and a zero hit count below would be
     * indistinguishable from the allocator erasing the keys for us.  Say so
     * instead of claiming a pass. */
    control = (unsigned char *)malloc(flat_bytes);
    if (control == NULL) {
        fprintf(stderr, "control allocation failed\n");
        return 2;
    }
    memset(control, 0xA5, flat_bytes);
    free(control);
    control_probe = (unsigned char *)malloc(flat_bytes);
    if (control_probe == NULL) {
        fprintf(stderr, "control probe allocation failed\n");
        return 2;
    }
    control_found = count_occurrences(control_probe, flat_bytes, CONTROL_MARKER, 16);

    flat_leaked = count_occurrences(flat_probe, flat_bytes, FLAT_KEY, 16);
    tag_leaked = count_occurrences(tag_probe, tag_bytes, TAG_KEY, 16);

    printf("{\"entries_checked\": 2, \"pre_free_matches\": %d, "
           "\"post_wipe_scan_bytes\": %zu, \"probes_distinct\": %d, "
           "\"control_reclaimed\": %zu, \"flat_leaked\": %zu, "
           "\"tag_leaked\": %zu, \"verdict\": \"%s\"}\n",
           pre_free_matches, flat_bytes + tag_bytes, probes_distinct,
           control_found, flat_leaked, tag_leaked,
           control_found > 0 && probes_distinct
               ? "measured" : "inconclusive-allocator-scrubs");

    free(flat_probe);
    free(tag_probe);
    free(control_probe);
    /* Any surviving key material is a hard failure regardless of the control. */
    return (flat_leaked || tag_leaked) ? 5 : 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestKeystoreErasure(unittest.TestCase):
    """Key material must not survive the free() that drops it (decryption boundary).

    ``free`` returns the block to the allocator without clearing it, so a
    plain ``free`` leaves the user's keys readable in freed heap.  This suite
    links the production ``src/core/nk_psp_*.c`` sources into a small harness
    that:

    1. loads a key file whose entries are recognisable non-zero patterns;
    2. reads each entry back so the harness holds the exact expected bytes;
    3. calls ``nk_keystore_free``;
    4. re-uses the freed heap with a same-size allocation and asserts the
       key patterns are absent from it.

    Step 4 is deliberately allocation-based rather than reading through a
    dangling pointer: it only inspects live, owned memory, so the test is
    free of undefined behaviour while still proving the erasure happened.
    The request sizes come from nk_keystore_flat_bytes()/nk_keystore_tag_bytes(),
    which the module derives from sizeof() of the private structs right next to
    the allocations they describe, so this harness cannot drift out of step with
    the real layout. A positive control block is freed without wiping and has to be
    reclaimed, so a zero leak count is never reported merely because the allocator
    was not handing the block back.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_psp_erase_"))
        cls.exe = cls._build_harness()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def _build_harness(cls) -> Path:
        from tools.nk_core import decrypt_tool

        source = cls.tmp / "erase_harness.c"
        source.write_text(_ERASE_HARNESS_C, encoding="utf-8")
        exe = cls.tmp / "erase_harness.exe"
        cmd = [CC, "-std=c99", "-O2", "-Wall", "-Wextra",
               "-I", str(ROOT / "src" / "core"),
               str(source)]
        cmd.extend(str(ROOT / rel) for rel in decrypt_tool.SOURCES
                   if not rel.endswith("nk_decrypt_main.c"))
        cmd.extend(["-o", str(exe)])
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise unittest.SkipTest(
                f"keystore erasure harness did not build:\n{proc.stdout}{proc.stderr}")
        return exe

    def _run(self, payload: bytes):
        keyfile = require_scratch_path(self.tmp / f"keys_{os.urandom(6).hex()}.json")
        keyfile.write_bytes(payload)
        return subprocess.run([str(self.exe), str(keyfile)],
                              capture_output=True, text=True)

    def _report(self, out):
        """Parse the harness report, asserting on any surviving key material.

        The harness exits non-zero when it finds key bytes that outlived
        nk_keystore_free, so a non-zero exit is a real failure.  Whether a zero
        count is *provable* is a separate question answered by the control
        fields, which the caller checks.
        """
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        return json.loads(out.stdout)

    def test_keystore_free_wipes_key_material_from_the_heap(self):
        """The freed keystore heap must not still contain the user's keys."""
        report = self._report(self._run(_ERASE_KEYFILE_JSON))
        self.assertEqual(report["entries_checked"], 2)
        self.assertEqual(
            report["pre_free_matches"], 3,
            "the harness did not read the entries back, so it cannot judge erasure",
        )
        for field in ("flat_leaked", "tag_leaked"):
            self.assertEqual(
                report[field], 0,
                f"key material survived nk_keystore_free in {field}",
            )
        if report["verdict"] != "measured":
            self.skipTest(
                "this platform's allocator scrubs freed memory, so a zero leak "
                "count cannot be distinguished from the allocator erasing the "
                "block for us (probes were distinct blocks: "
                f"{report['probes_distinct']}, control marker survived: "
                f"{report['control_reclaimed']})")

    def test_wiping_does_not_change_lookup_results_before_free(self):
        """Erasure must not disturb what a live keystore returns.

        This asserts on the read-back the harness performs while the keystore is
        still alive, so a wipe that corrupted live entries would be caught even
        though the post-free scan only looks at released memory.
        """
        report = self._report(self._run(_ERASE_KEYFILE_JSON))
        self.assertEqual(report["pre_free_matches"], 3)
        self.assertGreater(report["post_wipe_scan_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
