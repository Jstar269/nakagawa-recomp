# GCC and Clang comparison

This snapshot records issue #317 on Windows 11 with MSYS2 UCRT64. The experiment used only
public synthetic fixtures. No game, private input, hardware or title route was used.

BASE_SHA: `0d2b9e72ad859e57ebbb5da2ef4aef7286b66b78`
EXPERIMENT_SHA: `90c9bd2943695f22f039e59252d4fd2b804b1144`
MERGED_MAIN_SHA: `a56a31e88ed6e3ce96c36de506524836239379af`

After the experiment, the branch merged the newer `origin/main`. Its only change on the
compiler-gate paths materialized the display-smoke vblank selection as a list. The focused
display-smoke and link-map regressions passed after that merge; the measurements below remain
qualified to `EXPERIMENT_SHA`.

## Command

The matrix ran through the machine RAM lock. Make built serially at `-j1`. UCRT64's `bin`
directory was first on `PATH`.

```powershell
python tools/compiler_compare.py --make C:/msys64/ucrt64/bin/mingw32-make.exe --compilers gcc,clang --linkers default,lld --repeats 3 --output build/c317-final
```

On 2026-11-07, GCC/default was re-run alone through the RAM lock after fixing the runtime
workload build roots. It used three repeats and a short fresh output path:

```powershell
python tools/compiler_compare.py --make C:/msys64/ucrt64/bin/mingw32-make.exe --compilers gcc --linkers default --repeats 3 --output build/c317-s1
```

## Toolchains and flags

| Component | Version and target |
| --- | --- |
| Python | 3.14.8 |
| GNU Make | 4.4.1, x86_64-w64-mingw32 |
| GCC | 16.2.0, x86_64-w64-mingw32 |
| Clang | 23.1.3, x86_64-w64-windows-gnu |
| Default linker | GNU ld 2.47.20260726 |
| lld | 23.1.3 |

Each row used `-O0`, `-fno-fast-math`, `-fno-associative-math` and
`-ffp-contract=off`. The lld rows added `-fuse-ld=lld`. GCC-only
`-ftrack-macro-expansion=0` was omitted from both compiler rows. The default Makefile compiler
remains GCC.

## Veto results

| Compiler/linker | Result | First failure |
| --- | --- | --- |
| GCC/default | MEASURED | All veto gates and runtime workloads passed |
| GCC/lld | FAILED_VETO | `cosim-mutants`: `ld.lld: error: undefined symbol: mpeg_malloc_avc_es_buf` |
| Clang/default | FAILED_VETO | `src/rt/videorec.c:708:14: error: variable 's_clock_deterministic' set but not used [-Werror,-Wunused-but-set-global]` |
| Clang/lld | FAILED_VETO | `src/rt/videorec.c:708:14: error: variable 's_clock_deterministic' set but not used [-Werror,-Wunused-but-set-global]` |

GCC/lld passed production-smoke after the verifier learned lld's `file.o:(section)` map entries.
A locked rerun of only the unmutated `25-scheduler` baseline, with
`-Wl,--error-limit=0` and a fresh build root, exposed the first linker error:
`ld.lld: error: undefined symbol: mpeg_malloc_avc_es_buf`; the remaining diagnostics name
other undefined `mpeg_*` functions. `hle-thread-selftest-build` links `$(HLE_SRCS)`, which
includes `src/rt/hle_mpeg.c`, but its explicit source list omits `src/rt/mpeg.c`, where those
functions are defined. The baseline therefore remains a `no-run` veto. This follow-up records
the cause without changing the build to make lld pass.

Both Clang rows passed the display-smoke VRAM capture check, then failed compiling
`videorec.c` because Clang treats the unused-but-set global as an error. This is a project and
toolchain incompatibility; no compiler-specific guest-semantics workaround was added.

An earlier GCC/default attempt used a longer output root and the nested mutation linker could
not resolve a split `build/comp` path. The final short-root invocation passed all veto gates.
The earlier Clang display-capture failure was also superseded: the merged `origin/main` fix
made the capture check pass before Clang reached the warning-as-error above.

## Measurements

Only GCC/default passed the correctness veto. The other rows have no performance measurements.
The GCC/default values below are from the 2026-11-07 isolated rerun; the original three veto
failures remain unchanged.

| GCC/default measurement | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: |
| Generated-C compile wall time (s) | 12.413 | 13.894 | 14.371 |
| Peak working set (bytes) | 45,961,216 | 55,775,232 | 58,728,448 |
| Link wall time (s) | 6.045 | 6.977 | 7.397 |
| Object bytes | 16,228,566 | 16,228,566 | 16,228,566 |
| Executable bytes | 14,728,236 | 14,728,236 | 14,728,236 |

The three GCC/default run-output hashes matched at
`a227c65f31a424817b8ac6ab545773bbcaff57fd219afd62934fc159e6834925`. The semantic-hash
comparison across all four rows is `INCOMPLETE`, because three rows were vetoed.

All nine GCC/default runtime workloads passed. Their elapsed wall times and local `SR_PERF`
summary paths are in `build/c317-s1/compiler_compare.json`; the generated `perf.json` files
are local build artifacts and are not committed. No cross-compiler runtime comparison is
available because neither Clang row passed the veto.

| Runtime workload | Status | Elapsed wall time (s) |
| --- | --- | ---: |
| production-smoke-gap | PASS | 180.381 |
| cosim | PASS | 32.410 |
| psmf-media | PASS | 67.338 |
| audio | PASS | 9.737 |
| audio-resampler | PASS | 8.733 |
| atrac3p | PASS | 10.579 |
| platform-ladder-gap | PASS | 89.964 |
| platform-ladder-sched | PASS | 85.553 |
| platform-ladder-fs | PASS | 85.919 |

The three platform-ladder workloads use fixed trees under `build/platform-ladder` and do not
honor the row's `BUILD_ROOT`. `run_perf_benchmarks.py` runs all workload rows sequentially, so
these fixed trees are shared across toolchain rows but are not built concurrently.

This one-row measurement does not establish a compiler winner or behavior equivalence. A
default-toolchain change still needs repeatable results for both compilers and linkers, plus
the runtime and residency evidence requested by #317. AOT/interpreter residency, sanitizer
comparison, hosted CI, private-title acceptance and hardware acceptance are `NOT_RUN`.
