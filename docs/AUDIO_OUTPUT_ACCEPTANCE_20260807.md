# Title BGM end-to-end output acceptance — 2026-08-07

Campaign scope: trace the decoded ATRAC PCM proven by [#334] through SAS / sceAudio,
the runtime queue and mixer, and the SDL audio callback, and determine whether title
BGM actually reaches a host audio device.

Base: `origin/main` = `f9ee1639842f0124c2055f478624d73200694fff`.

This is **private-route evidence**. The runs below use the lawful private title in
`place_game_here/`; no captured audio, no retail bytes, and no path-bearing output are
committed. Only the aggregate counters quoted here left the run.

The SAS handler-gap notes in this dated snapshot describe the pre-stateful
implementation. For current NID routing, validation, and supported-state
disposition, see [`SAS_NID_SIGNATURES.md`](SAS_NID_SIGNATURES.md).

## Instrument

`SR_AUDIOSTAT=1` adds bounded aggregate counters at four stages. It exists because the
pre-existing `SR_AUDIOLOG` / `SR_SASLOG` / `SR_ATRACLOG` traces are **power-of-two
decayed** — they emit calls 1, 2, 4, … 512 only. On this route those samples land almost
entirely inside the silent boot window, so a decayed trace showing `AUDIO_PUSH … peak=0`
is consistent with both "the chain is silent" and "the chain is fine and we sampled the
wrong calls". Totals distinguish them; samples cannot. Everything is off by default and
costs nothing when unset.

## Runs

Route `logs/route_boot_mainmenu_20260725.pad`, `-ExitAtVblank 10000`, save baseline held
still with `-SaveBase`. All runs report `host_data: indexed 56672 files`, so each is
admissible under the extracted-data precondition.

| Run | wall | vbl/s | ATRAC decoded | nonzero at device |
| --- | --- | --- | --- | --- |
| `audioout1` | 369.3 s | 27.1 | 105.5 s | 106.3 s |
| `audioout2` | 146.4 s | 68.3 | 106.3 s | 107.7 s |
| `audiocensus` | 145.1 s | 68.9 | 105.5 s | 106.3 s |
| **`audiofinal`** | 147.3 s | 67.9 | 106.1 s | 106.8 s |

All four reached vblank 10000 organically with exit code 0.

`audiofinal` is the **pinned reference**: it was produced by the exact tree this branch
commits, so every number below is reproducible from the committed source rather than from
an intermediate probe build. The earlier three agree with it and are retained because the
`audioout1`/`audioout2` pair is the throughput control.

## Stage-by-stage result

```text
AUDIOSTAT_ATRAC: frames=2282 nonzero=2282 peak=32768
AUDIOSTAT_SAS:   calls=6502 withmix=6076 pre_nonzero=6140 post_nonzero=6139 erased=4
                 pre_peak=20808 post_peak=32768 no_voice=6266 no_voice_overwrite=359
AUDIOSTAT_BUF:   atrac_out n=1 0x09e67780
AUDIOSTAT_BUF:   sas_out   n=2 0x0a04a7c0 0x0a04b3c0
AUDIOSTAT_BUF:   ch=8      n=2 0x0a04a7c0 0x0a04b3c0
AUDIOSTAT_DEV:   inited=1 ok=1 stream=<non-null>
AUDIOSTAT_PUSH:  ch=8 calls=6502 nonzero_calls=6139 frames=4993536 peak=32768
                 clamps=0 dropped=0 snaps=215
AUDIOSTAT_CB:    calls=14473 frames=6382593 nonzero_frames=4711103 peak=32768 put_fail=0
```

### Buffer identity

The three `AUDIOSTAT_BUF` lines close the chain by address, not by inference: ATRAC
decodes into `0x09e67780`, the game's own mixer moves that PCM into the double-buffered
`0x0a04a7c0` / `0x0a04b3c0`, SAS mixes sound effects into **those same two buffers**, and
`sceAudioOutput2OutputBlocking` submits **exactly those two addresses**. There is no
fourth buffer and no copy that goes missing.

### Call census

Taken with `SR_CALLCOUNT=1 SR_CALLCOUNT_ALL=1`, so the dump is uncapped — it reports
`HLE calls (110 of 110 distinct NIDs)` — and a NID that is absent was never called.

| NID | calls |
| --- | --- |
| `sceAudioOutput2OutputBlocking` | 6502 |
| `__sceSasGetEndFlag` | 6502 |
| `__sceSasCoreWithMix` | 6076 |
| `__sceSasCore` | 426 |
| `sceAtracDecodeData` | 2282 |
| `__sceSasSetKeyOn` / `SetVoice` / `SetVolume` | 25 each |
| `__sceSasSetVoiceATRAC3` `0x4aa9ead6` | **0** |
| `__sceSasConcatenateATRAC3` `0x7497ea85` | **0** |
| `__sceSasUnsetATRAC3` `0xf6107f00` | **0** |
| `sceAtracGetMaxSample` `0xd6a5f2f7` | **0** |
| `sceAtracGetOutputChannel` `0xb3b5d042` | **0** |
| `sceAtracGetChannel` `0x31668baa` | **0** |
| `sceAtracGetBitrate` `0xa554a158` | **0** |

`6076 + 426 = 6502` exactly matches both the SAS call total and the Output2 submission
count: every SAS mix is submitted, one for one. The title never calls the SAS ATRAC3
voice APIs, and never calls the four unmodelled `sceAtrac` getters — so neither of those
`h_ok` families can be on the BGM path, and neither was touched.

1. **ATRAC decode** — every decoded frame is nonzero at full scale. No decode gap.
2. **Guest copy into the SAS buffer** — `pre_nonzero=6191` of 6567 SAS calls find PCM
   *already present* in the output buffer on entry. The game's own BGM copy works.
3. **SAS mix** — `erased=4`: SAS wiped an already-populated buffer four times in 6567
   calls. `__sceSasCoreWithMix` (6090 calls) adds rather than overwrites.
4. **sceAudioOutput2** — 6189 of 6567 submissions carry nonzero audio.
5. **Ring → SDL callback → device** — a real device is open, `put_fail=0`, and
   4,749,270 nonzero frames reach it at up to full scale.

## Sustained playback

Duty cycle per ~5 s of device time (`AUDIOSTAT_WIN`):

| vbl range | duty |
| --- | --- |
| 16 – 521 | 0% (boot, before BGM starts) |
| 812 – 1414 | 19% → 1% → 62% (intro transitions) |
| **1709 – 6812** | **98–100% across 18 consecutive windows** |
| 7470 – 8162 | 51% → 0% → 45% (menu advance; route presses CROSS at 6876/6988/7910/7966) |
| 9141 | 96% |

In the steady state the guest supplies 220,416 frames per 220,500 the device consumes —
**99.96%**. The dip at 7470–8162 coincides exactly with the route's menu inputs, i.e. a
BGM track change, not a chain failure.

## Why no downstream defect exists

The one measurement that could still hide loss is the push path's own drop sites.
`sr_audio_push` can lose guest audio in exactly two places — a clamp that truncates a
buffer the ring cannot hold, and a snap that abandons a write cursor the playhead has
passed. Both are now counted, and the run reports **`clamps=0 dropped=0`**: not one frame
of guest audio was discarded. (`snaps=215` of 6502 is the cursor resyncing to the
playhead after an underrun window; a snap relocates the write position, it does not drop
samples.)

The overwrite mechanism that would erase BGM at the SFX stage does occur — `__sceSasCore`
ran with no active voice 359 times — but `erased=4` shows only four of those found any
audio in the buffer to destroy. Overwrite-on-`__sceSasCore` is also the correct PSP
behaviour, so those four are the game's own sequencing, not an emulation defect.

**The control**: `audioout1` ran at 27.1 vbl/s and `audioout2` at 68.3 vbl/s — a 2.5×
wall-clock difference on the same route and same save baseline. They delivered 106.3 s
and 107.7 s of nonzero audio respectively. If guest slowness caused the chain to drop
audio, the slow run would have delivered materially less. It did not. Guest throughput
changes the *proportion* of wall time that is silent; it does not change the amount of
real audio delivered.

The reason is that the guest audio thread paces against the host device queue
(`sr_audio_queued`), not against vblanks — so it produces 44.1 kHz of audio per
*wall-clock* second even while video renders at 45% of PSP rate. Comparing frames pushed
against *guest* elapsed time invents a deficit that does not exist.

The whole-run duty figure therefore says nothing about chain health: `audioout1` reads
29% and `audioout2` reads 75% purely because the device pulled 365 s versus 144 s of real
time around the same ~107 s of music.

## Disposition

Title BGM is audible, continuous and sustained. No defect was found downstream of the
decoder, so no behavioural fix is included. The load-bearing property that the acceptance
rests on — that `__sceSasCoreWithMix` **adds** to the caller's PCM while `__sceSasCore`
**overwrites** it — is now pinned by a production-dispatch regression in
`src/rt/hle_thread_selftest.c`, verified failing-before/passing-after by mutating
`h_SasCoreWithMix` to overwrite (3 assertions trip).

Known gaps deliberately **not** addressed here, because none of them is on the proven BGM
path and each would be a speculative semantics change:

- `__sceSasCoreWithMix` ignores its `leftVol`/`rightVol` arguments (affects SFX level,
  not BGM presence). Tracked by [#75].
- `sceAtracGetMaxSample`, `sceAtracGetOutputChannel`, `sceAtracGetChannel` and
  `sceAtracGetBitrate` are `h_ok` and leave their out-pointers untouched (tracked by
  [#286]) — **but the census shows this title calls none of them**, so implementing them
  could not have affected this route and would have been a change without evidence.
- `__sceSasSetVoiceATRAC3` / `ConcatenateATRAC3` / `UnsetATRAC3` are `h_ok` and the SAS
  model has no ATRAC3 voice state at all. **The census shows zero calls to all three**;
  this title feeds BGM through its own guest-side mixer into the SAS output buffer, not
  through SAS ATRAC3 voices.

[#75]: https://github.com/Jstar269/nakagawa-recomp/issues/75

[#334]: https://github.com/Jstar269/nakagawa-recomp/pull/334
[#286]: https://github.com/Jstar269/nakagawa-recomp/issues/286
