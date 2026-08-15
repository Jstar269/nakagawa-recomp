# Debugging Guide

Maintained guide to the runtime's primary debug categories, commonly used environment variables,
and supported diagnostic workflows. It is not an exhaustive inventory of every specialized
`SR_*` switch: the implementing source and `hst_manager.ps1` are authoritative for diagnostic
switches that are added for a focused investigation and have not yet been promoted into this guide.

## Quick Start

```powershell
# Enable all debug categories
$env:SR_DEBUG = "0xFF"
.\build\hst\hst.exe --image build\hst\hst_image.bin 0 0 none none --gui

# Enable only memory and HLE tracing
$env:SR_DEBUG = "0x03"
.\build\hst\hst.exe --image build\hst\hst_image.bin 0 0 none none --gui
```

Prefer the profiles in `hst_manager.ps1`, which clear stale diagnostics before launching. Most
legacy Boolean `SR_*` switches are presence-based: assigning the string `"0"` can still enable
them. Disable such a switch with `Remove-Item Env:NAME -ErrorAction SilentlyContinue` (or
`$env:NAME = $null` in `pwsh`), not `NAME=0`. Numeric settings such as
`SR_FBSNAP=<N>` and the `SR_DEBUG` bitmask are exceptions.

For performance investigations, add `-GuestProfile` to any manager `Run` profile. It enables the
existing low-noise guest-PC profiler independently of the verbose scheduler/HLE diagnostics and
dumps its call/block summary periodically and when the runtime exits. The manager's default period
is 3,600 vblanks (about one minute at the intended cadence); tune it with
`-GuestProfilePeriod`, or pass `0` for exit-only capture. Combine it with `-Profile Benchmark` to
correlate guest hot paths with `logs/perf.csv`; compare the unprofiled Benchmark run first because
guest instrumentation and profile output have measurable overhead.

## Debug Categories (SR_DEBUG bitmask)

The `SR_DEBUG` environment variable accepts a hex bitmask to enable multiple categories at once:

| Bit | Hex | Category | Description |
| ----- | ------ | ---------- | ------------- |
| 0 | 0x01 | `SR_DBG_MEM` | Memory access logging (out-of-range, watches) |
| 1 | 0x02 | `SR_DBG_HLE` | HLE syscall dispatch tracing |
| 2 | 0x04 | `SR_DBG_SCHED` | Thread scheduling events |
| 3 | 0x08 | `SR_DBG_GE` | GE command processing |
| 4 | 0x10 | `SR_DBG_INPUT` | Input state changes |
| 5 | 0x20 | `SR_DBG_FS` | Filesystem / I/O operations |
| 6 | 0x40 | `SR_DBG_VIDEO` | Display, framebuffer, vblank |
| 7 | 0x80 | `SR_DBG_MISC` | Everything else (fonts, callbacks, etc.) |

**Examples:**

- `SR_DEBUG=0xFF` — All categories
- `SR_DEBUG=0x03` — Memory + HLE
- `SR_DEBUG=0x0D` — Memory + Sched + GE
- `SR_DEBUG=0x02` — HLE only

## Legacy Environment Variables

Individual `SR_*` variables still work for backward compatibility. When `SR_DEBUG` is not set, these are checked and mapped to categories:

### Memory & Access (→ SR_DBG_MEM)

| Variable | Description |
| ---------- | ------------- |
| `SR_OORLOG=1` | Log out-of-range memory accesses |
| `SR_BREAKLOG=1` | Log sr_break() calls |

### HLE & Syscalls (→ SR_DBG_HLE)

| Variable | Description |
| ---------- | ------------- |
| `SR_HLELOG=1` | Trace HLE syscall dispatch |
| `SR_NIDLOG=1` | Log NID lookups |

`SR_NIDLOG=1` writes one line per syscall to `nidseq_mine.txt` in the run
directory, NID first: `0x<nid> <uid> <vblank>` (e.g. `0x780f88d1 0x105 123`).
The NID is always the first whitespace-separated token so simple `awk '{print
$1}'` extraction keeps working; consumers that only need the call sequence
should ignore the trailing uid/vblank columns.

### Thread Scheduling (→ SR_DBG_SCHED)

| Variable | Description |
| ---------- | ------------- |
| `SR_THLOG=1` | Trace thread create/start/exit |
| `SR_BLOCKLOG=1` | Log thread blocking events |

### GE & Graphics (→ SR_DBG_GE)

