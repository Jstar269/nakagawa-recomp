# Hardware finding: `sceKernelWaitThreadEnd` on a terminated-target joiner

Measured 2026-08-05 on PSP-3001, firmware 6.61 with ARK CFW, over PSPLINK.
This is the project's first hardware-derived correction to a Nakagawa semantic.

## What was measured

Codex's `thread-delete-lifecycle` probe reported `DIFFERENCE` at the exact head:
Nakagawa `out0=0x00001fff`, PSP `out0=0x00001fdf`. Those differ in **exactly one
bit**, bit 5:

```c
((uint32_t)((uint32_t)term_join_wait == 0x800201acu) << 5)
```

`term_join_wait` is `sceKernelWaitThreadEnd(term_joiner, NULL)` — the main thread
waiting on a *joiner* thread, where that joiner had itself been waiting on a
target that was `sceKernelTerminateDeleteThread`'d.

The bit told us only "not `0x800201ac`". The raw value was never emitted, so the
probe was extended to emit it as `out9` and re-run on the same hardware:

| | `out9` (`term_join_wait`) |
| --- | --- |
| Nakagawa | `0x800201ac` — `SCE_KERNEL_ERROR_THREAD_TERMINATED` |
| PPSSPP | `0x800201ac` — identical to Nakagawa |
| **PSP-3001 / 6.61-ARK** | **`0x800200d2`** — **`SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT`** |

`0x800200D2` is `SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT` per PSPSDK `pspkerror.h`, and
this repository already uses that value under that name at `src/rt/hle.c:1513`.

Not a near-miss: hardware returns a different *error class* entirely
(`0x800200xx`, argument validation) where both software implementations return a
thread-state error (`0x800201xx`).

## The control that makes it precise

The same capture contains the parallel case, and it **agrees**:

| | `out8` (`exit_join_wait`) |
| --- | --- |
| Nakagawa | `0x00000066` |
| PSP | `0x00000066` |

`exit_join_wait` is the identical operation — main waiting on a joiner — but that
joiner's target called `sceKernelExitDeleteThread(0x66)` instead of being
terminate-deleted. So the joiner's own exit status was `0x66` rather than
`0x800201ac`.

Everything else in the record matches hardware exactly: `out1`–`out7` are
identical on both sides, including `out4 = 0x800201ac` (the value the joiner
itself received). So the divergence is not in termination, not in the joiner's
wake-up value, and not in delete semantics generally — it is confined to what a
*second-order* join returns when the joined thread's exit status is itself an
error-shaped value.

## Hypothesis before the discriminator (historical state)

The obvious reading is that real PSP rejects the wait rather than reporting the
joiner's latched exit status — perhaps because the joiner is already dormant by
the time the wait is issued, or because an error-shaped exit status is not a
legal thing to return through this path.

At this point the `exit_join_wait` control was consistent with either
explanation, since the two cases differed in both the joiner's exit value *and*
its timing. It therefore required the synchronized follow-up below; no runtime
change was justified by the original capture alone.

1. Have the joiner return a *positive* status after joining a terminate-deleted
   target. If hardware then returns that value, the trigger is the error-shaped
   status; if it still returns `ILLEGAL_ARGUMENT`, the trigger is timing/state.
2. Query the joiner's state (`sceKernelReferThreadStatus`) immediately before the
   wait, and emit it.

The original measurement remains solid, but the mechanism was intentionally
left unresolved until the discriminating controls completed.

## Follow-up probe added

The source-owned fixture now has a separate `CASE=thread-delete-followup` so the
original capture remains reproducible. It runs two otherwise identical
terminate-delete controls:

1. an intermediate joiner receives `0x800201ac` from its target and returns that
   same error-shaped value;
2. an intermediate joiner receives `0x800201ac` from its target and explicitly
   returns the normal positive value `0x77`.

