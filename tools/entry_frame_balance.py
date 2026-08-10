# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Stack-symbolic entry classification: callable boundary versus resume PC.

Issue #51 separated *callable* entries from *resume* (continuation) entries in
the codegen entry catalog, but the role of each HST seed was asserted from a
one-off manual audit of the private image and recorded as a constant.  This
module supplies the general, image-independent instrument that re-derives the
role structurally, so the classification is checked on every run instead of
being carried on trust.

The measurement is the net ``$sp`` delta along the intra-procedural CFG from an
entry PC to each ``jr $ra``:

* a well-formed o32 **callable** allocates its frame in its own prologue and
  releases exactly that frame in its epilogue, so every return path reports
  ``delta == 0``;
* an interior **continuation** begins inside a frame it never allocated and
  reaches its owner's epilogue, so it reports ``delta == +owner_frame`` -- it
  releases a frame it did not create.

That positive delta is precisely the quantity the pre-#51 codegen destroyed: the
standalone-entry contract captured the mid-frame ``$sp`` and forced it back after
the guest epilogue had already restored the caller's ``$sp`` correctly.

Delay slots are load-bearing here.  MIPS compilers routinely hoist
``addiu $sp, $sp, +N`` into the ``jr $ra`` delay slot, so a walker that stops at
the jump without executing its delay instruction reports every such function as
unbalanced.  Every transfer below applies its delay slot before recording a
delta, and branch-likely forms nullify that slot on the not-taken path.

