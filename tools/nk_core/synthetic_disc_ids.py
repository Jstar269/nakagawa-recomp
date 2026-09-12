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


SYNTHETIC_DISC_IDS = {
    "synthetic-allegrex-v1": "TEST00001",
    "synthetic-title2-v1": "TEST00002",
    "pspdev-phase5-v1": "TEST00005",
    "display-smoke-v1": "TEST00006",
}


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
