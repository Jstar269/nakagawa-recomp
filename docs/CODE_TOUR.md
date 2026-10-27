# Code tour: from a disc image to a frame on screen

This page follows one game through Nakagawa Recomp, from the disc image a user adds in the player
to the first frame the game draws. Each step names the file and the function to open, so you can
read along. [`ARCHITECTURE.md`](ARCHITECTURE.md) explains the design and the reasons behind it;
this page is the route through the code. When the two disagree with the source, the source wins.

Function names are the quickest way in: search for them. Line numbers are left out on purpose,
because they change with every pull request.

## The pieces

| Folder | Language | What it is |
| --- | --- | --- |
| `src/player/` | C, SDL3 | The launcher: library, title screens, settings, building and starting games. |
| `src/ui/`, `src/player/ui/` | C | The launcher's newer UI toolkit and the screens built with it. |
| `src/core/` | C | Code the player and the tools share: reading disc images, decryption, title manifests, starting a game. |
| `tools/` | Python | Analysis and code generation, the `nk_cli.py` command line, and the test suite (`tools/test_*.py`). |
| `src/rt/` | C | The runtime: everything linked into each built game, standing in for the PSP's system software and hardware. |
| `build/` | generated | Generated C and objects. Never committed, never edited by hand. |

## 1. The user adds a disc

The player starts in `main()` in `src/player/main.c`. It loads the user's library and settings
(`player_app_init` and friends in `src/player/player_state.c`), opens the window, and runs the
event loop. The library and title screens are drawn by the newer UI in `src/player/ui/`
(`player_ui_nk_render`); other screens by `ui_render_frame()` in `src/player/ui_renderer.c`.

When the user picks a disc image, the player inspects it with `src/core/nk_iso.c`:
`nk_iso_inspect()` reads `PARAM.SFO` for the disc ID and title, and
`nk_iso_classify_executable()` works out whether the game's `EBOOT.BIN` is a plain MIPS ELF or
encrypted.

## 2. Staging and decryption

Staging copies what the game needs from the disc into the user's private per-title folder. The
player runs it on a worker thread (`start_staging_job()` in `main.c`), which calls
`player_stage_title()` in `src/player/setup_staging.c`; that copies the files with
`nk_iso_extract_game()` from `nk_iso.c`. The disc image is untrusted input, so every size, offset and file name read from it
is checked before use.

Most retail games ship an encrypted executable. The player decrypts it locally, from the user's
own disc, the way PPSSPP does. Before a title is built, the player checks it
(`player_build_compatibility_preflight_for_game()` in `src/player/player_state.c`); for an
encrypted `EBOOT.BIN` that check calls `player_decrypt_disc_eboot()`, which reads the file from
the image and decrypts it with `nk_container_decrypt()` in `src/core/nk_psp_container.c` and the
other `src/core/nk_psp_*.c` files. The same code is also a command-line program, `nk_decrypt`
(`src/core/nk_decrypt_main.c`). Firmware files are refused by name before any decryption step,
and decrypted output never leaves the user's machine.

## 3. Building the runtime package

Each game becomes its own native program, called its runtime package. When the user presses
BUILD PACKAGE, `src/player/package_builder.c` finds Python and the build tools and runs
`tools/nk_cli.py build-package <disc id>` (`cmd_build_package()`), then shows the JSON progress
lines that command prints as stages and a progress bar. A package that is still valid for the
same inputs is reused, not rebuilt.

`build-package` hands the title's manifest to `tools/title_codegen_plan.py`, which plans the
build and then runs the two steps below.

## 4. Finding the functions: `tools/analyze.py`

A PSP executable has no reliable list of its functions, so `analyze()` in `tools/analyze.py`
finds them: it starts from the entry point and the module's exports, follows every direct call it
can reach, and adds pointers into code it finds in constructor tables and data. The result is a
set of function start addresses and the executable spans each one owns. Those spans matter later:
only code inside an analyzer-owned span may ever run.

## 5. Translating MIPS to C: `tools/codegen.py`

`main()` in `tools/codegen.py` turns every function the analyzer found into a C function.
`emit_function()` walks the function's instructions and `effect()` (with `vfpu_effect()` and
`fpu_effect()` for the coprocessors) gives the C for each one: a register write such as
`s->r[2] = ...`, a guarded memory access, or a call into the runtime. A branch becomes a `goto`
to a label in the same C function. A direct call (`jal`) to another translated function becomes
a plain C call to it. A call or jump whose target is only known at run time (`jalr`, `jr`), or
whose target was not translated, goes through the runtime's dispatch instead.

The output is split into several `build/<game>/<game>_recomp_<n>.c` files, sized from the
number of functions, plus `sr_register_all()`, which registers every translated function at
start-up. The default output must stay byte-identical when an opt-in flag such as `--lle-cpu` is
off.

`title_codegen_plan.py` then compiles the generated C together with the runtime in `src/rt/`
using `make`. The result is the game's executable.

## 6. Starting the game

When the user presses Play, the player starts the game on another worker thread
(`player_launch_job_start()` in `main.c`). `src/core/nk_launch.c` checks the staged executable
and the runtime package once more, then `nk_launch_start()` starts the game as a separate process,
normally with `--gui` (`nk_platform_spawn_process()` in `src/core/nk_platform_win32.c` or
`nk_platform_posix.c`). The player keeps running and follows the process with
`nk_launch_is_running()` until it exits.