| Variable | Description |
| ---------- | ------------- |
| `SR_GELOG=1` | Log GE command processing |
| `SR_GEWATCH=1` | Interleave GE present with GELIST lines |
| `SR_GEWATCH_AFTER=N` | Start GE watch after frame N |
| `SR_GEMATW=1` | Log GE matrix writes |
| `SR_NO2DZ=1` | Disable 2D Z-buffer |
| `SR_GPU_STATS=1` | Emit bounded Vulkan submission/batch/texture/snapshot counters without per-submit logging |
| `SR_GEDUMP=1` | Per-primitive `GE PRIM` lines (through and transform), bounded to the first ~40 |
| `SR_GE_ENQUEUE_TRACE=1` | Trace GE enqueue/stall-update provenance without enabling the broader GE dump |
| `SR_GE_ENQUEUE_TRACE_WINDOWS=a-b[,c-d]` | Restrict enqueue trace output to up to eight inclusive vblank ranges; malformed ranges fail closed |
| `SR_GESTAT=1` | 60-frame stat windows: `GESTAT` totals, `GE3D` distinct 3D draw signatures, `ASHADE`/`ACLUT` alpha-test-failure decode |
| `SR_RTRACE=1` | Exhaustive render trace: one `TRIDRW` line per 3D draw with the complete GE state (render target `fbp`/`zbp`, texture address/format/`bufw`/swizzle, texture function, blend, alpha test, cull, scissor, viewport), plus per-triangle `TRIDEC` lines |
| `SR_RTRACE_FRAMES=N` | Frames traced per stat window (default 2) |
| `SR_TEXDUMP=1` | Write each distinct sampled texture from transform- or through-mode draws once as `tex_ADDR_fF_WxH.ppm` decoded through the real sampler (swizzle + CLUT), and log its CLUT address/format. First 32 distinct addresses per run |
| `SR_TEXDUMP_AFTER=N` | Defer texture dumping until GE frame `N`, preserving the fixed distinct-texture budget for a late deterministic scene |

> [!IMPORTANT]
> **`SR_RTRACE` re-arms only when `SR_GESTAT` is also set.** Its per-window frame budget is reset
> inside the `SR_GESTAT` 60-frame window block in `ge_set_frame()`. With `SR_RTRACE=1` alone you get
> `TRIDRW` lines for the first `SR_RTRACE_FRAMES` transform-mode frames of the **whole run** — which
> during boot means you capture the logo screens and nothing else. Always pair them:
> `SR_GESTAT=1 SR_RTRACE=1 SR_RTRACE_FRAMES=1`.
>
> Note also that `SR_TEXDUMP`'s 32-address budget is per **run**, not per window. On a long route,
> pair it with `SR_TEXDUMP_AFTER` so boot textures cannot consume that budget. Generated texture
> and alpha images are private runtime artifacts and must not be committed.

`SR_GE_ENQUEUE_TRACE` is intended for a narrow renderer-independent submission check. Each record
contains the vblank, operation, thread UID, `$ra`-derived callsite, list/stall/callback values, and
the last HLE and guest callback observed on that thread. A paired result record reports whether the
list was deferred, stalled, completed, or missing, plus the completed list's command signature,
primitive count, backend-independent `through`/`transform` command-vertex-sprite tuples, and write
counters. Pair it with
`SR_GE_ENQUEUE_TRACE_WINDOWS`, for example `8200-8300,34800-35200`, on long deterministic routes.
The diagnostic does not skip guest work or change GE execution.

### Input (→ SR_DBG_INPUT)

| Variable | Description |
| ---------- | ------------- |
| `SR_INLOG=1` | Log input state changes |
| `SR_PAD=HEX` | Override pad state (hex buttons) |
| `SR_PADPERIOD=N` | Automatic pad-pulse period in vblanks (default 240) |
| `SR_PADWIDTH=N` | Automatic pad-pulse width in vblanks (default 4) |
| `SR_PADSTART=N` | Pad override start frame |
| `SR_PADSCRIPT=FILE` | Scripted pad input: a state-qualified route program, or the legacy `frame hexmask width` table |
| `SR_NOINPUT=1` | Disable the automatic START pulse; live and scripted input still work |
| `SR_ROUTE_LEARN=1` | Print the route signature of every sampled and captured frame (`ROUTE_SIG v=<n> <hex>`) |
| `SR_ROUTE_NO_EXIT=1` | Do not terminate on a route failure (executable regression tests only) |

