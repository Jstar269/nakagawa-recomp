# Source-owned PSP oracle probes

This directory contains source-owned PSPDEV/PSPLink fixtures. They are
synthetic and independent of the HST executable, retail assets, firmware
files, keys, and private traces.

The fixture prints the versioned `NAKAGAWA_PSP_META` and
`NAKAGAWA_PSP_TEST` records defined in
[`tools/psp_oracle/protocol.py`](../../tools/psp_oracle/protocol.py). The
default `CASE=smoke` build emits `PSP-SMOKE-001`; kernel sessions build one
case per launch with `CASE=callback-notify-check`, `CASE=wait-cancel`,
`CASE=thread-lifecycle`, `CASE=thread-delete-lifecycle`,
`CASE=thread-delete-followup`, `CASE=thread-delete-explicit`, or
`CASE=thread-delete-boundary`. DMA sessions use `CASE=dma-concurrency` or one
of the four `CASE=dma-invalid-tail-*` cases described below.

The thread-delete follow-up is a bounded two-control probe for the
second-order `sceKernelWaitThreadEnd` discrepancy: semaphore handshakes prove
both joiners entered and completed the inner wait on a terminate-deleted
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

The bounded `thread-delete-boundary` case passes
`SCE_KERNEL_ERROR_WAIT_TIMEOUT` (`0x800201a8`) through explicit
`sceKernelExitThread`, then uses the existing ordinary `-17` control. It is
intentionally limited to those two boundary values; it is not an error-code
matrix.

All records contain only scalar arithmetic and API results; pointers and raw
memory are never treated as stable evidence.

## Issue 23 DMA cases

The probe separates three claims: `HARDWARE_MEASURED` is the physical-PSP
result, `RUNTIME_IMPLEMENTED` is behavior exercised through Nakagawa's
production dispatch, and `RUNTIME_UNIMPLEMENTED` is a measured contract that
the host runtime does not yet model. The reported campaign covers 3 x 64 = 192
concurrency trials and four isolated `0xC001` invalid-tail launches. The public
tree contains the source-owned probe and scalar protocol, not private capture
bytes; a release evidence packet must retain the model/firmware/CFW/clock,
source/PRX digest, and canonical scalar records before treating the report as
externally trusted hardware evidence.

`CASE=dma-concurrency` runs three 64-trial API combinations:

- `sceDmacMemcpy` then `sceDmacTryMemcpy`;
- `sceDmacTryMemcpy` then `sceDmacTryMemcpy`;
- `sceDmacTryMemcpy` then `sceDmacMemcpy`.

Each trial starts the first 1 MiB VRAM-to-VRAM call in a priority-`0x10`
thread while the main thread remains at priority `0x20`. This reproduces the
public PSPAutotests scheduling shape. It records whether the first caller had
entered, whether it had returned, whether it remained pending when the second
caller started, and whether the measured call intervals overlapped. The record
also correlates BUSY with the pending/returned snapshot: BUSY after return is
the observable needed to show that the first syscall returned while its DMA
state remained active. Timing or thread state alone is not described as
in-flight DMA proof.

The concurrency record uses `result` for the last second-caller return (or a
setup error) and these scalar outputs:

| Field | Meaning |
| --- | --- |
| `out0` | Completed trial count |
| `out1` / `out2` | First / second API (`0` blocking, `1` try) |
| `out3` / `out4` | First-entered / first-returned snapshots before the second call |
| `out5` | First-entered/not-returned snapshots before the second call |
| `out6` | Second-call start times earlier than the first-call return time |
| `out7` / `out8` | First-call zero / other return counts |
| `out9` / `out10` / `out11` | Second-call BUSY (`0x80000021`) / zero / other counts |
| `out12` / `out13` | BUSY while first pending / after first returned |
| `out14` / `out15` | Minimum / maximum contiguous transferred prefix |
| `out16` | Trials whose contiguous prefix was exactly `0xC000` |
| `out17` | Trials with non-sentinel mutation after the contiguous prefix |
| `out18` / `out19` | First-call minimum / maximum wall time in microseconds |
| `out20` / `out21` | Second-call minimum / maximum wall time in microseconds |
| `out22` | First-caller thread priority |
| `out23` | Last first-caller return |