## 7. Boot, inside the game's process

The game's `main()` is in `src/rt/driver.c`. It loads the program into guest memory
(`sr_mem_init()` and `sr_load_segment()` in `src/rt/recomp.c`), calls `sr_register_all()` so the
runtime knows every translated function, and checks there is one at the entry point. Then
`gui_init()` in `src/rt/gui.c` opens the window and picks the presenter (SDL3 with Vulkan by
default), and `sched_run()` in `src/rt/sched.c` starts the game's first thread at the entry point.

All guest state lives in one `CpuState` (`src/rt/recomp.h`): the 32 registers (MIPS `$ra` is
`r[31]`), the FPU and VFPU registers, and the transfer state. Its layout is a versioned ABI
(`SR_CPUSTATE_ABI_VERSION`); [`ARCHITECTURE.md`](ARCHITECTURE.md#cpustate-abi) describes it.

## 8. Running guest code

A translated function runs as ordinary C and calls other translated functions directly. When the
target is only known at run time, it goes through the dispatch functions in `src/rt/recomp.c`:
`dispatch()`, `dispatch_call()` and `dispatch_tail_step()`. They look the target address up with
`sr_lookup()` and call the translated function.

If the target is valid code that was not translated, the interpreter floor in
`src/rt/guest_interp.c` runs it, but only inside a span the analyzer owns. Anything else (an
address outside every span, an instruction form the interpreter does not support) stops the run
with a named error instead of guessing. [AGENTS.md section 7](../AGENTS.md#7-current-execution-contract)
is the exact contract.

## 9. Threads: `src/rt/sched.c`

PSP games run several threads at once. The scheduler runs each guest thread on its own coroutine
(`src/rt/sr_coro.c`: Win32 fibers on Windows, `ucontext` on POSIX) and switches between them: the
highest-priority ready thread runs, and equal priorities take turns. Translated code calls
`SR_YIELD` at function entries and loop back-edges, which uses up the running thread's time slice
and lets the scheduler switch. Threads that wait on a semaphore, an event flag or a delay block
until the scheduler wakes them.

## 10. System calls: `src/rt/hle.c`

A game calls the PSP's system libraries through import stubs. Code generation turns each stub
into a call to `sr_syscall()` with the function's NID, the number that identifies it.
`sr_syscall()` at the end of `src/rt/hle.c` finds the NID's handler (`hle_find()`) and calls it.
Handlers read arguments 1-4 from `$a0`-`$a3` and 5-8 from `$t0`-`$t3`, and return the result in
`$v0`. Each handler is registered by name with `sr_hle_register()`, for example
`sr_hle_register(0x289d82fe, "sceDisplaySetFrameBuf", h_DisplaySetFrameBuf)`.

`hle.c` is very large; its opening comment lists the groups in order, and each group starts with
a `---- <name>` banner you can search for. An unknown NID is never answered with a made-up value:
it is reported by name and the run stops.

## 11. Drawing: `src/rt/ge.c`

The PSP's graphics chip, the GE, reads display lists from memory. The game hands one over with
`sceGeListEnQueue` (`h_GeListEnQueue` in `hle.c`), which calls `ge_run_list()` in `src/rt/ge.c`.
`ge.c` walks the list command by command, keeps the GE's state (matrices, lights, textures,
blending), decodes vertices, transforms and clips them, and hands each finished screen-space
primitive to the Vulkan backend in `src/rt/gpu_sdl3vk/ge_gpu.c` through the `GeGpuHooks` seam
(`gegpu_init()` installs them with `ge_set_gpu_hooks()`). The backend renders into GPU images it
keeps for each PSP framebuffer, so drawing never copies pixels back to guest memory unless
something on the PSP side reads them.

## 12. A frame on screen

When the game shows a finished frame it calls `sceDisplaySetFrameBuf` (`h_DisplaySetFrameBuf`
in `hle.c`). The frame is presented at once or at the next vblank (`sr_vblank_tick()`); either
way `display_present_active()` hands it to `gui_present()` in `src/rt/gui.c`. `gui_present()` paces output to the PSP's 59.94 Hz display rate, then asks the
Vulkan backend for the frame: `gegpu_present()` in `ge_gpu.c` finds the GPU image for that
framebuffer and `sdl3vk_present_image_ex()` in `src/rt/gpu_sdl3vk/sdl3vk.c` copies it to the
window's swapchain. A frame the CPU drew directly into memory takes the software path instead:
`gui.c` converts the pixels and `sdl3vk_present_rgba()` shows them.

That is the first frame on screen.

## Where to go next

- [`ARCHITECTURE.md`](ARCHITECTURE.md): the design, the `CpuState` ABI, the build system and the
  execution model in depth.
- [`DEBUGGING.md`](DEBUGGING.md#quick-recipes): find an HLE handler, run one native self-test,
  capture frames.
- [`SETUP.md`](SETUP.md) and [`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md): build it yourself.
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md) and [`../AGENTS.md`](../AGENTS.md): how to send a change.