`SR_PADSCRIPT` is the preferred way to make a visual route repeatable. Prefer a
**route program** (below) for anything that has to be trusted as evidence; the
legacy numeric table is still accepted unchanged for existing routes, and its
parser accepts numeric rows only, so do not put headings in such a file.
To turn a recorded run into a replay:

```powershell
$env:SR_INLOG = "1"
.\hst_manager.ps1 -Action Run -Profile Standard
python tools/padscript_from_log.py logs/stderr_run.log `
  --minimum-width 8 --output logs/route.pad
$env:SR_PADSCRIPT = (Resolve-Path logs/route.pad).Path
$env:SR_NOINPUT = "1"
.\hst_manager.ps1 -Action Run -Profile Standard
```

The converter expands shorter presses because a one-vblank desktop automation
pulse can fall between the game's controller reads. Use
`--minimum-width 1` only when an exact-width replay is required.

### State-qualified route programs (issue #64)

A route written as absolute vblanks is a bet that the guest is on the screen its author saw
when they recorded it. Boot and transition durations vary between otherwise identical
replays, so the bet loses: seven replays of one script from one restored save baseline
reached two different menu depths, and the two divergent runs spent their whole budget in
Story Mode rather than the intended Exhibition match. **Both still reported a complete run**,
because "reached vblank N" was the only thing anything checked. Elapsed vblanks are a budget,
never a proof of state.

A route program makes each input wait for the screen it assumes. `SR_PADSCRIPT` selects it
automatically: a file containing any keyword line is a program, a file of bare numeric rows
keeps the original behaviour exactly, and a file mixing the two is refused.

| Line | Meaning |
| ---- | ------- |
| `SIGGRID <cols> <rows>` | Signature grid, default `12 8`; must precede every `CHECKPOINT` |
| `SAMPLE_EVERY <n>` | Observation cadence in vblanks (default 20) |
| `TOLERANCE <n>` | Default match tolerance, mean absolute channel difference (default 12); must precede every `CHECKPOINT` |
| `CHECKPOINT <NAME> [tol=<n>] <hex>` | A screen signature; repeat `NAME` to record it again (see below) |
| `WAIT <NAME> <timeout>` | Block until `NAME` is observed; fail the run on timeout |
| `EXPECT <NAME>` | Assert `NAME` is on screen now; fail the run if it is not |
| `PRESS <hexmask> <width>` | Hold `hexmask` for `width` vblanks |
| `PRESS_UNTIL <NAME> <hexmask> <width> <period> <timeout>` | Repeat the press every `period` vblanks until `NAME` is observed; fail on timeout |
| `DELAY <n>` | Advance `n` vblanks (input cadence *within* one screen) |
| `END` | Route complete |

`#` starts a comment. A screen is "observed" by a coarse signature of the presented
framebuffer: the frame is split into `cols x rows` cells and each cell contributes its mean
R, G and B; a screen matches when the mean absolute difference from a recorded signature is
within tolerance. Sampling only runs while a `WAIT`, `EXPECT` or `PRESS_UNTIL` is pending,
so a route pays nothing for it while pressing or delaying.

**Record a screen twice when part of it varies.** HST draws its menus over a club backdrop
that is not the same every run, so a whole-frame comparison rejects the right screen: two
recordings of the Main Menu taken from different runs sit 14 apart, well outside any
tolerance that still separates the Main Menu from its submenu. Repeating a `CHECKPOINT`
name records the same screen again, and the bytes the recordings disagree on are dropped
from the comparison — they carry the variation, not the identity. On the two observed
backdrops that leaves about half the frame informative and the Main Menu ~8x closer to
itself than to the submenu. Recordings sharing less than a quarter of the frame are refused
at load: they are not one screen, and a route built on them could not fail.

**`PRESS_UNTIL` is for the boot prefix.** The warning screens and intro movie each need
their own START and there is no way to know in advance how many. As a fixed table, every
extra press is one that lands on whatever comes next when the run is faster than the
recording — which is precisely how a `CROSS` meant for the title screen ended up opening a
menu. `PRESS_UNTIL TITLE_SCREEN 0008 8 240 15000` stops the moment the title appears.

**Failure is loud and terminal.** A failed `WAIT` or `EXPECT` prints `ROUTE_FAIL:` naming the
step, the vblank and the screen that was actually there, then exits **86**. The manager reads
that narration back into `oracle_manifest.json` (`route_kind`, `route_checkpoints`,
`route_fail_reason`) and a failed or unfinished route makes the run **inadmissible**, so a run
that took a different path through the menus can no longer be archived as though it were the
route it names.