The invalid-tail cases isolate one API and invalid endpoint per launch:

- `dma-invalid-tail-memcpy-dst`;
- `dma-invalid-tail-memcpy-src`;
- `dma-invalid-tail-try-dst`;
- `dma-invalid-tail-try-src`.

The Makefile explicitly opts out of expanded memory. At the pinned PSPSDK
revision this requests the 24 MiB user partition described by the public uOFW
memory map. Before calling DMAC, the probe reserves the entire final valid
`0xC000`-byte prefix through `sceKernelAllocPartitionMemory`, checks that the
returned block begins at the requested address, and verifies that partition 2
rejects a new allocation beginning at the next address. If any safety check
fails, it emits `SKIP` and never issues the invalid-tail call. The requested
size is `0xC001`, so exactly one requested byte lies beyond the
allocator-proven boundary. No byte outside an owned block is read by the probe
itself.

For invalid-tail records, `result` is the DMAC return and `out0` is the setup
mask (`0x7` means every allocator gate passed). `out1`/`out2` are the requested
and measured-prefix sizes; `out3` is the invalid endpoint (`0` destination,
`1` source); `out4` is the API (`0` blocking, `1` try); `out5`/`out6` are the
prefix pattern-match and non-sentinel mutation counts; `out7` is lead-guard
mutation; `out8`/`out9` report the valid destination tail and post-request
guard where observable (`0xFFFFFFFF` otherwise); `out10` verifies the source
prefix; `out11` is wall time; and `out12` is the rejected boundary-allocation
error. `status=PASS` means that the call returned and the scalar measurement
completed; it does not mean that the observed DMA semantics match Nakagawa or
close issue #23.

Terminal outcomes are deliberately distinct:

| Outcome | Required evidence |
| --- | --- |
| Result | The expected one or three `PSP-DMAC-001` records exist; retain every scalar and status. |
| Skip | An explicit `status=SKIP` record proves that a pre-call safety gate stopped the case. |
| Hang | No test record, host `process_status=TIMEOUT`, and a human observes that the device remains stalled without rebooting. |
| Reset | No test record and a human observes a device reboot/reset and PSPLink session loss. Never infer this from host process exit alone. |
| Inconclusive | No record and neither physical observation is established, including launch or transport failures. |

Run the capture with `--out <case>.runner.json`. After a no-record outcome, the
operator records the physical observation without altering the original report:

```powershell
python tools/psp_oracle/run_psplink.py `
  --annotate-report <case>.runner.json `
  --observed-terminal-outcome HANG `
  --out <case>.hang.json
```

Use `RESET` instead only after observing a reset. The annotation command
rejects any capture that already contains a test record and marks terminal
outcomes ineligible for scalar-result acceptance.

## Build and hardware handoff

Build when PSPDEV is installed:

```powershell
make -C fixtures/psp_oracle
```

For example, to build the callback case in WSL with PSPDEV:

```bash
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=callback-notify-check EBOOT.PBP
```

To build the complete DMA matrix without running it:

```bash
for case in dma-concurrency \
  dma-invalid-tail-memcpy-dst dma-invalid-tail-memcpy-src \
  dma-invalid-tail-try-dst dma-invalid-tail-try-src; do
  make -C fixtures/psp_oracle clean
  make -C fixtures/psp_oracle CASE="$case" EBOOT.PBP
done
```

The loop above deliberately overwrites the ignored build output. A hardware
operator must build, launch, and capture each case before moving to the next;
do not use the loop as a hardware runner. Start with `dma-concurrency`. Run the
four invalid-tail cases separately only after confirming the expected PSP
model/firmware and a working PSPLink reset path.

The expected output is a PRX under the fixture's ignored `build/` directory.
Signing, PSPLink launch, and USB/driver setup remain explicit maintainer
actions. No firmware or driver installer is part of this repository.
