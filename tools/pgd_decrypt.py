# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Dependency-free decryptor for PSP PGD (amctrl) encrypted files.

Hot Shots Tennis stores a ~433 MB install-data cache at
ms0:/PSP/SAVEDATA/UCUS98701GAMEDATA/GAMEDATA.BDL as a PGD file (opened with the
O_PGD flag; the game hands its 16-byte version key to sceIoIoctl 0x04100001).
Reading it needs the amctrl/KIRK decryption the PSP firmware does in hardware.

This is a locally organized implementation with mixed provenance. The
PSP-specific BBMac/BBCipher/PGD flow is derived-translated from the public
amctrl/PGD implementation family consulted during development; the AES
primitive and later interface/error handling are independently expressed. See
docs/provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md.

  * AES-128 is built here from the GF(2^8) field definition (FIPS-197) -- the
    S-box and round constants are computed, not copied from any table, and the
    implementation self-checks against the official FIPS-197 test vector.
  * The KIRK "command 4/7" primitive is AES-128-CBC (IV=0) under a fixed key
    selected by a keyseed. The three keyseeds this file needs (0x38, 0x39, 0x63)
    and the amctrl mixing constants are console decryption values and are NOT
    shipped with this project; you supply them locally. See docs/PGD_KEYS.md.
  * BBMac (a CMAC-like MAC) and BBCipher (the block stream cipher) implement the
    derived-translated amctrl flow.

Verification is intrinsic: the PGD header carries a MAC (bytes 0x80..0x90)
computed using locally supplied fixed PSP platform data, so a correct
implementation reproduces it without using the title version key. A second MAC
(0x70..0x80) additionally confirms that version key. `pgd_open` refuses to
proceed unless both verify.

Design note: this tool can produce a plaintext sidecar offline. The native runtime
also contains a C port in src/rt/pgd.c and can decrypt blocks on demand after the
guest supplies its version key. Both paths fail closed when the locally supplied
PSP constants are absent; see src/rt/hle.c h_IoIoctl and docs/PGD_KEYS.md.

Usage:
  python tools/pgd_decrypt.py --selftest
  python tools/pgd_decrypt.py <in.pgd> <out.bin> --vkey <32-hex>
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

# ---------------------------------------------------------------------------
# AES-128, derived from the field GF(2^8) with the AES reduction polynomial
# 0x11B. Nothing here is a copied lookup table: the S-box comes from the
# multiplicative inverse plus the FIPS-197 affine map, and Rcon is 2^i in the
# field. Self-tested below against the FIPS-197 Appendix B vector.
# ---------------------------------------------------------------------------

def _gf_mul(a: int, b: int) -> int:
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


# exp/log tables over the generator 3, used to invert bytes in the field.
_EXP = [0] * 256
_LOG = [0] * 256
_x = 1
for _i in range(255):
    _EXP[_i] = _x
    _LOG[_x] = _i
    _x = _gf_mul(_x, 3)
_EXP[255] = _EXP[0]


def _gf_inv(a: int) -> int:
    return 0 if a == 0 else _EXP[255 - _LOG[a]]


def _rotl8(b: int, n: int) -> int:
    return ((b << n) | (b >> (8 - n))) & 0xFF


# Build the S-box: inverse, then the affine transform x ^ rotl(x,1) ^ rotl(x,2)
# ^ rotl(x,3) ^ rotl(x,4) ^ 0x63.
SBOX = []
for _b in range(256):
    _q = _gf_inv(_b)
    _s = _q ^ _rotl8(_q, 1) ^ _rotl8(_q, 2) ^ _rotl8(_q, 3) ^ _rotl8(_q, 4) ^ 0x63
    SBOX.append(_s)
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i