Authoring a checkpoint takes one learning run:

```powershell
$env:SR_ROUTE_LEARN = "1"
.\hst_manager.ps1 -Action VisualOracle -Route logs/route_legacy.pad -ExitAtVblank 9500 `
    -SnapEvery 60 -SnapWindows "7800-9200" -SaveBase logs/oracle_savebase -OracleName learn
```

Every captured frame emits its signature at the same vblank
(`ROUTE_SIG v=8220 <hex>` beside `snap_v8220.ppm`), so you convert the frame you actually
looked at — `python tools/ppm2png.py snap_v8220.ppm out.png`, identify the screen, then paste
that vblank's hex into a `CHECKPOINT` line. Signatures are derived from retail frames: they
belong in the private route file beside the rest of the run inputs and must never be
committed.

Two habits keep a program honest. Gate every screen *transition* with `WAIT`, and use `DELAY`
only for input cadence inside one screen — a `DELAY` standing in for a transition is the
fixed-vblank bet again. And put an `EXPECT` after a press whose effect you care about: `WAIT`
proves you arrived, `EXPECT` proves the press did what the route claims.

### Visual-oracle runs (`-Action VisualOracle`)

A visual regression oracle cares about a handful of frames around one transition, but a plain
`Run` replays the whole route with captures on from vblank 0 and stops on a wall-clock
`-Duration` guess. Guessing low silently truncates the route before its last inputs fire;
guessing high replays a finished scene for minutes. Each capture writes a ~380 KB PPM *and* an
unbounded `build/snapshots` PNG, so a long route spends hundreds of megabytes of I/O on frames
nobody looks at.

```powershell
.\hst_manager.ps1 -Action VisualOracle -Route logs/route_X.pad `
    -ExitAtVblank 41400 -SnapEvery 60 -SnapAfter 39000 -OracleName deep_return_run1
```

It reuses whatever `hst.exe` is already built (build once, replay many), stops at a **vblank**
count rather than a wall-clock guess so the stop point is machine-independent, captures only
inside the window of interest, and archives `snap_*.ppm`, `stderr.log`, `oracle_summary.txt` and
`oracle_manifest.json` under `logs/oracle_<name>/`. Add `-Profile Benchmark` to collect
`perf.csv` alongside the captures; it goes through the same runner, so a measured run and a
visual run are the same replay.

**One run per archive.** Snapshots are numbered per run, so a shorter second run into the same
directory leaves the first run's tail behind in a set that still looks complete. Reusing an
`-OracleName` whose directory is non-empty is therefore **rejected**, not merged; pass
`-OverwriteOracle` to discard the old evidence deliberately.

**Every run is adjudicated.** `oracle_manifest.json` records Git HEAD (and whether the worktree
was dirty), the SHA-256 of both `hst.exe` and the route file, every parameter, the process exit
code, capture count, wall time, and the vblanks actually delivered. The run is reported
`complete` only if it reached its requested vblank, exited 0, was not killed at the backstop, and
produced captures; otherwise the reasons are listed and the action fails. A truncated capture set
looks exactly like a complete one on disk — reading one as complete already cost two full replays.

**The backstop is a deadline, not a duration.** The runner returns the instant `hst.exe` exits
and only kills at the deadline, so an over-generous backstop costs nothing.

#### Holding guest save state still (`-SaveBase`)

A route replay is deterministic in its **inputs** only. The game writes a real save (the
give-up path ends in "Finished saving data."), so run N+1 starts from whatever run N left
behind. This is not theoretical: two replays of the identical deep-return route diverged
because the first run's save cleared a first-time tutorial popup — the second run hit that
popup, took a different branch, and ended in a new match instead of at the club.

```powershell
.\hst_manager.ps1 -Action VisualOracle ... -SaveBase logs/oracle_savebase
```

First use captures the current save as the baseline; every later run restores it, so all runs
start byte-identical. It is a snapshot-and-restore, **not** a wipe — deleting the save would
put the game in a "no save data" state no existing route was authored against. `*GAMEDATA` is
never touched: that is the ~400 MB install, and removing it would trigger a reinstall that
changes the route's timing completely.

