# GCC and Clang comparison

This snapshot records issue #317 on Windows 11 with MSYS2 UCRT64. The experiment used only
public synthetic fixtures. No game, private input, hardware or title route was used.

BASE_SHA: `0d2b9e72ad859e57ebbb5da2ef4aef7286b66b78`
EXPERIMENT_SHA: `90c9bd2943695f22f039e59252d4fd2b804b1144`

## Command

The matrix ran through the machine RAM lock. Make built serially at `-j1`. UCRT64's `bin`
directory was first on `PATH`.

```powershell
python tools/compiler_compare.py --make C:/msys64/ucrt64/bin/mingw32-make.exe --compilers gcc,clang --linkers default,lld --repeats 3 --output build/c317-final
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
| GCC/lld | FAILED_VETO | `cosim-mutants`: `ld.lld: error: too many errors emitted, stopping now (use --error-limit=0 to see all errors)` |
| Clang/default | FAILED_VETO | `src/rt/videorec.c:708:14: error: variable 's_clock_deterministic' set but not used [-Werror,-Wunused-but-set-global]` |
| Clang/lld | FAILED_VETO | `src/rt/videorec.c:708:14: error: variable 's_clock_deterministic' set but not used [-Werror,-Wunused-but-set-global]` |

GCC/lld passed production-smoke after the verifier learned lld's `file.o:(section)` map entries.
It stopped when the unmutated scheduler baseline could not link in mutation lane
`25-scheduler`: `COSIM_MUTATION FAIL: AOT boundary baseline verdict=no-run`. Make reported the
failed recipes at `mk/selftests.mk:499`, `mk/selftests.mk:513` and `mk/cosim_snapshot.mk:121`.
The lld diagnostic did not include a source file and line.

Both Clang rows passed the display-smoke VRAM capture check, then failed compiling
`videorec.c` because Clang treats the unused-but-set global as an error. This is a project and
toolchain incompatibility; no compiler-specific guest-semantics workaround was added.

An earlier GCC/default attempt used a longer output root and the nested mutation linker could
not resolve a split `build/comp` path. The final short-root invocation passed all veto gates.
The earlier Clang display-capture failure was also superseded: the merged `origin/main` fix
made the capture check pass before Clang reached the warning-as-error above.

## Measurements

Only GCC/default passed the correctness veto. The other rows have no performance measurements.

| GCC/default measurement | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: |
| Generated-C compile wall time (s) | 11.842 | 12.052 | 13.519 |
| Peak working set (bytes) | 53,600,256 | 54,853,632 | 55,615,488 |
| Link wall time (s) | 5.859 | 6.053 | 6.132 |
| Object bytes | 16,128,567 | 16,128,567 | 16,128,567 |
| Executable bytes | 14,656,192 | 14,656,192 | 14,656,192 |

The three GCC/default run-output hashes matched at
`a227c65f31a424817b8ac6ab545773bbcaff57fd219afd62934fc159e6834925`. The semantic-hash
comparison across all four rows is `INCOMPLETE`, because three rows were vetoed.

All nine GCC/default runtime workloads passed. Their elapsed wall times and local `SR_PERF`
summary paths are in `build/c317-final/compiler_compare.json`; the generated `perf.json` files
are local build artifacts and are not committed. No cross-compiler runtime comparison is
available because neither Clang row passed the veto.

| Runtime workload | Status | Elapsed wall time (s) |
| --- | --- | ---: |
| production-smoke-gap | PASS | 171.755 |
| cosim | PASS | 27.816 |
| psmf-media | PASS | 59.989 |
| audio | PASS | 8.045 |
| audio-resampler | PASS | 7.516 |
| atrac3p | PASS | 8.415 |
| platform-ladder-gap | PASS | 76.221 |
| platform-ladder-sched | PASS | 76.237 |
| platform-ladder-fs | PASS | 75.729 |

This one-row measurement does not establish a compiler winner or behavior equivalence. A
default-toolchain change still needs repeatable results for both compilers and linkers, plus
the runtime and residency evidence requested by #317. AOT/interpreter residency, sanitizer
comparison, hosted CI, private-title acceptance and hardware acceptance are `NOT_RUN`.
