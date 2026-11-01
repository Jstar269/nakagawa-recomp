# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

"""Canonical CFG report for a PSP image.

Moved verbatim out of ``analyze.py`` (#1531): the report, its verifier, the gate and the static
coverage census. ``analyze.py`` imports every name back, so ``analyze.canonical_cfg_report`` and the
rest keep working. ``analyze.py`` imports this module at load time; the one thing this module needs back,
the branch decode (``branch_target``, ``branch_kind``), is imported inside ``canonical_cfg_state``.

The package cache key ``analyzer_sha256`` hashes ``analyze.py`` only, so a change here that alters
analysis results must also bump ``ANALYZER_CODEGEN_SEMANTICS_EPOCH`` in ``nk_core/package_cache.py``.
"""

import hashlib
import struct
from array import array
from collections.abc import Iterable


def section_bytes(elf, s):
    # For a relocated PRX, read the section from the rebased+relocated image so that any
    # R_MIPS_32 function pointers it holds are already concrete. Otherwise read the raw file.
    if elf.reloc is not None:
        b = elf.read_at_vaddr(s["addr"], s["size"])
        return b if b is not None else b""
    return elf.data[s["off"]:s["off"] + s["size"]]


UINT32_END_MAX = 0x100000000


#: REGIMM ``rt`` selectors that actually encode a PC-relative branch:
#: ``bltz``/``bgez`` (0/1), their likely forms (2/3), and the linking
#: ``bltzal``/``bgezal``/``bltzall``/``bgezall`` (0x10-0x13).  Every other
#: REGIMM ``rt`` is a trap-immediate (``tgei``, ``tlti``, ``teqi``, ...) or
#: ``synci``, whose immediate field is a trap code -- decoding it as a branch
#: displacement manufactures an edge that does not exist.  ``trace_function``
#: above still decodes those trap forms as branches: narrowing its notion of a
#: branch would move recovered function extents, which is a separate change.
#: (It does treat a linking branch to a callable as a call, #968.)  The
#: evidence census below has no such coupling and is therefore exact.
REGIMM_BRANCH_RT = frozenset({0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13})


CFG_SCHEMA_VERSION = 2


CFG_SUCCESSOR_CLASSIFICATIONS = frozenset({
    "same-region",
    "continuation-resume-entry",
    "inter-function-handoff-dispatch",
    "interpreter-fail-closed-boundary",
    "invalid-unowned",
})


def _cfg_field(value, name, default=None):
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def _cfg_normalize_ranges(ranges):
    if ranges is None:
        return []
    normalized = []
    try:
        iterator = iter(ranges)
    except TypeError as exc:
        raise ValueError("CFG ranges must be an iterable of (start, end) pairs") from exc
    for item in iterator:
        if not isinstance(item, (tuple, list)) or len(item) != 2:
            raise ValueError(f"invalid CFG range: {item!r}")
        lo, hi = item
        if (
            type(lo) is not int or type(hi) is not int
            or lo < 0 or hi < 0 or hi > UINT32_END_MAX
        ):
            raise ValueError(f"invalid CFG range: {item!r}")
        if hi > lo:
            normalized.append((lo, hi))
    merged = []
    for lo, hi in sorted(normalized):
        if merged and lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def _cfg_ranges_for(image, ranges):
    if ranges is not None:
        return _cfg_normalize_ranges(ranges)
    intervals = getattr(image, "executable_intervals", ())
    return _cfg_normalize_ranges(
        ((_cfg_field(span, "start"), _cfg_field(span, "end")) for span in intervals)
    )


def _cfg_opcode_identity(word):
    op = word >> 26
    funct = word & 0x3F
    names = {
        0x00: "special", 0x01: "regimm", 0x02: "j", 0x03: "jal",
        0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
        0x0F: "lui", 0x11: "cop1", 0x12: "cop2", 0x14: "beql", 0x15: "bnel",
        0x16: "blezl", 0x17: "bgtzl",
    }
    if op == 0 and funct == 8:
        mnemonic = "jr"
    elif op == 0 and funct == 9:
        mnemonic = "jalr"
    elif op == 1:
        mnemonic = {
            0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl",
            0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
        }.get((word >> 16) & 0x1F, "regimm-other")
    elif op in (0x11, 0x12) and ((word >> 21) & 0x1F) == 8:
        prefix = "bc1" if op == 0x11 else "bc2"
        mnemonic = {
            0: f"{prefix}f", 1: f"{prefix}t", 2: f"{prefix}fl", 3: f"{prefix}tl",
        }.get((word >> 16) & 0x1F, prefix)
    else:
        mnemonic = names.get(op, f"op_{op:02x}")
    return {"op": op, "funct": funct, "mnemonic": mnemonic, "word": word & 0xFFFFFFFF}


def _cfg_branch_likely(word):
    op = word >> 26
    if op == 1:
        return ((word >> 16) & 0x1F) in (2, 3, 0x12, 0x13)
    if op in (20, 21, 22, 23):
        return True
    return op in (0x11, 0x12) and ((word >> 21) & 0x1F) == 8 and ((word >> 16) & 0x1F) in (2, 3)


def _cfg_is_eret(word):
    return word == 0x42000018


def _cfg_control_transfer(word):
    op = word >> 26
    funct = word & 0x3F
    if op in (2, 3):
        return True
    if op == 1 and ((word >> 16) & 0x1F) in REGIMM_BRANCH_RT:
        return True
    if op in (4, 5, 6, 7, 20, 21, 22, 23):
        return True
    if op in (0x11, 0x12) and ((word >> 21) & 0x1F) == 8:
        return True
    return op == 0 and funct in (8, 9)


def _cfg_section_blob(elf, section):
    name = _cfg_field(section, "name", _cfg_field(section, "nm", ""))
    if isinstance(section, dict):
        name = section.get("nm", name)
        try:
            return name, section_bytes(elf, section)
        except (KeyError, TypeError):
            return name, b""
    address = _cfg_field(section, "address", 0)
    size = _cfg_field(section, "size", 0)
    blob = elf.read_at_vaddr(address, size) if size else b""
    return name, blob or b""


_CFG_NONE = 0x100000000


_CFG_KIND_NAMES = (
    "delay-slot",
    "direct-jump",
    "call",
    "fallthrough",
    "direct-branch",
    "branch",
    "unresolved-indirect",
    "tail-call",
)


_CFG_KIND_IDS = {name: index for index, name in enumerate(_CFG_KIND_NAMES)}


_CFG_DETAIL_NAMES = (
    "annulled-when-not-taken",
    "always-executed",
    "j",
    "jal",
    "call-return",
    "linking-branch",
    "unconditional-branch",
    "conditional-branch",
    "branch-not-taken",
    "computed-jump",
    "computed-call",
    "linear",
    "known-entry-tail-transfer",
    "computed-tail-transfer",
    "landed-delay-slot",
)