For each joiner, the probe uses a higher-priority entry and two semaphores: one
proves that the inner wait was entered before the target was deleted, and one
proves that the inner wait returned before `sceKernelReferThreadStatus` and the
outer wait. It records only scalar API/state fields: the inner result, outer
result, refer-status return, `status`, `waitType`, `waitId`, and exposed
`exitStatus`.

The Nakagawa production-HLE stream for this follow-up currently emits:

```text
error-shaped inner=0x800201ac outer=0x800201ac status=0x10 waitType=0 waitId=0 exitStatus=0x800201ac
positive inner=0x800201ac outer=0x00000077 status=0x10 waitType=0 waitId=0 exitStatus=0x00000077
```

The synchronized PSP-3001/6.61-ARK capture used PRX SHA-256
`375a951d06527832c263ee029f1cecd203082473e550c05854d234be65754220` and
reported:

```text
out0=0x000fffff out1=0x800201ac out2=0x800200d2 out3=0x00000000
out4=0x00000010 out5=0x00000000 out6=0x00000000 out7=0x800200d2
out8=0x800201ac out9=0x00000077 out10=0x00000000 out11=0x00000010
out12=0x00000000 out13=0x00000000 out14=0x00000077
```

The raw PSP stream and the local production-HLE stream are an exact scalar
`MATCH` after provenance canonicalization. The first unsynchronized attempt is
kept separately as transport/probe evidence only: its joiner was still
`READY` (`status=0x2`) and its inner result had not yet been written.

This resolves the two hypotheses for the synchronized implicit control. Both
joiners were `STOPPED` with no wait object, both inner waits returned
`SCE_KERNEL_ERROR_THREAD_TERMINATED`, and only the exit shape differed. An
error-shaped implicit thread-entry return is latched and joined as
`SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT` (`0x800200d2`); a positive return (`0x77`)
propagates unchanged.

## Explicit `sceKernelExitThread` discriminator (Case B)

The sibling `CASE=thread-delete-explicit` keeps the same target, priority, and
semaphore ordering, but the intermediate joiner calls
`sceKernelExitThread(0x800201ac)` after recording its inner wait result. Its
positive control calls `sceKernelExitThread(0x78)`. The outer wait is issued
before either `sceKernelReferThreadStatus` sample, so the status query cannot
observe a still-running joiner.

The first hardware launch used PRX SHA-256
`8276e83801f88d438c9cb237d42481901d3397c7b2f738a055219e6f3c04de7a` and
reported `status=FAIL` only because that probe build still expected explicit
error propagation. Its raw values already showed the Case-B result. The
expectation-adjusted synchronized probe used PRX SHA-256
`b5927c717b18579d9607aa36f09b0d7d2491c0e099915feae6015991edbb08ef` and
reported:

```text
out0=0x003fffff out1=0x800201ac out2=0x800200d2 out3=0x00000000
out4=0x00000010 out5=0x00000000 out6=0x00000000 out7=0x800200d2
out8=0x800201ac out9=0x00000078 out10=0x00000000 out11=0x00000010
out12=0x00000000 out13=0x00000000 out14=0x00000078
```

Thus the explicit error-shaped status also becomes
`SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT` (`0x800200d2`), while explicit positive
`0x78` propagates. Both `ReferThreadStatus` calls returned success (`out3` and
`out10` zero), reported `status=0x10`, `waitType=0`, `waitId=0`, and exposed
the same latched values in `exitStatus`. This is a real-PSP Case-B result, not
an inference from Nakagawa or PPSSPP.

The raw fixture metadata remains the checked-in placeholder by design. The
repository comparator canonicalized that capture with the measured PSP-3001,
6.61-ARK session facts and the exact PRX/source commit; its companion report
`build/audit/psplink-followup-explicit-454e530.json` is `MATCH` with
`acceptance_eligible=true` and no blockers. The raw files are preserved under
the ignored `oracle/hardware-results/` directory and were not rewritten.

## Signed-negative boundary discriminator

