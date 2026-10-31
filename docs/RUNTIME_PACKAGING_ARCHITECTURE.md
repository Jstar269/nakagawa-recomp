# Runtime Packaging Architecture

> **Status: CURRENT — maintained packaging decision record.** The process-isolation
> boundary, a local developer AOT package build, and the library's build-package →
> validate → launch route are implemented ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)).
> `tools/nk_cli.py build-package <disc_id>` resolves the executable from a library disc
> image, or from the decrypted `EBOOT.elf` in the title's folder for an encrypted disc (the
> player decrypts it from the user's disc, #295), and the
> player consumes the resulting package. Retail-disc hash validation, installers, and
> one-click end-user preparation without the developer toolchain (#308) are not built.

## 1. Context and Goals

Nakagawa Recomp is evolving from developer-centric tooling into an authentic cross-platform PSP recompilation platform.
The ultimate end-user experience target is:

~~~text
Download/install Nakagawa Recomp
  → Select a PSP ISO you own
  → Instant recognition & authentic local preparation
  → Launch & Play
~~~

A crucial design decision is how the native player (`nakagawa_player`) and the game execution runtime relate to each other at packaging and execution time.

---

## 2. Evaluation of Architectural Options

### Option A: Monolithic Multi-Title Executable

A single executable contains the UI launcher, the generic PSP runtime, the AOT recompilation chunks for all supported titles, and all HLE/LLE subsystems.

* **Advantages:**
  * Single binary distribution (`nakagawa.exe` / `nakagawa`).
  * No IPC or child process management.
* **Disadvantages:**
  * **Violates title isolation:** Different PSP titles require distinct code chunks, per-title build settings and different executable span allocations.
  * **Binary bloat:** Recompiled C for a large PSP game generates tens of megabytes of native machine code. Bundling 10 games into one executable would result in gigabyte-scale binaries.
  * **Crash vulnerability:** A crash in a guest module immediately terminates the entire launcher application, destroying UI state and unsaved player configuration.
  * **LLE/Fidelity violation:** Guest memory arena mappings (e.g. 192 MiB fixed address reservations) would conflict if multiple titles or sessions were managed in the same address space.

### Option B: Core Engine + Dynamic Title Plug-in (`.dll` / `.so`)

The native player loads a generic runtime engine, which dynamically loads a title-specific shared library (`hst_recomp.dll`, `phase5_recomp.so`) providing the entry points and chunk dispatch tables.

* **Advantages:**
  * Clean modular separation between front-end UI, generic runtime, and title code.
  * Extensible without rebuilding the launcher.
* **Disadvantages:**
  * **Fragile ABI boundary:** Exposing C function pointer tables between host modules across different compilers (e.g. MinGW vs MSVC vs Clang) easily creates ABI mismatches.
  * **Shared address space risks:** Like Option A, guest memory arena collisions and unhandled guest exceptions still jeopardize host launcher stability.

### Option C: Isolated Process per Title + Native Host Launcher (Adopted Architecture)

> **Implementation boundary:** the public source validates and launches a prepared
> runtime as a child process, and the library's **BUILD PACKAGE** action creates that
> runtime from a staged disc entry with live progress. It does not create a runtime
> from an arbitrary ISO in one click; that, and the distribution work in Section 4,
> remain target work.

The native player (`nakagawa_player`) acts as an authentic front-end and library manager. When a game is launched, it constructs a typed `NkLaunchSession` and spawns the title's standalone recompiled runtime as an isolated child process via `nk_platform_spawn_process`.
Communication and handoff occur via:

* Clean environment variables: `PSP_ISO`, `SR_FPS_CAP`, `SR_GPU_GE=1`, `SR_DEBUG`.
* Process lifecycle tracking: native wait, status queries, and platform-specific termination (forced on Windows; SIGTERM with bounded grace and SIGKILL escalation on POSIX).

* **Advantages:**
  * **Total Fault Isolation:** A crash or abort in guest execution never brings down the launcher UI. The launcher detects child process exit, captures the exit code, and reports diagnostic facts cleanly to the user.
  * **Independent Optimization:** Each title binary is compiled with its exact manifest tuning (`RUNTIME_OPT`, `RECOMP_OPT`, custom span definitions) without polluting other titles.
  * **Sandboxing & OS Portability:** Native child processes follow standard OS process lifecycles on Windows, Linux (SteamOS/Steam Deck), and macOS.
  * **Boundary benefit:** Keeps the launcher and title runtime in separate processes;
    regression risk and title acceptance still require focused testing.

---

## 3. Concrete Implementation in `src/core/`

The adopted process boundary is implemented in the following public source slice; this
list is not evidence that a distributable installer or title package exists:

* `src/core/nk_types.h`: `NkGameEntry`, `NkResult`.
* `src/core/nk_platform.h`: `NkProcessHandle`, `nk_platform_spawn_process`, `nk_platform_is_process_running`, `nk_platform_wait_process`.
* `src/core/nk_platform_win32.c`: Win32 `CreateProcessW` with UTF-16 executable/command-line conversion, environment block generation, and wait handles.
* `src/core/nk_platform_posix.c`: POSIX `fork` + `execv` with environment configuration.
* `src/core/nk_launch.h` & `src/core/nk_launch.c`: typed `NkLaunchSession` candidate resolution, environment construction, and process lifecycle management.
* `src/player/ui/player_ui_nk.c`: the default launcher's "PLAY" / "STOP GAME" action triggers; the older `src/player/ui_renderer.c`, kept only in the regression-test build (`--ui=legacy`), keeps its "PLAY NOW" trigger and active PID indicators.

---

## 4. Packaging and Distribution Roadmap

Maintainer decisions of 2026-10-23 (the per-OS choices; the shared install layout and manifest come
first and each OS is a thin wrapper around them):

| Target Environment | Packaging Strategy | Binary Artifacts |
| --- | --- | --- |
| **Windows x86-64** | Portable ZIP now; a per-user Inno Setup wrapper later. No MSI or MSIX | `nakagawa_player.exe`, `bin/nakagawa_core.dll` (or static), `build/<game>/<game>.exe`, SDL3.dll, SDL3_ttf.dll + resolved closure, `THIRD_PARTY_NOTICES/` |
| **Linux / Steam Deck** | Tarball plus a per-user `.desktop` entry, written by the same stager (`tools/stage_install.py`); AppImage later. No Flatpak: its sandbox conflicts with the host compiler the player needs | `nakagawa_player`, `bin/<game>`, `libSDL3.so` |
| **macOS ARM64** | App Bundle (`Nakagawa.app`), not yet planned in detail | `Nakagawa.app/Contents/MacOS/nakagawa_player`, helper executables in `MacOS/` |
| **Android** | Planned later (see `PLATFORM_PORTABILITY.md`); the user may move their own packages and saves to their own device | to be designed |

Roadmap items decided with the table:

* **C toolchain on Linux:** use the distribution's gcc, make and SDL3/Vulkan development packages,
  with guided install commands in the player. The project ships and downloads nothing for them.
* **Python on Linux:** the builder and `pyproject.toml` accept CPython 3.14 and 3.15
  (`src/player/package_builder.c`, `requires-python`). 3.15 stays the default. Widen further only to
  what the tool suite actually passes on, and only after the full suite runs green on that version.
* **One app ID on every host:** `nakagawa-recomp`, with the OS's own casing only where the OS
  expects it. Today the data folders are `Nakagawa` (Windows), `nakagawa-recomp` (Linux) and
  `NakagawaRecomp` (macOS); unify them before a release ships on a second host, with a migration from
  the old Windows folder.
* **Code signing:** v0.0.1 ships unsigned (maintainer decision, 2026-10-25). SignPath Foundation's
  free open-source programme declined the project for now because it is not yet widely used; reapply
  once releases have users. Until a build is signed, each release publishes the ZIP's SHA-256 and
  GitHub build attestations for every binary, and the release notes explain the SmartScreen prompt.
  Paid certificates stay a maintainer decision.

## 5. Local AOT package contract (v2)

The planner can run the existing analyzer, code generator, and two-phase Make
pipeline for a plaintext executable ELF. Run it from the repository root with the
documented UCRT64 toolchain:

~~~powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
python tools/title_codegen_plan.py assets/titles/my-title.json `
  --package `
  --game-elf place_game_here/EBOOT.elf `
  --output-dir "$env:LOCALAPPDATA\Nakagawa\data\developer-packages\my-title"
~~~

When the manifest requires PSP-header BSS metadata, also pass
`--psp-header <path>`. Guest PRXs are resolved by their manifest names beneath
`--module-dir <directory>`; required PRXs are selected automatically. Add one
`--include-optional-module <manifest-name>` for each optional PRX to include in
the AOT build. `--game-name` defaults to the manifest's `game_name`, then to its
portable `id`. The manifest's `codegen_profile` is authoritative. `--public-safe`
selects the synthetic/public-safe runtime backends for fixture builds.

The output directory must be dedicated and untracked. Retail package identity
contains local executable and module hashes, so the planner refuses to write a
retail package inside the repository; use the per-user data root as above or
the player's `build-package` action. The command refuses to reuse an existing
package directory when its manifest, executable, selected PRXs, or PSP header
hashes differ. The manifest remains unchanged. Make's `all` target still runs the generation and
compile phases separately so generated chunks are linked on a clean build.
A package build chunks generated C by size: `CHUNK_TARGET_BYTES` defaults to 4,000,000
bytes of C per translation unit (a value the caller sets wins, and the cache key names
it). Every function's C text, its order and the compile flags stay the same; only the
boundaries between translation units move (gcc inlines the runtime header's static inline
helpers into more functions of a smaller unit). Each compile job is then given memory in
proportion to the target (256 bytes per byte, at least 1 GiB, at most the 3 GiB that
count-based chunks need), so a host with little free memory still compiles several chunks
at once.

Only a single body can make its unit larger than the target (#1730). Codegen gives every body
over `BODY_FALLBACK_BYTES` (4,000,000 bytes of C, the same in every build) a unit of its own
that compiles at `-O0`: the level most packages used before, at gcc's lowest time cost. The
level does not bound its memory, which grows with the body at every level (about 170 bytes per
byte of C, linear up to the 4 MB budget: a generated body chained to 4 MB compiles at `-O2` in
about a minute and 670 MB). The compile phase therefore budgets each job from the largest
generated unit at the same 256 bytes per byte, without the 3 GiB cap, so a host runs fewer jobs
while such a unit is in the build. The generated `<game>_recomp_opt.mk` sets the unit's level
after `RECOMP_FLAGS`, so it is in the compile command and the object's cache key. It is a
required generated-code output, and any line in it other than a comment, a unit row or that
unit's level refuses the build before Make reads it. `build-report.json` lists each such unit
under `optimization.fallback_units`, with the level the rest compiled at. These are the only
compile flags that differ between units.
Current Make recipes cannot transport whitespace or shell-sensitive characters
in bound paths. Inputs at such paths are copied byte-for-byte into
`<output-dir>/staged-inputs/` (hashes are unchanged). When the output directory
contains spaces, packaging builds in a Make-safe workspace (`NK_BUILD_ROOT` or
Windows 8.3 short paths) and atomically promotes into the destination; otherwise
an unsupported path is rejected as `PACKAGE_UNSUPPORTED_PATH` (#296).

`package.json` is canonical JSON with this versioned shape:

| Field | Meaning |
| --- | --- |
| `format`, `schema_version` | `nakagawa-aot-package`, version `2`. |
| `title` | Manifest `id`, display name, kind, raw manifest SHA-256, and protected-semantics digest. |
| `inputs` | SHA-256 for the manifest, executable ELF, selected PRXs with load addresses, and optional PSP header. Absolute input paths are omitted. |
| `title_input_identity` | Versioned exact local inputs: manifest/profile and schema, DISC_ID/region/DISC_VERSION, selected PARAM.SFO facts, ISO container facts, decrypted main executable name/hash, required guest PRX names/hashes, raw selected executable and required module hashes from their ISO members, and optional PSP-header hash/magic. Retail hashes remain only in per-user package data. |
| `runtime` | `CpuState` ABI version and header hash, resolved guest run entry, runtime contract, bindings, and required bindings. |
| `executable` | Relative native executable path and hash plus the guest ELF entry address. |
| `generated_objects` | Sorted relative object paths and SHA-256 hashes. |
| `required_local_assets` | Manifest title-data/resource roots, selected or optional guest PRXs, and host SDL3/SDL3_ttf/Vulkan runtime requirements. These are references; title assets are not copied into the package. |
| `build_report` | Relative path `build-report.json`. |
| `cache` | Versioned AOT/native cache key, codegen options, and runtime ABI compatibility decision. |
| `completion-manifest.json` | Completion marker containing the cache key, the exact title input identity, and SHA-256 for every published artifact; written last. |

The cache key includes the canonical `title_input_identity` digest. The same
record is stored under `<user data>/title-input-identities/<DISC_ID>/` and the
completion marker. Native package validation compares the package against that
current per-user record and the selected library entry's SFO revision before
launch. It reports changed classes such as `main executable`, a named guest
module, `SFO revision`, or `manifest/profile`; it never prints input hashes.
When a library entry names a source ISO, validation also hashes the selected
executable and each recorded required module directly from that ISO before a
cached result can be reused. This rejects changed source bytes even when
`DISC_ID` and `DISC_VERSION` remain the same, and launch repeats the check
against the selected ISO before starting the runtime ([issue #315](https://github.com/Jstar269/nakagawa-recomp/issues/315)).

Validation is memoized for the life of a process, so the memo is keyed on a
status identity that covers every input it must not go stale against: the
package metadata files, the selected executable and the runtime image derived
from it, the current per-user title input record, and every artifact the
completion marker publishes. Each of those files contributes its size, its
write timestamp, and the platform's change value (`ChangeTime` on Windows,
`ctime` on POSIX). When that value advances, it detects a same-size edit inside
one write-timestamp tick, where size and write time are unchanged ([issue #683](https://github.com/Jstar269/nakagawa-recomp/issues/683));
it only ever turns a would-be hit into a revalidation. These values are
filesystem timestamps, not generation counters: resolution and support vary,
and an edit inside both timestamp granularities can still produce a cache hit.
Where a filesystem reports no change value, or a file cannot be opened for its
metadata, the identity falls back to size and write timestamp. Guaranteed
byte-level detection would require hashing package contents on every lookup.

`build-report.json` has format `nakagawa-build-report`, schema version `1`, the
same `input_hashes`, Python/planner/analyzer/codegen/Make versions, the compiler
identity, runtime ABI, and deterministic coverage counts (`analyzed_functions`,
`aot_entries`, callable/resume entries, fallbacks, and unsupported imports,
instructions, and regions). `coverage.static` is the total of the code generator's
`<game>_recomp_coverage.json`: owned, unowned, translated and interpreter-only words and
functions from the canonical CFG ([`DEBUGGING.md`](DEBUGGING.md#execution-share-and-static-coverage));
it is `null` only for a generation stage that predates the report. `unsupported.imports`, `unsupported.instructions`,
and `unsupported.regions` are always arrays. Each listed boundary includes a
reason, status `in the works`, and its tracking issue: import coverage uses #71;
unsupported Allegrex translation and AOT function regions use #118. Empty arrays
mean the analyzer/code generator did not report that boundary for these inputs;
they do not establish title acceptance. These records do not patch behavior:
generated unsupported functions retain the runtime's fail-closed path.

`build-report.json` is the one package artifact whose size grows with the title,
because it carries one record per AOT gap, unsupported instruction and analysis
diagnostic; a measured flagship report is about 1.3 MB. The package reader
therefore bounds it at 4194304 bytes and 262144 structural nodes instead of the
1 MiB ceiling that applies to `package.json`, the completion manifest and the
title input identity record, and the planner refuses to write a report larger
than that ceiling rather than leaving it to fail validation. The native runtime
validator still caps the same artifact at 262144 bytes, so a report between the
two sizes builds but is rejected there with `build-report.json is larger than
the supported JSON limit`; raising that native cap is a separate open gap.

This package command still requires the developer toolchain. It takes a plain
executable and does not unwrap a disc itself; `tools/nk_cli.py build-package
<disc_id>` performs the library-disc extraction and the player consumes the
resulting package. Checkout-independent run-directory provisioning landed
under #297, and the analyzer ownership and stack-balance gates under #291.

### Host runtime DLL staging (#421)

Every Windows route that ships or runs a player stages the same host runtime
DLLs with one mechanical step, `tools/stage_runtime_dlls.py`: `SDL3.dll` and
`SDL3_ttf.dll` (each overridable via `SDL3_DLL` / `SDL3_TTF_DLL`, otherwise
resolved from `C:/msys64/ucrt64/bin`), plus the SDL3_ttf dependency closure
resolved by walking PE import tables recursively and stopping at Windows system
DLLs. The step runs from the Makefile's `player` target (beside
`build/nakagawa_player.exe`) and from `tools/nk_cli.py build-package` (into each
built package). It never stages a partial closure: when an import is
unresolved, it names it. The player target then fails. A package build still
succeeds with its required SDL3, libiconv and Vulkan runtimes, and prints the
unresolved import as a warning. The prerequisite installer pins SDL3_ttf and its
closure (`assets/prereq_manifest.json`), so this happens only on a host without
them; that player draws with the bitmap fallback and logs why. Licence texts for
every staged DLL are copied beside them as `THIRD_PARTY_NOTICES/` by `tools/package_notices.py`, driven by
`assets/third_party_components.json`. Because the closure sits beside the
executable, launching from Explorer or a plain `cmd.exe` without MSYS2 on `PATH`
uses the readable TTF font; the 8x8 SDL bitmap font remains only as a logged
last-resort fallback (`NK_UI_NO_TTF=1` forces it), and the workspace doctor
reports `RUNTIME_SDL3_TTF`/`RUNTIME_SDL3_TTF_CLOSURE`.

## 6. Private content-addressed cache contract (#316)

A package is reusable only when every semantic input that can affect its output
is identified. The package layer, rather than a project release string, owns the
following key components:

| Key component | AOT generated C | Native objects/runtime package |
| --- | --- | --- |
| Exact executable bytes SHA-256 | required | required |
| Validated manifest/profile, exact title-input identity, and selected guest-module/PSP-header digests | required | required |
| Analyzer/codegen semantics epoch and analyzer/codegen content digests | required | required |
| Codegen options (profile, base/entry, spans, chunking, selected options, correctness modes) | required | required |
| Generated-code ABI epoch | required | required |
| Runtime ABI epoch | compatibility decision | required |
| Compiler identity (driver, version, `cc1`/`as`/`ld` content hashes) and target triple | not required | required |
| Runtime source/header digest (section 7) and output-affecting compile/link flags | not required | required |

The current package-layer epochs are `analyzer-codegen-v1`,
`GENERATED_CODE_ABI_EPOCH = 2`, and `RUNTIME_ABI_EPOCH = 2`. The generated-code
and runtime epoch values are compatibility contracts, not release versions. A
change to the generated C/runtime helper boundary must bump
`GENERATED_CODE_ABI_EPOCH`; a runtime-package ABI change must bump
`RUNTIME_ABI_EPOCH`. The CpuState layout (`SR_CPUSTATE_ABI_VERSION` in
`src/rt/recomp.h`) is part of both boundaries: generated C addresses CpuState
fields by offset, and the runtime package shares the struct with the generated
objects, so every CpuState version bumps both epochs. `CPUSTATE_ABI_EPOCHS` in
`tools/nk_core/package_cache.py` records the pairing (CpuState v2: epochs 1/1;
v3, which appended `llbit`: epochs 2/2), the native validator carries the twins
`NK_AOT_GENERATED_CODE_ABI_EPOCH` and `NK_AOT_RUNTIME_ABI_EPOCH` in
`src/core/nk_title_manifest.h`, and `tools/test_package_cache.py` pins the header
version, the table, the constants and the twins to each other. A package whose
cache key names an older epoch is `aot-regenerate` for the tools and
`NK_RUNTIME_PACKAGE_INCOMPATIBLE` for the player (the same outcome the
codegen/runtime digests and the package `abi_version` already force; the epochs
make the contract explicit). The package layer documents the bump because the
codegen agent owns the implementation epoch source.

The cache decision has three levels:

* `reuse`: the complete AOT and native key matches. No AOT generation or native
  compilation is run.
* `native-recompile`: generated C is reusable, but a compiler, target, runtime
  source, flag, or explicitly compatible runtime ABI change requires native
  objects to be rebuilt and relinked.
* `aot-regenerate`: executable bytes, manifest/bindings, analyzer/codegen
semantics, codegen options, title-input identity, or an incompatible generated/runtime ABI requires
  fresh AOT output.

A runtime ABI change reuses generated C only when the package layer explicitly
maps the previous epoch to the current epoch as compatible. Unknown or missing
compatibility is never treated as compatible. The decision record names every
changed component (`aot:executable_sha256`, `native:compiler_identity`, and so
on) for the CLI, player preflight, and Doctor.

The private cache root is `<user data>/cache/packages/<DISC_ID>/`, never the
repository. Builds are written to a content-addressed entry below that root,
assembled in a staging directory, and promoted with a directory rename. The
completion manifest is written last as `completion-manifest.json`; it contains
the canonical cache key and a SHA-256 record for every published file. Missing,
incomplete, symlinked, or digest-mismatched completion entries are not valid
packages and cannot be launched. The player and launcher report the failed
component and the `build-package <DISC_ID>` action instead of falling back to a
possibly stale executable. Completed cache entries are bounded to eight by
default; set `NK_AOT_CACHE_MAX_ENTRIES` to a positive per-title limit. Cleanup
never follows entry symlinks and preserves the entry used by the current build.

The cache contains private title-derived bytes and is intentionally outside the
public export and provenance tree. Publication policy remains default-deny for
`cache/` paths; a cache artifact accidentally proposed for publication is
rejected by the public-scope audit.

### Shared compiled objects (#1306)

Below the package entries, the compile phase shares objects between builds. Before
Make compiles, the planner keys every object it plans from the compiler identity
(driver hash and `gcc -v` report), the exact compile command with the build folder
written as a placeholder (so the optimization level and every flag count), and the
preprocessed source. A cached object with that key is copied into place, and the
compile writes the object's dependency file as Make would, so a later header change
still rebuilds it. An object that is compiled is stored only when its key, taken
again after the build, has not changed. The headers the build generates
(`make compile-inputs`) are written before any key is taken, and a key that still
cannot be taken is a miss, never a guess. Keys are taken on up to one worker per logical
CPU (at most 16), not on the Make job count: a key only preprocesses its source, which
needs a small fraction of the memory a compile of that source needs, so a build that
memory limits to one compile job still keys its objects in parallel. No compile carries
the build folder: the runtime finds its own folder at run time for its exit
diagnostics. The runtime
therefore compiles once and serves every game, and a rebuild of an unchanged game
compiles nothing.

* `<user data>/cache/objects/` holds runtime and game objects.
  `NK_OBJECT_CACHE_MAX_BYTES` bounds it (default 2 GiB, least recently used first).
  `NK_OBJECT_CACHE_DIR=0` turns reuse off for one build.
* `<user data>/cache/shared-libraries/` holds the system libraries a disc carries.
  Codegen writes a runtime-placed module that the build carries once to its own
  `nk_lib_<fnv1a64>.c`, named by the module's bytes, with symbols `l<fnv>_<link>` and
  `sr_l<fnv>_base` and nothing that names the game. A module over the library budget
  (`LIBRARY_UNIT_TARGET_BYTES` of C or `LIBRARY_UNIT_MAX_FUNCS` bodies, fixed in codegen
  and independent of the build's own chunk settings) is cut into
  `nk_lib_<fnv1a64>_<n>.c` units instead, each with every declaration (#1730). Another game with the same module
  compiles the identical file, and the build log names each library it reused or
  compiled. Settings > BUILD TOOLS AND CACHE turns this sharing off
  (`build-package --no-shared-libraries`) and clears the folder. A module carried
  twice in one build, or a stack-census build, keeps its indexed `m<k>` names inside
  the game's chunks.

Neither folder is shipped, uploaded or shared: both stay in the player's data
folder like the package entries.

### Packages built by an older Nakagawa (v0.0.2)

The package format, the cache schema and both ABI epochs are 2 in v0.0.1 and
v0.0.2, so none of them tells a v0.0.1 package from one v0.0.2 builds. What
does differ is the part of the cache key that comes from Nakagawa itself. Every
package since v0.0.1 records it, and the native validator already requires it:

| Value | Where the package records it | What it covers |
| --- | --- | --- |
| `runtime_source_digest` | `cache.key.native.components` | `src/rt`, `src/core`, `src/ui` and the third-party headers they include (`third_party/clay`, `stb`, `portable-snippets`), the `Makefile` and `mk/*.mk`, `assets/vfpu` (`package_cache.source_tree_digest`) |
| `analyzer_sha256` | `cache.key.aot.components` | `tools/analyze.py` |
| `codegen_sha256` | `cache.key.aot.components` | `tools/codegen.py` |
| `planner_sha256` | `cache.codegen_options` | `tools/title_codegen_plan.py` |

Measured on the tree: `source_tree_digest` gives `d1de9386...0beb` at the
v0.0.1 tag and a different value on main, and the three tool hashes changed
too. The player computes the same four values from the source tree a rebuild
would use (the folder above `tools/nk_cli.py`: a checkout, or a release's
`source/`), in C (`nk_title_manifest_source_build_identity`, byte for byte
the Python digest; `tests/native/test_player_state.c` and
`tests/native/test_player_ui.py` pin the two together). It reads a package's
values from `package.json` alone (`nk_title_manifest_aot_package_build_identity`).
A package whose values differ is *outdated*: rebuilding it compiles this
version's runtime, and regenerates its C when the translator changed. These
are exactly the components the build tools compare before they reuse a
package, so "outdated" means "a rebuild would change it".

An outdated package is not incompatible. Nothing in the format changed, the
validator accepts it, and it launches. A package that records no identity
(no planner digest, as hand-made fixtures have) is not judged at all.

**How a future release bumps it: it does not have to.** Any change to those
files moves the identity, so every release that changes the runtime or the
translator marks the packages the previous one built, and a release that
changes neither leaves them alone. A development checkout behaves the same
way: its packages read as outdated after a runtime edit, because they lack it.
Changes that must not trigger a rebuild stay out of those files.

What the player does (owner decision, 2026-10-29: suggest, never force):

* At start it scans the library on a worker. With outdated games and a
  `settings.json` whose `rebuild_prompt_version` is not this player's version,
  it shows one prompt naming the count, with Rebuild all and Later. Either
  answer records the version, so the prompt shows once per update.
  `rebuild_prompt_version` is an optional key: a file without it has never
  shown the prompt.
* After Later, a library line ("N games were built by an older Nakagawa.")
  offers Rebuild all and Hide. Hide lasts for the session; the line comes back
  at the next start while outdated games remain.
* Each outdated card says "Built by an older Nakagawa. Rebuild for the latest
  fixes.", and the title page offers Rebuild beside PLAY. PLAY asks once a
  session (Rebuild now or Play anyway); Play anyway launches as before.
* Rebuild all queues the outdated games and builds them one at a time, behind
  whatever the user is doing, through the same package builder as BUILD
  PACKAGE. The library line counts ("Rebuilding 2 of 5") and each card shows
  its state and compile progress. A game can be played until its own rebuild
  starts; while it runs, that game cannot launch and its card says why. A
  game whose disc image is missing is skipped and its card asks for the disc
  again; a failed or cancelled build is noted on its card and the queue moves
  on.
* The queue survives a restart in `rebuild-queue.json` beside `settings.json`:
  `{"format": "nakagawa-rebuild-queue", "schema_version": 1, "titles":
  [{"disc_id": "...", "state": "pending" | "done" | "missing_disc" |
  "failed"}]}`. A build that was running is saved as `pending` and starts
  again. A file with a newer `schema_version`, another format or an unknown
  member is left in place and not resumed. The file goes when nothing is
  pending.

A rebuild never removes the old package first. The build runs in the
content-addressed cache entry, is validated, copied next to the package,
validated again, and only then swapped in, with the old package moved back if
the swap fails (`_promote_cache_entry` in `tools/nk_cli.py`). A failed or
cancelled build therefore leaves the old package in place and launchable. The
one gap is a build stopped between the swap's two renames, which leaves the old
package in `cache/packages/<DISC_ID>/previous-<n>`: the next build of that
title moves it back before anything else (`_restore_interrupted_swap`).
Saves live outside the package and are not touched, and the cache keeps its
reuse: when only the runtime changed, the generated C is kept and only the
native objects are rebuilt.

### Generated-code stage reuse (bring-up)

`nk_cli.py bringup` generates each title's code once. Its codegen stage runs
inside the package build, after the package's preflight and cache-key
computation, as a planner call with `--aot-stage`: the same planner command,
inputs, title input identity and Make environment as the package build,
running only Make's generation phase (`pipeline`) into the work directory's
empty `codegen-stage/`. The stage writes the package completion manifest last,
holding the build's cache key, the title input identity and the SHA-256 of
every file it contains.

The package build then receives `--reuse-aot-stage codegen-stage/`. It compiles
the stage's output with `NK_AOT_PREGENERATED=1` (Make's `compile` phase) only
after the checks the native-only reuse of a package's generated C performs:
the completion record must be complete and every recorded artifact intact,
with no file added or missing, and the generated-code components of its cache
key must equal the build's own (`compare_cache_keys`, generated C reusable).
Each copied file is hashed as it is written and compared with the record, so
a stage that changes after its check is refused rather than compiled. Any
refusal regenerates the code from source with Make's `all` and is named:

| Reason | Cause |
| --- | --- |
| `AOT_STAGE_RECORD_MISSING` | The stage or its completion record is missing (an interrupted stage). |
| `AOT_STAGE_RECORD_INVALID` | The record is unreadable, incomplete, or inconsistent with its key. |
| `AOT_STAGE_ARTIFACT_MISMATCH` | A recorded file changed, is missing, or a file was added. |
| `AOT_STAGE_INCOMPLETE` | The record does not cover a required generation output. |
| `AOT_STAGE_INPUT_CHANGED` | Executable, manifest, module, PSP header or title input identity differ. |
| `AOT_STAGE_GENERATOR_CHANGED` | Analyzer or code generator content, or an epoch, differ. |
| `AOT_STAGE_OPTIONS_CHANGED` | Codegen options (profile, spans, chunking, user arguments) differ. |

The planner prints its decision as one `AOT_STAGE_REUSE:` line before Make
runs, and the sanitized bring-up report records it as `codegen_reuse`
(`REUSED`, `REGENERATED` with the reason, or `NOT_RUN` when a valid package
already held the key's generated C). The report's
`counts.unsupported_opcodes` is read from the stub report inside the built
package, so it describes the code that shipped; only a build that fails
before packaging falls back to the stage's stub report.

## 7. Per-user data formats and versions

A user who updates Nakagawa keeps every library entry, setting, mapping and
save, and rebuilds a package only when something the package was built from has
changed. Every file the player keeps for the user carries its own version, and
one rule (`src/core/nk_user_data_schema.h`) governs each writer:

* An **older** file is read and carried forward one version at a time
  (v1 to v2 to the current version). It is not rewritten on load; the first save
  first copies the original, byte for byte, to `<file>.v<version>.bak`, and does
  not write when that copy fails.
* A **newer** file, written by a later player, is refused by name and never
  replaced: the session runs on defaults, the screen says why, and every save
  over it is refused with `NK_ERROR_NEWER_SCHEMA`. Returning to the newer player
  finds the file as it left it.
* A file with no readable version (empty, damaged, not JSON) is reported, and the
  reader falls back as before (`library.json` to its `.bak`, the others to defaults).

The folders are the per-user ones of [`SETUP.md`](SETUP.md) (on Windows
`%LOCALAPPDATA%\Nakagawa\data` and `...\config`, plus the saves folder).

| File | Version field and history | An older file | A newer file |
| --- | --- | --- | --- |
| `<data>/library.json` (`src/core/nk_library.c`; read too by `tools/nk_core/library.py` and `nk_cli`) | `schema_version` 1, the only version so far (v0.0.1 and later). `source_image_path`, added after v0.0.1, is an optional member v0.0.1 skips | (none exist) | Refused; the Library screen names it. The older `.bak` beside it is not loaded in its place, and no save replaces either file |
| `<config>/settings.json` (`src/player/player_state.c`) | `schema_version` 1 (before v0.0.1), 2 (v0.0.1 and later) | 1 to 2: an `fps_cap` of 30 or 60 becomes PSP timing; `settings.json.v1.bak` on the first save | Refused; this session runs on defaults and every Settings page says changes are not saved |
| `<config>/input_profile.json` (`src/core/nk_input_profile.c`) | `schema_version` 1, 2 (per-title map, #520), 3 (keyboard keys, #1299; v0.0.1 and later) | 1 to 2: no per-title map; 2 to 3: default keys. `.v1.bak` or `.v2.bak` on the first save | Refused; the Controls page names it and SAVE does not replace it |
| `<saves>/<DISC_ID>/` | The PSP's own Memory Stick layout (`PSP/SAVEDATA/...`), defined by the PSP rather than by Nakagawa; the root rule (`nk_launch.c`) is unchanged since v0.0.1 | n/a | n/a |
| `<data>/packages/<DISC_ID>/package.json`, `build-report.json`, `completion-manifest.json` | `nakagawa-aot-package` schema 2, cache key and completion schema 2, and the ABI epochs of section 6 (all unchanged since v0.0.1) | Built output, not user data: a package from another epoch is INCOMPATIBLE and its card offers REBUILD PACKAGE | INCOMPATIBLE with the card line "built by a newer Nakagawa (package format N; this player reads 2)". The player never changes it; only a rebuild the user asks for replaces it |
| `<data>/title-input-identities/<DISC_ID>/title-input-identity.json` | `schema_version` 2 | Rebuilt by the next package build | Read as absent; the package card asks for a rebuild |
| `<data>/fonts/v2/manifest.json` | `schema_version` 2, also named by the folder | n/a (v0.0.1 wrote `v2`) | A later layout gets a folder of its own and leaves `v2` as it is |
| `<data>/experimental/<DISC_ID>/profile.json` | `schema_version` 1 | n/a | Not accepted: the package check fails closed |
| `<data>/registry/system.json` (the PSP system settings a game changes; `src/rt/hle.c`, `SR_SYSTEM_REGISTRY` overrides the path) | `schema_version` 1, the only version so far (v0.0.1 and later) | (none exist) | Ignored: the game sees the modeled defaults, and the next registry flush moves the file to `system.json.corrupt` (kept, not deleted) before writing a fresh one |

Logs, the update download (kept only when its SHA-256 matches) and the
content-addressed package cache (section 6) hold no state a user could lose.

`fixtures/user_data/` holds each of these files as an earlier player's own writer
wrote it, and `tests/native/test_player_state.c`,
`tests/native/test_input_profile.c` and `tools/test_nk_core.py` load, migrate and
round-trip them, and prove that a newer file is kept.

**What a package is keyed on after an update.** The native key's
`compiler_identity` names the driver by its content hash, its version
(`-dumpfullversion`) and the content hash of each program the driver runs to build
the package (`cc1`, `as`, `ld`, as `-print-prog-name` resolves them), with no
paths: a binutils update replaces the assembler and linker without touching the
driver, and now recompiles the native package (generated C is reused), while an
unchanged toolchain at another path keeps the key. `runtime_source_digest` covers
what a package is compiled from: `src/rt`, `src/core`, the overlay's UI library
`src/ui`, the third-party headers those include (`third_party/clay`, `stb`,
`portable-snippets`), the `Makefile` and its `mk/*.mk` fragments. The player's own
sources (`src/player`) are not in a package and no longer rebuild one;
`tools/test_package_cache.py` checks that every third-party directory the runtime
includes is listed.

## 8. Portable layout, runtime identity and user state

This section records what the Windows portable ZIP holds today and where the player keeps
everything it writes ([#324](https://github.com/Jstar269/nakagawa-recomp/issues/324)). The
installer, upgrade and uninstall rules that #324 also asks for are not built yet; the portable
ZIP is the one supported distribution form.

### The install image (read-only)

The release ZIP (`docs/PREVIEW_RELEASE.md`) and `tools/stage_install.py --dest` share one
layout. Nothing in it is written at run time, so it can sit in a read-only folder:

| Path | Contents |
| --- | --- |
| `bin/nakagawa_player.exe` | The player. Release builds also carry `bin/production_smoke.exe` and the showcase demos in `bin/demos/`. |
| `bin/*.dll` | `SDL3.dll`, `SDL3_ttf.dll` and their resolved closure (Section 5, #421). |
| `bin/runtime-manifest.json` | The runtime identity record (below). |
| `bin/assets/fonts/ui/`, `bin/font/nk*.pgf` | The project's own UI fonts and generated PGF fonts. No firmware font. |
| `bin/THIRD_PARTY_NOTICES/`, `SOURCE.txt`, `RELINK.md` | Licence texts for every bundled binary, the exact source commit, LGPL relinking. |
| `source/` | The public-safe source export BUILD PACKAGE runs from (`tools/nk_cli.py`, the Makefile, `assets/prereq_manifest.json`). |
| `prereqs/` | The pinned build prerequisites (#1285), checked by the player against `installed.json` and `bundle-inventory.txt` before BUILD PACKAGE uses them. |
| `docs/`, `README.md`, `LICENSE`, `NOTICE.md`, `START HERE.txt` | User documentation. |
| `SBOM.*`, `toolchain.json`, `SHA256SUMS.txt` | SPDX 2.3, SPDX 3 and CycloneDX SBOMs, the attested toolchain record, and the SHA-256 of every other file. |

The image never holds a game, decrypted module, generated title code, save, key or firmware
font: the release script's forbidden-file and private-path scans refuse them.

### Runtime identity manifest

`bin/runtime-manifest.json` (format `nakagawa-runtime-manifest`, schema 1) lists every
`.exe` and `.dll` directly in `bin/` with its size, SHA-256 and the non-system DLLs it imports,
plus `host_libraries`, the imports the host provides and no route ships (today only the Vulkan
loader `vulkan-1.dll`, from `host_resolved_components` in `assets/third_party_components.json`).
`tools/stage_runtime_dlls.py --runtime-manifest` writes it after staging; writing fails with
`RUNTIME_CLOSURE_INCOMPLETE` when a binary imports a DLL that is neither beside it, a Windows
system DLL nor a host library, which is the loader failure a clean machine would otherwise show.
The attestation workflow writes and attests it with the binaries, and
`--verify-runtime-manifest` checks a staged or extracted `bin/`: every listed file present with
its size and SHA-256 (`RUNTIME_FILE_MISSING`, `RUNTIME_FILE_CHANGED`), no unlisted `.exe` or
`.dll` (`RUNTIME_FILE_UNLISTED`), imports unchanged (`RUNTIME_IMPORTS_CHANGED`) and a licence
record in `THIRD_PARTY_NOTICES/index.json` for every DLL (`RUNTIME_LICENCE_RECORD_MISSING`).
`tools/stage_install.py` writes the same file into an installed `bin/`, so portable and
installed layouts share one identity contract. The player does not read it yet; a start-time
check is open under #324.

### Reproducible portable ZIP

`tools/stage_install.py --portable-zip <zip> --zip-stage <folder>` writes the ZIP from the
staged folder. It rewrites `SHA256SUMS.txt` in ordinal path order, verifies
`bin/runtime-manifest.json`, and stores one entry per file and per empty folder, sorted by
UTF-8 path, every entry dated `SOURCE_DATE_EPOCH` (the commit time of `--revision` unless the
variable or `--source-date-epoch` sets it; times before 1980 become 1980-01-01), mode 0644 for
files and 0755 for folders, Deflate level 9, no extra fields and no comment. A link, junction,
special file, absolute or `..` name is refused. The same staged bytes then give the same ZIP
bytes with the same CPython zlib build, which the tool reports as `zlib`.

Measured on 2026-10-29 at `77a1a5e3`, before this change: two packages built from one commit
differed in every ZIP entry's timestamp (`ZipFile.CreateFromDirectory` stores each file's write
time, and the prerequisite tree is unpacked fresh each time), and the player executable differed
in its PE `TimeDateStamp` and checksum because its link line lacked `--no-insert-timestamp`.
Both are fixed; `tools/test_stage_install.py` builds one stage twice in different orders and
with different file times and compares the bytes.

### Per-user state (writable)

Everything the player writes lives outside the install image, under the per-user root, so an
upgrade or a second extracted version finds the same library, saves and settings. On Windows
the root is `%LOCALAPPDATA%\Nakagawa` (then `FOLDERID_LocalAppData`, then `%APPDATA%`; never
`USERPROFILE` alone, `nk_platform_resolve_windows_data_base`). `--user-data-root=<folder>`
moves all of it into one folder (`nk_platform_set_app_data_dir_override`).

| Folder (Windows) | Holds | Owner |
| --- | --- | --- |
| `Nakagawa\config` | `settings.json` | player |
| `Nakagawa\data` (`<user data>`) | the library and title manifests | player, `nk_cli` |
| `<user data>\titles\<DISC_ID>\` | staged title inputs; `decrypted\` holds the player's decryption output or the user's own plain files | user's private inputs |
| `<user data>\title-input-identities\<DISC_ID>\` | the exact local input identity a package must match (Section 5) | `nk_cli` |
| `<user data>\cache\packages\<DISC_ID>\` | built AOT packages, content-addressed, promoted by rename (Section 6) | `nk_cli` |
| `<user data>\cache\objects\`, `cache\shared-libraries\` | shared compiled objects and disc libraries (#1306) | `nk_cli` |
| `<user data>\fonts\v2\` | fonts imported from the user's own PSP, with `manifest.json` | user |
| `<user data>\prerequisites\` | the consented per-user prerequisite download (absent when `prereqs/` is bundled) | player |
| `<user data>\downloads\` | a verified update ZIP and, outside a release folder, its unpacked copy | player |
| `<user data>\logs\` | build logs (`build_<DISC_ID>_progress.jsonl`) | player, `nk_cli` |
| `Nakagawa\saves\<DISC_ID>\` | the title's Memory Stick root (`SR_MEMSTICK`) when the install is not writable | runtime |
| `Nakagawa\logs\` | `game-<DISC_ID>-last.txt`, the last game session's output | player |
| `Nakagawa\cache\` | the platform cache folder (`NK_PATH_CACHE`) | player |
| `Nakagawa\fonts\` | optional UI font overrides | user |

Linux uses `$XDG_DATA_HOME/nakagawa-recomp` (else `~/.local/share/nakagawa-recomp`) and the XDG
config and cache homes; macOS uses `~/Library/Application Support/NakagawaRecomp`. Each launch
still starts one child process per title (Section 2, Option C), with the #297 run-package and
save-root semantics unchanged.

Updates today: Check for updates (`src/player/update_check.c`) downloads a release ZIP into
`<user data>\downloads\`, keeps it only when its SHA-256 matches the published `.sha256`, and
INSTALL unpacks it into `<folder>.partial` beside the running version, checks every file
`SHA256SUMS.txt` lists, and renames it into place. It never replaces the running version and
never touches the per-user folders. Removing an extracted folder removes the install image
only; the per-user folders above stay until the user deletes them.
