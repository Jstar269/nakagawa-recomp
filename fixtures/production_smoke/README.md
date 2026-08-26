# Full-production smoke guest

`generate.py` is the source of truth for a synthetic PSP-shaped guest used by the
full-production build smoke. It creates an ignored ELF32 PRX and matching `~PSP`
header; no binary fixture is committed.

The guest deliberately exercises two load segments, BSS recovery from the PSP
header, type-A relocations, import extraction, analyzer discovery, chunked C
generation, a real HLE import, and a relocation-dependent result write. Names,
addresses, instructions, and data are project-authored test values. They do not
contain or derive from a retail title.

## Execution modes

`generate.py generate/verify/run --mode <mode>` selects the mode plan. All modes share the same
base, entry, helper address (`0x08804028`), import stub, result slot and sentinel.

- `aot` — the plain production path: every discovered function is emitted as native code and the
  run must pass the relocation-dependent sentinel.
- `aot-gap` — the AOT/dispatch seam: the mode's build-time codegen choice
  (`--omit-aot=0x08804028`) removes the helper from native emission/registration ONLY. Its bytes
  stay complete in the guest image; region A's linked `jal` compiles to the typed production
  `dispatch_call(s, 0x08804028, 0x08804018)` seam. The call's resume PC is carried separately
  from `$ra`, so the interpreter executes the omitted helper and its return delay slot, then
  hands back before the native continuation. Generated registration explicitly owns the
  helper's executable span; the production interpreter executes the helper bytes only because
  that span is explicitly registered. Its tail transfers to registered AOT region B. Region B
  commits the interpreted `0x00001235` result before the real HLE path, so the production-driver
  assertion cannot pass on logs or dispatch alone.
