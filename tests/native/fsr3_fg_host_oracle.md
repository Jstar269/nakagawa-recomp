# FSR 3 frame generation host record

`fsr3_fg_host_oracle.txt` is what AMD's own FSR 3 frame interpolation and optical flow host code does
for a fixed table of inputs. `fsr3_fg_host_selftest.c` replays the same calls through the project's C
port (`src/rt/fsr3_fg_host.c`) and requires the same record. This file says how the record was made.

## Source

- AMD FidelityFX SDK v2.4.0, commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` (FSR 3.1.6).
- Compiled unmodified, Git blob IDs as at that commit:
  - `Kits/FidelityFX/framegeneration/fsr3/internal/ffx_frameinterpolation.cpp`, blob `45668249af3acee34cee23f53f653186a623973e`.
  - `Kits/FidelityFX/framegeneration/fsr3/internal/ffx_opticalflow.cpp`, blob `837af0d8288b8fa0cab4e4720a56ebf29ebdcee8`.
  - `Kits/FidelityFX/api/internal/ffx_object_management.cpp`, blob `c22b3c999288f814ef6dd67f6603922b50090bcd`.
- The same two stand-ins as the upscaler's record (`fsr3_host_oracle.md`): `api/internal/ffx_provider.h`
  replaced by a header that includes only `ffx_api.hpp`, `ffx_api_types.h`, `ffx_internal_types.h` and
  `ffx_api_helper.h`, and a `git_hash_branch.h` that defines `FFX_GIT_HASH_BRANCH` as `"oracle"`.
- Compiler: MSVC 19.51.36260 for x64, with `cl /std:c++20 /EHsc /O2 /DNDEBUG /MD`. MSVC's `<cmath>`
  declares float overloads of `tan` and `atan` at global scope, so AMD's
  `atan(tan(fov / 2) * aspectRatio)` calls the float functions. The C port calls `tanf` and `atanf`.

## The provider's glue

The two files above are driven the way AMD's frame generation provider
(`Kits/FidelityFX/framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp`, blob
`5df09b9e95089b8f28e255c1a3b1654d05faa461`) drives them. The provider itself is not compiled: it
needs the SDK's API layer and its swapchain. The driver transcribes the parts that decide what the
two files receive, keeping AMD's expressions:

- configure: whether a HUD-less colour is attached, and the frame ID it was configured for;
- prepare: the description kept per frame (`prepareDescriptions[frameID % 2]`), the shared set of
  prepared images flipped each prepare, and the frame interpolation prepare;
- dispatch: the disjoint-frame test (a frame ID that went back, skipped, or is not the one last
  configured), the reset, then optical flow and frame interpolation with the descriptions the provider
  builds (the optical flow colour, the block size of 8, the scale of 1 / display, the
  interpolation rectangle), and the last frame ID.

So this record checks the port's glue against a transcription of AMD's provider, and the two files
it calls against AMD's own code. The glue's behaviour over a real frame sequence is the G-S cell's.

## The recording backend

A project-authored driver links the files above with a backend whose functions record instead of
recording GPU commands:

- A resource that frame interpolation creates is numbered with its AMD resource id, and one that optical
  flow creates with 64 plus its AMD id. Each resource the caller or the provider passes is tagged with the
  port's number for it (`FSR3FG_RES_*` in `fsr3_fg_host.h`, from 128), so a binding resolves to the same
  number in the record and in the port. Those caller resources are 2D textures of the display size in
  the back-buffer format, the only description fields FSR 3.1.6 reads.
- A pipeline gets the binding names that the project's SPIR-V declares for that pass
  (`src/rt/gpu_sdl3vk/fsr3fg_*_comp.inc`), constant buffers, images and storage buffers apart. AMD's
  `patchResourceBindings` then resolves each name through its own tables.
- Device capabilities are 32-wide waves. They select permutations only, and no record line depends
  on them.

## Record format

One line per event, in AMD's order:

| Line | Meaning |
| --- | --- |
| `case <name> flags <f> max <rw> <rh> display <dw> <dh>` | New contexts, with frame interpolation's creation flags and sizes. |
| `configure <frame ID> <hudless>` | The provider's configure. |
| `prepare <frame ID> <rw> <rh> <9 x float bits> <reset>` | The provider's prepare: render size, jitter, motion-vector scale, frame time, near, far, field of view, metres per unit, and its reset. The events follow. |
| `dispatch <frame ID> <reset> <rect> <transfer> <2 x float bits>` | The provider's dispatch: its reset, the generation rectangle (0 0 0 0 is the display), the transfer function and the luminance range. The events follow. |
| `stage <n> <words>` | The n-th constant buffer the call stages. |
| `clear <res> <4 x 32-bit words>` | A clear, AMD's colour as bits (integer clears copy their values into the same words). |
| `barrier <res> <type> <state>` | A barrier: type 0 is a transition, to state 2 (unordered access) or 32 (copy destination). |
| `discard <res>` | A discard. |
| `copy <src> <dst> <src offset> <dst offset> <size>` | A copy of a whole image. |
| `dispatch <pipeline> <x> <y> <z>` | A dispatch and its group counts. |
| `set <k>`, indented two spaces | The pipeline's k-th distinct binding set in this case. The first use of a set is followed by its lines. |
| `cb <name> <n>`, `srv <name> <res>`, `uav <name> <res> <mip>`, `srvbuf <name> <res>`, `uavbuf <name> <res>`, each indented two spaces | One binding of a set: a constant buffer (by stage), a read, a write, a read-only and a writable storage buffer. |
| `result <code>` | The call's return code. |

## Inputs

Every call's inputs are in its line. Every case creates its contexts with inverted depth and render
size motion vectors, the configuration the project's shaders are built for.

| Case | Flags | Render to display | Frames | What varies |
| --- | --- | --- | --- | --- |
| `steady` | inverted | 480x272 to 960x544 | 20 | Jitter. Covers both parities, optical flow's queued-frame wrap at 16 and the provider's two prepared sets. |
| `no_hudless` | inverted | 480x272 to 960x544 | 4 | No HUD-less colour. |
| `mixed_hudless` | inverted | 480x272 to 960x544 | 6 | The HUD-less colour attached and detached. |
| `cut` | inverted | 480x272 to 960x544 | 12 | Dispatch resets at frames 5 and 10, prepare resets at 7 and 8. |
| `frame_ids` | inverted | 480x272 to 960x544 | 16 | Frame IDs that skip and go back, a frame prepared and never generated, one configured under another ID and one not configured. |
| `dynamic_res` | inverted | up to 480x272, down to 1x1 | 10 | The render size changes, including odd sizes. |
| `rect` | inverted | 480x272 to 960x544 | 5 | Generation rectangles. |
| `camera`, `camera_inf` | inverted; inverted and infinite | 480x272 to 960x544 | 5 each | Near, far, field of view, metres per unit 0, 1, 2.5, -1 and 0.01, near beyond far. |
| `frame_time` | inverted | 480x272 to 960x544 | 8 | Frame times of 0, -5, 1000 and 2000 ms, passed through unclamped. |
| `hdr` | inverted, HDR | 480x272 to 960x544 | 4 | The transfer function and luminance range. |
| `motion_scale` | inverted | 480x272 to 960x544 | 5 | Motion-vector scales, negative and 0. |
| `x1`, `x3` | inverted | 480x272 to 480x272, 1440x816 | 3 each | Scale. |
| `odd`, `tiny` | inverted | 480x272 to 3811x2143, 100x60 | 3 each | Display sizes that do not divide the group sizes. |
| `max_1080p` | inverted | 1920x1080 to 3840x2160 | 3 | A larger maximum. |

## Output

- `fsr3_fg_host_oracle.txt`: 22362 lines, 115 prepares and 114 dispatches.
- SHA-256: `50371f13b74c3ddad39b2b20799a7634f712a5db064b917b6b42e33d667ad130`.
- Two clean builds and runs gave the same bytes.
- SHA-256 of the driver source: `b14786c95b3cd4e781c89f3f955aa4a9c07956b76a024346618a35911def90ac`.
  It is kept privately with the build script, because building it needs MSVC and an AMD SDK checkout.

## What the selftest accepts

- Every line must be equal, barriers included, with two exceptions.
- Optical flow's SPD constants (`cbOF_SPD`) have three padding words that AMD's code leaves
  uninitialised, so the record holds whatever was on the stack (0 in every line of this record). The
  port writes 0, and those words are not compared. No compiled pass declares `cbOF_SPD`: the input
  pyramid asks the downsampler for 6 mips, which returns before it reads the buffer.
- Three words of `cbFI` come from `<math.h>` float functions: `deviceToViewDepth[2]` and `[3]`
  (`cosf`, `sinf`) and `fTanHalfFOV` (`tanf`, `atanf`, `tanf`). A UCRT build (MSVC, MSYS2 UCRT64)
  must match them exactly. Elsewhere each is compared within a bound worked out from that dispatch's
  inputs, assuming each call may differ by one unit in the last place between C libraries and carrying
  that through the slope of what follows. glibc 2.39 is 2 units from UCRT in `fTanHalfFOV` in some
  frames, within that frame's bound.
- AMD's debug-bar colour index is one function-static value for the whole process. The record is one
  process, so the selftest carries the port's index from case to case. The inpainting pass reads that
  colour only to draw debug tear lines, behind a dispatch flag the port never sets (its
  `dispatchFlags` is 0).

Then the selftest checks the refusals that the record does not exercise: creation flags the shaders
were not built for, sizes of 0, render sizes above the maximum and generation rectangles outside
the display.

`tools/fsr3_mutants.py --fg` applies 25 semantic defects, one at a time, to copies of the port. Each
one must build and then fail the replay on the line it changes.
