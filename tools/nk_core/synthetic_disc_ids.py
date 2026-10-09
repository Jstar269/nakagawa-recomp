#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Disc-id assignment for the public synthetic titles.

The manifest schema forbids a ``disc`` block on a non-retail manifest, so a
synthetic title cannot declare its own disc id and the assignment has to live
somewhere. It lives here, once.

It deliberately does **not** live in ``tools/title_manifest.py``: that module is
one of the generic helpers that must never name a concrete title id, and
``tools/test_generic_title_planning_proof.py`` enforces exactly that. It also
does not live in either consumer, because it used to live in *both* --
``tools/title_catalog_codegen.py`` for the native catalog and
``tools/nk_core/title_registry.py`` for the Python tooling -- and the two copies
drifted. This module is the single home they share.

The historical defect worth remembering: both consumers resolved an unassigned
title to a shared ``"TEST00000"`` sentinel. Because the sentinel was the same for
everyone, the *second* unassigned synthetic title to appear collided with the
first -- the registry reported a conflict with the public canonical catalog that
did not exist, and the native catalog emitted two entries claiming one disc id,
where lookup simply returns whichever comes first. An unassigned title is a
mistake to report, not a value to invent.
"""

from __future__ import annotations

import re


SYNTHETIC_DISC_IDS = {
    "synthetic-allegrex-v1": "TEST00001",
    "synthetic-title2-v1": "TEST00002",
    "pspdev-phase5-v1": "TEST00005",
    "display-smoke-v1": "TEST00006",
    "showcase-scene-v1": "TEST00007",
    "showcase-breakout-v1": "TEST00008",
}

#: Disc ids for tests and player test views that need an id no catalog title
#: owns. Replace a real title's id with one of these (or add the next number);
#: never invent a retail-format id for a test.
SYNTHETIC_TEST_DISC_IDS = frozenset({
    "TEST80001", "TEST80002", "TEST80003", "TEST80004", "TEST80005", "TEST80006",
})

#: The PSP retail disc-id formats: UMD ids (``U[CL]`` + two letters, such as
#: ULUS or UCES) and PSN ids (``NP`` + two letters), then five digits. Matching
#: ignores case and one ``-``, ``_`` or space separator, the forms PARAM.SFO,
#: file names and prose use.
RETAIL_DISC_ID = re.compile(
    r"(?<![A-Za-z0-9])((?:U[CL]|NP)[A-Z]{2})[-_ ]?([0-9]{5})(?![0-9])",
    re.IGNORECASE,
)

#: Retail-format ids the public tree names on purpose.
PUBLIC_RETAIL_DISC_IDS = {
    "UCUS98701": "the flagship title, named publicly in README.md and docs/COMPATIBILITY.md",
    "UCES01402": "another region's id for the flagship; tests assert the catalog never resolves it",
}

#: Retail-format stand-ins that existing tests use: placeholder numbers (the
#: 99xxx block, 00000, 00001, 12345) that no catalog title owns. Prefer
#: SYNTHETIC_TEST_DISC_IDS for new tests.
SYNTHETIC_RETAIL_DISC_IDS = frozenset({
    "UCES99902", "UCUS00000", "UCUS99901", "UCUS99911",
    "UCUS99990", "UCUS99991", "UCUS99992", "UCUS99993", "UCUS99994",
    "UCUS99996", "UCUS99997", "UCUS99999", "ULES99999",
    "ULUS00001", "ULUS12345",
    "ULUS99994", "ULUS99995", "ULUS99996", "ULUS99997", "ULUS99998", "ULUS99999",
})


def unregistered_retail_disc_ids(text: str) -> list[str]:
    """Return each retail-format disc id in ``text`` that this module does not list.

    tools/publish_audit.py applies this to every tracked text file, so a real
    title's id cannot enter source, tests or documentation unless it is
    deliberately registered above. Ids are returned normalised (upper case, no
    separator), once each, in first-seen order.
    """
    found: list[str] = []
    for match in RETAIL_DISC_ID.finditer(text):
        disc_id = (match.group(1) + match.group(2)).upper()
        if disc_id in PUBLIC_RETAIL_DISC_IDS or disc_id in SYNTHETIC_RETAIL_DISC_IDS:
            continue
        if disc_id not in found:
            found.append(disc_id)
    return found


class SyntheticDiscIdError(ValueError):
    """A synthetic title has no assigned disc id."""


def synthetic_disc_id(title_id: str) -> str:
    """Return the disc id for a PUBLIC synthetic title, or fail closed.

    For catalog generation, where an unassigned public manifest is a mistake.

    Callers resolving a **private overlay** want ``SYNTHETIC_DISC_IDS.get()``
    instead: an overlay loaded from outside this repository legitimately has no
    entry here, and having no disc id is the honest answer for it. What must not
    happen is substituting one shared value for every unassigned title.
    """
    try:
        return SYNTHETIC_DISC_IDS[title_id]
    except KeyError:
        raise SyntheticDiscIdError(
            f"synthetic title {title_id!r} has no assigned disc id; add one to "
            f"SYNTHETIC_DISC_IDS in tools/nk_core/synthetic_disc_ids.py. Synthetic "
            f"titles used to share a 'TEST00000' sentinel, which made any two "
            f"unassigned titles collide with each other."
        ) from None
