# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Compare two CPU-state traces (tools/TRACE_FORMAT.md) and report the first divergence.
# Exit 0 when the two traces are identical step-for-step, 1 when they diverge, 2 on a
# malformed input. The first divergence is reported with the guest PC where it happened,
# which is the located bug for differential testing against the PPSSPP oracle.
#
# Three comparison modes:
#   * default: informational v1 equality over the permissive legacy loader.
#   * --strict-hardware: complete v2 identity/coverage contract, PSP_HARDWARE required.
#   * --strict-local: the LOCAL contract for gates that run without PSP metadata. It
#     requires an identity header, a positive record count and contiguous indices, and
#     --expect-steps N additionally requires exactly N records. Equal lengths are not
#     coverage, so an incomplete stream cannot pass.

import re
import sys


HARDWARE_TRACE_MAX_STEPS = 1_000_000
HARDWARE_TRACE_MAX_DECIMAL_DIGITS = len(str(HARDWARE_TRACE_MAX_STEPS))
HARDWARE_TRACE_SOURCES = frozenset({
    "LOCAL_COSIM",
    "PPSSPP_CORROBORATIVE",
    "PSP_HARDWARE",
})
HARDWARE_TRACE_SHARED_FIELDS = (
    "fixture_id",
    "cell_id",
    "binary_sha256",
    "source_commit",
    "model",
    "firmware",
    "start_pc",
    "steps",
    "complete",
)
HARDWARE_TRACE_REQUIRED_FIELDS = frozenset(("source_tier",) + HARDWARE_TRACE_SHARED_FIELDS)
HARDWARE_TRACE_ID_RE = re.compile(r"^[A-Za-z0-9_.:/+-]{1,96}$")
HARDWARE_TRACE_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
HARDWARE_TRACE_COMMIT_RE = re.compile(r"^[0-9a-f]{40,64}$")
HARDWARE_TRACE_PC_RE = re.compile(r"^0x[0-9a-f]{8}$")
HARDWARE_TRACE_HEX32_RE = re.compile(r"^0x[0-9a-f]{8}$")
HARDWARE_TRACE_MEMORY_RE = re.compile(r"^m(8|16|32)\[0x[0-9a-f]{8}\]$")
HARDWARE_TRACE_REGISTER_RE = re.compile(
    r"^(?:r(?:[1-9]|[12][0-9]|3[01])|hi|lo|f(?:[0-9]|[12][0-9]|3[01])|"
    r"fcr31|v(?:[0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])|"
    r"epc|cause|badvaddr|status)$"
)
HARDWARE_TRACE_UNMEASURED = frozenset({
    "unknown",
    "unset",
    "placeholder",
    "none",
    "n/a",
    "na",
    "tbd",
})


class HardwareTraceError(ValueError):
    """A strict v2 hardware trace violates its identity or stream contract."""


class LocalTraceError(ValueError):
    """A local (non-hardware) trace violates the local coverage contract."""


def parse_step_line(line, lineno, path, report=None):
    # "<step> pc=<hpc> op=<hword> <tokens...>" -> (step, pc, op, {name: value})
    report = fail if report is None else report
    parts = line.split()
    if len(parts) < 3:
        report(f"{path}:{lineno}: step line has fewer than 3 fields: {line!r}")
    try:
        step = int(parts[0], 10)
    except ValueError:
        report(f"{path}:{lineno}: step index is not a decimal integer: {parts[0]!r}")
    if not parts[1].startswith("pc=") or not parts[2].startswith("op="):
        report(f"{path}:{lineno}: expected 'pc=' then 'op=', got {parts[1]!r} {parts[2]!r}")
    pc = parts[1][3:]
    op = parts[2][3:]
    writes = {}
    for tok in parts[3:]:
        name, eq, value = tok.partition("=")
        if eq != "=" or not name or not value:
            report(f"{path}:{lineno}: malformed write token: {tok!r}")
        if name in writes:
            report(f"{path}:{lineno}: duplicate write token for {name!r}")
        writes[name] = value
    return step, pc, op, writes


