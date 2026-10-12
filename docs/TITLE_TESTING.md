# Title testing: the public qualification route

Status: CURRENT. This page documents the public, source-owned route that qualifies
a title candidate: validate its manifest and input profile, then launch it through
the native player and record the result. It covers public synthetic and homebrew
titles only. Retail titles are local-only and are not qualified here.

Tracking: [#699](https://github.com/Jstar269/nakagawa-recomp/issues/699). Umbrella
for title intake: [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).

## What the route proves

| Claim | Evidence the route produces | Where it stops |
| --- | --- | --- |
| The manifest is a valid public title | `tools/title_manifest.py` (the normative validator) accepts it; its kind is `synthetic` or `homebrew` | Schema and validator agree on the root contract (checked on every `validate`) |
| The input profile maps the standard controls | Every PSP control and navigation action is bound exactly once; host names come from the runtime's own tables | Structural and vocabulary checks; the runtime parser remains the acceptance authority at load time |
| The title's staged build exists | `build/<title id>/<title id>.exe` is present (`--require-build`) | A build is not re-run by `validate`; `title-qualification-smoke` rebuilds the fixture it launches |
| The native player launches the title in an isolated profile | The player spawns the staged runtime, the display name it advertises matches the manifest, and the user data root and `LOCALAPPDATA` are sandboxed | Only the bundled sample entry the player exposes for the title (see below) |
| A first presented frame carries visible pixels | `BOOT_EVENT phase=first_frame` reports nonzero pixels, and ordered `frame_present` and `HOST_PRESENT_SUBMITTED` markers agree | The offscreen presenter. An on-screen window is not captured by this route |
| The run ends cleanly | The child exits with code 0 after its guest finishes | Window-close `QUIT` is not exercised; see "Not proven" |

The route does not prove title correctness, retail compatibility, PSP hardware
behaviour, audio, or input responsiveness. Those are separate evidence classes;
see [`COMPATIBILITY.md`](COMPATIBILITY.md) and [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md).

## Commands

Validate every checked-in public manifest and input profile. Nothing is built or launched:

```powershell
mingw32-make title-qualification
```

Build the source-owned display fixture and the native player, then launch the fixture
through the player and write its report:

```powershell
mingw32-make title-qualification-smoke
```

The same tool is available directly:

```powershell
python tools/title_qualification.py validate [--manifest PATH] [--input-profile PATH] [--report PATH] [--require-build]
python tools/title_qualification.py smoke --manifest assets/titles/display-smoke.json [--input-profile PATH] [--timeout SECONDS]
```

Exit status 2 means different things per command. For `validate` it means at least
one candidate failed validation, and the first problem in each failing candidate is
printed. For `smoke` it means the candidate was refused before any launch, and no
report is written. `validate` exits 0 when every candidate passes. `smoke` exits 0 for
a launch that passes, and 1 for a launch that runs and fails or whose staged build is
missing; in both cases the report names the stage. `--help` states the same codes.

## Refusals

A refusal stops before any launch and writes no report:

- a manifest whose kind is `retail`, or that declares a `disc_image`;
- a title with no public launch surface (the player does not bundle a sample entry for it);
- a native player that is not built.

A candidate whose staged build is missing is not a refusal. It writes a schema-valid
report that fails at the `compile` stage with `ENTRY_NOT_COMPILED`, so the absence
is recorded rather than skipped.

## The input profile

`assets/input_profiles/standard-gamepad.json` is the public default mapping. It is
the runtime's own built-in default written out in the schema-2 profile format that
the player saves to `<config>/input_profile.json`, and the route stages it into the
sandbox config folder for the run.

Rules the validator enforces, in addition to the runtime parser's:

- `schema_version` is 2, and the file carries only the global mapping. A `per_title`
  entry is refused, so no disc identity lives in a public profile.
- `psp_bindings` names each of the 14 PSP controls exactly once. An unbound control is
  written as `"none"`, never omitted.
- `navigation_bindings` names each of the nine navigation actions exactly once.
- A host input drives at most one PSP control.
- Calibration fields are present, and their values are in range. Combined deadzones
  stay below 32767.

The vocabulary (control names, host buttons, host axes, navigation actions) is taken
from `src/core/nk_input_profile.c`. `tools/test_title_qualification.py` parses those
tables and fails if the validator drifts from them.

## The qualification report

`smoke` writes `build/<title id>/title-qualification/bringup-report.json`. The file is
a bring-up report: it validates against `assets/bringup_report.schema.json` and uses
the same stage names, failure classes, issue numbers and presentation evidence that
`tools/nk_cli.py bringup` writes. `tools/library_sweep.py` therefore reads a public run
with its own vocabulary.

Stage mapping, from the bring-up report to the library sweep's furthest-stage names:

| Report state | Sweep stage |
| --- | --- |
| `compile` FAIL (`ENTRY_NOT_COMPILED`) | `compile` |
| `launch` FAIL or TIMED_OUT | `launch` |
| `launch` PASS with `presentation.frame_submissions` above 0 | `first presented frame` |

Failure classes used by this route, all from the bring-up `failure_class` enum:

| Failure class | Meaning |
| --- | --- |
| `ENTRY_NOT_COMPILED` | The staged build is absent |
| `LAUNCH_FAILED` | The child exited nonzero, or the player launched something other than the staged title |
| `LAUNCH_TIMEOUT` | The run exceeded `--timeout`; the player and its runtime were stopped |
| `NATIVE_RUNTIME_CRASH` | The runtime printed a crash report |
| `HEADLESS_UNAVAILABLE` | The runtime reported no video device |
| `DISPLAY_PROGRESS_UNVERIFIED` | No offscreen window, or no first frame with visible pixels |
| `NO_FRAME_SUBMISSIONS` | Presentation evidence is missing or out of order |

The raw player output is kept beside the report as `player-output.log`.

## Isolation

Every launch runs in a temporary sandbox inside the title's build directory:

- `LOCALAPPDATA` and `APPDATA` point into the sandbox, so `%LOCALAPPDATA%\Nakagawa`
  and `%APPDATA%` are never read or written;
- `HOME` and the `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and `XDG_STATE_HOME`
  bases point into the sandbox as well, so a POSIX player never reads or writes `~/.config`,
  `~/.local/share`, `~/.cache` or `~/.local/state`;
- `--user-data-root` points at the sandbox, so the library and caches stay there;
- video and audio use the dummy drivers and the offscreen presenter, so no window opens;
- `SR_FLIGHT*` and `SR_DATAROOT` are removed from the child's environment.

The sandbox is deleted when the run ends.

## Launch surfaces

The native player launches a sample entry only from its bundled demo list, and the
route names that entry by index. `tools/title_qualification.py` lists the titles it can
launch in `PUBLIC_LAUNCH_SURFACES`. Today that is `display-smoke-v1` at index 1.
The route also checks that the display name the player advertises for that index
matches the manifest's `display_name`, and that the spawned runtime is the staged
title. A mismatch is a `LAUNCH_FAILED`, never a pass.

Adding a title means adding a launchable public entry to the player first, then one
line to the table, then a test that runs it.

## Not proven

These items remain open. Each needs the named input or change before the route can
claim it.

- **On-screen checkpoint.** The first frame is captured by the offscreen presenter. A
  window checkpoint needs an interactive display session and the windowed launch
  (`smoke` without `SR_VIDEO=offscreen`). Nothing in this route opens a window.
- **Window-close `QUIT`.** The run ends with the guest's own exit. Closing the
  player window while the title runs is not exercised.
- **Input responsiveness.** The standard profile is validated, but no input is sent to
  the title. The `input-responsive` stage is not reported.
- **Generic intake.** Only the display fixture has a launch surface. Other public
  titles need a bundled entry or a generic launch path, tracked under
  [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) and
  [#309](https://github.com/Jstar269/nakagawa-recomp/issues/309).
- **Hardware.** Nothing here runs on a PSP. Hardware claims need the oracle route
  in [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md).
