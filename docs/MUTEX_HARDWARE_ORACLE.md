# Plain PSP Mutex Hardware Oracle Specification and Evidence

This document records the physical hardware oracle probe specification and probe design for the plain PSP Mutex family (`sceKernelCreateMutex`, `LockMutex`, `TryLockMutex`, `UnlockMutex`, `CancelMutex`, `ReferMutexStatus`), building on PR #52.

Physical device authority: **PSP-3001 / ARK-5 6.61**.

## Overview & Scope

PR #52 implemented a dedicated typed object model in `src/rt/mutex.{c,h}` pinned to PSPAutotests expectations. Four specific cells remained unmeasured or unresolved in the original PR description:
1. `sceKernelReferMutexStatus` raw `lockThread` when unlocked (`0x00000000` vs `0xffffffff`).
2. Timeout behavior around alleged PPSSPP 25µs/250µs quantization across 1µs to 2500µs intervals.
3. Priority inheritance behavior (whether owner priority is boosted by higher-priority waiters).
4. Interrupt-context precedence for `LockMutex`/`LockMutexCB` across bad UID, bad count, and valid unlocked cells.

The `PSP-MUTEX-001` synthetic oracle probe suite (`fixtures/psp_oracle/`) isolates and measures these four cells directly on physical PSP hardware.

## Disassembly Proofs (`psp-objdump -d`)

PRX stub generation for the plain Mutex NIDs (`ThreadManForUser` library, `0x40010000` flags) in `fixtures/psp_oracle/mutex_imports.S` disassembles to clean MIPS instruction stubs under `psp-objdump -d`:

```text
fixtures/psp_oracle/build/nakagawa_psp_oracle.elf: file format elf32-littlemips

Disassembly of section .sceStub.text:

0001f938 <sceKernelCreateMutex>:
   1f938:	03e00008 	jr	ra
   1f93c:	00000000 	nop

0001f940 <sceKernelDeleteMutex>:
   1f940:	03e00008 	jr	ra
   1f944:	00000000 	nop

0001f948 <sceKernelLockMutex>:
   1f948:	03e00008 	jr	ra
   1f94c:	00000000 	nop

0001f950 <sceKernelLockMutexCB>:
   1f950:	03e00008 	jr	ra
   1f954:	00000000 	nop

0001f958 <sceKernelTryLockMutex>:
   1f958:	03e00008 	jr	ra
   1f95c:	00000000 	nop

0001f960 <sceKernelUnlockMutex>:
   1f960:	03e00008 	jr	ra
   1f964:	00000000 	nop

0001f968 <sceKernelCancelMutex>:
   1f968:	03e00008 	jr	ra
   1f96c:	00000000 	nop

0001f970 <sceKernelReferMutexStatus>:
   1f970:	03e00008 	jr	ra
   1f974:	00000000 	nop
```

Raw NID words:
- `sceKernelCreateMutex`: `0xB7D098C6`
- `sceKernelDeleteMutex`: `0xF8170FBE`
- `sceKernelLockMutex`: `0xB011B11F`
- `sceKernelLockMutexCB`: `0x5BF4DD27`
- `sceKernelTryLockMutex`: `0x0DDCD2C9`
- `sceKernelUnlockMutex`: `0x6B30100F`
- `sceKernelCancelMutex`: `0x87D9223C`
- `sceKernelReferMutexStatus`: `0xA9C2CB9A`

## Probe Test Matrix & Protocol

| Case ID | Probe Case Name | Test Description & Recorded Output Words | Status |
| --- | --- | --- | --- |
| **Case 13** | `mutex-refer-unlocked` | Measures `ReferMutexStatus` on unlocked mutex (initial count 0 and post-unlock). `out0`=create, `out1`=unlocked `lockThread`, `out2`=lock, `out3`=unlock, `out4`=post-unlock `lockThread`. | **PROBE READY** (Pending HW Capture) |
| **Case 14** | `mutex-timeout-quanta` | Measures `LockMutex` timeout requested vs remaining `pTimeout` across 1µs, 10µs, 25µs, 50µs, 100µs, 250µs, 500µs, 1000µs, 2500µs intervals (10 trials each). `out0..out16` store raw return codes and microsecond tick deltas. | **PROBE READY** (Pending HW Capture) |
| **Case 15** | `mutex-priority-inheritance` | Low-priority owner (`0x30`) holds mutex while high-priority waiter (`0x20`) blocks. `out0`=init, `out1`=owner prio before wait, `out2`=owner prio during wait, `out3`=waiter self prio, `out4`=owner prio after wait, `out5`=owner init prio. Tests if owner priority is boosted. | **PROBE READY** (Pending HW Capture) |
| **Case 16** | `mutex-interrupt-context` | 20 VBLANK ISR trials (`mutex-interrupt-context-t00`..`t19`). Calls `sceKernelIsIntrContext()` as independent ISR proof (`out0`), then executes 6 cells: Bad UID (`out1`), Bad Count (`out2`), Valid Unlocked (`out3`), Valid Unlocked CB (`out4`), TryLock (`out5`), Unlock Non-Owner (`out6`). | **PROBE READY** (Pending HW Capture) |

## Probe Execution Protocol

Build and execute each case using the source-owned fixture infrastructure:

```bash
# Build probe cases individually with PSPDEV
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=mutex-refer-unlocked
make -C fixtures/psp_oracle CASE=mutex-timeout-quanta
make -C fixtures/psp_oracle CASE=mutex-priority-inheritance
make -C fixtures/psp_oracle CASE=mutex-interrupt-context
```

Test automation:
```powershell
python -m unittest tools/test_psp_oracle.py -v
```
