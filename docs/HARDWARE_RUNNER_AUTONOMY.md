# Hardware runner autonomy — architecture and runbook

Status: source-owned v2 conformance design and simulator, plus a host campaign
adapter for the existing scalar-probe route. The adapter starts the configured
USBHostFS process, drives PSPSH commands, bounds each case, unloads each module,
and records an evidence envelope. It follows the L0-L4 recovery contract tested
in [`tools/test_hardware_runner_protocol.py`](../tools/test_hardware_runner_protocol.py).
Campaigns require the source-owned `transport-write` case first. Its host0
round-trip passed in simulation but failed in the W1-psplink3 live run; that
run captured an envelope but accepted none.
The PSP-side resident v2 runner and dual-path result protocol remain unbuilt;
see [What still requires a human / build order](#9-build-order-and-open-attestation).
This document extends, and defers to, the implemented scalar-probe subset
described in [HARDWARE_ORACLE.md](HARDWARE_ORACLE.md) and
[`fixtures/psp_oracle/README.md`](../fixtures/psp_oracle/README.md).

## 1. Goal and current autonomy level

Ultimate goal: the maintainer connects the PSP once and a single command —
`hardware-runner run <manifest>` conceptually — runs qualified experiment
matrices for hours or days without anyone typing PSPLink commands: it must
identify source/probe identity, build, hash, stage, qualify transport,
execute approved cases with repeat contracts, capture dual-path evidence,
classify faults, recover along a bounded ladder, record META, and stop
rather than fabricate when evidence becomes invalid.

Current autonomy level, measured in the W1-psplink campaign on 2026-09-23:

| Step | State |
| --- | --- |
| Probe build/hash/stage | The adapter accepts prebuilt source-owned PRXs from a host0 scratch directory; it does not build them. |
| Transport qualification | Live replies identified PSPLink v3.2.1 and firmware 6.6.1. In W1-psplink3 the first `ver` exited 1, the second passed, and the post-attach verification passed; each attempt is retained in recovery events. |
| Case execution and unload | `model-profile` `ldstart` returned a UID and left its module loaded. `modstun`, then `modinfo` `Unknown module`, confirmed unload. PSP-B3 `ldstart` returned promptly while its main thread remained parked. |
| Result capture | `model-profile` and W1-psplink3 `transport-write` produced no schema records or host0 result file. The live envelope ended `HOST0_ROUNDTRIP_FAILED`, was not acceptance-eligible, and did not run the optional smoke case. Raw model code remains `NOT_CAPTURED`. |
| Recovery | After the W1 reset timed out, `Shared` to attach to `Connected` restored the WSL route without a power cycle. W1-psplink2 saw the device already `Attached` and `ver` passed. In W1-psplink3 the first `ver` exited 1, the retry passed, and the attached-device verification passed; the shell stayed qualified until the transport-write result failed. |

A fresh USBHostFS connection can lose its first shell command: a prior
USBHostFS log showed `Error, unknown command 00000000` after the first
`pspsh -e ver` timed out, and a later `ver` succeeded without PSP intervention.
W1-psplink3 observed the same first-command loss as a `ver` process exit 1,
followed by a successful retry. Initial shell qualification and qualification
after each USBHostFS connect or
re-attach now share a bounded verifier: at most three `ver` attempts, each
limited to the remaining time or 15 seconds, within a configurable total
deadline (`--shell-verification-timeout`, default 45 seconds). Every attempt is
recorded in recovery events. Lost replies and USBHostFS unknown-command output
are retryable transport events; only exhaustion escalates to
`PHYSICAL_INTERVENTION_REQUIRED` with manual commands. This routine retries
shell qualification only; semantic probe results are never retried.

The reset command's timeout alone did not establish its outcome; the subsequent
`Shared` to attach to `Connected` to `ver` sequence did establish restored
PSPLink control without a power cycle. The later campaign CLI failure accepted no
semantic result and was not retried. This measures one soft-reset recovery on
the attached console. It does not measure a live probe hang or standalone
PSPLink relaunch. The physical model was not read during this campaign and must
remain `NOT_CAPTURED` in any envelope for it.

The largest single autonomy blocker is fault containment: today a crashing
experiment poisons the only control plane. Everything below is organized
around fixing that.

## 2. Process topology

```text
HOST ORCHESTRATOR (state machine, this doc §4)
   |-- usbhostfs_pc (host0: mirror = staging/results directory)
   |-- PSPSH process adapter (`tools/psp_oracle/run_psplink.py`)
   |-- evidence writer (envelopes, §6)
   '-- recovery ladder driver (§5)
          |  qualified command protocol (framed, §3)
          v
   PSP RESIDENT ORACLE RUNNER (future PRX, source-owned)
          |-- experiment cells: META, VFPU, kernel/user API, later cache
          |-- per-case isolated execution + controlled reset between cases
          '-- heartbeat + watchdog state
```

Design rules carried over from measured transport findings:

- VSH-injected and standalone PSPLink stacks collide; duplicate user-module
  registration produces `0x8002013B`. The runner assumes a clean standalone
  context and the orchestrator must never "fix" a missing module by loading
  another copy.
- `pluser=1` is required for the proven stdout route.
- USB replug alone does not reliably revive device USBHostFS; a full
  standalone relaunch does. Recovery levels are ordered accordingly.
- Crashing probes can wedge reload cycles: the runner must isolate case
  execution from the control plane (§7).

## 3. Command protocol (v2 framing)

Schema-1 line protocol (`tools/psp_oracle/protocol.py`) stays the result
payload format. v2 wraps every message in a length-prefixed frame so partial
reads, echoes, and interleaved banner noise are detectable:

```text
frame := MAGIC(2B 'NR') | version(1B) | type(1B) | seq(2B LE) |
         payload_len(4B LE) | payload_len ones-complement(4B LE) | payload
```

Verbs (request -> response payload):

| Verb | Purpose |
| --- | --- |
| `HELLO` | handshake; returns protocol version, runner build SHA256, source commit |
| `CAPABILITIES` | cell inventory + max case size + repeat limits |
| `META` | console metadata block (raw model value, FW/CFW, clock) |
| `LOAD_CASE` | stage one synthetic case descriptor + expected-shape hash |
| `RUN_CASE` | execute loaded case `iteration` times under watchdog budget |
| `RESULT` | framed schema-1 result lines + raw-result length/hash |
| `RESET_CASE` | tear case state down without disturbing control plane |
| `PING` | liveness only; never treated as qualification |
| `STOP` | orderly shutdown |

Every response carries: protocol version, runner build SHA256, source commit,
case id, iteration index, status, raw-result length + sha256, and a fault
classification field. Unframed stdout is never the sole evidence channel:
the orchestrator records both the framed stream and the file-mirror copy of
the same result under `host0:out/`, and compares them (dual-path rule). A
mismatch is an evidence failure, not a semantic result.

Identity binding rules (fail closed): HELLO runner SHA must match the staged
binary's recorded SHA256; source commit must match the orchestrator run
manifest; a mismatch aborts the epoch before any case runs.

## 4. Host orchestrator state machine

States: `OFFLINE, WAIT_DEVICE, USB_SERVER_START, WAIT_ATTACH, SHELL_QUALIFY,
RUNNER_DISCOVER, RUNNER_HANDSHAKE, META, READY, RUN_CASE, VERIFY_RESULT,
RECOVERABLE_FAULT, SESSION_WEDGED, STOPPED`.

Every transition declares: entry condition, timeout, success discriminator,
failure discriminator, bounded recovery action. Non-negotiables, each enforced
by the reference implementation in the test suite:

- **Qualification is behavioral.** `SHELL_QUALIFY` requires real responses to
  `ver`, `usbstat`, `pwd`, and a host0 round-trip marker — command echo alone
  is never success.
- **No semantic result without qualification.** After any qualification loss
  the epoch stops collecting results; already-collected results stay valid
  only if their envelopes say `qualification_status=QUALIFIED` at capture time.
- **Stale rejection.** Every RESULT carries epoch id + case iteration;
  anything not matching the current epoch/case is discarded as stale.
- **Timeouts are discriminated**: distinguish "no bytes" (transport dead),
  "bytes but no frame" (echo/banner pollution), "complete frame, error status"
  (semantic failure) — they recover differently.

## 5. Recovery ladder (bounded)

| Level | Action | Bound |
| --- | --- | --- |
| L0 | retry one protocol request | 1 retry, short timeout |
| L1 | restart owned host usbhostfs_pc process; if it reports `waiting for device`, identify the PSPLink device from `usbipd list`, attach once when `Shared`, wait for `Connected to device`, and verify `pspsh -e ver` | 1 cycle and 1 attach attempt, then requalify from `WAIT_ATTACH` |
| L2 | qualified PSPLink `reset`, followed by the same dynamic USB re-attach and `ver` check | 1 reset and 1 attach attempt; allowed only if SHELL_QUALIFY still answers |
| L3 | standalone PSPLink relaunch boundary | not implemented by the scalar adapter; in the works under [issue #352](https://github.com/Jstar269/nakagawa-recomp/issues/352); not measured |
| L4 | declare `PHYSICAL_INTERVENTION_REQUIRED`, preserve all state, stop | final; looping is not recovery |

Additional hard rules: never load a duplicate PSPLink user module (see
`0x8002013B` above); never continue collecting evidence after qualification is
lost; every escalation appends a `RECOVERY_EVENTS` record to the epoch
envelope. The ladder fails closed: exhaustion of L3 ends in L4, not in more L0s.
The scalar adapter implements one L0 qualification/cleanup attempt, one L1 host
USBHostFS restart or transport re-attach, and one qualified L2 `reset` followed
by transport re-attach. It stops at L4 if recovery does not qualify. The adapter
never binds or detaches USB devices: an absent or unbound PSPLink device includes
the exact manual command in the L4 reason, and a failed attach or `ver` check
stops after the single attempt.

## 6. Evidence envelope

One envelope per case iteration, written before the next case starts:

```text
CONSOLE_ID, PHYSICAL_MODEL_LABEL, SOFTWARE_MODEL_RAW_VALUE, FW, CFW, CLOCK,
TRANSPORT_PROFILE, SOURCE_COMMIT, BINARY_SHA256, RUNNER_SHA256, CASE_ID,
ITERATION, RAW_RESULT, CONTROL_RESULT, START_TIME, END_TIME,
RECOVERY_EVENTS[], QUALIFICATION_STATUS, EVIDENCE_CLASS, WHAT_IS_NOT_PROVEN
```

Two standing rules:

- Raw vs interpreted device values are stored separately and never silently
  reconciled. The known physical-PSP-3000-series vs `kuKernelGetModel()==3`
  contradiction must survive into every envelope that reports a model field:
  `PHYSICAL_MODEL_LABEL=psp-3000-series (chassis observation)` alongside
  `SOFTWARE_MODEL_RAW_VALUE=3` plus `MODEL_CONTRADICTION=RECORDED`.
- `EVIDENCE_CLASS` uses the repository vocabulary (`PSP_HARDWARE`,
  `PRODUCTION_DISPATCH`, ...). A runner self-test or empty-fixture pass is at
  best `PRODUCTION_HELPER`; it is never promoted to hardware truth by the
  tooling.

## 7. Fault containment

Principle: experiment failure may cost the experiment, never the control
plane.

- Cases run in child threads/modules with bounded lifecycle under the resident
  supervisor; the control thread keeps a reserved stack and never executes
  case code in-process.
- User-mode risk cells are separated from kernel-risk cells into explicit
  sessions; dangerous classes run in dedicated sessions bracketed by reboots.
- Where PSP semantics allow, risky payloads load as child modules under the
  supervisor rather than replacing it.
- If an Allegrex exception or kernel panic still destroys the process, the
  orchestrator classifies the epoch `SESSION_WEDGED` and enters the ladder at
  the level matching the last observed heartbeat — it does not fake recovery.

## 8. Zero-touch feasibility

Remaining human actions, classified:

| Action | Class | Software path forward |
| --- | --- | --- |
| USB_CABLE | physical | none; external powered-hub automation could remove replug needs but is out of scope |
| POWER_SWITCH | physical | hard wedges need power cycling; honest answer: requires external actuation or human. Not solvable in software from PSPLink alone once USB is dead |
| XMB_APP_LAUNCH | software-feasible | ARK autostart of a dedicated source-owned runner plugin designed to coexist with (not collide into) the standalone stack — design only; installation requires explicit maintainer authorization |
| PLUGIN_ENABLE/DISABLE | human-gated | same as above |
| PSPLINK_RELAUNCH | partially automatable | L3 covers the proven relaunch shape where it can be initiated host-side |
| HARD_WEDGE_POWER_CYCLE | physical | L4 + external actuation would be required for literal 100% |

Therefore: near-zero-touch is achievable for the common crash classes
(software relaunchable), while a hardware-level freeze fundamentally requires
physical power intervention. The system says so plainly instead of looping.

## 9. Build order and open attestation

1. Conformance suite (landed): `tools/test_hardware_runner_protocol.py`
   implements the reference state machine, codec, fake transport, and every
   mandated fault scenario. Future implementations must pass it unchanged.
2. Resident runner PRX: new implementation path; require a trusted
   detailed-ledger record before commit (`PROVENANCE_UNRESOLVED` otherwise),
   plus maintainer authorization before any persistent plugin/autostart change
   touches the device. The scalar-probe host adapter is implemented in the
   existing `run_psplink.py` path; the resident framed protocol is still absent.
   Campaigns use repeated `--campaign-case CASE_ID=PRX_PATH` arguments, put
   `transport-write` first, and set one bounded `--timeout` shared by each case.
   An optional `--out` must be inside the host0 scratch directory.
3. REQ_002 prerequisite: an import/startup fixture that resolves ThreadManForUser
   mutex NIDs, starts clean, emits one marker, asserts nothing — validated on
   host/toolchain first so PSP sessions debug semantics, not linker plumbing.
4. Model discriminator (bounded): compare `kuKernelGetModel`, plain syscall
   paths, and other identity sources to explain raw value 3 on the
   PSP-3000-series unit; measure first, generalize never.