**Safety contract.** `-SaveBase` is a path *inside the repository root* (the manager
anchors every managed path to its own script location, never the caller's CWD). The baseline
and the live `memstick/PSP/SAVEDATA` root must be distinct canonical directories, and neither
may contain the other. On first use the manager writes `.hst_savebase_manifest.json` into the
baseline directory recording a creation timestamp, a non-secret source identity, the relative
file inventory and per-file SHA-256s. A baseline without that manifest — an arbitrary directory
someone pointed at — is **refused** on restore, as are empty or tampered baselines. Restore
preflights the baseline, stages a verified copy beside the live root, swaps the save directories
into a rollback shelter and the staged copies into place, verifies the result against the
manifest, and only then drops the rollback; any failure rolls back and aborts loudly. The
manifest lives inside the ignored baseline directory, so no save contents are ever tracked.

**Interrupted-restore residual.** The swap is two same-volume renames (live → rollback, staged →
live). A hard OS/process termination (not a caught exception) landing between them cannot be made
transactionally atomic, so the live root can be left partially populated with the original data
stranded in an orphan `.hst_savebase_*` directory beside it. The next restore invocation detects
any such orphan and **fails closed** with an explicit message rather than running against a
possibly-partial state; manual recovery (re-inspecting the orphan and either completing or
removing it) may be required. This is an acknowledged residual, not a crash-atomicity guarantee.

`-OracleName` is an identifier, not a path: it must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}` and
archive directories are created (and cleared) only under the logs root, with reparse-point
escapes refused. The manager itself fails closed when run from outside a validated workspace —
copy it into an unrelated tree and it refuses to start.

Without `-SaveBase`, treat two runs as two different experiments, not two samples of one.

#### Capturing both ends of a transition (`-SnapWindows`)

A "did this screen come back correctly?" comparison needs the *before* and *after* frames from
the **same run**. The club backdrop varies with host wall-clock time, so a capture from an
earlier session is not a valid reference and a difference against it proves nothing.

```powershell
.\hst_manager.ps1 -Action VisualOracle -Route logs/route_E_deep_return_20260725.pad `
    -ExitAtVblank 44000 -SnapEvery 60 -SnapWindows '8300-9200,35500-44000' `
    -OracleName deep_return_run1