_CFG_DETAIL_IDS = {name: index for index, name in enumerate(_CFG_DETAIL_NAMES)}


_CFG_REASON_NAMES = ("entry",) + _CFG_KIND_NAMES


_CFG_REASON_BITS = {name: 1 << index for index, name in enumerate(_CFG_REASON_NAMES)}


def _cfg_address_comparable(value):
    """Return whether a value can compare equal to an integer address.

    ``bool`` is included on purpose: these checks compared with ``==``, where
    ``True == 1``, so excluding it would change their findings. A malformed
    address of any type is already reported where it is read.
    """
    return isinstance(value, (int, float))


def _cfg_successor_classifications(report):
    """Classify every CFG edge using only the serialized ownership model."""
    def rows(name):
        # The verifier reports a malformed field; a scalar here must not raise.
        # A string or object still iterates to strings, which every loop below
        # skips exactly as before, so only non-iterables need replacing.
        value = report.get(name, ())
        return value if isinstance(value, Iterable) else ()

    nodes = {
        row.get("address"): row
        for row in rows("instructions")
        if isinstance(row, dict) and type(row.get("address")) is int
    }
    entries = {
        entry for entry in rows("entries")
        if type(entry) is int
    }
    interior = set()
    for row in rows("interior_entries"):
        if not isinstance(row, dict):
            continue
        source, target, kind = row.get("source"), row.get("address"), row.get("reason")
        if type(source) is int and type(target) is int and isinstance(kind, str):
            interior.add((source, target, kind))

    classifications = []
    external_transfer_kinds = {"call", "direct-jump", "direct-branch", "branch", "tail-call"}
    for edge in rows("edges"):
        if not isinstance(edge, dict):
            continue
        source, target, kind = edge.get("source"), edge.get("target"), edge.get("kind")
        source_node = nodes.get(source) if type(source) is int else None
        target_node = nodes.get(target) if type(target) is int else None
        source_owner_values = source_node.get("owners", ()) if source_node else ()
        target_owner_values = target_node.get("owners", ()) if target_node else ()
        source_owners = {
            owner for owner in source_owner_values
            if type(owner) is int
        } if isinstance(source_owner_values, list) else set()
        target_owners = {
            owner for owner in target_owner_values
            if type(owner) is int
        } if isinstance(target_owner_values, list) else set()

        if kind == "unresolved-indirect":
            classification = "interpreter-fail-closed-boundary"
        elif isinstance(kind, str) and kind in ("call", "tail-call"):
            classification = (
                "inter-function-handoff-dispatch"
                if target_node is None or target_owners
                else "invalid-unowned"
            )
        elif target is None:
            classification = "invalid-unowned"
        elif target_node is None:
            classification = (
                "inter-function-handoff-dispatch"
                if isinstance(kind, str) and kind in external_transfer_kinds
                else "invalid-unowned"
            )
        elif (
            _cfg_address_comparable(source) and isinstance(kind, str)
            and (source, target, kind) in interior
        ):
            classification = "continuation-resume-entry"
        elif source_owners.intersection(target_owners):
            classification = "same-region"
        elif (
            target in entries and isinstance(kind, str)
            and kind in external_transfer_kinds
        ):
            classification = "inter-function-handoff-dispatch"
        else:
            classification = "invalid-unowned"

        row = {
            "source": source,
            "target": target,
            "kind": kind,
            "classification": classification,
        }
        if "detail" in edge:
            row["detail"] = edge["detail"]
        classifications.append(row)
    return classifications


