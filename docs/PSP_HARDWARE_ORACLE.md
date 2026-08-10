# PSP hardware-oracle readiness

**Status: a PSP-3001 on 6.61 ARK CFW with PSPLINK was verified in the latest
local session, and the Nakagawa production-HLE stream is now available.** The
stream is emitted by the bounded `hle_thread_selftest.exe` path, enters the
registered NIDs through `sr_syscall`, and hashes that executable artifact. The
three kernel cases now have measured hardware acceptance; broader routed issue
criteria remain separate and open. This page does not install firmware, CFW,
USB drivers, or PSPDEV.

## What is checked in

- `fixtures/psp_oracle/` is a synthetic PSPDEV `BUILD_PRX=1` fixture. It has no
  HST bytes, retail assets, firmware, keys, screenshots, or private traces.
- `tools/psp_oracle/protocol.py` parses deterministic scalar records and emits
  `MATCH`, `DIFFERENCE`, `PSP_ONLY`, `NAKAGAWA_ONLY`, or `INCONCLUSIVE` without
  assigning causality to a difference.
- Every comparison additionally carries `acceptance_eligible` and
  `acceptance_blockers`. A classification is a *comparison fact*; acceptance
  evidence additionally requires host-measured provenance on both streams. The
  checked-in fixture emits `model=unknown`, `firmware=unknown`, and all-zero
  `binary_sha256`/`source_commit`, which parse as valid but are reported as
  placeholders, so a placeholder-vs-placeholder `MATCH` can never be read as a
  hardware pass. `provenance_issues()` is the programmatic form of this rule.
- `tools/psp_oracle/manifest.json` maps the first smoke, kernel, I/O, display,
  system, and audio probe groups to the open issue matrix. `PSP-KERNEL-001` and
  `PSP-SMOKE-001` are implemented from measured sessions. The smoke comparison
  used the generated guest body from the source-owned PSP ELF, not a host-side
  sum implementation; its exact PRX/ELF/host hashes and report are recorded in
  the manifest. The other group IDs remain explicitly `planned` and none may
  be reported as hardware PASS until a real producer and its API cases exist.
- `tools/psp_oracle/run_psplink.py --dry-run` produces a redacted launch plan;
  capture mode requires an explicit host command and bounded timeout.
- The Nakagawa-side stream is the `--psp-oracle` mode of
  `src/rt/hle_thread_selftest.c`, built by `hle-thread-selftest-build` and
  invoked with `psp-oracle-nakagawa`. It drives callback notification,
  semaphore poll/signal, and thread lifecycle through the production NID
  registry; output scalars and PASS/FAIL status are derived from those returns.
  `out2`/`out3` retain the raw semaphore and deleted-thread return codes rather
  than only their sign predicates. The mode hashes the exact selftest executable
  that emits the record; the MinGW PE link uses `--no-insert-timestamp` so a
  repeated build under the same toolchain is byte-reproducible. The
  existing `oracle/hardware-results/` Nakagawa files made by the removed
  emitter remain invalid and must not be used as acceptance evidence.
- The old coroutine fix is retained: `16fbb0a` made main-coroutine adoption
  idempotent and `1d8d494` added bounded lifecycle instrumentation. The normal
  `mingw32-make hle-thread-selftest` recipe is now a safe bounded runnable gate;
  the current exact-head run reports 341 checks and 0 failures. Use the
  `psp-oracle-nakagawa` sub-mode when only one scalar production-HLE stream is
  needed.
- `docs/PSP_ISSUE_MATRIX.json` is a generated snapshot of all currently open
  issues. Refresh it with `python tools/psp_issue_matrix.py` before a new session.
- `python tools/psp_readiness.py --json` is the single preflight command. A
  missing PSPDEV/PSPLINK is reported as `OPTIONAL_MISSING` or
  `HARDWARE_NOT_CONNECTED`; it is not silently treated as a passing hardware
  result.

## Local preflight

From the repository root:

```powershell
python tools/psp_readiness.py --json --run-focused
python tools/pspdev_probe.py --out build/audit/pspdev-tool-probe.json
python tools/psp_oracle/run_psplink.py --dry-run --prx fixtures/psp_oracle/nakagawa_psp_oracle.prx
```

`psp_readiness.py` resolves each PSPDEV tool from the host `PATH`, from the
default WSL distribution, or from an operator-configured client directory, since
on Windows the supported layout is PSPDEV under WSL with the PSPLINK PC clients
running natively (WSL2 has no USB passthrough without `usbipd`). Two optional
environment variables keep host-specific paths out of the tracked tree:

