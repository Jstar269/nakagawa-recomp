# Workspace Doctor

`tools/hst_doctor.py` is the fail-closed preflight for Nakagawa Recomp. It checks the supported
Windows 11 x64 / PowerShell 7.6+ / CPython 3.14.x host contract, current MSYS2 UCRT64 tools,
Vulkan SDK discovery, the repository, private game-input layout, runtime dependencies, and build
products without copying, decrypting, extracting, modifying, or uploading private material.

The simplified `hst.ps1` front end exposes it directly:

```powershell
.\hst.ps1 Doctor
```

Use a narrower scope when diagnosing one layer:

```powershell
.\hst.ps1 Doctor -Scope repo
.\hst.ps1 Doctor -Scope inputs
.\hst.ps1 Doctor -Scope build
.\hst.ps1 Doctor -Scope run
```

For automation or the optional local dashboard:

```powershell
.\hst.ps1 Doctor -Scope all -Json
```

`-Strict` makes warnings produce a nonzero result. Without it, warnings remain visible but only
`FAIL` results make the action fail.

## Scopes

| Scope | Checks |
| --- | --- |
| `repo` | Required public-facing documents, root GPLv3 text, project-metadata transition warnings, core disclaimers, and tracked-private-path hygiene when a local Git checkout is available |
| `inputs` | Decrypted MIPS ELF/PRXs, original `~PSP` header, ISO selection/format/disc-ID signal, and populated XB extraction tree |
| `build` | Windows 11/x64, PowerShell/Python, UCRT64 compiler and Make tools, SDL3/Vulkan link inputs, and private code-generation inputs |
| `products` | Built `hst.exe` and `hst_image.bin` only |
| `run` | Windows/x64/Python, ISO and XB assets, VFPU tables, runtime DLLs, `hst.exe`, and `hst_image.bin` |
| `all` | Every check above |

## What it validates

### Toolchain identity

The doctor requires Windows 11 x64, PowerShell 7.6+ (`pwsh`), and CPython 3.14.x. It resolves
`mingw32-make`, `gcc`, and `g++`, records their first version line, and fails when they do not
resolve from an MSYS2 UCRT64 path. It checks the SDL3 import library and discovers a usable Vulkan
SDK using `-vulkan-sdk`, `VULKAN_SDK`, then the newest numerically named `C:\VulkanSDK\<version>`
installation. SDK validity requires the headers and loader import library needed by the build,
not just a directory name. `glslc` is not required when the checked-in shader source, embedding,
and manifest hashes pass `shader_embed.py verify`; it is checked when that authoritative
provenance gate reports that regeneration is required.

The output uses `ERROR`/`FAIL` for missing or unsupported required prerequisites, `WARNING`/`WARN`
for optional workflow capabilities, and `INFO` for detected state. Private game inputs remain a
separate scope and never turn a toolchain absence into a toolchain pass.

This does not claim that any executable named `gcc` is a supported compiler. A non-UCRT64 compiler
can link against a different C runtime and produce a binary that fails later in ways that look like
runtime defects.

### Private input format

The doctor does more than test path existence:

- `EBOOT.elf` and the three PRXs must be ELF32, little-endian, MIPS, contain bounded program headers,
  and contain at least one `PT_LOAD` segment.
- `EBOOT.BIN` must contain a valid `~PSP` header with a bounded segment count and nonzero declared
  segment sizes.
- `EBOOT.elf` and `EBOOT.BIN` must agree on their load-segment count.
- exactly one ISO candidate must be selected; ambiguous multiple-ISO workspaces fail closed;
- the ISO must contain an ISO9660 primary volume descriptor;
- the supported `UCUS98701` disc ID is searched for as an additional identity signal;
- `xbdata_extracted/` must contain files rather than merely exist as an empty placeholder.

The disc-ID scan is deliberately an additional signal, not a cryptographic identity guarantee. A
future importer should parse `PARAM.SFO`, record exact source hashes, and bind all generated inputs to
a private workspace manifest.

### Runtime closure

The run scope verifies:

- all 15 required VFPU table names and exact byte sizes;
- an x86-64 `SDL3.dll` from the build directory, repository root, or configured UCRT64 bin path;
- an x86-64 Vulkan loader;
- an x86-64 `build/hst/hst.exe`; and
- a nonempty `build/hst/hst_image.bin`.

The doctor's VFPU check remains a name/size baseline for workspace diagnosis; content
authentication, semantic invariants, checked indexing, and thread-safe publication now live in
the runtime loader itself (issue #187, `src/rt/vfpu_tables.c`, embedded SHA-256 manifest).

## Exit status

| Code | Meaning |
| ---: | --- |
| `0` | no failures; warnings are allowed unless strict mode is enabled |
| `1` | one or more fail-closed checks failed |
| `2` | strict mode was requested and one or more warnings remain |

The JSON output includes a schema version, counts, individual results, remediations, and the exit
code. Consumers should use the structured fields rather than scraping the human text.

## Privacy and legal boundaries

The doctor never reads more private data than needed for local validation, and it never transmits
file names, hashes, bytes, or reports. JSON output can still contain local paths, so treat saved
reports as private diagnostic material unless paths have been reviewed and redacted.

The doctor does not determine whether a user is legally entitled to decrypt, adapt, or use a game in
a particular jurisdiction. It verifies only the project's technical input contract. Users must
supply their own lawfully obtained copy and are responsible for applicable copyright,
anti-circumvention, contract, and local-law questions. See `NOTICE.md` and
`docs/PUBLICATION_READINESS.md`.

## Deliberate non-goals

The first version does not:

- install MSYS2, Python, SDL3, or Vulkan;
- download game files, keys, firmware, or third-party binaries;
- decrypt `EBOOT.BIN` or PRXs;
- run PPSSPP or collect its dumps;
- extract XB archives;
- create a portable player package;
- certify the repository or a binary as legally cleared, secure, reproducible, or release-ready.

Those operations require separate, transactional workflows with their own provenance and tests.

## Next simplification steps

The doctor is intended to become the validation layer beneath three later commands:

```text
ImportGame  -> prepare and bind private inputs transactionally
Play        -> choose no-build/incremental/full rebuild from manifests, then launch
PackageLocal -> produce a private runtime closure for a second Windows machine
```

Before implementing those commands, the project should complete the following investigations:

1. parse the ISO's `PARAM.SFO` and bind the disc ID/version to every derived input;
2. test current decrypters against the exact three game PRXs and record a versioned compatibility
   matrix rather than relying on generic tool claims;
3. pin `libxb` and make extraction containment, interruption recovery, and completeness
   machine-verifiable;
4. determine whether a direct XB virtual filesystem can remove the expanded asset tree entirely;
5. separate build-time dependencies from the files required only to run a completed local build;
6. unify the flattened `fs/` and hierarchical `memstick/` mappings under one safe Memory Stick root.
