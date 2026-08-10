# HLE call census — which registrations HST actually executes

Measured 2026-08-05. Route `route_boot_mainmenu_20260725.pad`
(`6A06FBD004DD…`), 4500 vblanks, `SR_DATA_EXPECTED_COUNT` satisfied
(`host_data: indexed 56672`). Source head `c36d75e`.

**Evidence tier 1 (production dispatch).** `SR_NIDLOG` records every NID at the
top of `sr_syscall`, before handler lookup, undeduplicated. This is the
production dispatch path with nothing stubbed for the measurement.

## Why this was needed

`import_audit` reported 371 registrations of which 90 were `fake_success` — a
generic always-success handler standing in for an unimplemented API. That number
was being read as "24% of the HLE surface is wrong," which is not a conclusion
the classification can support: a registration that never executes cannot be
wrong at runtime, and some no-ops are genuinely correct.

The existing `SR_HLELOG` output cannot answer this. It **deduplicates per
(thread, nid) pair**, so a call appearing four times in a log may have executed
four times or forty thousand. An early read of this investigation drew exactly
that wrong inference before the dedup was noticed.

## Result

181,032 syscalls, **105 distinct NIDs**. So **266 of 371 registrations never
execute at all** on this route — including **75 of the 90** `fake_success`
entries.

The 15 `fake_success` registrations that do fire account for 45,806 calls,
**25.3% of all syscalls**:

| calls | NID | name |
| ---: | --- | --- |
| 15601 | `0xbea46419` | `sceKernelLockLwMutex` |
| 15601 | `0x15b6446b` | `sceKernelUnlockLwMutex` |
| 6189 | `0x79d1c3fa` | `sceKernelDcacheWritebackAll` |
| 4315 | `0xb435dec5` | `sceKernelDcacheWritebackInvalidateAll` |
| 4049 | `0x36faabfb` | `sceAtracGetNextSample` |
| 28 | `0xcbcd4f79` | `__sceSas_ok` |
| 9 | `0xe175ef66` | `__sceSas_ok` |
| 3+3+2 | — | three further `__sceSas_ok` entries |
| 2 | `0x090ccb3f` | `sceKernelPowerTick` |
| 1 each | — | `sceImposeSetLanguageMode`, `sceCtrlSetSamplingMode`, `sceCtrlSetSamplingCycle`, `scePowerSetClockFrequency350` |

`sceKernelCreateLwMutex` fires twice. It is classified `dedicated` rather than
`fake_success` only because it was routed to the semaphore constructor — which,
as shown below, is itself the defect.

### What the distribution actually says

- **The two `Dcache*` entries are 10,504 of the 14,604 non-mutex stub calls
  (72%), and no-op is the correct implementation.** Guest memory here is a flat
  coherent host buffer; there is no cache to write back. These are misclassified
  as `fake_success`, not defective. Removing them from the "wrong" column is a
  reclassification, not a fix.
- **`sceAtracGetNextSample` at 4049 calls is, once the mutexes below are
  handled, the largest genuine defect left in the set.** It reports success
  without reporting how many samples remain, on a path
  the game polls every frame. It is the single highest-value target left in the
  `fake_success` list and it sits directly on the open audio work (#31/#32).
- **`__sceSas_ok` totals 47 calls across five NIDs** — far lower than the
  19-registration count suggests. This route never reaches gameplay, so this is
  a floor, not a ceiling: sceSas is the SFX engine and a rally would exercise it
  much harder. Not yet measured.
- The remainder are one-shot configuration calls.

## Lightweight mutexes

`sceKernelLockLwMutex` is the **third most frequent syscall in the game** at
15,601 calls, exactly balanced against 15,601 unlocks. It was registered to the
generic success handler.

Three defects, all source-verifiable:

1. **`sceKernelCreateLwMutex` was routed to `h_CreateSema`**, whose signature is
   `(name, attr, initCount, maxCount)`. The real signature is
   `(workarea, name, attr, initialCount, opt)` — every argument shifted by one
   register, so the semaphore was created with `count=attr`, `max=initialCount`.
2. **It returned the uid where the API returns 0 on success.** Any caller
   testing `ret != 0` would read success as failure.
3. **The workarea was never written.** `SceLwMutexWorkarea` lives in guest
   memory and the guest reads `lockLevel`/`lockThread`/`uid` out of it directly;
   under the old registration those fields held whatever was there before.

The justifying comment read *"single-threaded recompiler: all are no-ops except
Create returns a uid"*. That premise is false — this runtime schedules real PSP
threads cooperatively — and two distinct guest threads (`0x115`, `0x12a`) are
observed locking the same LwMutex.

### The fix, and an honest negative result

`src/rt/hle.c` now implements Create/Delete/Lock/TryLock/Unlock against the
documented `SceLwMutexWorkarea` layout, with real blocking through
`sched_block_on`/`sched_wake`. `ReferLwMutexStatus` is deliberately **left as a
no-op**: its `SceKernelLwMutexInfo` layout is not in `pspthreadman.h` and this
project has no measured record of it, so writing one would be invention. Error
codes for the abusive paths (non-owner unlock, non-recursive relock) are
likewise not invented — they stay permissive-but-state-coherent and log under
`SR_HLELOG`.

**Instrumented contention count over the full route: zero.** Every acquisition
found the mutex unlocked or already owned by the caller. So:

- The old no-op was **not** producing an observable race on this path, and this
  does **not** support the hypothesis that the open heap corruption comes from
  missing mutual exclusion. That hypothesis is unsupported, not confirmed.
- The change fixes defects 1–3, which are unconditional and occur on every call,
  and makes contention correct if it ever occurs. It is not a fix for an
  observed race.
- Zero contention is partly *expected* under a cooperative scheduler: another
  thread can only enter a critical section if it contains a yield point.

Verified: 4500 vblanks reached, exit 0, no deadlock, lock/unlock still exactly
balanced (18,533/18,533), same 105 distinct NIDs. 708 Python tests pass;
`import_audit_gate` passes with `fake_success` 90 → 80, `dedicated` 279 → 289.

## Method caveat, recorded because it bit this investigation

Two runs of the *same* route differed by 3× in guest rate (16.5 vs 47.6
vblanks/s) and ~17% in total syscall count. The game's persisted save changes
the screen sequence between runs, so **route replay alone is not a control** —
`-SaveBase` is required for any before/after comparison. The pre/post census
diff in this session was uncontrolled for that reason and no causal claim is
drawn from it.

## What this changes about priorities

The `fake_success` count was never the right risk metric. 266 registrations are
unexercised on this route, the largest stub cluster is correctly a no-op, and
the real remaining item is `sceAtracGetNextSample`. Re-running this census on a
gameplay route — which will exercise sceSas and the audio path properly — is the
obvious next measurement.