```

`SR_FBSNAP_WINDOWS=<a>-<b>[,<c>-<d>...]` (up to 8 ranges) captures only inside the listed vblank
ranges. With windows active, files are named by the vblank that produced them
(`frame_v<vcount>.ppm`) rather than rotating through 8 slots — so neither window can overwrite the
other, and every file records when it was taken. Without windows the rotating name is unchanged,
so existing routes and tooling are unaffected. `-SnapEvery` still sets the cadence inside a
window. Like the other controls this is a host-side gate: no guest work is skipped and captured
frames are byte-identical to an ungated run.

FBSNAP/FBDUMP capture is **present-truthful** (the old `sdl3vk_capture_swapchain_ppm`
was an invalid acquisition that could read a stale/undefined image and published a PPM under a
`.png` name). The swapchain capture is now armed *before* the present and recorded inside the same
command buffer that blits the displayed frame, then published atomically as an exact P6 `.ppm`
(`build/snapshots/frame_<n>.ppm`, or `frame_v<vcount>.ppm` with windows). The legacy guest-VRAM
`snap_*.ppm`/`snap_v*.ppm` files (route evidence via `dump_fb_fmt`) are still written unchanged.
`SR_FBDUMP=<N>` publishes the presented frame as `present_source.ppm` and exits; the exit status is
0 only if a capture was truly published, 1 otherwise (the run must not be claimed as captured when
nothing was written). `gpu-capture-selftest` (Verify step 15) byte-checks both CPU- and GPU-source
captures and asserts zero validation-layer errors under `SR_VULKAN_VALIDATION`.

#### Where `SR_EXIT_AT_VBLANK` actually stops

It is the **last statement of `sr_vblank_tick()`**. At that point vblank *V* is complete in
everything that function owns: the frame counter is advanced, `ge_set_frame(V)` has run,
`sr_ctrl_sample()` has latched *V*'s controller sample (so a pad-script press scheduled *for* V is
delivered before the exit), and the latch assist and no-frame watchdog have run.

It does **not** wait for work outside the tick. Guest threads this vblank resumed run after it
returns, and a frame whose `sceDisplaySetFrameBuf` lands later in vblank *V* is neither presented
nor captured. So:

- schedule a route's last input comfortably before *V* — a few hundred vblanks of settle;
- expect the last useful capture to come from an earlier vblank than *V*.

The controls are host-side only. The guest executes every vblank exactly as it would under
`Run`; pacing is untouched, no guest work is skipped, and captured frames are byte-identical to
the ungated run. **Do not** reach for `SR_NOVBPACE=1` to speed a route up: that is turbo mode, it
jumps over idle delay waits, and it demonstrably changes game speed (it is the behaviour vblank
pacing was added to fix). A faster route is worthless if it is not the same route.

`tools/hst_run_support.ps1` holds the three helpers whose failure modes are silent (bounded wait,
archive reset, completeness verdict); `tools/test_visual_oracle.py` exercises them against real
processes and directories.

### Filesystem & I/O (→ SR_DBG_FS)

| Variable | Description |
| ---------- | ------------- |
| `SR_IOLOG=1` | Log file open/read/write operations |
| `SR_STATLOG=1` | Log stat operations |
| `SR_PATHHEX=1` | Log path hex values |
| `SR_UMDDUMP=1` | Dump UMD data |

### Display & Video (→ SR_DBG_VIDEO)

| Variable | Description |
| ---------- | ------------- |
| `SR_VBLOG=1` | Log vblank events |
| `SR_FBSNAP=N` | Every N vblanks: legacy guest-VRAM `snap_<n>.ppm` (route evidence) plus present-truthful P6 `build/snapshots/frame_<n>.ppm` (see below) |
| `SR_FBSNAP_AFTER=V` | Suppress every capture before vblank V (host-side gate only) |
| `SR_FBSNAP_WINDOWS=a-b[,c-d]` | Capture only inside these vblank ranges; names files `frame_v<vcount>.ppm` so windows cannot overwrite each other (legacy `snap_v<vcount>.ppm` still written) |
| `SR_EXIT_AT_VBLANK=V` | Terminate cleanly (status 0) at the **end** of vblank V's tick (see above) |
| `SR_FBDUMP=N` | At vcount=N publish the presented frame as `present_source.ppm` and exit; status 0 only if a capture was truly published, else 1 |
| `SR_NOVBPACE=1` | Disable vblank pacing |

For a replayable GE fixture, set `SR_GE_CAPTURE_FRAME=<vblank>` or
`SR_GE_CAPTURE_FRAME=<first>-<last>`. The range form captures the first submitted frame in that
bounded window that meets `SR_GE_CAPTURE_MIN_PRIMS`; it is useful when a deterministic input route
does not submit a display list on exactly the same vblank every run. `SR_GE_CAPTURE_PATH` selects
the private `.ngef` output path. Captures remain private game-derived inputs and must not be
committed.

### Misc (→ SR_DBG_MISC)

| Variable | Description |
| ---------- | ------------- |
| `SR_FONTLOG=1` | Log font operations |
| `SR_MPEGLOG=1` | Log mpeg operations |
| `SR_DLGLOG=1` | Log dialog operations |
| `SR_CBLOG=1` | Log callback operations |
| `SR_SYSLOG=1` | Log system calls |
| `SR_WAKELOG=1` | Log thread wakeup events |
| `SR_FONTDIR=ABSOLUTE_PATH` | Override font directory (relative values are rejected; unset uses the executable's sibling `font`) |
| `SR_DATAROOT=ABSOLUTE_PATH` | Override the extracted-XB data root (relative values are rejected; unset uses the executable-anchored HST tree). The executable-anchored root and walked descendants reject reparse points; an explicitly configured root is operator-trusted and may be a junction for a staged long-path fixture. Access-time replacement races inside that trusted root are not a containment boundary. |
| `SR_FSDIR=PATH` | Override writable host storage; relative paths, including `.`/`..`, are resolved against the current directory |

### Scheduling & Behavior

| Variable | Description |
| ---------- | ------------- |
| `SR_VBLANK_Q_US=N` | Vblank quantum in microseconds |
| `SR_WATCHDOG_EXIT=N` | Abort after N vblanks with no new frame; a firing is a NO-NEW-FLIP observation, not a hang verdict -- classify it with the display counters, thread dump, and MPEG/PSMF activity the watchdog prints |
| `SR_NO_RELAUNCH=1` | Disable thread relaunch |
| `SR_NO_THREAD_REUSE=1` | Disable thread reuse |
| `SR_NOAUDIO=1` | Disable audio output |
| `SR_POSTUMD=1` | Post-UMD processing |
| `SR_HEAP_BASE=HEX` | Override heap base address |
| `SR_PARTITION_TOP=HEX` | Override partition top |
| `SR_MEMSTICK=PATH` | Override memstick path |
| `SR_CALLCOUNT=1` | Enable call counting |
| `SR_CBLOG=1` | Log callback create/register/notify/dispatch to stderr |
| `SR_PGD_KEYS=PATH` | Optional local PSP KIRK/amctrl constants binding; the PGD/amctrl implementation and key guidance are excluded from the public-safe candidate |

There is no `SR_HLE_CONTINUE` switch. In a scheduled game run, an unimplemented NID is fatal;
returning zero would turn an unknown operation into phantom success. Register the NID with real
semantics, and use `SR_DISPATCH_FATAL=1` when locating silent non-PLT dispatch misses.

## Interpreting the audio-thread semaphore trace

PSP thread UIDs are allocated at runtime and can change between runs. Identify this worker by its
guest entry address, `0x00082a14`, rather than by a UID such as `0x133` or `0x134`.

The generated guest logic disproves the earlier "WaitSema retries without SignalSema" theory:

- helper `0x00086d9c` waits at `0x00086df0` and unconditionally signals at `0x00086e54`;
- helper `0x00082f14` waits at `0x00082f5c`, scans state, and unconditionally signals at
  `0x00082fbc`; only its `sceSasCore` work is conditional.

Generic `HLE: calling` lines are emitted once per `(uid, nid)`, while the WaitSema handler can
emit a line on each call. A repeated `count=1 need=1` line therefore does not show that no signal
ran between waits; it shows an immediately satisfiable wait. Trace both handler bodies or the
guest PCs before inferring a missing branch.

## Audio-observation sampling traps

Two sampling traps produced a false "voice audio is silent" diagnosis for a short title/logo
voice sting. Both are methodology lessons, not game-specific behavior:

- **Power-of-two-only push sampling can miss short audio windows entirely.** The `AUDIO_PUSH`
  logger under `SR_AUDIOLOG=1` samples the first 16 calls and then only power-of-two call counts.
  A short voice/SE window (tens of vblanks) can fall entirely between sampled calls, so "every
  sampled push was peak 0" does not prove silence. When testing a short audio event, log by
  semantic trigger (keyon) or over a bounded vblank window around the event instead.
- **A VAG voice slot can legitimately begin with a zero block before real ADPCM data.** Dumping
  only the first 16 bytes at keyon (or decoding only the first 28-sample block) can classify a
  live stream as zero/silent. The decoder's second block at source offset `+0x10` is the first
  data block. Under `SR_SASLOG=1`, `SAS_VAG_B0`/`SAS_VAG_B16` capture both blocks per voice
  (bounded, two per voice), and `SAS_MIX_V` shows whether voice position advances across grains.

## CLUT start offset

Do not change the renderer's `((clut_fmt >> 16) & 0x1f) << 4` start calculation based on the old
scan-note hypothesis. PPSSPP's
[`getClutIndexStartPos`](https://github.com/hrydgard/ppsspp/blob/f0baf3ade7bcb6c86f0835962b36eb4e51559d8f/GPU/GPUState.h)
uses the identical expression: the start field selects a 16-byte unit, not an individual
palette entry. Both renderers share this decode path. A palette bug still needs draw-level
evidence, but `<< 4` itself is not one.

## Memory Watch System

The debug framework supports watching specific memory address ranges. When `SR_DBG_MEM` is enabled, any read/write to a watched address is logged.

### Adding Watches in Code

```c
#include "debug.h"

