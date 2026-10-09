# Source-owned showcase demos

The showcase is a pair of project-authored PSP programs, built from source with
PSPDEV/PSPSDK and passed through the normal analyzer, code generator, runtime,
and `nakagawa-aot-package` v1 path. It contains no retail identity or retail
content.

## Source and generated art

Demo sources live in `fixtures/showcase/<demo>/`; the shared PSPDEV makefile and
host-side ISO/art builder live in `fixtures/showcase/`. The builder writes
intermediate artifacts under `build/showcase/` and the copyable distribution
under `build/demos/`, including each `ICON0.PNG`, ISO image, ELF, native
package, smoke log, and captured frame. No binary icon or game image is
committed. The icon is a deterministic 144×80 PNG rasterized from a
small source-owned pixel pattern by the Python standard library.

The image writer creates a deterministic ISO9660 volume containing
`PSP_GAME/PARAM.SFO`, `PSP_GAME/ICON0.PNG`, and
`PSP_GAME/SYSDIR/EBOOT.BIN`. `EBOOT.BIN` is the plaintext PSPDEV ELF, so the
existing ISO inspector and package route can identify and analyze it without a
decryption step. The SFO `DISC_ID` values are `TEST00007` and `TEST00008`; the
catalog maps those IDs only to the two `showcase-*` synthetic manifests. Their
titles are Nakagawa showcase names, not commercial product names.

Both manifests are `kind: synthetic`, use the `profile-zero-v1` runtime
contract, declare their PSPDEV source/build inputs, and keep acceptance status
separate from package availability. The manifests keep profile-zero acceptance
in `scaffold` until the full evidence for each case is recorded. That status
does not disable local package generation or validated package launch. It does
not promote the separate `synthetic.json` / `synthetic-title2.json` #309
acceptance matrix.

## Build and package

`mingw32-make showcase` requires the installed PSPDEV toolchain in WSL at
`/usr/local/pspdev`; it does not download or install a toolchain. It builds each
PSP PRX ELF from its fixture sources, emits the ISO, then calls
`tools/title_codegen_plan.py --package --public-safe` with the matching public
title manifest. Each package is validated before it is copied into
`build/demos/packages/<DISC_ID>/`; images are placed in
`build/demos/images/<DISC_ID>.iso`.

The release layout copies the read-only `demos/` directory next to the player
executable. At startup the player consults its generated public title catalog
and discovers `showcase-*` entries only when both the catalogued image and a
valid package are present. The `Play` action uses the existing package
validator and launcher with the demo directory as the package root. The GUI
labels each bundled entry **Showcase demo**. User-added titles keep using the
user library and its package root. The launcher continues to pass a writable
per-user memory-stick root to the runtime, so the bundled demo directory stays
read-only while savedata works.

## Smoke evidence and limits

`mingw32-make showcase-smoke` launches each package with a scripted controller
sample and a bounded process timeout. It enables the runtime's existing GE,
input, audio, savedata, and framebuffer telemetry, fails on fatal dispatch,
checks that a guest frame was captured and that input/audio were submitted,
and retains PNG frame captures beneath `build/showcase/screenshots/`. The
Breakout smoke also checks the savedata file created under its temporary
memory-stick root. These are synthetic host-runtime results, not PSP hardware
evidence or commercial-title acceptance.

The smoke's budget is counted on the guest display clock. Each frame run exits
cleanly at vblank 180 (`SR_EXIT_AT_VBLANK`), three of the runtime's 60-vblank GE
statistics windows, inside a 15-second host timeout. The Breakout savedata run
ends instead when its scripted Start saves and exits. The first-frame checkpoint
is the first window whose counters show the demo's own drawing: 3D triangles for
the scene, sprites for Breakout. The smoke reads that window and its closing
vblank from `runtime.log`. A busy host can step VCOUNT past 60 and can boot the
guest later, so the checkpoint may land in a later window. The framebuffer capture
judged is the one named for that window. Every run writes its log (`runtime.log`,
or `runtime-save.log` for the savedata run) under
`build/showcase/smoke/<disc id>/`, even on a timeout. Linux CI uploads those logs
and captures when the smoke fails. The scripted Cross and Start presses are still
timed in vblanks, so a host that falls far behind can deliver one late or skip it.
When an input, audio, or savedata check fails that way, its message names the
late or skipped press.

In the 3D scene, the analog stick rotates the lit, checker-textured cube, Cross
plays a short tone, and Start exits. Breakout uses the analog stick or d-pad to
move its paddle, Cross to play a tone or restart after a loss, and Start to save
the high score and exit. Its score uses a source-owned seven-segment font, so
it does not depend on firmware fonts. It exercises `sceUtilitySavedata`'s automatic
load/save modes; the runtime's user-data root owns those bytes. Audio is sent
through `sceAudio` and the public SDL3 host output backend. Unsupported imports
remain an **in the works** boundary tracked by #281, and unsupported Allegrex
forms remain an **in the works** boundary tracked by #118. `sceKernelReferThreadStatus`
reports modeled scheduler state, but its run clock is host-scheduler time and
its interrupt-preemption, thread-preemption, and release counters are not yet
measured PSP semantics; that boundary is **in the works** under #309. The
profile-zero production ladder and its second-title assertions also remain
tracked by #309.

PSPDEV/PSPSDK is BSD-licensed. Any distributed demo image or package that
contains linked PSPSDK library code must carry the applicable copyright and
license notice; generated build outputs are not a substitute for that notice.