| Variable | Purpose |
| --- | --- |
| `PSP_ORACLE_PSPLINK_PC_DIR` | directory holding `usbhostfs_pc`/`pspsh` |
| `PSP_ORACLE_PPSSPP_HEADLESS` | explicit PPSSPP headless binary |

**Tool presence is not device presence.** The `pspdev:link` check looks for the
PSPLINK USB endpoint (Sony `VID_054C`, `PID_01C9`), which exists only while
PSPLINK is running on the PSP. A PSP in USB mass-storage mode presents `PID_02D2`
and is deliberately rejected — it cannot answer a probe. `hardware_ready` stays
false until that endpoint is present.

## Emulator smoke test before hardware

PPSSPP headless captures test output through a pseudo-device rather than
`printf`: `sceIoDevctl("emulator:", 2, buf, len, NULL, 0)`, with command `3`
reporting whether an emulator is present at all (see PPSSPP
`Core/HLE/sceIo.cpp`; this is the same channel `pspautotests` uses). The probe
detects that device and emits through it, falling back to `printf` for PSPLINK
on real hardware, so one binary serves both.

```powershell
make -C fixtures/psp_oracle EBOOT.PBP
& $env:PSP_ORACLE_PPSSPP_HEADLESS --timeout=20 -j fixtures/psp_oracle/EBOOT.PBP
```

The emulator run emits `source=ppsspp`, so the comparator's role check reports
`acceptance_eligible: false` even when every provenance field is measured. That
is intentional and must not be worked around: PPSSPP is a third comparison
point, never a PSP oracle. Use it to prove the probe, protocol, and comparator
chain before spending a hardware session.

When PSPDEV is absent, install it outside this worktree as a deliberate human
action. The official Windows route uses Ubuntu under WSL; these commands are
recorded here so the next session does not have to rediscover the setup, but
the agent must not run the administrator/restart step:

```powershell
# Administrator PowerShell; human action, may require a restart.
wsl --install -d Ubuntu
```

Then, in the Ubuntu shell, use the pinned distribution from
`assets/upstream/pspdev.lock.json` rather than an unrecorded moving download:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkgconf libreadline8 libusb-0.1 libgpgme11 libarchive-tools fakeroot wget
cd "$HOME"
wget https://github.com/pspdev/pspdev/releases/download/v20260501/pspdev-ubuntu-latest-x86_64.tar.gz
sha256sum pspdev-ubuntu-latest-x86_64.tar.gz
tar -xvf pspdev-ubuntu-latest-x86_64.tar.gz
printf '\nexport PSPDEV="$HOME/pspdev"\nexport PATH="$PATH:$PSPDEV/bin"\n' >> "$HOME/.bashrc"
. "$HOME/.bashrc"
psp-config --pspdev-path
```

The recorded archive digest is
`cac06732ba81efcc6a9e5b6196e60a5c373b0a87f28518a188b7c99b70ccb012`; stop if
the measured digest differs. If Docker Desktop is intentionally available,
the locked image is an alternative that does not install into WSL:

```powershell
$fixture = Join-Path (Get-Location) 'fixtures/psp_oracle'
docker run --rm --mount "type=bind,src=$fixture,dst=/workspace" -w /workspace `
  pspdev/pspdev@sha256:184eb28094f907493928f556df68f5d3e71bcf340259c5c6b78b1d9ac28b6c7d make
```