// At startup, add watches:
sr_add_mem_watch(0x002cf6dc, 0x002cf6e0, "asset_bucket");
sr_add_mem_watch(0x002de908, 0x002deb60, "param_name_bucket");
```

### Programmatic Usage

```c
// Check if an address is watched (returns 1 if logged)
sr_check_mem_watch(addr, val, 1/*write*/, pc);

// Or use the macro for conditional logging
if (SR_DBG(SR_DBG_MEM)) {
    dbg_mem(addr, val, 1/*write*/, pc);
}
```

When a transient buffer's address changes between runs, watch the written value instead:

```powershell
$env:SR_VALUE_WATCH_0 = '0x440b4000,PANEL_X0'
```

`SR_VALUE_WATCH_0` through `SR_VALUE_WATCH_15` accept an unsigned 32-bit value and label.
Matches are reported as `MEM_VALUE_WATCH[...]` with the destination address, value, and guest PC.
Address and value watches share the 16-entry watch table. Value watches are diagnostics only and
do not change guest memory or execution. Configuring a watch explicitly enables its focused log;
`SR_DEBUG=MEM` is not also required.

For a bounded register snapshot at one exact writer, add
`SR_WATCH_CONTEXT_PC=0x<guest-pc>`. `SR_WATCH_CONTEXT_LIMIT` defaults to one and may be set from
1 through 1024. A snapshot is emitted only when an address/value watch matches at that PC; it
includes all GPR/FPR raw values and does not pause or alter guest execution. This is useful when a
dynamic buffer value identifies the final writer but its source operands must be traced further.
For a writer shared by several values, `SR_WATCH_CONTEXT_FPR=<index>,0x<raw-value>` adds an exact
raw FPR-bit condition (for example, `20,0x436e0000` selects `f20 = 238.0f`).

When the value itself is common enough that a value watch would flood a long route, use
`SR_STORE_CONTEXT_PC=0x<guest-pc>` instead. It emits a bounded register snapshot only for a store
originating at that exact generated-code PC, without logging other writes. `SR_STORE_CONTEXT_LIMIT`
defaults to one and accepts 1 through 1024. `SR_STORE_CONTEXT_MEM=<gpr>,<offset>,<words>` optionally
dumps 1 through 32 guest words relative to a GPR; for example, `16,0x24,7` snapshots seven words
starting at `r16 + 0x24`. These switches are observational and do not pause or modify execution.

To correlate a decoded through-sprite with the guest code that builds its dynamic vertices, set
`SR_GE_ARM_RECT=x0,y0,x1,y1`. When GE observes that exact integer rectangle it adds deduplicated
write watches for both source vertex records. Later reuse of those records is reported through
the normal `MEM_WATCH[...]` log with the exact guest writer PC. The shared 16-range watch limit
still applies, and the option never changes vertex data or rendering.

## Crash Reporter

When the program crashes (access violation, etc.), the crash reporter dumps:

1. **Exception info** — Exception code and host address
2. **Fault address** — Mapped to guest address if in arena
3. **Memory watches** — Shows if fault address is in a watched range
4. **Host registers** — Full x64 register state (RIP, RSP, RAX, etc.)
5. **Guest registers** — Full PSP CpuState (PC, SP, RA, all GPRs, HI/LO, FCR31)

Example output:

```text
=== PSP RECOMPILER CRASH REPORT ===
Exception: 0xc0000005 at host 0x00007ff6abc12345
Fault: READ of host 0x000001a3b5c00000 -> guest 0x0830b000 [WATCHED: asset_bucket]