Only structural quantities -- frame sizes, deltas, control-flow relationships --
are produced.  No instruction text is reproduced.
"""

from __future__ import annotations

from dataclasses import dataclass

from analyze import (
    BRANCH_LINK,
    code_pointer_evidence,
    direct_branch_edges,
    direct_j_edges,
    in_ranges,
    is_hard_terminator,
    trace_function,
)

SP = 29
RA = 31

#: SPECIAL ``fn`` values that write ``rd``. Membership is enumerated rather than
#: defaulted, because ``rd`` is only a register selector for these: ``syscall``
#: and ``break`` carry a 20-bit code across the same bits, so treating any
#: SPECIAL word with ``rd == $sp`` as an unmodelled write would report spurious
#: indeterminates for particular syscall codes.
_SPECIAL_WRITES_RD = frozenset(
    {
        0x00, 0x02, 0x03,        # sll, srl, sra
        0x04, 0x06, 0x07,        # sllv, srlv, srav
        0x01,                    # movci (movf/movt)
        0x09,                    # jalr (link register is rd)
        0x0A, 0x0B,              # movz, movn
        0x10, 0x12,              # mfhi, mflo
        0x2A, 0x2B,              # slt, sltu
    }
    | set(range(0x20, 0x28))     # add, addu, sub, subu, and, or, xor, nor
)

#: Instruction budget for one entry walk.  Exceeding it sets ``truncated`` and
#: makes the profile indeterminate, rather than silently reporting a partial
#: delta set as if the walk had finished.
DEFAULT_STEP_LIMIT = 40000

#: How far back the shared-tail search looks for additional structural owners.
#: Bounded because it traces one function per candidate.  The declared owner is
#: checked exactly by :func:`owner_covers`, which uses no window, so a window
#: miss can only weaken the multiple-owner search -- never the role assertion.
OWNER_SEARCH_WINDOW = 0x8000


def read32(elf, pc):
    raw = elf.read_at_vaddr(pc, 4)
    if raw is None or len(raw) < 4:
        return None
    return int.from_bytes(raw, "little")


def frame_size(elf, addr):
    """Frame allocated by an ``addiu $sp, $sp, -N`` prologue at ``addr``.

    ``None`` when ``addr`` does not begin with that prologue form, which is one
    of the resume-entry signals listed in wiki doc 26 section 30.
    """
    word = read32(elf, addr)
    if word is None:
        return None
    if (word >> 16) != 0x27BD or not (word & 0x8000):
        return None
    return 0x10000 - (word & 0xFFFF)


def sp_effect(word):
    """``(delta, unknown)`` contribution of one instruction to ``$sp``.

    ``unknown`` marks a write to ``$sp`` this module deliberately refuses to
    model -- a register-computed stack adjustment, a reload of ``$sp`` from
    memory, an alloca-style pattern.  Such an entry is reported indeterminate
    rather than guessed at.
    """
    if word is None:
        return 0, True
    op, fn = word >> 26, word & 0x3F
    if op == 0x09 and ((word >> 16) & 0x1F) == SP:  # addiu $sp, rs, imm
        if ((word >> 21) & 0x1F) == SP:
            imm = (word & 0xFFFF) - (0x10000 if word & 0x8000 else 0)
            return imm, False
        return 0, True
    if op == 0 and ((word >> 11) & 0x1F) == SP and fn in _SPECIAL_WRITES_RD:
        return 0, True
    if op in (0x08, 0x0C, 0x0D, 0x0E, 0x0F) and ((word >> 16) & 0x1F) == SP:
        return 0, True  # addi/andi/ori/xori/lui writing $sp
    if op in (0x20, 0x21, 0x23, 0x24, 0x25) and ((word >> 16) & 0x1F) == SP:
        return 0, True  # a load into $sp
    return 0, False


#: REGIMM ``rt`` selectors whose branch is the *likely* form: bltzl, bgezl,
#: bltzall, bgezall. The plain and ``al`` variants sit at rt 0/1/16/17.
_REGIMM_LIKELY_RT = frozenset({0x02, 0x03, 0x12, 0x13})


def branch_likely(word):
    """True when ``word`` nullifies its delay slot on the not-taken path.

    Three encoding families carry the likely bit, and missing any of them is
    unsafe in one specific direction: the not-taken path wrongly applies the
    slot, so a hoisted ``addiu $sp`` is counted on a path that never executes
    it. Two genuinely disagreeing return paths then collapse to one value and
    the entry is reported as a confident role instead of indeterminate.
    """
    op = word >> 26
    if op in (20, 21, 22, 23):          # beql, bnel, blezl, bgtzl
        return True
    if op == 1:                          # REGIMM: the likely bit is in rt
        return ((word >> 16) & 0x1F) in _REGIMM_LIKELY_RT
    if op == 0x11 and ((word >> 21) & 0x1F) == 8:
        # COP1 BC: rt bit 0 selects true/false, bit 1 is nullify-delay.
        return bool((word >> 17) & 1)   # bc1fl / bc1tl
    return False


@dataclass(frozen=True)
class FrameProfile:
    """Structural stack summary of one entry PC."""

    addr: int
    has_prologue: bool
    frame: int | None
    return_deltas: frozenset[int]
    tail_deltas: frozenset[int]
    unknown_sp: bool
    truncated: bool
    #: Every net ``$sp`` delta observed on entry to each reached PC, relative to
    #: this entry.  Inside a live frame the values are negative; the magnitude is
    #: the stack depth a continuation entering there would have to release.
    depths: dict[int, frozenset[int]]

    def depth_at(self, addr):
        """Live stack depth at ``addr``, or ``None`` if not reached/ambiguous.

        Ambiguity is not resolved by picking a value: a PC reachable at two
        different stack depths has no single continuation contract, and the
        caller must treat that as a stop condition.
        """
        deltas = self.depths.get(addr)
        if not deltas or len(deltas) != 1:
            return None
        (delta,) = deltas
        return -delta

    @property
    def indeterminate(self):
        """True when the walk could not model every ``$sp`` write it saw."""
        return self.unknown_sp or self.truncated or not self.return_deltas

    @property
    def balanced(self):
        """True for the o32 callable contract: every return path nets zero."""
        return not self.indeterminate and self.return_deltas == frozenset({0})

    @property
    def continuation_delta(self):
        """The single positive net delta, or ``None`` if not a continuation.

        A continuation releases a frame it never allocated, so every return path
        must agree on one strictly positive delta.  Disagreeing paths are not
        collapsed to one value: they yield ``None`` and are reported as
        indeterminate by :func:`classify`.
        """
        if self.indeterminate or len(self.return_deltas) != 1:
            return None
        (delta,) = self.return_deltas
        return delta if delta > 0 else None


#: Role verdicts produced by :func:`classify`.
CALLABLE = "callable"
CONTINUATION = "continuation"
FRAME_LEAK = "frame-leak"
INDETERMINATE = "indeterminate"

# Verdicts for the deliberately narrower direct-``j`` audit.  ``CONTINUATION``
# is reused for the structural role above; ``AMBIGUOUS`` means that the
# candidate remains an analyzer entry but is not promoted to a resume contract.
AMBIGUOUS = "ambiguous"

# The direct-branch audit adds a third outcome.  ``CALLABLE_BOUNDARY`` is a
# positive finding, not a fallback: the candidate is a genuine fresh-call entry.
# It is reported separately from ``AMBIGUOUS`` because the two mean opposite
# things -- one is proven, the other is unproven -- even though neither changes
# production metadata (every analyzer start is already callable by default).
CALLABLE_BOUNDARY = "callable-boundary"

#: Source kind for a direct ``j`` predecessor folded into the branch audit by
#: B13.  It is not a branch, and it never puts an address in this slice's
#: population -- it only ever adds a proof obligation.
EDGE_J = "j"


def classify(profile):
    """Reduce a :class:`FrameProfile` to one structural role verdict.

    ``FRAME_LEAK`` -- a strictly negative net delta -- is neither role: the
    entry returns having consumed stack it never released.  It is surfaced
    separately rather than folded into ``INDETERMINATE`` because it usually
    means the recovered extent is wrong (a mis-split boundary), not that the
    analysis failed.
    """
    if profile.indeterminate:
        return INDETERMINATE
    if profile.balanced:
        return CALLABLE
    if profile.continuation_delta is not None:
        return CONTINUATION
    if all(delta < 0 for delta in profile.return_deltas):
        return FRAME_LEAK
    return INDETERMINATE


def profile_entry(elf, addr, ranges, limit=DEFAULT_STEP_LIMIT):
    """Walk the intra-procedural CFG from ``addr`` accumulating the ``$sp`` delta.

    ``jal``/``jalr`` are treated as returning calls (execution continues after
    the delay slot); ``j``, ``jr`` and conditional branches fork or terminate a
    path as :func:`analyze.trace_function` does, so the traversal agrees with
    the extent the analyzer recovers.
    """
    seen = set()
    stack = [(addr, 0)]
    returns = set()
    tails = set()
    depths = {}
    unknown = False
    truncated = False
    steps = 0

    while stack:
        pc, delta = stack.pop()
        while True:
            steps += 1
            if steps > limit:
                truncated = True
                break
            if not in_ranges(pc, ranges):
                break
            if (pc, delta) in seen:
                break
            seen.add((pc, delta))
            depths.setdefault(pc, set()).add(delta)
            word = read32(elf, pc)
            if word is None:
                break
            step, unk = sp_effect(word)
            delta += step
            unknown = unknown or unk
            op, fn = word >> 26, word & 0x3F

            slot_step, slot_unknown = sp_effect(read32(elf, pc + 4))

            if op == 3 or (op == 0 and fn == 0x09):  # jal / jalr: the call returns
                unknown = unknown or slot_unknown
                delta += slot_step
                pc += 8
                continue
            if op == 2:  # j
                unknown = unknown or slot_unknown
                delta += slot_step
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    stack.append((target, delta))
                break
            if op == 0 and fn == 0x08:  # jr
                unknown = unknown or slot_unknown
                delta += slot_step
                if ((word >> 21) & 0x1F) == RA:
                    returns.add(delta)
                else:
                    tails.add(delta)
                break
            if op == 0 and fn == 0x0C:  # syscall: an HLE boundary, not a return
                break

            is_likely = branch_likely(word)
            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (
                op == 0x11 and ((word >> 21) & 0x1F) == 8
            )
            if is_branch:
                unknown = unknown or slot_unknown
                offset = word & 0xFFFF
                offset -= 0x10000 if offset & 0x8000 else 0
                target = pc + 4 + (offset << 2)
                if in_ranges(target, ranges):
                    stack.append((target, delta + slot_step))
                if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
                    break  # unconditional `b`: no fall-through path
                # A branch-likely nullifies its delay slot when not taken.
                delta += 0 if is_likely else slot_step
                pc += 8
                continue
            pc += 4

    return FrameProfile(
        addr=addr,
        has_prologue=frame_size(elf, addr) is not None,
        frame=frame_size(elf, addr),
        return_deltas=frozenset(returns),
        tail_deltas=frozenset(tails),
        unknown_sp=unknown,
        truncated=truncated,
        depths={pc: frozenset(values) for pc, values in depths.items()},
    )


def owner_covers(elf, owner, addr, ranges, callables):
    """Does ``owner``'s recovered extent actually reach ``addr``?

    ``callables`` is passed through as :func:`analyze.trace_function`'s
    high-confidence boundary set, so the extent stops where a genuine callable
    boundary stops instead of running on through adjacent functions.
    """
    covered, calls = set(), set()
    trace_function(elf, owner, ranges, covered, calls, set(callables))
    return addr in covered


def find_frame_owners(elf, addr, ranges, callables, limit=DEFAULT_STEP_LIMIT,
                      window=OWNER_SEARCH_WINDOW):
    """Balanced framed callables whose extent covers ``addr``.

    Wiki doc 26 section 14 requires that discovering *incompatible* multiple
    owners for one resume PC stops rather than silently picking one.  This
    returns ``owner -> live stack depth at addr`` for every structural owner
    found within ``window`` bytes before ``addr``, so the caller can apply that
    rule.  Depth -- not prologue frame size -- is the compatibility test: two
    owners form an ordinary shared tail exactly when a continuation entering
    ``addr`` would have to release the same amount for both.  The two coincide
    only when neither owner adjusts ``$sp`` again after its prologue.
    """
    owners = {}
    for candidate in sorted(callables):
        if candidate >= addr or candidate < addr - window:
            continue
        if frame_size(elf, candidate) is None:
            continue
        profile = profile_entry(elf, candidate, ranges, limit=limit)
        if not profile.balanced:
            continue
        depth = profile.depth_at(addr)
        if depth is None:
            continue
        owners[candidate] = depth
    return owners


def verify_resume_entry(elf, ranges, resume, owner, limit=DEFAULT_STEP_LIMIT):
    """Structurally re-derive one declared resume/owner pair.

    Returns a list of human-readable problems; empty means the image itself
    confirms the declaration.  A pair whose stack behavior cannot be modelled
    reports an ``indeterminate`` problem instead of passing silently, so an
    unverifiable declaration is never mistaken for a verified one.
    """
    problems = []

    owner_frame = frame_size(elf, owner)
    if owner_frame is None:
        problems.append(
            f"owner 0x{owner:08x} has no o32 frame prologue, so resume "
            f"0x{resume:08x} has no frame to continue"
        )

    owner_profile = profile_entry(elf, owner, ranges, limit=limit)
    if owner_profile.indeterminate:
        problems.append(
            f"owner 0x{owner:08x} stack behavior is indeterminate "
            f"(unknown_sp={owner_profile.unknown_sp}, "
            f"truncated={owner_profile.truncated}, "
            f"returns={sorted(owner_profile.return_deltas)})"
        )
    elif not owner_profile.balanced:
        problems.append(
            f"owner 0x{owner:08x} is not a balanced callable (net jr $ra "
            f"deltas {sorted(owner_profile.return_deltas)}, expected [0])"
        )

    resume_profile = profile_entry(elf, resume, ranges, limit=limit)
    if resume_profile.has_prologue:
        problems.append(
            f"resume 0x{resume:08x} begins with its own frame prologue "
            f"(0x{resume_profile.frame:x}); that is the callable contract, "
            "not a continuation"
        )
    if resume_profile.indeterminate:
        problems.append(
            f"resume 0x{resume:08x} stack behavior is indeterminate "
            f"(unknown_sp={resume_profile.unknown_sp}, "
            f"truncated={resume_profile.truncated}, "
            f"returns={sorted(resume_profile.return_deltas)})"
        )
    else:
        delta = resume_profile.continuation_delta
        if delta is None:
            problems.append(
                f"resume 0x{resume:08x} does not release exactly one frame "
                f"(net jr $ra deltas {sorted(resume_profile.return_deltas)})"
            )
        else:
            # The authoritative expectation is the owner's *live* stack depth at
            # the resume PC, which equals the prologue frame only when the owner
            # makes no further $sp adjustment before reaching it.
            depth = owner_profile.depth_at(resume)
            if depth is None:
                if resume in owner_profile.depths:
                    problems.append(
                        f"resume 0x{resume:08x} is reachable inside owner "
                        f"0x{owner:08x} at more than one stack depth "
                        f"{sorted(-d for d in owner_profile.depths[resume])}, "
                        "so it has no single continuation contract"
                    )
                else:
                    problems.append(
                        f"owner 0x{owner:08x} never reaches resume "
                        f"0x{resume:08x}"
                    )
            elif delta != depth:
                problems.append(
                    f"resume 0x{resume:08x} releases 0x{delta:x} but owner "
                    f"0x{owner:08x} is only 0x{depth:x} deep there"
                )
    return problems


def census(elf, starts, ranges, limit=DEFAULT_STEP_LIMIT):
    """Group analyzer-discovered starts by structural role.

    Returns ``{verdict: [addr, ...]}``.  This is a *diagnostic*: a start landing
    in ``CONTINUATION`` is a candidate for reclassification, not proof that the
    generated code is wrong, because an interior entry reached only from its own
    owner has its host SP restore masked by the owner's.  Acting on the census
    requires evidence that the entry is dispatched from outside its owner.
    """
    grouped = {CALLABLE: [], CONTINUATION: [], FRAME_LEAK: [], INDETERMINATE: []}
    for addr in sorted(starts):
        if not in_ranges(addr, ranges):
            continue
        grouped[classify(profile_entry(elf, addr, ranges, limit=limit))].append(addr)
    return grouped


@dataclass(frozen=True)
class DirectJumpCandidate:
    """One structural continuation candidate reached by direct ``j`` edges.

    ``role`` is the stack-symbolic verdict for the target itself.  The direct-j
    slice only records targets with ``role == CONTINUATION``; ``classification``
    is the conservative control-flow decision used by codegen.  A candidate is
    decisive only when exactly one balanced analyzer callable reaches every
    direct-j source and the target at one live depth, and the target's CFG does
    not reach an incoming source (which would make the edge a loop back-edge).
    """

    addr: int
    sources: tuple[int, ...]
    role: str
    classification: str
    owners: tuple[int, ...]
    continuation_delta: int | None
    reason: str


def audit_direct_j_candidates(elf, starts, ranges, limit=DEFAULT_STEP_LIMIT):
    """Audit the high-confidence direct-``j`` continuation slice.

    The analyzer's tail-call promotion intentionally keeps every hard-
    terminator target that looks like a boundary.  This audit does not infer a
    role from that shape alone.  It first requires the independent stack
    signature (a deterministic positive return delta with no prologue), then
    proves a one-way owner edge from a balanced analyzer callable.  Backwards
    loop edges, missing owners, and owner/source disagreements remain
    :data:`AMBIGUOUS` and are not changed in production.

    The result is image-independent and address-free: HST's three historical
    seeds are not special-cased, and they are absent from this slice unless an
    image actually supplies the same direct-j/owner evidence.
    """
    callable_starts = frozenset(
        addr for addr in starts if in_ranges(addr, ranges)
    )
    edges = direct_j_edges(elf, ranges, callable_starts)
    profile_cache = {}

    def profile(addr):
        if addr not in profile_cache:
            profile_cache[addr] = profile_entry(elf, addr, ranges, limit=limit)
        return profile_cache[addr]

    records = []
    for addr in sorted(edges):
        target_profile = profile(addr)
        role = classify(target_profile)
        if role != CONTINUATION:
            continue
        sources = edges[addr]
        if target_profile.has_prologue:
            records.append(
                DirectJumpCandidate(
                    addr=addr,
                    sources=sources,
                    role=role,
                    classification=AMBIGUOUS,
                    owners=(),
                    continuation_delta=target_profile.continuation_delta,
                    reason=(
                        "target has its own frame prologue; a positive return "
                        "delta is not sufficient for a resume contract"
                    ),
                )
            )
            continue
        cycle_sources = tuple(
            source for source in sources if source in target_profile.depths
        )

        # ``find_frame_owners`` is intentionally a broad structural search;
        # tighten it here by requiring that every direct source belongs to the
        # same balanced callable CFG.  A target reached only through another
        # path is not evidence for this direct-j slice.
        owners = find_frame_owners(
            elf, addr, ranges, callable_starts, limit=limit
        )
        proven = []
        for owner, depth in sorted(owners.items()):
            owner_profile = profile(owner)
            if owner_profile.depth_at(addr) != target_profile.continuation_delta:
                continue
            if not all(source in owner_profile.depths for source in sources):
                continue
            proven.append(owner)

        if cycle_sources:
            classification = AMBIGUOUS
            reason = (
                "target CFG reaches direct-j source(s) "
                + ", ".join(f"0x{source:08x}" for source in cycle_sources)
                + "; edge is a loop/back-edge, not a one-way shared tail"
            )
        elif len(proven) == 1:
            classification = CONTINUATION
            reason = (
                f"balanced owner 0x{proven[0]:08x} reaches every direct-j source "
                "and the target at one live depth"
            )
        elif len(proven) > 1:
            classification = AMBIGUOUS
            reason = (
                "multiple balanced owners reach the direct-j sources: "
                + ", ".join(f"0x{owner:08x}" for owner in proven)
            )
        else:
            classification = AMBIGUOUS
            if owners:
                reason = (
                    "no single balanced owner reaches every direct-j source; "
                    "owner/source control flow is incomplete"
                )
            else:
                reason = "no balanced analyzer callable proves an owning edge"

        records.append(
            DirectJumpCandidate(
                addr=addr,
                sources=sources,
                role=role,
                classification=classification,
                owners=tuple(proven),
                continuation_delta=target_profile.continuation_delta,
                reason=reason,
            )
        )
    return tuple(records)


def direct_j_resume_owners(elf, starts, ranges, limit=DEFAULT_STEP_LIMIT):
    """Return decisive direct-j resume entries as ``target -> owner``.

    This is the production-facing projection of
    :func:`audit_direct_j_candidates`; keeping the full records available to
    diagnostics makes the ambiguity stop auditable without adding a second
    classification path in codegen.
    """
    return {
        candidate.addr: candidate.owners[0]
        for candidate in audit_direct_j_candidates(
            elf, starts, ranges, limit=limit
        )
        if candidate.classification == CONTINUATION
    }


def index_frame_owners(elf, starts, ranges, interest, limit=DEFAULT_STEP_LIMIT):
    """Balanced framed callables reaching each address in ``interest``.

    The direct-``j`` slice used :func:`find_frame_owners`, which searches only
    ``OWNER_SEARCH_WINDOW`` bytes back from one address.  A window is a bound on
    the *search*, not on reality: an owner outside it is silently not found, and
    "no second owner was found" then reads like "no second owner exists".  For
    the branch slice that distinction is load-bearing, because a conditional
    branch target normally also has a linear fall-in predecessor, so the
    multiple-owner question is the common case rather than the exotic one.

    This makes one pass over the whole start set instead, and keeps only the
    depths at the handful of addresses actually under audit, so the exact answer
    costs about the same as one census.  The result is
    ``{addr: {owner: depth_or_None}}``; an owner present with ``None`` reaches
    the address at more than one stack depth.
    """
    interest = frozenset(interest)
    found = {addr: {} for addr in interest}
    for owner in sorted(starts):
        if not in_ranges(owner, ranges) or frame_size(elf, owner) is None:
            continue
        profile = profile_entry(elf, owner, ranges, limit=limit)
        if not profile.balanced:
            continue
        for addr in interest.intersection(profile.depths):
            found[addr][owner] = profile.depth_at(addr)
    return found


@dataclass(frozen=True)
class DirectBranchCandidate:
    """One analyzer start reached by at least one direct PC-relative branch.

    ``sources`` carries ``(source_pc, kind)`` pairs so the kind that decided a
    verdict stays visible.  ``contradictions`` lists independent provenance that
    argues against a continuation contract (a direct call edge, an address-taken
    constant, a code pointer in data); it is populated even when some other rule
    already settled the verdict, so the audit output shows *all* the reasons an
    address was not promoted rather than only the first one hit.
    """

    addr: int
    sources: tuple[tuple[int, str], ...]
    role: str
    classification: str
    owners: tuple[int, ...]
    continuation_delta: int | None
    contradictions: tuple[str, ...]
    reason: str

    @property
    def source_pcs(self):
        return tuple(source for source, _ in self.sources)


def _branch_record(base, classification, reason, owners=()):
    """Finish one :class:`DirectBranchCandidate` from its per-address facts.

    A module-level constructor, deliberately not a closure over the audit loop:
    verdicts for the still-pending candidates are built after that loop has
    finished, and a closure would then report every one of them against the last
    address it saw.
    """
    return DirectBranchCandidate(
        classification=classification,
        owners=tuple(owners),
        reason=reason,
        **base,
    )


def _fall_in_predecessor(elf, addr, ranges):
    """The PC that falls linearly into ``addr``, or ``None`` if nothing can.

    Control reaches ``addr`` by fall-through unless the instruction two slots
    back is a hard terminator, in which case ``addr - 4`` is that terminator's
    delay slot and execution leaves.  Returns ``addr - 4`` when a fall-in exists.
    """
    if not in_ranges(addr - 8, ranges) or not in_ranges(addr - 4, ranges):
        return None
    word = read32(elf, addr - 8)
    if word is None or is_hard_terminator(word):
        return None
    return addr - 4


def audit_direct_branch_candidates(elf, starts, ranges, limit=DEFAULT_STEP_LIMIT):
    """Audit analyzer starts reached by a direct conditional-branch edge (#51).

    A conditional branch is **not** a weaker ``j`` and does not inherit the
    direct-``j`` rule.  Four differences drive separate proof obligations:

    1.  **A branch has a fall-through.**  Its target therefore normally has a
        linear predecessor as well, so "one owner reaches every branch source"
        does not by itself establish a single incoming contract.  Rule B12 below
        requires the fall-in predecessor, when one exists, to belong to the same
        owner.
    2.  **The family contains calls.**  ``bal``/``bgezal``/``bltzal`` and their
        likely forms write ``$ra``; their target is a callable boundary.  ``j``
        has no such form, so the direct-``j`` audit never needed this veto.
    3.  **Backward branches are the loop idiom.**  A ``j`` back-edge is
        comparatively rare; a backward conditional branch is what every loop
        compiles to, and a loop header has no single resume contract.  Any
        backward source is disqualifying here (B8) rather than only a proven
        CFG cycle (B9), which is retained as well for forward edges that close a
        cycle later.
    4.  **Multiple predecessors are normal, not exceptional.**  The owner search
        is therefore exact (:func:`index_frame_owners`) rather than windowed.

    Production promotion to a resume contract requires **all** of:

    * **B1** the address is an analyzer start in an executable range with at
      least one direct branch edge into it;
    * **B2** its stack role is :data:`CONTINUATION` -- no unmodelled ``$sp``
      write, walk not truncated, and every return path agreeing on one strictly
      positive net delta;
    * **B3** it has no frame prologue of its own;
    * **B4** no source is a linking branch;
    * **B5** it is not a ``jal`` or linking-branch target anywhere;
    * **B6** it is not address-taken -- neither materialized as a constant in
      code nor present as a code pointer in any data section (callback table,
      vtable, jump table, export/import table);
    * **B7** it is not the delay slot of a hard terminator;
    * **B8** every source lies strictly below it;
    * **B9** its own CFG does not reach any of its sources;
    * **B10** exactly one balanced framed callable in the whole start set
      reaches it;
    * **B11** that owner reaches it, and every source, at exactly one live
      ``$sp`` depth, and its depth at the target equals the target's own
      release;
    * **B12** if the address can be fallen into, that linear predecessor is
      inside the same owner.

    **B13, which is really a scoping rule for B8/B9/B11: "source" means every
    direct predecessor, not only the branch ones.**  A branch edge is what puts
    an address in this slice's *population*, but it is not the only way in.  An
    address reached by a branch from inside one function and by a direct ``j``
    from three others is a shared tail with several reaching owners, and
    proving the branch edge alone would promote it on a partial predecessor
    set.  The source set is therefore the union of the direct branch edges and
    the direct ``j`` edges, and B8, B9 and B11 all range over that union.  This
    is not hypothetical: it is what the first version of this audit got wrong,
    and it showed up as two promotions the direct-``j`` slice had deliberately
    declined for exactly this reason.

    Anything else is reported :data:`AMBIGUOUS` and left unchanged, except that
    a balanced target is reported :data:`CALLABLE_BOUNDARY` -- a positive
    finding that it is a genuine fresh-call entry.  The audit is image-driven
    and address-free; no title address takes part in any rule.
    """
    start_set = frozenset(addr for addr in starts if in_ranges(addr, ranges))
    edges = direct_branch_edges(elf, ranges, start_set)
    # B13: a branch edge defines the population, but every direct predecessor
    # takes part in the proof.  Merging the `j` edges in here, rather than
    # checking branch sources alone, is what stops a shared tail entered by
    # `j` from several bodies being promoted on one branch edge.
    jump_edges = direct_j_edges(elf, ranges, start_set)
    evidence = code_pointer_evidence(elf, ranges)

    profile_cache = {}

    def profile(addr):
        if addr not in profile_cache:
            profile_cache[addr] = profile_entry(elf, addr, ranges, limit=limit)
        return profile_cache[addr]

    # Only continuation-role candidates need an owner, and the owner index is the
    # one expensive step, so settle the role and the provenance vetoes first and
    # then index exactly the addresses still in play.  ``settled`` holds finished
    # records; ``pending`` holds the candidates that still need an owner.
    settled = []
    pending = []
    for addr in sorted(edges):
        sources = tuple(sorted(
            edges[addr]
            + tuple((source, EDGE_J) for source in jump_edges.get(addr, ()))
        ))
        target_profile = profile(addr)
        role = classify(target_profile)

        contradictions = []
        if addr in evidence["jal"]:
            contradictions.append("direct-jal-target")
        if addr in evidence["branch-link"]:
            contradictions.append("linking-branch-target")
        if any(kind == BRANCH_LINK for _, kind in sources):
            contradictions.append("linking-branch-source")
        if addr in evidence["immediate"]:
            contradictions.append("address-taken-constant")
        if addr in evidence["data"]:
            contradictions.append("code-pointer-in-data")
        contradictions = tuple(contradictions)

        base = dict(
            addr=addr,
            sources=sources,
            role=role,
            continuation_delta=target_profile.continuation_delta,
            contradictions=contradictions,
        )

        if role == CALLABLE:
            corroborated = [
                kind for kind in ("direct-jal-target", "linking-branch-target")
                if kind in contradictions
            ]
            settled.append(_branch_record(
                base, CALLABLE_BOUNDARY,
                "balanced o32 frame contract on every return path"
                + (" corroborated by " + ", ".join(corroborated) if corroborated
                   else " with no direct call edge"),
            ))
            continue

        if role != CONTINUATION:  # B2
            settled.append(_branch_record(
                base, AMBIGUOUS, f"stack role is {role}, not a continuation",
            ))
            continue

        # --- continuation-signature candidates: the branch-specific rules -----
        call_evidence = [
            kind for kind in (
                "direct-jal-target", "linking-branch-target", "linking-branch-source",
            ) if kind in contradictions
        ]
        if call_evidence:  # B4 / B5
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "call evidence (" + ", ".join(call_evidence) + ") contradicts the "
                "continuation stack signature; the address is entered as a fresh "
                "call on at least one path",
            ))
            continue

        if target_profile.has_prologue:  # B3
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "target has its own frame prologue; a positive return delta is "
                "not sufficient for a resume contract",
            ))
            continue

        taken_evidence = [
            kind for kind in ("address-taken-constant", "code-pointer-in-data")
            if kind in contradictions
        ]
        if taken_evidence:  # B6
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "address-taken evidence (" + ", ".join(taken_evidence) + "); the "
                "incoming contract of an indirect dispatch is not visible to this "
                "analysis",
            ))
            continue

        previous = read32(elf, addr - 4) if in_ranges(addr - 4, ranges) else None
        if previous is not None and is_hard_terminator(previous):  # B7
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "target is the delay slot of a hard terminator, not an "
                "instruction boundary",
            ))
            continue

        backward = tuple(source for source, _ in sources if source >= addr)
        if backward:  # B8
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "backward source(s) "
                + ", ".join(f"0x{source:08x}" for source in backward)
                + "; a loop header has no single resume contract",
            ))
            continue

        cycle = tuple(
            source for source, _ in sources if source in target_profile.depths
        )
        if cycle:  # B9
            settled.append(_branch_record(
                base, AMBIGUOUS,
                "target CFG reaches source(s) "
                + ", ".join(f"0x{source:08x}" for source in cycle)
                + "; the edge closes a loop rather than entering a tail",
            ))
            continue

        pending.append((base, target_profile))

    interest = set()
    for base, _ in pending:
        interest.add(base["addr"])
        interest.update(source for source, _ in base["sources"])
        fall_in = _fall_in_predecessor(elf, base["addr"], ranges)
        if fall_in is not None:
            interest.add(fall_in)

    owner_index = (
        index_frame_owners(elf, start_set, ranges, interest, limit=limit)
        if interest else {}
    )
    records = list(settled)

    for base, target_profile in pending:
        addr, sources = base["addr"], base["sources"]
        owners = owner_index.get(addr, {})

        # B10: exactly one balanced framed callable may reach the target.  An
        # owner that reaches it at several depths is counted here rather than
        # skipped -- such an owner is itself the evidence that no single
        # contract exists, and dropping it would turn that into a clean proof.
        if not owners:
            records.append(_branch_record(
                base, AMBIGUOUS, "no balanced framed callable reaches the target",
            ))
            continue
        if len(owners) > 1:
            records.append(_branch_record(
                base, AMBIGUOUS,
                "multiple balanced owners reach the target: "
                + ", ".join(f"0x{owner:08x}" for owner in sorted(owners)),
            ))
            continue

        (owner, depth), = owners.items()

        # B11: one live depth at the target, matching what the target releases,
        # and one live depth at every branch source.
        if depth is None:
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"owner 0x{owner:08x} reaches the target at more than one stack "
                "depth, so it has no single continuation contract", owners=(owner,),
            ))
            continue
        if depth != target_profile.continuation_delta:
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"target releases 0x{target_profile.continuation_delta:x} but "
                f"owner 0x{owner:08x} is 0x{depth:x} deep there", owners=(owner,),
            ))
            continue
        unreached = tuple(
            source for source, _ in sources if owner not in owner_index.get(source, {})
        )
        if unreached:
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"owner 0x{owner:08x} does not reach source(s) "
                + ", ".join(f"0x{source:08x}" for source in unreached)
                + "; the edge crosses a boundary this analysis cannot own",
                owners=(owner,),
            ))
            continue
        multi_depth = tuple(
            source for source, _ in sources
            if owner_index.get(source, {}).get(owner) is None
        )
        if multi_depth:
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"owner 0x{owner:08x} reaches source(s) "
                + ", ".join(f"0x{source:08x}" for source in multi_depth)
                + " at more than one stack depth", owners=(owner,),
            ))
            continue

        # B12: a linear predecessor, where one exists, must be inside this owner.
        fall_in = _fall_in_predecessor(elf, addr, ranges)
        if fall_in is not None and owner not in owner_index.get(fall_in, {}):
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"target can be fallen into from 0x{fall_in:08x}, which owner "
                f"0x{owner:08x} does not reach; the address has an incoming path "
                "outside the proven owner", owners=(owner,),
            ))
            continue

        # The extent claim itself: the owner's *recovered function boundary*, not
        # merely a stack walk, must contain the target.
        #
        # Retained as a backstop, not as a live rule.  `trace_function` stops at
        # a foreign start only when that start carries a prologue or is a known
        # call target, and B3 and B5 have already excluded both, so for this
        # population the check cannot currently fail -- the mutation sweep in
        # `tools/test_entry_frame_balance.py` reports it as the one surviving
        # mutant for exactly that reason.  It stays because it is the only thing
        # tying the promotion to the analyzer's own boundary decision, and a
        # future relaxation of B3/B5 would otherwise silently lose that tie.
        if not owner_covers(elf, owner, addr, ranges, start_set):
            records.append(_branch_record(
                base, AMBIGUOUS,
                f"owner 0x{owner:08x} stack-reaches the target but its recovered "
                "extent does not cover it", owners=(owner,),
            ))
            continue

        kinds = "/".join(sorted({kind for _, kind in sources}))
        records.append(_branch_record(
            base, CONTINUATION,
            f"balanced owner 0x{owner:08x} reaches the target and every forward "
            f"{kinds} predecessor at one live depth, with no call, "
            "address-taken, loop, or fall-in evidence against it",
            owners=(owner,),
        ))

    records.sort(key=lambda candidate: candidate.addr)
    return tuple(records)


def direct_branch_resume_owners(elf, starts, ranges, limit=DEFAULT_STEP_LIMIT):
    """Decisive direct-branch resume entries as ``target -> owner``.

    The production-facing projection of :func:`audit_direct_branch_candidates`,
    mirroring :func:`direct_j_resume_owners`.
    """
    return {
        candidate.addr: candidate.owners[0]
        for candidate in audit_direct_branch_candidates(
            elf, starts, ranges, limit=limit
        )
        if candidate.classification == CONTINUATION
    }


def main(argv):  # pragma: no cover - operator entry point
    import argparse

    from analyze import Elf, analyze

    parser = argparse.ArgumentParser(
        description="Classify entry points by o32 stack-frame balance."
    )
    parser.add_argument("elf")
    parser.add_argument("--base", type=lambda v: int(v, 0), default=0)
    parser.add_argument(
        "--addr", type=lambda v: int(v, 0), action="append", default=[],
        help="report one address in detail (repeatable)",
    )
    parser.add_argument(
        "--census", action="store_true",
        help="classify every analyzer-discovered start",
    )
    parser.add_argument(
        "--direct-j", action="store_true",
        help="audit continuation-classified starts reached by direct unconditional j edges",
    )
    parser.add_argument(
        "--direct-branch", action="store_true",
        help="audit analyzer starts reached by a direct PC-relative branch",
    )
    parser.add_argument(
        "--verify", nargs=2, metavar=("RESUME", "OWNER"), action="append",
        default=[], help="re-derive one declared resume/owner pair",
    )
    args = parser.parse_args(argv[1:])

    elf = Elf(args.elf, base=args.base)
    starts, ranges = analyze(elf)
    print(f"analyzer starts: {len(starts)}")

    for addr in args.addr:
        profile = profile_entry(elf, addr, ranges)
        print(
            f"0x{addr:08x} {classify(profile)} prologue={profile.has_prologue} "
            f"frame={profile.frame} returns={sorted(profile.return_deltas)} "
            f"unknown_sp={profile.unknown_sp} truncated={profile.truncated}"
        )

    failures = 0
    for resume_s, owner_s in args.verify:
        resume, owner = int(resume_s, 0), int(owner_s, 0)
        problems = verify_resume_entry(elf, ranges, resume, owner)
        if problems:
            failures += 1
            print(f"0x{resume:08x} -> 0x{owner:08x}: " + "; ".join(problems))
        else:
            print(f"0x{resume:08x} -> 0x{owner:08x}: CONFIRMED")

    if args.census:
        grouped = census(elf, starts, ranges)
        for verdict in (CALLABLE, CONTINUATION, FRAME_LEAK, INDETERMINATE):
            print(f"{verdict}: {len(grouped[verdict])}")

    if args.direct_j:
        candidates = audit_direct_j_candidates(elf, starts, ranges)
        breakdown = {}
        for candidate in candidates:
            breakdown[candidate.classification] = (
                breakdown.get(candidate.classification, 0) + 1
            )
        print(f"direct-j continuation candidates: {len(candidates)}")
        for verdict in (CONTINUATION, AMBIGUOUS):
            print(f"direct-j {verdict}: {breakdown.get(verdict, 0)}")
        for candidate in candidates:
            owners = ",".join(f"0x{owner:08x}" for owner in candidate.owners)
            sources = ",".join(f"0x{source:08x}" for source in candidate.sources)
            print(
                f"0x{candidate.addr:08x} {candidate.classification} "
                f"sources={sources} owners={owners or '-'} "
                f"delta=0x{candidate.continuation_delta:x} "
                f"reason={candidate.reason}"
            )

    if args.direct_branch:
        candidates = audit_direct_branch_candidates(elf, starts, ranges)
        breakdown = {}
        for candidate in candidates:
            breakdown[candidate.classification] = (
                breakdown.get(candidate.classification, 0) + 1
            )
        print(f"direct-branch candidates: {len(candidates)}")
        for verdict in (CONTINUATION, CALLABLE_BOUNDARY, AMBIGUOUS):
            print(f"direct-branch {verdict}: {breakdown.get(verdict, 0)}")
        for candidate in candidates:
            owners = ",".join(f"0x{owner:08x}" for owner in candidate.owners)
            sources = ",".join(
                f"0x{source:08x}:{kind}" for source, kind in candidate.sources
            )
            delta = candidate.continuation_delta
            print(
                f"0x{candidate.addr:08x} {candidate.classification} "
                f"role={candidate.role} sources={sources} owners={owners or '-'} "
                f"delta={'-' if delta is None else f'0x{delta:x}'} "
                f"contradictions={','.join(candidate.contradictions) or '-'} "
                f"reason={candidate.reason}"
            )
    return 1 if failures else 0


if __name__ == "__main__":  # pragma: no cover
    import sys

    sys.exit(main(sys.argv))