Verify the image/archive against the lock before treating a PRX as toolchain
evidence. The normal HST build does not depend on PSPDEV. These commands follow
the [official Windows installation guidance](https://pspdev.github.io/installation/windows.html)
and the repository's pinned distribution record.

When PSPDEV is present, build only the source-owned fixture:

```powershell
make -C fixtures/psp_oracle
```

Build a selected kernel case one at a time (`smoke` is the default):

```powershell
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=callback-notify-check EBOOT.PBP
```

Build the matching Nakagawa production stream from the repository root, one
case at a time. Supply the measured PSP model and firmware when preparing a
comparison; `unknown` is useful for a local protocol smoke test but blocks
acceptance eligibility:

```powershell
$commit = (git rev-parse HEAD).Trim()
mingw32-make psp-oracle-nakagawa PSP_ORACLE_CASE=callback-notify-check `
  PSP_ORACLE_SOURCE_COMMIT=$commit PSP_ORACLE_MODEL=PSP-3001 `
  PSP_ORACLE_FIRMWARE=6.61-ARK `
  PSP_ORACLE_OUTPUT=build/mygame/psp_oracle_nakagawa.txt
```

Repeat for `wait-cancel` and `thread-lifecycle`. For the second-order
`sceKernelWaitThreadEnd` finding, run the separate source-owned
`CASE=thread-delete-followup` control after the baseline lifecycle case; it
records both the error-shaped and positive intermediate-exit variants plus
bounded `sceKernelReferThreadStatus` scalars immediately before the outer wait.
The synchronized PSP-3001/6.61-ARK follow-up measured
`out2=out7=0x800200d2` for the error-shaped implicit return and
`out9=out14=0x77` for the positive control, with both status queries reporting
`status=0x10`, `waitType=0`, and `waitId=0`. The corresponding production-HLE
oracle matches those scalars. The sibling
`CASE=thread-delete-explicit` then measured
`sceKernelExitThread(0x800201ac)` -> `out2=out7=0x800200d2` and
`sceKernelExitThread(0x78)` -> `out9=out14=0x78`, again with successful
`ReferThreadStatus`, `status=0x10`, `waitType=0`, and `waitId=0`. The production
normalization therefore lives at the shared non-delete ThreadMan exit boundary.
The bounded `CASE=thread-delete-boundary` then measured explicit
`SCE_KERNEL_ERROR_WAIT_TIMEOUT` (`0x800201a8`) and ordinary `-17`; both also
latch/return `0x800200d2`, while positive values remain unchanged. The
implementation predicate is consequently signed-negative status (`status < 0`),
not the previously used `0x8002xxxx` prefix. `ExitDeleteThread` and module
self-unload remain outside this probe's evidence. The boundary PRX SHA-256 is
`826a97e79c0770c6aba30077dd9b4a2927c88cf45cac622a22088e5de78a31d5` at source
commit `88cc5ea709f65bf4e219fa56e51c704181070167`; its canonicalized companion
comparison is `MATCH` with no blockers. PPSSPP was not rerun for this control.
Do not substitute the PSP PRX digest, a host reimplementation, PPSSPP output,
or a copied scalar record. The
exact-head 2026-08-05 session captured all three cases at source commit
`6a8bc30796686e5194e369c35e80870f988b36a8` on a PSP-3001 running 6.61-ARK;
the exact PRX digests, raw error scalars, and `MATCH`/`acceptance_eligible=true`
reports are recorded in the manifest under
`oracle/hardware-results/psp-kernel-current-6a8bc30/`. The Nakagawa selftest
SHA-256 is `7d7efe835e5fbe36185fea48fc031d5b3ce95a57d7805e440afebf766171d084`
for all three cases. An additional `thread-delete-lifecycle` diagnostic at the
same source/model/firmware is acceptance-eligible but `DIFFERENCE` (Nakagawa
`out0=0x00001fff`, PSP `out0=0x00001fdf`; all other recorded scalars match), so
the delete-lifecycle issue remains open. This proves the three-case probe-group
invariant, not every acceptance criterion of the 19 routed kernel issues.

For the arithmetic smoke case, build the fixture with the default `CASE=smoke`
first, then use the generated-code route from the repository root:

```powershell
Push-Location fixtures/psp_oracle
wsl --cd . -e bash -lc 'export PSPDEV=$HOME/pspdev; export PATH=$PATH:$PSPDEV/bin; make -B CASE=smoke EBOOT.PBP'
Pop-Location
$commit = (git rev-parse HEAD).Trim()
mingw32-make psp-oracle-nakagawa-smoke `
  PSP_ORACLE_SOURCE_COMMIT=$commit PSP_ORACLE_MODEL=PSP-3001 `
  PSP_ORACLE_FIRMWARE=6.61-ARK `
  PSP_ORACLE_SMOKE_OUTPUT=build/mygame/psp_oracle_smoke_nakagawa.txt
```

This target runs `tools/codegen.py` on the fixture ELF and links the generated
guest function into the existing production selftest. It does not calculate the
sum in host C. The smoke fixture's PRX remains the binary supplied to PSPLINK;
the Nakagawa record hashes the running host executable itself, never the
caller-supplied `--artifact` path. The fresh physical comparison at source
commit `570600e9dcbe7ecc8693974647accea5a9b3bfb5` returned `MATCH` with
`acceptance_eligible=true`; see the manifest for the exact hashes and private
report path.

The build output is ignored. Record the resulting PRX SHA-256 in the private
hardware result manifest; do not put generated PRX/PBP files in Git.

## PSPLINK handoff

### Session hygiene — three findings from the first hardware session

**Set `resetonexit=0` in `ms0:/PSP/GAME/psplink/psplink.ini`.** At the shipped
default of `1`, PSPLINK calls `psplinkStop()` then `sceKernelLoadExec` to reload
itself whenever a program exits (`psplink/main.c:189,208`). That re-enumerates
the USB endpoint on *every* probe — audible as a host disconnect/reconnect — and
the runbook launches one probe at a time. With `resetonexit=0` PSPLINK prints
`sceKernelExitGame caught!` and stays up; measured across three consecutive
probe launches, `Resetting psplink` occurred zero times and the link never
dropped. PSPLINK re-reads the file when it reloads, so the setting takes effect
at the next reset or launch. The file can be updated over the live link without
USB mass-storage mode:

```text
cp host0:/psplink.ini ms0:/PSP/GAME/psplink/psplink.ini
```

Verify by reading it back (`cp ms0:/... host0:/readback.ini`) — PSPLINK's `ls`
can report a stale size and timestamp after a write.

**Never send `exit` to `pspsh`.** It terminates PSPLINK on the console, not just
the client, and drops the PSP to the XMB. Close the client's stdin instead.

**Returning from `main()` does not avoid the exit path.** The PSPSDK CRT emits
`jal sceKernelExitGame` in `_main` once `main()` returns, so a probe cannot opt
out of it; `resetonexit` is the only control that matters here.

The maintainer performs the physical setup: a supported PSP model/firmware,
PSPLink, the data cable and driver, and a running `usbhostfs_pc`/`pspsh` pair.
The agent must not flash firmware or install drivers. Before the first probe,
perform one human-confirmed `host0:` round trip. Launch one short probe at a
time, capture the output, and reset to PSPLINK after a timeout or crash.

The capture must be canonicalized with metadata for the exact model, firmware,
toolchain, source commit, and PRX SHA-256 before comparison. The fixture's
placeholder metadata is intentionally not acceptance evidence until the PC
runner replaces it with these measured values. Pass all four provenance flags
together — they are all-or-nothing and the runner rejects a partial set:

```powershell
python tools/psp_oracle/run_psplink.py --command "<explicit host command>" `
  --psp-output <psp.txt> --nakagawa-output <nakagawa.txt> `
  --binary fixtures/psp_oracle/build/nakagawa_psp_oracle.prx `
  --source-commit (git rev-parse HEAD) --model PSP-2000 --firmware 6.61-ME
```

Omitting them is allowed for a transport smoke check, but the resulting report
will say `"acceptance_eligible": false` and name each placeholder field. Do not
record such a run against an issue. Retain raw results only under
the ignored `oracle/hardware-results/` directory. A redacted summary may be
copied into an issue; never copy game-derived output, memory dumps, or paths.

Recommended first session order:

1. `PSP-SMOKE-001` transport/arithmetic record;
2. `PSP-KERNEL-001` one callback/wait case at a time;
3. `PSP-IO-001` descriptor/seek/async cases on a disposable path;
4. `PSP-DISPLAY-001` scalar latch/VBLANK/GE-sync validation;
5. `PSP-SYSTEM-001` controller/RTC/settings/power queries;
6. `PSP-AUDIO-001` scalar channel/SAS/ATRAC queries;
7. only then issue-specific HST or visual/media experiments.

The complete issue-to-test routing is machine-readable in
[`PSP_ISSUE_MATRIX.json`](PSP_ISSUE_MATRIX.json) and the probe-level details
are in [`../tools/psp_oracle/manifest.json`](../tools/psp_oracle/manifest.json).

## Evidence boundary

A scalar probe proves only the behavior that executed for that model,
firmware, and case. PPSSPP or PSPAutotests can provide a third comparison but
cannot turn a disagreement into a PSP conclusion. A hardware result does not
close an issue until the canonical issue's acceptance criteria, source commit,
fixture hash, and reset/reproducibility metadata are all recorded.

`acceptance_eligible: true` is a necessary condition, not a sufficient one: it
means the provenance is measured, not that the issue's criteria are met.
`PSP-KERNEL-001` and `PSP-SMOKE-001` are implemented from measured sessions;
the other four group IDs in the manifest are `planned`. None may be reported as
hardware PASS until a real producer and the relevant API cases exist in
`probe.c`.
