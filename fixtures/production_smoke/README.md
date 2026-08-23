# Full-production smoke guest

`generate.py` is the source of truth for a synthetic PSP-shaped guest used by the
full-production build smoke. It creates an ignored ELF32 PRX and matching `~PSP`
header; no binary fixture is committed.

The guest deliberately exercises two load segments, BSS recovery from the PSP
header, type-A relocations, import extraction, analyzer discovery, chunked C
generation, a real HLE import, and a relocation-dependent result write. Names,
addresses, instructions, and data are project-authored test values. They do not
contain or derive from a retail title.