def _key_expansion(key: bytes) -> list[list[int]]:
    assert len(key) == 16
    words = [list(key[i * 4:i * 4 + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(words[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]                      # RotWord
            t = [SBOX[b] for b in t]               # SubWord
            t[0] ^= rcon
            rcon = _gf_mul(rcon, 2)
        words.append([words[i - 4][j] ^ t[j] for j in range(4)])
    return [sum(words[4 * r:4 * r + 4], []) for r in range(11)]


def _add_round_key(s: list[int], rk: list[int]) -> None:
    for i in range(16):
        s[i] ^= rk[i]


def _sub_bytes(s: list[int], box: list[int]) -> None:
    for i in range(16):
        s[i] = box[s[i]]


# State is column-major (AES standard): byte index = row + 4*col.
def _shift_rows(s: list[int]) -> None:
    for r in range(1, 4):
        row = [s[r + 4 * c] for c in range(4)]
        row = row[r:] + row[:r]
        for c in range(4):
            s[r + 4 * c] = row[c]


def _inv_shift_rows(s: list[int]) -> None:
    for r in range(1, 4):
        row = [s[r + 4 * c] for c in range(4)]
        row = row[-r:] + row[:-r]
        for c in range(4):
            s[r + 4 * c] = row[c]


def _mix_columns(s: list[int]) -> None:
    for c in range(4):
        col = [s[4 * c + r] for r in range(4)]
        s[4 * c + 0] = _gf_mul(col[0], 2) ^ _gf_mul(col[1], 3) ^ col[2] ^ col[3]
        s[4 * c + 1] = col[0] ^ _gf_mul(col[1], 2) ^ _gf_mul(col[2], 3) ^ col[3]
        s[4 * c + 2] = col[0] ^ col[1] ^ _gf_mul(col[2], 2) ^ _gf_mul(col[3], 3)
        s[4 * c + 3] = _gf_mul(col[0], 3) ^ col[1] ^ col[2] ^ _gf_mul(col[3], 2)


def _inv_mix_columns(s: list[int]) -> None:
    for c in range(4):
        col = [s[4 * c + r] for r in range(4)]
        s[4 * c + 0] = _gf_mul(col[0], 14) ^ _gf_mul(col[1], 11) ^ _gf_mul(col[2], 13) ^ _gf_mul(col[3], 9)
        s[4 * c + 1] = _gf_mul(col[0], 9) ^ _gf_mul(col[1], 14) ^ _gf_mul(col[2], 11) ^ _gf_mul(col[3], 13)
        s[4 * c + 2] = _gf_mul(col[0], 13) ^ _gf_mul(col[1], 9) ^ _gf_mul(col[2], 14) ^ _gf_mul(col[3], 11)
        s[4 * c + 3] = _gf_mul(col[0], 11) ^ _gf_mul(col[1], 13) ^ _gf_mul(col[2], 9) ^ _gf_mul(col[3], 14)


def aes_encrypt_block(rks: list[list[int]], block: bytes) -> bytes:
    s = list(block)
    _add_round_key(s, rks[0])
    for r in range(1, 10):
        _sub_bytes(s, SBOX)
        _shift_rows(s)
        _mix_columns(s)
        _add_round_key(s, rks[r])
    _sub_bytes(s, SBOX)
    _shift_rows(s)
    _add_round_key(s, rks[10])
    return bytes(s)


def aes_decrypt_block(rks: list[list[int]], block: bytes) -> bytes:
    s = list(block)
    _add_round_key(s, rks[10])
    for r in range(9, 0, -1):
        _inv_shift_rows(s)
        _sub_bytes(s, INV_SBOX)
        _add_round_key(s, rks[r])
        _inv_mix_columns(s)
    _inv_shift_rows(s)
    _sub_bytes(s, INV_SBOX)
    _add_round_key(s, rks[0])
    return bytes(s)


def aes_cbc_encrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    rks = _key_expansion(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        blk = bytes(a ^ b for a, b in zip(data[i:i + 16], prev))
        prev = aes_encrypt_block(rks, blk)
        out += prev
    return bytes(out)


def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    rks = _key_expansion(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        ct = data[i:i + 16]
        pt = aes_decrypt_block(rks, ct)
        out += bytes(a ^ b for a, b in zip(pt, prev))
        prev = ct
    return bytes(out)


# ---------------------------------------------------------------------------
# PSP KIRK / amctrl constants.
#
# These are NOT shipped with this project. They are PSP console decryption
# values that the user supplies locally. Excluding the values is a concrete
# boundary; it does not resolve the separate legal question of distributing the
# implementation itself (NOTICE.md and issue #104).
#
# Format: `name = <32 hex chars>` lines, `#` for comments. Located via
# $SR_PGD_KEYS, else ./keys/pgd_keys.txt (gitignored). The same file is read by
# the C runtime in src/rt/pgd.c. See docs/PGD_KEYS.md for the schema.
# ---------------------------------------------------------------------------

PGD_KEY_NAMES = (
    "kirk_keyseed_38",
    "kirk_keyseed_39",
    "kirk_keyseed_63",
    "amctrl_loc_1cd4",
    "amctrl_loc_1ce4",
    "amctrl_loc_1cf4",
    "dnas_1a90",
    "dnas_1aa0",
)

_KEYSEED_NAMES = {0x38: "kirk_keyseed_38", 0x39: "kirk_keyseed_39", 0x63: "kirk_keyseed_63"}


class PgdKeysUnavailable(RuntimeError):
    """The locally supplied PSP constants are missing, partial, or malformed."""


def pgd_keys_path() -> str:
    """Where the constants are read from. $SR_PGD_KEYS wins, else the default."""
    return os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt")


def load_pgd_keys(path: str | None = None) -> dict[str, bytes]:
    """Parse the key file. Fails closed: every name in PGD_KEY_NAMES must be
    present exactly once as 32 hex characters, or PgdKeysUnavailable is raised."""
    path = path or pgd_keys_path()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        raise PgdKeysUnavailable(
            f"PSP constants not installed at {path!r}: {exc.strerror}. "
            "See docs/PGD_KEYS.md for the schema and how to supply them."
        ) from exc

    found: dict[str, bytes] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, sep, value = line.partition("=")
        if not sep:
            continue
        name, value = name.strip(), value.strip()
        if name not in PGD_KEY_NAMES:
            continue
        if name in found:
            raise PgdKeysUnavailable(f"{path}:{lineno}: duplicate entry {name!r}")
        if len(value) != 32:
            raise PgdKeysUnavailable(
                f"{path}:{lineno}: {name!r} must be exactly 32 hex characters, got {len(value)}"
            )
        try:
            found[name] = bytes.fromhex(value)
        except ValueError as exc:
            raise PgdKeysUnavailable(f"{path}:{lineno}: {name!r} is not valid hex") from exc

    missing = [n for n in PGD_KEY_NAMES if n not in found]
    if missing:
        raise PgdKeysUnavailable(
            f"{path}: missing {len(missing)} entr{'y' if len(missing) == 1 else 'ies'}: "
            f"{', '.join(missing)}. See docs/PGD_KEYS.md."
        )
    return found


_KEYS: dict[str, bytes] | None = None


def _k(name: str) -> bytes:
    """Lazily load and return one constant."""
    global _KEYS
    if _KEYS is None:
        _KEYS = load_pgd_keys()
    return _KEYS[name]


def _keyvault(keyseed: int) -> bytes:
    try:
        return _k(_KEYSEED_NAMES[keyseed])
    except KeyError:
        raise PgdKeysUnavailable(f"no constant defined for keyseed 0x{keyseed:02x}") from None


ZERO16 = bytes(16)


def kirk4(data: bytes, keyseed: int) -> bytes:
    """KIRK cmd 4: AES-128-CBC encrypt, IV=0, fixed key."""
    return aes_cbc_encrypt(_keyvault(keyseed), ZERO16, data)


def kirk7(data: bytes, keyseed: int) -> bytes:
    """KIRK cmd 7: AES-128-CBC decrypt, IV=0, fixed key."""
    return aes_cbc_decrypt(_keyvault(keyseed), ZERO16, data)


# ---------------------------------------------------------------------------
# BBMac: an AES-CMAC-derived MAC (per-firmware fixed-key variant, mac_type 1/3).
# ---------------------------------------------------------------------------

def _cmac_shift(b: bytes) -> bytes:
    """Left-shift a 16-byte value by one bit; xor 0x87 on carry (CMAC subkey)."""
    out = bytearray(16)
    carry = 0x87 if (b[0] & 0x80) else 0
    for i in range(15):
        out[i] = ((b[i] << 1) | (b[i + 1] >> 7)) & 0xFF
    out[15] = ((b[15] << 1) & 0xFF) ^ carry
    return bytes(out)


def _bbmac_cbc(running: bytes, block: bytes, keyseed: int) -> bytes:
    """CBC-MAC one 16-aligned run, continued from `running`; return new running."""
    b = bytearray(block)
    for i in range(16):
        b[i] ^= running[i]
    return aes_cbc_encrypt(_keyvault(keyseed), ZERO16, bytes(b))[-16:]


def bbmac(data: bytes, mac_type: int, vkey: bytes | None) -> bytes:
    """Compute the amctrl BBMac over `data` (len a multiple of 16)."""
    keyseed = 0x3A if mac_type == 2 else 0x38   # types 1 and 3 -> 0x38
    if len(data) % 16 != 0:
        raise ValueError("BBMac input must be 16-aligned for this DRM path")
    # sceDrmBBMacUpdate holds back the final block; CBC-MAC the rest.
    body, last = data[:-16], data[-16:]
    running = ZERO16
    if body:
        running = _bbmac_cbc(running, body, keyseed)
    # Finalization: subkey K1 = shift(E(k,0)); XOR into the retained block.
    k1 = _cmac_shift(aes_encrypt_block(_key_expansion(_keyvault(keyseed)), ZERO16))
    padb = bytes(last[i] ^ k1[i] for i in range(16))
    t = bytes(padb[i] ^ running[i] for i in range(16))
    mac = bytearray(aes_encrypt_block(_key_expansion(_keyvault(keyseed)), t))
    for i in range(16):
        mac[i] ^= _k("amctrl_loc_1cd4")[i]
    if vkey is not None:
        for i in range(16):
            mac[i] ^= vkey[i]
        mac = bytearray(aes_encrypt_block(_key_expansion(_keyvault(keyseed)), bytes(mac)))
    return bytes(mac)


def bbmac_verify(data: bytes, stored: bytes, mac_type: int, vkey: bytes | None) -> bool:
    mac = bbmac(data, mac_type, vkey)
    check = kirk7(stored, 0x63) if mac_type == 3 else stored
    return check == mac


# ---------------------------------------------------------------------------
# BBCipher: the amctrl block stream cipher (cipher_type 1, fixed-key path).
# ---------------------------------------------------------------------------

def bbcipher_decrypt(header_key: bytes, vkey: bytes, seed: int, data: bytes) -> bytes:
    """Decrypt `data` (multiple of 16) under the amctrl stream cipher.

    `seed` is the base block counter (0 for the header params; block_offset>>4
    for data blocks). `header_key` is xored with the version key to form the
    cipher key, exactly as sceDrmBBCipherInit(mode=2) does.
    """
    key = bytes(header_key[i] ^ vkey[i] for i in range(16))
    ckey_seed = seed + 1
    # Derive the per-file mixing block (tmp2).
    kb = bytes(key[i] ^ _k("amctrl_loc_1cf4")[i] for i in range(16))
    kb = kirk7(kb, 0x39)
    tmp2 = bytes(kb[i] ^ _k("amctrl_loc_1ce4")[i] for i in range(16))

    out = bytearray()
    p = 0
    while p < len(data):
        size = min(0x800, len(data) - p)
        if ckey_seed == 1:
            tmp1 = ZERO16
        else:
            tmp1 = tmp2[:12] + struct.pack("<I", (ckey_seed - 1) & 0xFFFFFFFF)
        # Build the counter blocks: tmp2[0:12] || seed (LE), seed incrementing.
        kblock = bytearray()
        for _ in range(0, size, 16):
            kblock += tmp2[:12] + struct.pack("<I", ckey_seed & 0xFFFFFFFF)
            ckey_seed += 1
        ks = bytearray(kirk7(bytes(kblock), 0x63))
        for i in range(16):
            ks[i] ^= tmp1[i]
        chunk = data[p:p + size]
        out += bytes(chunk[i] ^ ks[i] for i in range(size))
        p += size
    return bytes(out)


# ---------------------------------------------------------------------------
# PGD file: header parse + block decrypt.
# ---------------------------------------------------------------------------

class PgdError(Exception):
    pass


class Pgd:
    def __init__(self, header: bytes, vkey: bytes):
        if header[:4] != b"\x00PGD":
            raise PgdError("not a PGD file (bad magic)")
        self.key_index = struct.unpack_from("<I", header, 4)[0]
        self.drm_type = struct.unpack_from("<I", header, 8)[0]
        if self.drm_type == 1:
            self.mac_type = 3 if self.key_index > 1 else 1
            self.cipher_type = 1
            pgd_flag = 2 | 4 | (8 if self.key_index > 1 else 0)
        else:
            self.mac_type = 2
            self.cipher_type = 2
            pgd_flag = 2
        if self.mac_type == 2 or self.cipher_type == 2:
            raise PgdError(
                "drm_type %d needs the per-console fuse key (KIRK cmd5/8), which "
                "is not a public constant; this file is not fixed-key decryptable"
                % self.drm_type)
        fkey = _k("dnas_1a90") if (pgd_flag & 2) else _k("dnas_1aa0")

        # MAC over 0x00..0x80 under the fixed PSP platform key.
        if not bbmac_verify(header[0x00:0x80], header[0x80:0x90], self.mac_type, fkey):
            raise PgdError("header MAC(0x80) failed -- implementation or keys wrong")
        # MAC over 0x00..0x70 under the game's version key -- confirms vkey.
        if not bbmac_verify(header[0x00:0x70], header[0x70:0x80], self.mac_type, vkey):
            raise PgdError("header MAC(0x70) failed -- wrong version key for this file")
        self.vkey = vkey

        params = bbcipher_decrypt(header[0x10:0x20], vkey, 0, header[0x30:0x60])
        self.dkey = params[0x00:0x10]
        self.data_size = struct.unpack_from("<I", params, 0x14)[0]
        self.block_size = struct.unpack_from("<I", params, 0x18)[0]
        self.data_offset = struct.unpack_from("<I", params, 0x1C)[0]

    def __repr__(self):
        return ("Pgd(drm=%d key_index=%d data_size=%d block_size=%d data_offset=%d)"
                % (self.drm_type, self.key_index, self.data_size,
                   self.block_size, self.data_offset))

    def decrypt_block(self, block_index: int, ciphertext: bytes) -> bytes:
        seed = (block_index * self.block_size) >> 4
        return bbcipher_decrypt(self.dkey, self.vkey, seed, ciphertext)


# ---------------------------------------------------------------------------
# Self-test and CLI.
# ---------------------------------------------------------------------------

def selftest() -> bool:
    # FIPS-197 Appendix B / NIST AES-128 known-answer vector.
    key = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    ct = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    rks = _key_expansion(key)
    got = aes_encrypt_block(rks, pt)
    ok_enc = got == ct
    ok_dec = aes_decrypt_block(rks, ct) == pt
    # FIPS-197 also fixes the S-box values 0x00->0x63, 0x53->0xed.
    ok_sbox = SBOX[0x00] == 0x63 and SBOX[0x53] == 0xED and INV_SBOX[0x63] == 0x00
    print("AES-128 FIPS-197 encrypt vector:", "OK" if ok_enc else "FAIL (%s)" % got.hex())
    print("AES-128 FIPS-197 decrypt vector:", "OK" if ok_dec else "FAIL")
    print("S-box (computed) spot check:    ", "OK" if ok_sbox else "FAIL")
    return ok_enc and ok_dec and ok_sbox


def main(argv):
    assert __doc__ is not None
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("infile", nargs="?", help="PGD-encrypted input file")
    ap.add_argument("outfile", nargs="?", help="decrypted output file")
    ap.add_argument("--vkey", help="16-byte version key as 32 hex chars")
    ap.add_argument("--selftest", action="store_true", help="run crypto self-tests and exit")
    ap.add_argument("--check-keys", action="store_true",
                    help="report whether the local PSP constants are installed, and exit")
    ap.add_argument("--info", action="store_true", help="parse+verify header only, no decrypt")
    ns = ap.parse_args(argv[1:])

    if ns.selftest:
        return 0 if selftest() else 1
    if ns.check_keys:
        try:
            load_pgd_keys()
        except PgdKeysUnavailable as exc:
            print("PSP constants: NOT AVAILABLE\n  %s" % exc, file=sys.stderr)
            return 3
        print("PSP constants: OK (%d entries at %s)" % (len(PGD_KEY_NAMES), pgd_keys_path()))
        return 0
    if not ns.infile:
        ap.error("infile required (or use --selftest)")
    if not selftest():
        print("ERROR: AES self-test failed; refusing to run", file=sys.stderr)
        return 1
    # Fail early with actionable guidance rather than a traceback deep in the MAC.
    try:
        load_pgd_keys()
    except PgdKeysUnavailable as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 3
    if not ns.vkey:
        ap.error("--vkey required")
    vkey = bytes.fromhex(ns.vkey)
    if len(vkey) != 16:
        ap.error("--vkey must be 16 bytes (32 hex chars)")

    with open(ns.infile, "rb") as f:
        header = f.read(0x90)
        try:
            pgd = Pgd(header, vkey)
        except PgdError as e:
            print("PGD open failed:", e, file=sys.stderr)
            return 2
        print("header verified:", pgd)
        if ns.info:
            return 0
        if not ns.outfile:
            ap.error("outfile required unless --info")

        f.seek(pgd.data_offset)
        aligned = (pgd.data_size + 15) & ~15
        nblocks = (aligned + pgd.block_size - 1) // pgd.block_size
        written = 0
        with open(ns.outfile, "wb") as out:
            for b in range(nblocks):
                ct = f.read(pgd.block_size)
                if not ct:
                    break
                if len(ct) % 16:
                    ct = ct + bytes(16 - (len(ct) % 16))
                pt = pgd.decrypt_block(b, ct)
                take = min(len(pt), pgd.data_size - written)
                out.write(pt[:take])
                written += take
                if b % 256 == 0:
                    print("  block %d/%d (%d MiB)" % (b, nblocks, written // (1 << 20)),
                          end="\r", file=sys.stderr)
        print("\nwrote %d bytes -> %s" % (written, ns.outfile))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