The remaining question was whether the implementation could safely use the
`0x8002xxxx` range as its predicate. The bounded
`CASE=thread-delete-boundary` probe added two explicit controls without
changing the synchronization: `SCE_KERNEL_ERROR_WAIT_TIMEOUT` (`0x800201a8`),
which is a second PSP kernel error already used by this project, and ordinary
`-17` (`0xffffffef`), which is outside that range.

The corrected PSP-3001/6.61-ARK probe used PRX SHA-256
`826a97e79c0770c6aba30077dd9b4a2927c88cf45cac622a22088e5de78a31d5` and
reported:

```text
out0=0x003fffff out1=0x800201ac out2=0x800200d2 out3=0x00000000
out4=0x00000010 out5=0x00000000 out6=0x00000000 out7=0x800200d2
out8=0x800201ac out9=0x800200d2 out10=0x00000000 out11=0x00000010
out12=0x00000000 out13=0x00000000 out14=0x800200d2
out15=0x800201a8 out16=0xffffffef
```

`out15` and `out16` are the supplied explicit arguments. Both controls
normalize to `0x800200d2`: the second PSP kernel error is not an isolated
`THREAD_TERMINATED` special case, and the ordinary negative control falsifies
the previous `0x8002xxxx` range predicate. The existing positive controls
(`0x77` and `0x78`) still propagate unchanged. The host production-HLE stream
matches all scalars in `build/audit/psplink-boundary-88cc5ea.json` with
`acceptance_eligible=true` and no blockers.

The first boundary launch used PRX SHA-256
`4afe7b4b8f90de81e10a74d5231a474064217f84e45a29c1e2ae8d809fc1cf52` and is
preserved as a rejected instrumentation iteration at
`oracle/hardware-results/psplink-20260805-thread-delete-boundary-synchronized.stdout.txt`:
it expected `-17` to propagate and therefore reported `FAIL`; its outer and
latched values already showed `0x800200d2`. A subsequent probe fixed the
diagnostic argument field to the correct two's-complement value `0xffffffef`;
neither earlier raw file is rewritten.

The installed PSPSDK `pspkerror.h` names `0x800201a8` as
`SCE_KERNEL_ERROR_WAIT_TIMEOUT`, `0x800201ac` as
`SCE_KERNEL_ERROR_THREAD_TERMINATED`, and `0x800200d2` as
`SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT` in its `PspKernelErrorCodes` namespace.
Together with the two negative hardware controls and the positive controls,
the smallest supported semantic boundary is signed-negative status, not the
numeric `0x8002xxxx` prefix. The production helper therefore uses
`status < 0` at the common non-delete seam. This is a measured category rule,
not a claim that every possible termination mechanism or unmeasured status has
been exhaustively characterized.

`sceKernelExitDeleteThread` and module self-unload remain outside the helper
and outside the hardware claim.

## Why this matters beyond the closed #26 milestone

GitHub issue #26 is now closed for the implemented thread-delete lifecycle
milestone. This second-order wait discrepancy remains a separate unresolved
hardware finding and is not evidence that the broader routed criteria are
complete.

The broader point is that both software implementations agreed with each other
and both were wrong. PPSSPP returns `0x800201ac`, Nakagawa returns `0x800201ac`,
hardware returns `0x800200d2`. Any amount of Nakagawa-vs-PPSSPP comparison would
have reported agreement forever.

`docs/HARDWARE_ORACLE.md` states the risk as *"where PPSSPP is wrong or
approximate, we inherit the error invisibly."* This is a measured instance of
exactly that, and it is the first one the project has caught.

## Reproduction

```powershell
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=thread-delete-lifecycle EBOOT.PBP
# the signed-negative boundary discriminator is CASE=thread-delete-boundary
# stage the PRX to the PSPLINK host directory, then run it from pspsh.
# Never send `exit` to pspsh -- close stdin instead.
```

PPSSPP control (`--timeout=30 -j`) reproduces the Nakagawa value, confirming the
probe change itself is not the source of the difference.