--- Host Registers ---
RIP=0x00007ff6abc12345  RSP=0x000000f1a3b00000
RAX=0x0000000000000000  RBX=0x000001a3b5c00000
...

--- Guest CpuState ---
PC=0x00222a00  SP(r29)=0x09f00000  RA(r31)=0x00222b00
r4=0x0830b000  r5=0x00000100  r6=0x00000004  r7=0x00000000
...
=== END CRASH REPORT ===
```

## Debug Output Format

All debug output goes to stderr with consistent formatting:

```text
CATEGORY: key=value key=value ...
```

Examples:

```text
MEM_R: addr=0x0830b000 val=0x12345678 pc=0x00222a00
HLE: nid=0x00000001(sceDisplaySetFrameBuf) pc=0x00222b00
SCHED: create_thread uid=0x111 entry=0x00222a00 pri=32 stack=0x2000
GE: list_submit addr=0x04000000 stall=0x00000000
INPUT: buttons=0x00000000
FS: Open(./sce_lbn0x0e0f) -> 0x00000000
VIDEO: present fb=0x04000000 fmt=3 stride=512
```

## Troubleshooting

### Black screen

1. Set `SR_DEBUG=0x0D` (MEM + SCHED + GE) to trace boot sequence
2. Check for HLE calls that halt: look for `HLE:` lines followed by process exit
3. Verify thread scheduling: look for `SCHED:` lines showing thread creation

### Crash on startup

1. Check crash report for guest PC — this shows where execution stopped
2. Look for `MEM_R` or `MEM_W` lines just before the crash
3. If address is out of range, the game may need additional memory regions

### Performance issues

1. Avoid `SR_DEBUG=0xFF` in production — causes massive stderr output
2. Use specific categories: `SR_DEBUG=0x02` for HLE tracing only
3. Set `SR_WATCHDOG_EXIT=N` to abort after N vblanks without a newly presented frame; the firing is a NO-NEW-FLIP observation, so classify it with the display counters, thread dump, and MPEG/PSMF activity the watchdog prints, not the threshold alone
