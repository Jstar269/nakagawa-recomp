# Source-owned PSP oracle probes

This directory contains the first PSPDEV/PSPLINK smoke fixture.  It is
source-owned, synthetic, and independent of the HST executable, retail assets,
firmware files, keys, and private traces.

The fixture prints the versioned `NAKAGAWA_PSP_META` and
`NAKAGAWA_PSP_TEST` records defined in [`tools/psp_oracle/protocol.py`](../../tools/psp_oracle/protocol.py).
The default `CASE=smoke` build emits `PSP-SMOKE-001`; kernel sessions build one
case per launch with `CASE=callback-notify-check`, `CASE=wait-cancel`,
`CASE=thread-lifecycle`, `CASE=thread-delete-lifecycle`,
`CASE=thread-delete-followup`, `CASE=thread-delete-explicit`, or
`CASE=thread-delete-boundary`. The follow-up
is a bounded two-control probe for
the second-order `sceKernelWaitThreadEnd` discrepancy: semaphore handshakes
prove both joiners entered and completed the inner wait on a terminate-deleted
target, one returns the error-shaped inner result and the other explicitly
returns `0x77`, and each `SceKernelThreadInfo` state is recorded after the
outer wait. The explicit sibling uses the same synchronization but calls
`sceKernelExitThread(0x800201ac)` and `sceKernelExitThread(0x78)` directly.
The PSP-3001/6.61-ARK controls establish that signed-negative status values
(`0x800201a8`, `0x800201ac`, and ordinary `-17`) normalize to `0x800200d2` on
the measured non-delete exit paths, while ordinary positive values propagate
unchanged. `ExitDeleteThread` is deliberately outside this measurement. The
raw hardware stream remains local until its current-session model/firmware
metadata is confirmed.
The records contain only scalar arithmetic and API results; pointers and raw
memory are never treated as stable evidence.

The bounded `thread-delete-boundary` case passes
`SCE_KERNEL_ERROR_WAIT_TIMEOUT` (`0x800201a8`) through explicit
`sceKernelExitThread`, then uses the existing ordinary `-17` control. It is
intentionally limited to those two boundary values; it is not an error-code
matrix.

Build when PSPDEV is installed:

```powershell
make -C fixtures/psp_oracle
```

For example, to build the callback case in WSL with PSPDEV:

```bash
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=callback-notify-check EBOOT.PBP
```

The expected output is a PRX under the fixture's ignored `build/` directory.
Signing, PSPLink launch, and USB/driver setup remain explicit maintainer
actions.  No firmware or driver installer is part of this repository.