def fail(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(2)


def load(path):
    steps = []
    header = None
    with open(path, "r", encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.rstrip("\r\n")
            if line.strip() == "":
                continue
            if line.startswith("#"):
                if header is None:
                    header = line
                continue
            steps.append(parse_step_line(line, lineno, path))
    if header is None:
        fail(f"{path}: missing required '# psp-recomp trace' header line")
    return header, steps


def _hardware_error(path, lineno, message):
    location = f"{path}:{lineno}" if lineno is not None else str(path)
    raise HardwareTraceError(f"{location}: {message}")


def _is_identity_header(line):
    """Report whether a comment line declares a psp-recomp trace identity header."""

    parts = line[1:].split()
    return parts[:2] == ["psp-recomp", "trace"]


def _parse_hardware_decimal(value, path, lineno, field, minimum, maximum):
    """Convert a bounded decimal field so bad metadata cannot escape the contract.

    The length bound is applied before ``int()`` so an oversized value is a named
    rejection instead of an interpreter-level conversion error.
    """

    if not re.fullmatch(r"[0-9]+", value):
        _hardware_error(path, lineno, f"{field} must be a decimal integer: {value!r}")
    if len(value) > HARDWARE_TRACE_MAX_DECIMAL_DIGITS:
        _hardware_error(
            path,
            lineno,
            f"{field} exceeds the {HARDWARE_TRACE_MAX_DECIMAL_DIGITS}-digit decimal "
            f"bound for a maximum of {maximum}",
        )
    number = int(value, 10)
    if number < minimum or number > maximum:
        _hardware_error(path, lineno, f"{field} must be between {minimum} and {maximum}")
    return number


def _parse_hardware_header(line, path, lineno):
    """Parse the exact v2 identity envelope used by strict comparisons."""

    parts = line[2:].split() if line.startswith("# ") else []
    if len(parts) < 3 or parts[:3] != ["psp-recomp", "trace", "v2"]:
        _hardware_error(path, lineno, "expected '# psp-recomp trace v2' header")
    fields = {}
    for token in parts[3:]:
        name, equals, value = token.partition("=")
        if equals != "=" or not name or not value or name in fields:
            _hardware_error(path, lineno, f"malformed or duplicate v2 metadata field {token!r}")
        fields[name] = value
    unknown = sorted(set(fields) - HARDWARE_TRACE_REQUIRED_FIELDS)
    if unknown:
        _hardware_error(path, lineno, f"unknown v2 metadata field(s): {', '.join(unknown)}")
    missing = sorted(HARDWARE_TRACE_REQUIRED_FIELDS - set(fields))
    if missing:
        _hardware_error(path, lineno, f"missing v2 metadata field(s): {', '.join(missing)}")

    source = fields["source_tier"]
    if source not in HARDWARE_TRACE_SOURCES:
        _hardware_error(path, lineno, f"unsupported source_tier {source!r}")
    for field in ("fixture_id", "cell_id", "model", "firmware"):
        value = fields[field]
        if not HARDWARE_TRACE_ID_RE.fullmatch(value):
            _hardware_error(path, lineno, f"{field} has invalid metadata value {value!r}")
        if value.casefold() in HARDWARE_TRACE_UNMEASURED:
            _hardware_error(path, lineno, f"{field} carries an unmeasured placeholder {value!r}")
    digest = fields["binary_sha256"]
    if not HARDWARE_TRACE_SHA256_RE.fullmatch(digest):
        _hardware_error(path, lineno, "binary_sha256 must be lowercase SHA-256")
    if set(digest) == {"0"}:
        _hardware_error(path, lineno, "binary_sha256 cannot be an all-zero placeholder")
    commit = fields["source_commit"]
    if not HARDWARE_TRACE_COMMIT_RE.fullmatch(commit):
        _hardware_error(path, lineno, "source_commit must be a lowercase git object id")
    if set(commit) == {"0"}:
        _hardware_error(path, lineno, "source_commit cannot be an all-zero placeholder")
    if not HARDWARE_TRACE_PC_RE.fullmatch(fields["start_pc"]):
        _hardware_error(path, lineno, "start_pc must be lowercase 0x plus eight hex digits")
    _parse_hardware_decimal(fields["steps"], path, lineno, "steps", 1, HARDWARE_TRACE_MAX_STEPS)
    if fields["complete"] != "1":
        _hardware_error(path, lineno, "complete must be 1 for a hardware comparison stream")
    return fields


def _parse_hardware_step(line, path, lineno):
    """Parse a v2 step without using the v1 process-exiting parser."""

    parts = line.split()
    if len(parts) < 3:
        _hardware_error(path, lineno, "step line has fewer than 3 fields")
    if not parts[1].startswith("pc=") or not parts[2].startswith("op="):
        _hardware_error(path, lineno, "expected 'pc=' then 'op='")
    step = _parse_hardware_decimal(parts[0], path, lineno, "step index", 0, HARDWARE_TRACE_MAX_STEPS)
    pc = parts[1][3:]
    op = parts[2][3:]
    if not HARDWARE_TRACE_HEX32_RE.fullmatch(pc):
        _hardware_error(path, lineno, f"pc is not a lowercase 32-bit address: {pc!r}")
    if not HARDWARE_TRACE_HEX32_RE.fullmatch(op):
        _hardware_error(path, lineno, f"op is not a lowercase 32-bit word: {op!r}")
    writes = {}
    for token in parts[3:]:
        name, equals, value = token.partition("=")
        if equals != "=" or not name or not value or name in writes:
            _hardware_error(path, lineno, f"malformed or duplicate write token {token!r}")
        memory = HARDWARE_TRACE_MEMORY_RE.fullmatch(name)
        register = HARDWARE_TRACE_REGISTER_RE.fullmatch(name)
        if not memory and not register:
            _hardware_error(path, lineno, f"unknown write field {name!r}")
        width = int(memory.group(1), 10) // 4 if memory else 8
        if not re.fullmatch(rf"0x[0-9a-f]{{{width}}}", value):
            _hardware_error(path, lineno, f"invalid value for {name}: {value!r}")
        writes[name] = value
    return step, pc, op, writes


def _decode_trace_lines(handle, path):
    """Yield stripped trace lines, turning a decode failure into a contract error."""

    try:
        for lineno, raw in enumerate(handle, start=1):
            yield lineno, raw.rstrip("\r\n")
    except UnicodeDecodeError as exc:
        _hardware_error(path, None, f"trace is not valid UTF-8 text: {exc}")


def read_source_tier(path):
    """Return the ``source_tier`` declared by a v2 trace header, reading only the header.

    The header is validated exactly as the strict loader validates it, but no step
    record is parsed, so asking a large oracle for its tier stays cheap. The tier is
    the trace's own metadata: it is not attested by this function or by the strict
    comparison. Raises HardwareTraceError when the stream has no valid v2 header.
    """

    try:
        handle = open(path, "r", encoding="utf-8")
    except OSError as exc:
        raise HardwareTraceError(f"{path}: cannot read trace: {exc}") from exc
    with handle:
        for lineno, line in _decode_trace_lines(handle, path):
            if line.strip() == "":
                continue
            if not line.startswith("#"):
                _hardware_error(path, lineno, "step record appears before the v2 header")
            return _parse_hardware_header(line, path, lineno)["source_tier"]
    _hardware_error(path, None, "missing required v2 hardware trace header")


def _load_hardware(path):
    """Load and completely validate one v2 stream before comparing it."""

    steps = []
    header = None
    header_line = None
    declared_steps = None
    try:
        handle = open(path, "r", encoding="utf-8")
    except OSError as exc:
        raise HardwareTraceError(f"{path}: cannot read trace: {exc}") from exc
    with handle:
        for lineno, line in _decode_trace_lines(handle, path):
            if line.strip() == "":
                continue
            if line.startswith("#"):
                if header is None:
                    header = _parse_hardware_header(line, path, lineno)
                    header_line = lineno
                    declared_steps = _parse_hardware_decimal(
                        header["steps"], path, lineno, "steps", 1, HARDWARE_TRACE_MAX_STEPS
                    )
                elif _is_identity_header(line):
                    _hardware_error(
                        path,
                        lineno,
                        "duplicate trace identity header: exactly one v2 identity "
                        f"header is allowed per stream (first one at line {header_line})",
                    )
                continue
            if header is None:
                _hardware_error(path, lineno, "step record appears before the v2 header")
            steps.append(_parse_hardware_step(line, path, lineno))
            if len(steps) > declared_steps:
                _hardware_error(
                    path,
                    lineno,
                    f"stream exceeds its step budget: header declares {declared_steps} steps, captured more",
                )
    if header is None:
        _hardware_error(path, None, "missing required v2 hardware trace header")
    if len(steps) < declared_steps:
        _hardware_error(
            path,
            header_line,
            f"stream is truncated: header declares {declared_steps} steps, captured {len(steps)}",
        )
    if len(steps) > declared_steps:
        _hardware_error(
            path,
            header_line,
            f"stream exceeds its step budget: header declares {declared_steps} steps, captured {len(steps)}",
        )
    for expected, record in enumerate(steps):
        if record[0] != expected:
            _hardware_error(
                path,
                header_line,
                f"step sequence is not complete at index {expected}: record says {record[0]}",
            )
    if steps[0][1] != header["start_pc"]:
        _hardware_error(
            path,
            header_line,
            f"start_pc {header['start_pc']} does not match first record PC {steps[0][1]}",
        )
    return header, steps


def strict_hardware_diff(path_a, path_b):
    """Compare complete v2 traces with exact identity and evidence-tier checks.

    One side must be ``PSP_HARDWARE``.  The other side may be a matching
    ``LOCAL_COSIM`` trace or another hardware capture.  A PPSSPP trace remains
    useful for corroboration, but it is deliberately refused by this gate.
    """

    header_a, a = _load_hardware(path_a)
    header_b, b = _load_hardware(path_b)
    for field in HARDWARE_TRACE_SHARED_FIELDS:
        if header_a[field] != header_b[field]:
            raise HardwareTraceError(
                f"identity mismatch for {field}: A={header_a[field]!r} B={header_b[field]!r}"
            )
    sources = {header_a["source_tier"], header_b["source_tier"]}
    if "PSP_HARDWARE" not in sources:
        raise HardwareTraceError(
            "strict hardware comparison requires at least one source_tier=PSP_HARDWARE"
        )
    if "PPSSPP_CORROBORATIVE" in sources:
        raise HardwareTraceError(
            "PPSSPP_CORROBORATIVE traces cannot satisfy the PSP_HARDWARE comparison gate"
        )
    # _load_hardware already proved each record's step index equals its
    # position and both headers declare the same step count, so records are
    # compared by position; a step-index difference cannot reach this loop.
    n = len(a)
    for i in range(n):
        _, pc_a, op_a, writes_a = a[i]
        _, pc_b, op_b, writes_b = b[i]
        if pc_a != pc_b:
            return (i, pc_a, f"pc {pc_a} vs {pc_b}")
        if op_a != op_b:
            return (i, pc_a, f"op {op_a} vs {op_b}")
        if writes_a != writes_b:
            only_a = {k: v for k, v in writes_a.items() if writes_b.get(k) != v}
            only_b = {k: v for k, v in writes_b.items() if writes_a.get(k) != v}
            detail = f"writes differ: A[{describe_writes(only_a)}] B[{describe_writes(only_b)}]"
            return (i, pc_a, detail)
    return None


def describe_writes(writes):
    return " ".join(f"{name}={writes[name]}" for name in sorted(writes))


def diff(path_a, path_b):
    """Informational legacy equality mode: v1 loader, pairwise and length compare."""
    _, a = load(path_a)
    _, b = load(path_b)
    n = min(len(a), len(b))
    for i in range(n):
        step_a, pc_a, op_a, w_a = a[i]
        step_b, pc_b, op_b, w_b = b[i]
        if step_a != step_b:
            return (i, pc_a, f"step index {step_a} vs {step_b}")
        if pc_a != pc_b:
            return (i, pc_a, f"pc {pc_a} vs {pc_b}")
        if op_a != op_b:
            return (i, pc_a, f"op {op_a} vs {op_b}")
        if w_a != w_b:
            only_a = {k: v for k, v in w_a.items() if w_b.get(k) != v}
            only_b = {k: v for k, v in w_b.items() if w_a.get(k) != v}
            detail = f"writes differ: A[{describe_writes(only_a)}] B[{describe_writes(only_b)}]"
            return (i, pc_a, detail)
    if len(a) != len(b):
        shorter, longer = (path_a, path_b) if len(a) < len(b) else (path_b, path_a)
        at = a[n - 1] if n else None
        pc = at[1] if at else "0x????????"
        return (n, pc, f"trace length differs: {len(a)} vs {len(b)} ({shorter} ends first)")
    return None


def _local_error(path, lineno, message):
    location = f"{path}:{lineno}" if lineno is not None else str(path)
    raise LocalTraceError(f"{location}: {message}")


def _local_report(message):
    raise LocalTraceError(message)


def _parse_local_identity_header(line, path, lineno):
    """Parse a local v1 identity header without the strict v2 field contract."""

    parts = line[1:].split()
    if parts[:2] != ["psp-recomp", "trace"]:
        _local_error(path, lineno,
                     "expected '# psp-recomp trace' identity header, got a bare comment")
    fields = {}
    for token in parts[2:]:
        if token in ("v1", "v2"):
            continue
        name, equals, value = token.partition("=")
        if equals != "=" or not name or not value or name in fields:
            _local_error(path, lineno, f"malformed or duplicate identity field {token!r}")
        fields[name] = value
    return fields


def _load_local(path, expect_steps=None):
    """Load one local trace and validate its identity and step coverage.

    This is the LOCAL contract used by the codegen/microtest gates. It is
    deliberately separate from the strict v2 hardware lane: those gates run
    without any PSP metadata, so they need their own positive, contiguous
    coverage proof instead of borrowing the hardware envelope. Equal lengths are
    not coverage, so a caller that knows how many records it requires passes
    ``expect_steps`` and an under-length stream is rejected here.
    """

    steps = []
    header = None
    header_line = None
    try:
        handle = open(path, "r", encoding="utf-8")
    except OSError as exc:
        _local_error(path, None, f"cannot read trace: {exc}")
    with handle:
        for lineno, line in _decode_trace_lines(handle, path):
            if line.strip() == "":
                continue
            if line.startswith("#"):
                if _is_identity_header(line):
                    if header is not None:
                        _local_error(
                            path, lineno,
                            "duplicate trace identity header: exactly one identity "
                            f"header is allowed per stream (first one at line {header_line})",
                        )
                    header = _parse_local_identity_header(line, path, lineno)
                    header_line = lineno
                elif header is None:
                    _local_error(path, lineno,
                                 "first comment line is not a '# psp-recomp trace' "
                                 "identity header")
                continue
            if header is None:
                _local_error(path, lineno, "step record appears before the identity header")
            steps.append(parse_step_line(line, lineno, path, report=_local_report))
            if expect_steps is not None and len(steps) > expect_steps:
                _local_error(path, lineno,
                             f"stream exceeds the required {expect_steps} records: "
                             f"captured more at index {len(steps) - 1}")
    if header is None:
        _local_error(path, None, "missing required '# psp-recomp trace' identity header")
    if not steps:
        _local_error(path, header_line,
                     "stream carries no step records: zero coverage is never "
                     "evidence that execution matched")
    if expect_steps is not None and len(steps) < expect_steps:
        _local_error(path, header_line,
                     f"required coverage of {expect_steps} step records is "
                     f"incomplete: captured {len(steps)}")
    for expected, record in enumerate(steps):
        if record[0] != expected:
            _local_error(path, header_line,
                         f"step sequence is not contiguous at index {expected}: "
                         f"record says {record[0]}")
    return header, steps


def strict_local_diff(path_a, path_b, expect_steps=None):
    """Compare two local traces under the local coverage contract.

    Raises LocalTraceError when either stream is not a positive, contiguous,
    identity-carrying local trace (optionally of exactly ``expect_steps``
    records). Returns the first divergence like diff(), or None.
    """

    header_a, a = _load_local(path_a, expect_steps)
    header_b, b = _load_local(path_b, expect_steps)
    start_a = header_a.get("start_pc")
    start_b = header_b.get("start_pc")
    if start_a is not None and start_b is not None and start_a != start_b:
        _local_error(path_a, None,
                     f"trace identity start_pc differs: A={start_a!r} B={start_b!r}")
    # _load_local proved both streams are contiguous from index 0 and, when a
    # required length was stated, exactly that long. Without a stated length the
    # two streams must still cover the same steps, or the comparison proves nothing
    # about the records only one side carries.
    if len(a) != len(b):
        _local_error(path_b, None,
                     f"step count differs: A has {len(a)} records, B has {len(b)}")
    for i in range(len(a)):
        _, pc_a, op_a, writes_a = a[i]
        _, pc_b, op_b, writes_b = b[i]
        if pc_a != pc_b:
            return (i, pc_a, f"pc {pc_a} vs {pc_b}")
        if op_a != op_b:
            return (i, pc_a, f"op {op_a} vs {op_b}")
        if writes_a != writes_b:
            only_a = {k: v for k, v in writes_a.items() if writes_b.get(k) != v}
            only_b = {k: v for k, v in writes_b.items() if writes_a.get(k) != v}
            detail = f"writes differ: A[{describe_writes(only_a)}] B[{describe_writes(only_b)}]"
            return (i, pc_a, detail)
    return None


def main(argv):
    if len(argv) >= 2 and argv[1] == "--strict-hardware":
        if len(argv) != 4:
            sys.stderr.write(
                "usage: tracediff.py --strict-hardware <trace-a> <trace-b>\n"
            )
            return 2
        try:
            result = strict_hardware_diff(argv[2], argv[3])
        except HardwareTraceError as exc:
            print(f"REJECTED: {exc}", file=sys.stderr)
            return 2
        if result is None:
            print("OK: strict hardware traces identical, zero divergences")
            return 0
        step, pc, detail = result
        print(f"DIVERGENCE at step {step}, pc {pc}: {detail}")
        return 1
    if len(argv) >= 2 and argv[1] == "--strict-local":
        return _main_strict_local(argv[2:])
    if len(argv) != 3:
        sys.stderr.write(
            "usage: tracediff.py [--strict-hardware|--strict-local] <trace-a> <trace-b>\n"
            "                            [--expect-steps N]\n"
        )
        return 2
    result = diff(argv[1], argv[2])
    if result is None:
        _, a = load(argv[1])
        print(f"OK: traces identical, {len(a)} steps, zero divergences")
        return 0
    step, pc, detail = result
    print(f"DIVERGENCE at step {step}, pc {pc}: {detail}")
    return 1


def _main_strict_local(args):
    expect_steps = None
    if len(args) >= 2 and args[-2] == "--expect-steps":
        raw = args[-1]
        if not re.fullmatch(r"[0-9]{1,9}", raw) or int(raw, 10) < 1:
            sys.stderr.write(
                "--expect-steps must be a positive decimal record count\n"
            )
            return 2
        expect_steps = int(raw, 10)
        args = args[:-2]
    if len(args) != 2:
        sys.stderr.write(
            "usage: tracediff.py --strict-local <trace-a> <trace-b> [--expect-steps N]\n"
        )
        return 2
    try:
        result = strict_local_diff(args[0], args[1], expect_steps)
    except LocalTraceError as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 2
    if result is None:
        print(f"OK: local traces identical, {expect_steps or 'all'} steps covered, "
              "zero divergences")
        return 0
    step, pc, detail = result
    print(f"DIVERGENCE at step {step}, pc {pc}: {detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
