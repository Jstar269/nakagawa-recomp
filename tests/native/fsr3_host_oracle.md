# FSR 3 host record

`fsr3_host_oracle.txt` is what AMD's own FSR 3 host code does for a fixed table of inputs.
`fsr3_host_selftest.c` replays the same inputs through the project's C port
(`src/rt/fsr3_host.c`, `src/rt/fsr3_jitter.c`) and requires the same record. This file says how the
record was made.

## Source

- AMD FidelityFX SDK v2.4.0, commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` (FSR 3.1.6).
- Compiled unmodified, Git blob IDs as at that commit:
  - `Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp`, blob `09edf3e99597c955ba5665ff4535e26eb7fdc86e`.
  - `Kits/FidelityFX/api/internal/ffx_object_management.cpp`, blob `c22b3c999288f814ef6dd67f6603922b50090bcd`.
- Two stand-ins that the upscaler does not use:
  - `api/internal/ffx_provider.h` is replaced by a header that includes only `ffx_api.hpp`,
    `ffx_api_types.h`, `ffx_internal_types.h` and `ffx_api_helper.h`. The v2.4.0 header names a type,
    `ffxProviderExternal`, that the release does not ship, so it does not compile.
  - `git_hash_branch.h`, which the release generates at build time, defines `FFX_GIT_HASH_BRANCH` as `"oracle"`.
- Compiler: MSVC 19.51.36260 for x64, AMD's reference compiler, with
  `cl /std:c++20 /EHsc /O2 /DNDEBUG /MD`. MSVC's `<cmath>` declares float overloads of `tan`, `atan`,
  `sin`, `cos`, `pow` and `exp2` at global scope, so AMD's float expressions call the float functions.
  The C port calls `tanf`, `atanf` and the rest to match.

## The recording backend

A project-authored driver links the two files above with a backend whose functions record instead of
recording GPU commands:

- A created resource is numbered with its AMD resource id. Each resource the caller passes is tagged
  with the AMD id of the slot it fills, so a binding resolves to the same number in the record and in
  the port. An absent optional input (exposure, reactive mask, transparency and composition mask) is
  a null resource.
- A pipeline gets the binding names that the project's SPIR-V declares for that pass
  (`src/rt/gpu_sdl3vk/fsr3_*_comp.inc`). AMD's `patchResourceBindings` then resolves each name through
  its own tables. The project builds accumulate only with `APPLY_SHARPENING=0`, so AMD's sharpening
  permutation gets the same names. The port then lets RCAS write the output after accumulate.
- Device capabilities are 32-wide waves with no FP16. They select permutations only, and no record
  line depends on them.

## Record format

One line per event, in AMD's order:

| Line | Meaning |
| --- | --- |
| `case <name> flags <f> max <rw> <rh> <uw> <uh>` | A new context, with its creation flags and maximum sizes. |
| `frame <i> in ...` | The dispatch inputs, with every float as its IEEE-754 bits. |
| `clear <res> <4 x float bits>` | A float clear. |
| `stage <n> <words>` | The n-th constant buffer the frame stages: 0 is `cbFSR3Upscaler`, 1 is `cbSPD` and 2 is `cbRCAS`. |
| `barrier <res> <type> <state>` | A transition barrier. |
| `discard <res>` | A discard. |
| `dispatch <pipeline> <x> <y> <z>` | A dispatch and its group counts. |
| `  set <k>` | The pipeline's k-th distinct binding set in this case. The first use of a set is followed by its lines. |
| `  cb <name> <n>`, `  srv <name> <res>`, `  uav <name> <res> <mip>` | One binding of a set: a constant buffer, a read or a write. |
| `result <code>` | The dispatch's return code. |
| `phase`, `jitter`, `jitter0`, `jitterneg` | `ffxFsr3UpscalerGetJitterPhaseCount` and `ffxFsr3UpscalerGetJitterOffset`. |
| `f16 <e> <3 results>` | `ffxF32ToF16` for 9-bit sign and exponent `e` with mantissas 0, `0x2000` and `0x7fffff`. Together these fix AMD's base and shift tables. |
| `f16all <hash>` | FNV-1a 64 over `ffxF32ToF16` of all 2^32 floats, two bytes per result, low byte first. |
| `rcas <s> <4 words>` | `FsrRcasCon` for sharpness s from 0 to 1 in steps of 0.01, after the dispatch's remap `(-2s) + 2`. |

## Inputs

Every frame's inputs are in its `frame` line. The cases:

| Case | Flags | Render to output | Frames | What varies |
| --- | --- | --- | --- | --- |
| `x2` | inverted depth | 480x272 to 960x544 | 20 | Jitter. Covers the ping-pong parity and the queued-frame wrap at 16. |
| `x1`, `x3`, `x6` | inverted depth | 480x272 to 480x272, 1440x816, 2880x1632 | 6 each | Scale. `x3` without sharpening, `x6` with sharpness 0. |
| `odd` | inverted depth | 1920x1080 to 3811x2143 | 6 | Sizes that do not divide the tile sizes. |
| `upscale0` | inverted depth | 480x272 to 0x0 (the maximum) | 3 | The output size defaults to the maximum. |
| `frame_time` | inverted depth | 480x272 to 960x544 | 8 | Frame times of 0, -5, 1000 and 2000 ms (clamped). |
| `cut` | inverted depth | 480x272 to 960x544 | 14 | Resets at frames 5, 6 and 13. |
| `dynamic_res` | inverted depth | up to 480x272, down to 1x1 | 10 | The render size changes each frame, and the phase count ramps. |
| `sharpen` | inverted depth | 480x272 to 960x544 | 12 | Sharpening on and off, sharpness 0 to 1. |
| `inputs` | inverted depth | 480x272 to 960x544 | 10 | Optional inputs present or absent. Pre-exposure 0, 0.5, 1, 2 and 4. Metres per unit 0 and 2.5. |
| `camera_inf`, `camera_fwd`, `camera_fwd_inf` | inverted and infinite, forward, forward and infinite | 480x272 to 960x544 | 4 each | Near, far and field of view, including near beyond far. |
| `auto_exposure` | inverted depth, auto exposure | 480x272 to 960x544 | 3 | Exposure from the frame-info resource. |
| `display_mv_cancel` | inverted depth, output-size motion vectors, jitter cancellation | 480x272 to 960x544 | 5 | Motion-vector scale and jitter cancellation. |
| `mv_cancel` | inverted depth, jitter cancellation | up to 480x272 | 5 | Jitter cancellation with a changing render size. |
| `hdr` | inverted depth, HDR | 480x272 to 960x544 | 2 | The HDR flag. |

The `phase` and `jitter` lines cover these render-to-output widths: 480 to 960, 480, 1440 and 2880;
1920 to 3811; 360 to 960; 1 to 1 and 1 to 3840; 1000 to 1500; 640 to 1000; 479 and 481 to 960. Short
sequences list every index from -3 to twice the phase count plus 2. Long ones list the first 64 and
the indices around the count and twice the count.

## Output

- `fsr3_host_oracle.txt`: 8352 lines.
- SHA-256: `57e91e7255239305873f88150f4db37251aaa18f564405d1737ddc84cffc27d7`.
- Two builds and runs gave the same bytes.
- SHA-256 of the driver source: `25f8afade537b887f8655f1e6651cf1edf2212013f8d874efc71d5e7d9421c3a`.
  It is kept privately with the build script, because building it needs MSVC and an AMD SDK checkout.

## What the selftest accepts

- Every line must be equal, with two exceptions.
- A `barrier` line is not replayed: a Vulkan discard is a transition from `UNDEFINED`. Each barrier must
  be a transition to unordered access of a resource that the same frame discards.
- Five values come from `<math.h>` float functions:
  - `deviceToViewDepth[2]` and `[3]` (`cosf`, `sinf`);
  - `tanHalfFOV` (`tanf`, `atanf`);
  - RCAS `con.x` and `con.y` (`exp2f`).
- A UCRT build (MSVC, MSYS2 UCRT64) must match those five exactly. Other C libraries may differ by one
  unit in the last place. glibc 2.39 rounds `tanHalfFOV` one unit the other way in some frames.
- The phase count and the SPD mip count come from the same functions but are integers, so they must
  match exactly.

`tools/fsr3_mutants.py` applies 16 semantic defects, one at a time, to copies of the port. Each one
must build and then fail the replay on the line it changes.
