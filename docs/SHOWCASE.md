# Source-owned showcase demos

The showcase is a pair of PSP programs the project wrote itself: a lit, rotating 3D cube and a small Breakout game.
They are built from source with PSPDEV/PSPSDK and go through the same analyzer, code generator, runtime and
`nakagawa-aot-package` v1 path as any game. They contain no retail game, name or content, so anyone can build and run
them.

## Source and generated art

Each demo's source lives in `fixtures/showcase/<demo>/`; the shared PSPDEV makefile and the host-side ISO and art
builder live in `fixtures/showcase/`. The builder writes its working files under `build/showcase/` and the finished,
copyable set under `build/demos/`: each `ICON0.PNG`, ISO image, ELF, native package, smoke log and captured frame.
No binary icon or game image is committed. Each icon is a 144×80 PNG drawn the same way every time from a small pixel
pattern in the source, using only the Python standard library.

The image writer makes an ISO9660 volume, identical on every build, that holds `PSP_GAME/PARAM.SFO`,
`PSP_GAME/ICON0.PNG` and `PSP_GAME/SYSDIR/EBOOT.BIN`. `EBOOT.BIN` is the plain PSPDEV ELF, so the usual ISO inspector
and package route can identify and analyze it without any decryption. The SFO `DISC_ID` values are `TEST00007` and
`TEST00008`, and the catalog maps those IDs only to the two `showcase-*` synthetic manifests. Their titles are
Nakagawa showcase names, not commercial product names.

Both manifests are `kind: synthetic`, use the `profile-zero-v1` runtime contract, declare their PSPDEV source and
build inputs, and keep their acceptance status separate from whether a package can be built. The manifests keep
profile-zero acceptance at `scaffold` until the full evidence for each case is recorded. That status doesn't stop you
building a package locally or starting a validated package. It also doesn't promote the separate `synthetic.json` /
`synthetic-title2.json` #309 acceptance matrix.

## Build and package

`mingw32-make showcase` needs the PSPDEV toolchain installed in WSL at `/usr/local/pspdev`; it doesn't download or
install a toolchain for you. On Windows, the steps to install it in WSL 2 are in
[SETUP](SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev). The build compiles each demo's PSP PRX ELF from its
sources, writes the ISO, then calls `tools/title_codegen_plan.py --package --public-safe` with the matching public
title manifest. Each package is checked before it is copied into `build/demos/packages/<DISC_ID>/`; the images go to
`build/demos/images/<DISC_ID>.iso`.

The release layout copies the read-only `demos/` folder next to the player executable. When the player starts, it
looks at its generated public title catalog and lists a `showcase-*` entry only when both the catalogued image and a
valid package are there. **Play** uses the usual package check and launcher, with the demo folder as the package
root. The library labels each bundled entry **Showcase demo**. Games you add yourself keep using your own library
and its package folder. The launcher still gives the runtime a writable Memory Stick folder in your user data, so the
bundled demo folder stays read-only while saving works.

## Smoke evidence and limits

`mingw32-make showcase-smoke` starts each package with a scripted controller press and a time limit on the process.
It turns on the runtime's existing GE, input, audio, save and framebuffer telemetry, fails if dispatch hits a fatal
error, checks that a frame from the demo was captured and that input and audio were submitted, and keeps PNG
captures under `build/showcase/screenshots/`. The Breakout smoke also checks the save file it creates under its
temporary Memory Stick folder. These are results from the host runtime with synthetic programs, not evidence from PSP
hardware or proof that any retail game works.

The smoke also tests pausing, through the scheduler pause path and `SR_PAUSE_SCRIPT`. Breakout pauses at VCOUNT 120
while Cross is scheduled for 121. The log checks that VCOUNT, VBLANK and the game's clock stop, that no controller
latch or GE window happens during the pause, and that the pause starts within two display periods. The cube scene
schedules Cross at VCOUNT 12 and arms its pause at VCOUNT 18, after the demo has had a scheduler turn to read the pad
and start its longer synthetic tone; the log checks that the tone is held, with fade telemetry and no extra underrun.
Breakout also pauses while it is saving, checks that Quit would be blocked, resumes, and lets the demo finish the
save. A turbo off-screen run pauses before the capture window and still needs every presented capture, and a pause
request from a user in a run without a window must be refused. Only runs with a scripted pause get a longer time
limit.

The smoke's time budget is counted on the demo's own display clock. Each frame run exits cleanly at VBLANK 180
(`SR_EXIT_AT_VBLANK`), which is three of the runtime's 60-VBLANK GE statistics windows, inside a 15-second host time
limit. The Breakout save run ends instead when its scripted Start press saves and exits. The first-frame checkpoint is
the first window whose counters show the demo's own drawing: 3D triangles for the cube scene, sprites for Breakout.
The smoke reads that window and the VBLANK that closes it from `runtime.log`. A busy PC can push VCOUNT past 60 and
start the demo later, so the checkpoint may land in a later window, and the framebuffer capture it judges is the one
named for that window. Every run writes its log (`runtime.log`, or `runtime-save.log` for the save run) under
`build/showcase/smoke/<disc id>/`, even when it times out. Linux CI uploads those logs and captures when the smoke
fails. The scripted Cross and Start presses are delivered in the demo's own time: each one is held until the demo has
read it, however far behind the PC falls (see
[`DEBUGGING.md`](DEBUGGING.md#scripted-input-is-delivered-in-guest-time)). A press the demo never reads fails the run
by name. When an input, audio or save check fails, its message says whether the demo read the press.

In the 3D scene, the analog stick turns the lit, checker-textured cube, Cross plays a short tone, and Start exits.
Breakout uses the analog stick or d-pad to move the paddle, Cross to play a tone or restart after a loss, and Start to
save the high score and exit. Its score uses a seven-segment font drawn in the source, so it doesn't need firmware
fonts. It uses `sceUtilitySavedata`'s automatic load and save modes, and the runtime's user-data folder holds those
bytes. Sound goes through `sceAudio` and the public SDL3 audio output.

Some limits remain. An import that isn't supported yet stops by name instead of being faked (see #281), and so
does an Allegrex instruction form the interpreter floor doesn't handle (see #118). `sceKernelReferThreadStatus`
reports the modelled scheduler state, but its run clock is the host scheduler's time, and its interrupt-preemption,
thread-preemption and release counters are not measured PSP behaviour yet. The profile-zero production
ladder and its second-title checks are described under #309.

PSPDEV/PSPSDK is BSD-licensed. Any demo image or package you share that contains linked PSPSDK library code must carry
the applicable copyright and licence notice; generated build output does not replace that notice.