class CanonicalCfgState:
    """Compact authoritative CFG state used by the scalable observation path.

    The public JSON report intentionally contains redundant projections so a
    serialized/tampered report can be checked independently.  That shape is
    useful at small scale but is not an appropriate in-process representation
    for a retail image.  This state keeps one compact edge/node/ownership model;
    :meth:`to_report` materializes the legacy schema only when a caller asks for
    it.
    """

    __slots__ = (
        "schema_version", "executable_intervals", "entries", "unmapped_entries",
        "addresses", "words", "terminators", "delay_of", "delay_annulled",
        "edge_sources", "edge_targets", "edge_kinds", "edge_details", "edge_offsets",
        "unreadable_spans", "owner_single", "owner_reason_masks", "multi_owner_masks",
        "interior_entries", "continuation_edges", "interior_targets", "data_spans",
        "jump_table_ownership", "image_entry", "_incoming",
    )

    def __init__(self):
        self.schema_version = CFG_SCHEMA_VERSION
        self.executable_intervals = ()
        self.entries = ()
        self.unmapped_entries = ()
        self.addresses = array("I")
        self.words = array("I")
        self.terminators = bytearray()
        self.delay_of = array("Q")
        self.delay_annulled = bytearray()
        self.edge_sources = array("I")
        self.edge_targets = array("Q")
        self.edge_kinds = bytearray()
        self.edge_details = array("b")
        self.edge_offsets = array("I")
        self.unreadable_spans = []
        self.owner_single = array("Q")
        self.owner_reason_masks = array("H")
        self.multi_owner_masks = {}
        self.interior_entries = []
        self.continuation_edges = []
        self.interior_targets = set()
        self.data_spans = []
        self.jump_table_ownership = []
        self.image_entry = None
        self._incoming = {}

    @property
    def node_count(self):
        return len(self.addresses)

    @property
    def edge_count(self):
        return len(self.edge_sources)

    @property
    def owner_count(self):
        return len(self.entries)

    def _owners_for_index(self, index):
        primary = self.owner_single[index]
        if primary == _CFG_NONE:
            return ()
        extra = self.multi_owner_masks.get(index)
        if not extra:
            return (primary,)
        return tuple(sorted(set(extra) | {primary}))

    def _owner_reason_mask(self, index, owner):
        if self.owner_single[index] == owner:
            return self.owner_reason_masks[index]
        return self.multi_owner_masks[index][owner]

    def _classification(self, index):
        owners = self._owners_for_index(index)
        if not owners:
            return "padding" if self.words[index] == 0 else "unowned-executable"
        if index in self.interior_targets:
            return "interior-entry"
        if len(owners) > 1:
            return "owner-conflict"
        return "owned"

    def _edge_dict(self, edge_index):
        source = self.addresses[self.edge_sources[edge_index]]
        target_value = self.edge_targets[edge_index]
        target = None if target_value == _CFG_NONE else target_value
        row = {
            "source": source,
            "target": target,
            "kind": _CFG_KIND_NAMES[self.edge_kinds[edge_index]],
        }
        detail = self.edge_details[edge_index]
        if detail >= 0:
            row["detail"] = _CFG_DETAIL_NAMES[detail]
        return row

    def _semantic_hash(self):
        digest = hashlib.sha256()
        digest.update(b"canonical-cfg-state/1" + bytes((0,)))
        for start, end in self.executable_intervals:
            digest.update(struct.pack("<QQ", start, end))
        digest.update(struct.pack("<I", len(self.entries)))
        for entry in self.entries:
            digest.update(struct.pack("<I", entry))
        for index, address in enumerate(self.addresses):
            digest.update(struct.pack(
                "<IIQ?QH", address, self.words[index], self.delay_of[index],
                bool(self.delay_annulled[index]), self.owner_single[index],
                self.owner_reason_masks[index],
            ))
            digest.update(struct.pack("<B", self.terminators[index]))
            for owner in self._owners_for_index(index):
                digest.update(struct.pack("<I", owner))
                digest.update(struct.pack("<H", self._owner_reason_mask(index, owner)))
        for index in range(self.edge_count):
            digest.update(struct.pack(
                "<IQBB", self.edge_sources[index], self.edge_targets[index],
                self.edge_kinds[index], self.edge_details[index] & 0xFF,
            ))
        for address, source, owner, reason in self.interior_entries:
            digest.update(struct.pack("<IIII", address, source, owner, reason))
        for source, target, owner in self.continuation_edges:
            digest.update(struct.pack("<III", source, target, owner))
        for start, end, classification in self.unreadable_spans:
            digest.update(struct.pack("<QQ", start, end))
            digest.update(classification.encode("ascii") + bytes((0,)))
        return digest.hexdigest()

    def summary(self):
        edge_classes = {name: 0 for name in _CFG_KIND_NAMES}
        for kind in self.edge_kinds:
            edge_classes[_CFG_KIND_NAMES[kind]] += 1
        conflict_count = len(self.multi_owner_masks)
        owned_nodes = sum(1 for index in range(self.node_count) if self.owner_single[index] != _CFG_NONE)
        return {
            "schema_version": self.schema_version,
            "instruction_count": self.node_count,
            "node_count": self.node_count,
            "edge_count": self.edge_count,
            "edges_by_class": edge_classes,
            "owner_count": self.owner_count,
            "owned_node_count": owned_nodes,
            "continuation_count": len(self.continuation_edges),
            "conflict_count": conflict_count,
            "unreadable_span_count": len(self.unreadable_spans),
            "data_span_count": len(self.data_spans),
            "jump_table_count": len(self.jump_table_ownership),
            "summary_hash": self._semantic_hash(),
        }

    def to_report(self):
        """Materialize the deterministic legacy report schema on demand."""
        instructions = []
        edges_by_node = [[] for _ in range(self.node_count)]
        edge_rows = []
        for edge_index in range(self.edge_count):
            row = self._edge_dict(edge_index)
            edge_rows.append(row)
            edges_by_node[self.edge_sources[edge_index]].append(row)

        for index, address in enumerate(self.addresses):
            owners = list(self._owners_for_index(index))
            owner_reasons = [
                {
                    "owner": owner,
                    "reasons": sorted(
                        name for name, bit in _CFG_REASON_BITS.items()
                        if self._owner_reason_mask(index, owner) & bit
                    ),
                }
                for owner in owners
            ]
            node = {
                "address": address,
                "word": self.words[index],
                "opcode": _cfg_opcode_identity(self.words[index]),
                "delay_slot_of": None if self.delay_of[index] == _CFG_NONE else self.delay_of[index],
                "delay_slot_annulled_when_not_taken": bool(self.delay_annulled[index]),
                "branch_likely": _cfg_branch_likely(self.words[index]),
                "content_classification": "padding" if self.words[index] == 0 else "instruction-word",
                "edges": sorted(
                    edges_by_node[index],
                    key=lambda edge: (
                        edge["kind"],
                        edge["target"] is None,
                        edge["target"] or 0,
                        edge.get("detail", ""),
                    ),
                ),
                "classification": self._classification(index),
                "owners": owners,
                "owner_reasons": owner_reasons,
            }
            if self.terminators[index] == 1:
                node["terminator"] = "return"
            elif self.terminators[index] == 2:
                node["terminator"] = "indirect-jump"
            elif self.terminators[index] == 3:
                node["terminator"] = "eret"
            instructions.append(node)

        direct_edges = [edge for edge in edge_rows if edge["target"] is not None]
        call_edges = [edge for edge in edge_rows if edge["kind"] == "call"]
        tail_call_edges = [edge for edge in edge_rows if edge["kind"] == "tail-call"]
        fallthrough_edges = [edge for edge in edge_rows if edge["kind"] == "fallthrough"]
        unresolved = [edge for edge in edge_rows if edge["kind"] == "unresolved-indirect"]
        byte_classification = [
            {"start": address, "end": address + 4, "classification": self._classification(index)}
            for index, address in enumerate(self.addresses)
        ]
        byte_classification.extend(
            {"start": start, "end": end, "classification": classification}
            for start, end, classification in self.unreadable_spans
        )
        byte_classification.sort(key=lambda item: (item["start"], item["end"], item["classification"]))
        conflicts = [
            {
                "address": self.addresses[index],
                "owners": list(self._owners_for_index(index)),
                "reason": "multiple-entry-reachability",
            }
            for index in sorted(self.multi_owner_masks, key=lambda item: self.addresses[item])
        ]
        report = {
            "schema_version": self.schema_version,
            "executable_intervals": [
                {"start": start, "end": end} for start, end in self.executable_intervals
            ],
            "entries": list(self.entries),
            "unmapped_entries": list(self.unmapped_entries),
            "instructions": instructions,
            "edges": sorted(edge_rows, key=lambda edge: (
                edge["source"], edge["kind"], edge["target"] is None, edge["target"] or 0, edge.get("detail", ""),
            )),
            "direct_edges": sorted(direct_edges, key=lambda edge: (
                edge["source"], edge["kind"], edge["target"] or 0, edge.get("detail", ""),
            )),
            "call_edges": sorted(call_edges, key=lambda edge: (edge["source"], edge["target"] or 0, edge.get("detail", ""))),
            "tail_call_edges": sorted(tail_call_edges, key=lambda edge: (edge["source"], edge["target"] or 0, edge.get("detail", ""))),
            "fallthrough_edges": sorted(fallthrough_edges, key=lambda edge: (edge["source"], edge["target"] or 0, edge.get("detail", ""))),
            "continuation_edges": [
                {"source": source, "target": target, "owner": owner}
                for source, target, owner in sorted(self.continuation_edges)
            ],
            "interior_entries": [
                {"address": address, "source": source, "owner": owner, "reason": _CFG_KIND_NAMES[reason]}
                for address, source, owner, reason in sorted(self.interior_entries)
            ],
            "unresolved_indirect_edges": sorted(unresolved, key=lambda edge: (edge["source"], edge.get("detail", ""))),
            "ownership_conflicts": conflicts,
            "ownership_map": [
                {
                    "address": address,
                    "classification": self._classification(index),
                    "owners": list(self._owners_for_index(index)),
                }
                for index, address in enumerate(self.addresses)
            ],
            "byte_classification": byte_classification,
            "padding": [
                address for index, address in enumerate(self.addresses)
                if self.words[index] == 0
            ],
            "unowned_executable": [
                address for index, address in enumerate(self.addresses)
                if self._classification(index) == "unowned-executable"
            ],
            "data_spans": [
                {"start": start, "end": end, "section": section, "classification": "data"}
                for start, end, section in sorted(self.data_spans)
            ],
            "jump_table_ownership": [
                {
                    "address": address,
                    "section": section,
                    "target": target,
                    "owners": list(owners),
                    "classification": "jump-table-candidate",
                }
                for address, section, target, owners in sorted(self.jump_table_ownership)
            ],
        }
        report["successor_classifications"] = _cfg_successor_classifications(report)
        return report


def canonical_cfg_state(image, ranges=None, entries=None):
    """Build compact canonical CFG semantic state without report projections."""
    # The branch decode stays in analyze.py, where tests patch its constants; analyze.py imports
    # this module, so the names are read here, when the walk runs, and not at import time.
    from analyze import BRANCH_LINK, BRANCH_UNCOND, branch_kind, branch_target
    reader = getattr(image, "read_at_vaddr", None)
    if not callable(reader):
        raise ValueError("CFG image must provide read_at_vaddr(address, size)")
    state = CanonicalCfgState()
    effective_ranges = _cfg_ranges_for(image, ranges)
    state.executable_intervals = tuple(effective_ranges)
    entry_values = entries
    if entry_values is None:
        entry_value = getattr(image, "entry_point", None)
        entry_values = [] if entry_value is None else [entry_value]
    try:
        entry_candidates = list(entry_values)
    except TypeError as exc:
        raise ValueError("CFG entries must be an iterable of guest addresses") from exc
    if any(
        type(value) is not int or value < 0 or value > UINT32_END_MAX - 1
        for value in entry_candidates
    ):
        raise ValueError("CFG entries must be 32-bit guest addresses")

    node_index = {}
    for lo, hi in effective_ranges:
        for address in range(lo, hi, 4):
            raw = reader(address, min(4, hi - address))
            if raw is not None and len(raw) == 4:
                node_index[address] = len(state.addresses)
                state.addresses.append(address)
                state.words.append(int.from_bytes(raw, "little"))
                state.terminators.append(0)
                state.delay_of.append(_CFG_NONE)
                state.delay_annulled.append(0)
            else:
                state.unreadable_spans.append((
                    address,
                    min(address + 4, hi),
                    "partial-executable" if raw else "unreadable-executable",
                ))

    entry_set = {value for value in entry_candidates if value in node_index}
    state.entries = tuple(sorted(entry_set))
    state.unmapped_entries = tuple(sorted(set(entry_candidates) - entry_set))
    entry_set = set(state.entries)
    state.owner_single = array("Q", [_CFG_NONE]) * state.node_count
    state.owner_reason_masks = array("H", [0]) * state.node_count

    edge_counts = array("I", [0]) * state.node_count

    def add_edge(source_index, target, kind, detail=None):
        state.edge_sources.append(source_index)
        state.edge_targets.append(_CFG_NONE if target is None else target)
        state.edge_kinds.append(_CFG_KIND_IDS[kind])
        state.edge_details.append(-1 if detail is None else _CFG_DETAIL_IDS[detail])
        edge_counts[source_index] += 1

    # Words a direct jump, call or branch lands on. Landing on a delay slot runs it as an
    # ordinary instruction, and execution continues at the word after it.
    landed = set()
    for address, word in zip(state.addresses, state.words, strict=True):
        if word >> 26 in (2, 3):
            landed.add(((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
        elif branch_target(address, word) is not None:
            landed.add(branch_target(address, word))

    for source_index, address in enumerate(state.addresses):
        word = state.words[source_index]
        control = _cfg_control_transfer(word)
        next_index = node_index.get(address + 4)
        if control and next_index is not None:
            state.delay_of[next_index] = address
            state.delay_annulled[next_index] = int(_cfg_branch_likely(word))
            add_edge(
                source_index,
                address + 4,
                "delay-slot",
                "annulled-when-not-taken" if _cfg_branch_likely(word) else "always-executed",
            )

        op = word >> 26
        funct = word & 0x3F
        if _cfg_is_eret(word):
            state.terminators[source_index] = 3
        elif op == 2:
            target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            add_edge(source_index, target, "direct-jump", "j")
        elif op == 3:
            target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            add_edge(source_index, target, "call", "jal")
            if node_index.get(address + 8) is not None:
                add_edge(source_index, address + 8, "fallthrough", "call-return")
        else:
            target = branch_target(address, word)
            if target is not None:
                kind = branch_kind(word)
                if kind == BRANCH_LINK:
                    add_edge(source_index, target, "call", "linking-branch")
                    if node_index.get(address + 8) is not None:
                        add_edge(source_index, address + 8, "fallthrough", "call-return")
                elif kind == BRANCH_UNCOND:
                    add_edge(source_index, target, "direct-branch", "unconditional-branch")
                else:
                    add_edge(source_index, target, "branch", "conditional-branch")
                    if node_index.get(address + 8) is not None:
                        add_edge(source_index, address + 8, "fallthrough", "branch-not-taken")
            elif op == 0 and funct == 8:
                rs = (word >> 21) & 0x1F
                if rs == 31:
                    state.terminators[source_index] = 1
                else:
                    add_edge(source_index, None, "unresolved-indirect", "computed-jump")
                    state.terminators[source_index] = 2
            elif op == 0 and funct == 9:
                if (word >> 11) & 0x1F:
                    add_edge(source_index, None, "unresolved-indirect", "computed-call")
                    if node_index.get(address + 8) is not None:
                        add_edge(source_index, address + 8, "fallthrough", "call-return")
                else:
                    add_edge(
                        source_index, None, "unresolved-indirect", "computed-tail-transfer"
                    )
            elif state.delay_of[source_index] == _CFG_NONE and next_index is not None:
                add_edge(source_index, address + 4, "fallthrough", "linear")
            elif next_index is not None and address in landed:
                add_edge(source_index, address + 4, "fallthrough", "landed-delay-slot")

    # A direct jump/branch to a known entry is a tail transfer.  Keep this as a
    # post-pass so all edge kinds use the same entry qualification.
    for edge_index, target in enumerate(state.edge_targets):
        if (
            target in entry_set
            and state.edge_kinds[edge_index] in {
                _CFG_KIND_IDS["direct-jump"], _CFG_KIND_IDS["direct-branch"],
            }
        ):
            state.edge_kinds[edge_index] = _CFG_KIND_IDS["tail-call"]
            state.edge_details[edge_index] = _CFG_DETAIL_IDS["known-entry-tail-transfer"]

    state.edge_offsets = array("I", [0])
    running = 0
    for count in edge_counts:
        running += count
        state.edge_offsets.append(running)

    def record_owner(index, owner, reason):
        bit = _CFG_REASON_BITS[reason]
        primary = state.owner_single[index]
        if primary == _CFG_NONE:
            state.owner_single[index] = owner
            state.owner_reason_masks[index] = bit
        elif primary == owner:
            state.owner_reason_masks[index] |= bit
        else:
            extra = state.multi_owner_masks.setdefault(index, {primary: state.owner_reason_masks[index]})
            extra[owner] = extra.get(owner, 0) | bit

    # Traversal uses CSR offsets rather than a second dict/list of edge rows.
    for entry in state.entries:
        entry_index = node_index[entry]
        stack = [(entry_index, "entry")]
        seen = set()
        while stack:
            index, reason = stack.pop()
            record_owner(index, entry, reason)
            if index in seen:
                continue
            seen.add(index)
            for edge_index in range(state.edge_offsets[index], state.edge_offsets[index + 1]):
                target = state.edge_targets[edge_index]
                if target == _CFG_NONE:
                    continue
                kind_id = state.edge_kinds[edge_index]
                if kind_id in {
                    _CFG_KIND_IDS["call"], _CFG_KIND_IDS["tail-call"],
                    _CFG_KIND_IDS["unresolved-indirect"],
                }:
                    continue
                target_index = node_index.get(target)
                if target_index is None:
                    continue
                if target in entry_set and target != entry:
                    source = state.addresses[index]
                    state._incoming.setdefault(target_index, set()).add((source, entry, kind_id))
                    continue
                stack.append((target_index, _CFG_KIND_NAMES[kind_id]))

    incoming = state._incoming
    for target_index, records in sorted(incoming.items(), key=lambda item: state.addresses[item[0]]):
        target = state.addresses[target_index]
        state.interior_targets.add(target_index)
        for source, owner, kind_id in sorted(records):
            state.interior_entries.append((target, source, owner, kind_id))
            if kind_id == _CFG_KIND_IDS["fallthrough"]:
                state.continuation_edges.append((source, target, owner))
    state._incoming.clear()

    # Data/jump-table evidence is a derived view, not part of the node graph.
    for section in getattr(image, "sections", ()):
        name, blob = _cfg_section_blob(image, section)
        section_type = _cfg_field(section, "section_type", _cfg_field(section, "typ", None))
        section_flags = _cfg_field(section, "flags", 0) or 0
        if section_type != 1 or not blob or section_flags & 4 or name in (".text", ".sceStub.text"):
            continue
        start = _cfg_field(section, "address", _cfg_field(section, "addr", 0))
        end = start + len(blob)
        state.data_spans.append((start, end, name))
        for offset in range(0, len(blob) - 3, 4):
            value = int.from_bytes(blob[offset:offset + 4], "little")
            target_index = node_index.get(value)
            if target_index is not None:
                state.jump_table_ownership.append((
                    start + offset,
                    name,
                    value,
                    state._owners_for_index(target_index),
                ))

    state.interior_entries.sort()
    state.continuation_edges.sort()
    state.data_spans.sort()
    state.jump_table_ownership.sort()
    return state


def verify_canonical_cfg_state(state):
    """Verify compact in-process state with linear/near-linear checks."""
    findings = []
    if not isinstance(state, CanonicalCfgState) or state.schema_version != CFG_SCHEMA_VERSION:
        return [{"code": "schema-version", "message": "unsupported CFG state schema"}]
    if any(start < 0 or end <= start or end > UINT32_END_MAX for start, end in state.executable_intervals):
        findings.append({"code": "interval-invalid", "message": repr(state.executable_intervals)})
    if any(
        index > 0 and state.addresses[index - 1] >= address
        for index, address in enumerate(state.addresses)
    ):
        findings.append({"code": "node-order", "message": "node addresses are not strictly increasing"})
    node_index = {address: index for index, address in enumerate(state.addresses)}
    for index, address in enumerate(state.addresses):
        if not any(start <= address < end for start, end in state.executable_intervals):
            findings.append({"code": "node-outside-executable", "message": repr(address)})
        delay = state.delay_of[index]
        if delay != _CFG_NONE:
            predecessor_index = node_index.get(delay)
            if predecessor_index is None or not _cfg_control_transfer(state.words[predecessor_index]):
                findings.append({"code": "delay-slot-owner-invalid", "message": repr(address)})
    if len(state.edge_offsets) != state.node_count + 1 or state.edge_offsets[-1:] != array("I", [state.edge_count]):
        findings.append({"code": "edge-offsets", "message": "edge CSR offsets are inconsistent"})
    allowed_external = {
        _CFG_KIND_IDS[name] for name in ("call", "direct-jump", "direct-branch", "branch", "tail-call")
    }
    for edge_index in range(state.edge_count):
        source_index = state.edge_sources[edge_index]
        target = state.edge_targets[edge_index]
        kind = state.edge_kinds[edge_index]
        if source_index >= state.node_count or kind >= len(_CFG_KIND_NAMES):
            findings.append({"code": "edge-invalid", "message": str(edge_index)})
            continue
        if target != _CFG_NONE and target not in node_index and kind not in allowed_external:
            findings.append({"code": "edge-target-invalid", "message": repr(target)})
    for index, _extra in state.multi_owner_masks.items():
        owners = state._owners_for_index(index)
        if index >= state.node_count or len(owners) < 2:
            findings.append({"code": "conflict-invalid", "message": repr(index)})
    return sorted(findings, key=lambda item: (item["code"], item["message"]))


def canonical_cfg_summary(image_or_state, ranges=None, entries=None):
    """Return a deterministic compact summary without materializing full JSON."""
    state = image_or_state if isinstance(image_or_state, CanonicalCfgState) else canonical_cfg_state(
        image_or_state, ranges=ranges, entries=entries
    )
    return state.summary()


STATIC_COVERAGE_SCHEMA_VERSION = 1


def static_coverage(image, ranges, entries, translated=(), interpreted=()):
    """Word-level static coverage of one image, from the canonical CFG.

    ``entries`` are the function entries code generation used. ``translated`` are the
    entries whose AOT body was emitted as C; ``interpreted`` are the entries left to the
    interpreter floor (an unsupported form, or an entry omitted from AOT). An owned word
    counts as translated when at least one of its owners was translated, and as
    interpreter-only when every owner was left to the floor; owned words of an owner in
    neither set are ``other_owned_words`` (an import stub, say), so the three always add
    up to ``owned_words``. Unowned executable words are what the analyzer could not
    attribute to any function: the dispatcher reaches them only as a miss.
    """
    state = canonical_cfg_state(image, ranges=ranges, entries=sorted(set(entries)))
    translated = frozenset(translated)
    interpreted = frozenset(interpreted) - translated
    classes = {"owned": 0, "interior-entry": 0, "owner-conflict": 0,
               "unowned-executable": 0, "padding": 0}
    translated_words = interpreter_words = 0
    for index in range(state.node_count):
        classes[state._classification(index)] += 1
        owners = state._owners_for_index(index)
        if not owners:
            continue
        if any(owner in translated for owner in owners):
            translated_words += 1
        elif all(owner in interpreted for owner in owners):
            interpreter_words += 1
    owned = classes["owned"] + classes["interior-entry"] + classes["owner-conflict"]
    owners = set(state.entries)
    unresolved = sum(1 for kind in state.edge_kinds if _CFG_KIND_NAMES[kind] == "unresolved-indirect")
    return {
        "schema_version": STATIC_COVERAGE_SCHEMA_VERSION,
        "executable_words": state.node_count,
        "executable_bytes": state.node_count * 4,
        "owned_words": owned,
        "owned_bytes": owned * 4,
        "unowned_words": classes["unowned-executable"],
        "unowned_bytes": classes["unowned-executable"] * 4,
        "padding_words": classes["padding"],
        "owner_conflict_words": classes["owner-conflict"],
        "interior_entry_words": classes["interior-entry"],
        "translated_words": translated_words,
        "interpreter_words": interpreter_words,
        "other_owned_words": owned - translated_words - interpreter_words,
        "functions": len(owners),
        "functions_translated": len(owners & translated),
        "functions_interpreter": len(owners & interpreted),
        "jump_table_candidates": len(state.jump_table_ownership),
        "unresolved_indirect_edges": unresolved,
    }


def canonical_cfg_report(image, ranges=None, entries=None):
    """Return the legacy deterministic report, materialized on demand."""
    return canonical_cfg_state(image, ranges=ranges, entries=entries).to_report()


def verify_canonical_cfg_report(report):
    """Return deterministic structural findings for a CFG report.

    Findings are intentionally data rather than automatic repairs. The verifier
    is also a trust boundary for serialized reports, so malformed JSON-shaped
    data produces findings instead of an incidental ``AttributeError`` or
    ``TypeError``.
    """
    findings = []
    if not isinstance(report, dict) or report.get("schema_version") != CFG_SCHEMA_VERSION:
        return [{"code": "schema-version", "message": "unsupported CFG report schema"}]

    def finding(code, value):
        findings.append({"code": code, "message": repr(value)})

    def known(value, names):
        """Test a string-name set without hashing an unhashable JSON value."""
        return isinstance(value, str) and value in names

    def owner_projection(owners):
        """Project node owners for the ownership-map comparison.

        A non-list value is already a node-owners-invalid finding. An iterable
        keeps the tuple() comparison it always had. A scalar such as 5 or None
        cannot become a tuple, so it is compared as-is: it never equals a row's
        owner tuple, which keeps the projection mismatch instead of raising.
        """
        return tuple(owners) if isinstance(owners, Iterable) else owners

    def list_field(name):
        value = report.get(name, [])
        if not isinstance(value, list):
            finding(f"{name}-type", value)
            return []
        return value

    intervals = list_field("executable_intervals")
    previous_end = None
    expected_spans = {}
    for interval in intervals:
        if not isinstance(interval, dict):
            finding("interval-invalid", interval)
            continue
        start, end = interval.get("start"), interval.get("end")
        if (
            type(start) is not int or type(end) is not int
            or start < 0 or end <= start or end > UINT32_END_MAX
            or start & 3
        ):
            finding("interval-invalid", interval)
            continue
        if previous_end is not None and start < previous_end:
            finding("interval-overlap", interval)
        previous_end = end if previous_end is None else max(previous_end, end)
        for address in range(start, end, 4):
            expected_end = min(address + 4, end)
            if address in expected_spans:
                finding("interval-overlap", interval)
            expected_spans[address] = expected_end

    interval_addresses = set(expected_spans)
    entries = list_field("entries")
    for entry in entries:
        if type(entry) is not int or entry < 0 or entry > UINT32_END_MAX - 1:
            finding("entry-invalid", entry)
        elif entry not in interval_addresses:
            finding("entry-outside-executable", entry)
    unmapped_entries = list_field("unmapped_entries")
    for entry in unmapped_entries:
        if type(entry) is not int or entry < 0 or entry > UINT32_END_MAX - 1:
            finding("entry-invalid", entry)
        elif entry in interval_addresses:
            finding("unmapped-entry-present", entry)

    instructions = list_field("instructions")
    seen = set()
    node_by_address = {}
    node_edge_keys = []
    allowed_classifications = {
        "owned", "interior-entry", "owner-conflict", "unowned-executable", "padding",
    }
    allowed_edge_kinds = {
        "delay-slot", "direct-jump", "call", "fallthrough", "direct-branch",
        "branch", "unresolved-indirect", "tail-call",
    }
    external_edge_kinds = {"call", "direct-jump", "direct-branch", "branch", "tail-call"}
    for node in instructions:
        if not isinstance(node, dict):
            finding("node-invalid", node)
            continue
        address = node.get("address")
        if type(address) is not int:
            finding("node-address-invalid", node)
            continue
        if address in seen:
            finding("duplicate-node", f"0x{address:08x}")
        seen.add(address)
        node_by_address[address] = node
        if address not in interval_addresses:
            finding("node-outside-executable", node)
        if not known(node.get("classification"), allowed_classifications):
            finding("node-unclassified", node)
        node_edges = node.get("edges", [])
        if not isinstance(node_edges, list):
            finding("node-edges-type", node)
            node_edges = []
        for edge in node_edges:
            if not isinstance(edge, dict):
                finding("edge-invalid", edge)
                continue
            source = edge.get("source")
            target = edge.get("target")
            kind = edge.get("kind")
            if source != address:
                finding("edge-source-mismatch", edge)
            if not known(kind, allowed_edge_kinds):
                finding("edge-kind-invalid", edge)
            if target is not None and (
                type(target) is not int or target < 0 or target > UINT32_END_MAX - 1
            ):
                finding("edge-target-invalid", edge)
                continue
            if (
                target is not None and target not in interval_addresses
                and not known(kind, external_edge_kinds)
            ):
                finding("edge-target-invalid", edge)
            if type(source) is int and (target is None or type(target) is int) and isinstance(kind, str):
                node_edge_keys.append((source, target, kind, edge.get("detail")))

    byte_rows = list_field("byte_classification")
    byte_starts = set()
    allowed_byte_classifications = allowed_classifications | {
        "unreadable-executable", "partial-executable",
    }
    for row in byte_rows:
        if not isinstance(row, dict):
            finding("byte-classification-invalid", row)
            continue
        start, end = row.get("start"), row.get("end")
        if type(start) is not int or type(end) is not int:
            finding("byte-classification-invalid", row)
            continue
        expected_end = expected_spans.get(start)
        if expected_end is None:
            finding("byte-classification-outside-executable", row)
        elif end != expected_end:
            finding("byte-classification-span-mismatch", row)
        if start in byte_starts:
            finding("duplicate-byte-classification", row)
        byte_starts.add(start)
        if not known(row.get("classification"), allowed_byte_classifications):
            finding("byte-classification-invalid", row)
    if byte_starts != interval_addresses:
        findings.append({
            "code": "executable-coverage-gap",
            "message": f"expected {len(interval_addresses)} spans, classified {len(byte_starts)}",
        })

    global_edges = list_field("edges")
    global_edge_keys = []
    for edge in global_edges:
        if not isinstance(edge, dict):
            finding("edge-invalid", edge)
            continue
        source, target, kind = edge.get("source"), edge.get("target"), edge.get("kind")
        if type(source) is not int or source not in node_by_address:
            finding("edge-source-missing", edge)
        if target is not None and (
            type(target) is not int or target < 0 or target > UINT32_END_MAX - 1
        ):
            finding("edge-target-invalid", edge)
        elif target in interval_addresses and target not in node_by_address:
            finding("edge-target-missing", edge)
        elif (
            target not in interval_addresses and not known(kind, external_edge_kinds)
            and target is not None
        ):
            finding("edge-target-invalid", edge)
        if type(source) is int and (target is None or type(target) is int) and isinstance(kind, str):
            global_edge_keys.append((source, target, kind, edge.get("detail")))
    edge_sort_key = lambda item: (item[0], item[2], item[1] is None, item[1] or 0, str(item[3]))
    if sorted(node_edge_keys, key=edge_sort_key) != sorted(global_edge_keys, key=edge_sort_key):
        findings.append({
            "code": "edge-projection-mismatch",
            "message": "instruction edge rows and top-level edge rows differ",
        })

    successor_rows = list_field("successor_classifications")
    for successor in successor_rows:
        if not isinstance(successor, dict):
            finding("successor-classification-invalid", successor)
            continue
        if not known(successor.get("classification"), CFG_SUCCESSOR_CLASSIFICATIONS):
            finding("successor-classification-invalid", successor)
    if successor_rows != _cfg_successor_classifications(report):
        finding(
            "successor-classification-coverage",
            "successor classifications do not cover the canonical edge set",
        )

    def edge_projection(name):
        """Normalize one duplicated edge projection without trusting its shape."""
        rows = list_field(name)
        result = []
        for edge in rows:
            if not isinstance(edge, dict):
                finding(f"{name}-edge-invalid", edge)
                continue
            source, target, kind = edge.get("source"), edge.get("target"), edge.get("kind")
            if type(source) is not int or (target is not None and type(target) is not int) or not isinstance(kind, str):
                finding(f"{name}-edge-invalid", edge)
                continue
            if kind not in allowed_edge_kinds:
                finding(f"{name}-edge-kind-invalid", edge)
            result.append((source, target, kind, edge.get("detail")))
        return sorted(result, key=edge_sort_key)

    expected_projections = {
        "direct_edges": [edge for edge in global_edge_keys if edge[1] is not None],
        "call_edges": [edge for edge in global_edge_keys if edge[2] == "call"],
        "tail_call_edges": [edge for edge in global_edge_keys if edge[2] == "tail-call"],
        "fallthrough_edges": [edge for edge in global_edge_keys if edge[2] == "fallthrough"],
        "unresolved_indirect_edges": [edge for edge in global_edge_keys if edge[2] == "unresolved-indirect"],
    }
    for name, expected in expected_projections.items():
        if edge_projection(name) != sorted(expected, key=edge_sort_key):
            finding(f"{name}-projection-mismatch", report.get(name))

    interior_rows = list_field("interior_entries")
    interior_keys = []
    interior_addresses = set()
    valid_entries = {entry for entry in entries if type(entry) is int}
    # Membership index for the per-row edge check; scanning the edge list for
    # each interior entry made verification quadratic in image size.
    global_edge_triples = {
        (source, target, kind) for source, target, kind, _detail in global_edge_keys
    }
    for item in interior_rows:
        if not isinstance(item, dict):
            finding("interior-entry-invalid", item)
            continue
        address, source, owner, reason = (
            item.get("address"), item.get("source"), item.get("owner"), item.get("reason")
        )
        if type(address) is not int or type(source) is not int or type(owner) is not int or not isinstance(reason, str):
            finding("interior-entry-invalid", item)
            continue
        if address not in node_by_address or source not in node_by_address:
            finding("interior-entry-node-missing", item)
        if owner not in valid_entries:
            finding("interior-entry-owner-missing", item)
        if (source, address, reason) not in global_edge_triples:
            finding("interior-entry-edge-missing", item)
        interior_keys.append((address, source, owner, reason))
        interior_addresses.add(address)

    continuation_rows = list_field("continuation_edges")
    continuation_keys = []
    for item in continuation_rows:
        if not isinstance(item, dict):
            finding("continuation-invalid", item)
            continue
        source, target, owner = item.get("source"), item.get("target"), item.get("owner")
        if type(source) is not int or type(target) is not int or type(owner) is not int:
            finding("continuation-invalid", item)
            continue
        continuation_keys.append((source, target, owner))
    expected_continuations = sorted(
        (source, address, owner)
        for address, source, owner, reason in interior_keys
        if reason == "fallthrough"
    )
    if sorted(continuation_keys) != expected_continuations:
        finding("continuation-projection-mismatch", report.get("continuation_edges"))

    delay_edge_pairs = {
        (source, target)
        for source, target, kind, _detail in global_edge_keys
        if kind == "delay-slot" and target is not None
    }
    delay_targets = {target for _source, target in delay_edge_pairs}
    for address, node in node_by_address.items():
        branch_likely = node.get("branch_likely")
        if type(branch_likely) is not bool:
            finding("branch-likely-invalid", node)
        predecessor = node.get("delay_slot_of")
        if predecessor is None:
            if address in delay_targets:
                finding("delay-slot-projection-mismatch", node)
            continue
        if type(predecessor) is not int or predecessor not in node_by_address:
            finding("delay-slot-owner-invalid", node)
            continue
        predecessor_node = node_by_address[predecessor]
        predecessor_word = predecessor_node.get("word")
        if type(predecessor_word) is not int or not _cfg_control_transfer(predecessor_word):
            finding("delay-slot-owner-invalid", node)
        if (predecessor, address) not in delay_edge_pairs:
            finding("delay-slot-projection-mismatch", node)
        expected_annulled = bool(predecessor_node.get("branch_likely"))
        if node.get("delay_slot_annulled_when_not_taken") != expected_annulled:
            finding("delay-slot-annulment-mismatch", node)

    for source, target in sorted(delay_edge_pairs):
        target_node = node_by_address.get(target)
        if target_node is None or target_node.get("delay_slot_of") != source:
            finding("delay-slot-projection-mismatch", {"source": source, "target": target})

    expected_padding = sorted(
        address for address, node in node_by_address.items()
        if node.get("content_classification") == "padding"
    )
    actual_padding = report.get("padding", [])
    if not isinstance(actual_padding, list) or actual_padding != expected_padding:
        finding("padding-projection-mismatch", actual_padding)
    expected_unowned = sorted(
        address for address, node in node_by_address.items()
        if node.get("classification") == "unowned-executable"
    )
    actual_unowned = report.get("unowned_executable", [])
    if not isinstance(actual_unowned, list) or actual_unowned != expected_unowned:
        finding("unowned-projection-mismatch", actual_unowned)

    conflicts = list_field("ownership_conflicts")
    actual_conflict_addresses = []
    for conflict in conflicts:
        if not isinstance(conflict, dict):
            finding("conflict-invalid", conflict)
            continue
        address = conflict.get("address")
        owners = conflict.get("owners")
        if type(address) is not int or address not in node_by_address:
            finding("conflict-node-missing", conflict)
        if not isinstance(owners, list) or len(owners) < 2:
            finding("conflict-underreported", conflict)
        else:
            actual_conflict_addresses.append(address)
    expected_conflict_addresses = sorted(
        address for address, node in node_by_address.items()
        if isinstance(node.get("owners"), list) and len(node["owners"]) > 1
    )
    # Any other value already has a conflict-node-missing finding and can never
    # equal a node address; it must not be ordered against integers.
    if (
        not all(_cfg_address_comparable(address) for address in actual_conflict_addresses)
        or sorted(actual_conflict_addresses) != expected_conflict_addresses
    ):
        finding("conflict-projection-mismatch", actual_conflict_addresses)
    for _address, node in node_by_address.items():
        owners = node.get("owners")
        if not isinstance(owners, list) or any(type(owner) is not int for owner in owners):
            finding("node-owners-invalid", node)
            continue
        if len(owners) > 1 and node.get("classification") != "owner-conflict":
            finding("owner-classification-mismatch", node)
        if len(owners) <= 1 and node.get("classification") == "owner-conflict":
            finding("owner-classification-mismatch", node)
    if "ownership_map" in report:
        ownership_rows = report.get("ownership_map")
        if not isinstance(ownership_rows, list):
            finding("ownership-map-type", ownership_rows)
        else:
            actual_ownership = []
            seen_ownership = set()
            for row in ownership_rows:
                if not isinstance(row, dict):
                    finding("ownership-map-row-invalid", row)
                    continue
                address = row.get("address")
                classification = row.get("classification")
                owners = row.get("owners")
                if type(address) is not int or address not in node_by_address:
                    finding("ownership-map-address-invalid", row)
                    continue
                if address in seen_ownership:
                    finding("ownership-map-duplicate", row)
                seen_ownership.add(address)
                if not known(classification, allowed_classifications):
                    finding("ownership-map-classification-invalid", row)
                if not isinstance(owners, list) or any(type(owner) is not int for owner in owners):
                    finding("ownership-map-owners-invalid", row)
                    continue
                node = node_by_address[address]
                if classification != node.get("classification") or owners != node.get("owners"):
                    finding("ownership-map-node-mismatch", row)
                actual_ownership.append((address, classification, tuple(owners)))
            expected_ownership = [
                (address, node.get("classification"), owner_projection(node.get("owners", ())))
                for address, node in sorted(node_by_address.items())
            ]
            # Expected addresses are unique, so ordering by address alone is
            # enough; a duplicate row is a mismatch whatever its other fields.
            if sorted(actual_ownership, key=lambda item: item[0]) != expected_ownership:
                finding("ownership-map-projection-mismatch", ownership_rows)
    for conflict in conflicts:
        address = conflict.get("address") if isinstance(conflict, dict) else None
        node = node_by_address.get(address) if _cfg_address_comparable(address) else None
        if node is not None and isinstance(conflict.get("owners"), list) and conflict["owners"] != node.get("owners"):
            finding("conflict-owner-mismatch", conflict)

    return sorted(findings, key=lambda item: (item["code"], item["message"]))


def cfg_compatibility_findings(report, legacy_entries):
    """Compare entry sets without making either analyzer authoritative by fiat."""
    observed = set(report.get("entries", ()))
    expected = {int(entry) for entry in legacy_entries}
    findings = []
    for address in sorted(expected - observed):
        findings.append({"code": "legacy-entry-missing", "address": address})
    for address in sorted(observed - expected):
        findings.append({"code": "new-entry-not-in-legacy", "address": address})
    return findings


def canonical_cfg_json(report):
    """Serialize a CFG report deterministically for regression/build-cache use."""
    import json
    return json.dumps(report, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"


def canonical_cfg_gate(report):
    """Return fail-closed ownership-gate findings for a CFG report.

    ``verify_canonical_cfg_report`` checks that the serialized projections agree
    with one another and that every CFG edge has exactly one successor class.
    This gate requires every known in-range successor of an owned instruction to
    have an owner, and keeps ambiguous ownership or unmapped entries fail-closed.
    Non-padding words that are not reached from the supplied entries remain
    explicitly classified as unowned; that is not proof that no dynamic entry
    exists, but file adjacency alone is not a control-flow edge.
    Continuation/interior entries are valid classifications.
    """
    findings = verify_canonical_cfg_report(report)
    if findings:
        return findings
    if not isinstance(report, dict):
        return [{"code": "report-invalid", "message": "CFG report is not an object"}]
    if "ownership_map" not in report:
        findings.append({
            "code": "ownership-map-missing",
            "message": "machine-readable ownership map is absent",
        })

    node_by_address = {
        node["address"]: node
        for node in report.get("instructions", ())
        if isinstance(node, dict) and type(node.get("address")) is int
    }
    for edge in report.get("edges", ()):
        if not isinstance(edge, dict):
            continue
        source = node_by_address.get(edge.get("source"))
        target = node_by_address.get(edge.get("target"))
        if (source is None or target is None
                or not source.get("owners") or target.get("owners")):
            continue
        findings.append({
            "code": "ownership-gap",
            "message": (
                f"owned {edge.get('kind')} successor from "
                f"0x{edge['source']:08x} reaches unowned executable word "
                f"at 0x{edge['target']:08x}"
            ),
        })
    for conflict in report.get("ownership_conflicts", ()):
        address = conflict.get("address") if isinstance(conflict, dict) else conflict
        findings.append({
            "code": "ownership-conflict",
            "message": f"ambiguous ownership at 0x{address:08x}",
        })
    for address in report.get("unmapped_entries", ()):
        findings.append({
            "code": "entry-unmapped",
            "message": f"entry outside executable coverage at 0x{address:08x}",
        })
    return sorted(findings, key=lambda item: (item["code"], item["message"]))
