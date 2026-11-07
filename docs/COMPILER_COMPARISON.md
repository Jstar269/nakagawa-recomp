# GCC and Clang comparison

This snapshot records issue #317 on one Windows machine using MSYS2 UCRT64. The source
revision was b55f6cd304a17ea06c414b1c462df7fbef26b1f2. The final, relative-path matrix was
one locked invocation after three earlier attempts stopped before accepted measurements. It
requested three measurement repetitions for each passing compiler/linker row.

## Command

The harness was called through the machine RAM lock; its Make invocations use `-j1`.

```powershell
python tools/compiler_compare.py --make C:/msys64/ucrt64/bin/mingw32-make.exe --compilers gcc,clang --linkers default,lld --repeats 3 --output build/compiler-compare-317-run4-20261106
```

## Toolchains

| Component | Version and target |
| --- | --- |
| Python | 3.14.8 |
| GNU Make | 4.4.1, x86_64-w64-mingw32 |
| GCC | gcc.exe (Rev6, Built by MSYS2 project) 16.2.0; x86_64-w64-mingw32 |
| Clang | clang 23.1.3, MSYS2 MINGW-packages commit 067886dba977a6b4c97dfedfb25286b5eda55620; x86_64-w64-windows-gnu, posix |
| Default linker | GNU ld (GNU Binutils) 2.47.20260726 |
| lld | LLD 23.1.3, MSYS2 MINGW-packages commit 067886dba977a6b4c97dfedfb25286b5eda55620 |

Both compilers and linkers resolved inside the same MSYS2 UCRT64 installation. The JSON
report records their full version output, paths, effective flags, and compiler argument
vectors.

## Veto results

| Compiler | Linker | Result | First failed gate |
| --- | --- | --- | --- |
| GCC | default | FAILED_VETO | cosim-mutants |
| GCC | lld | FAILED_VETO | production-smoke |
| Clang | default | FAILED_VETO | display-smoke |
| Clang | lld | FAILED_VETO | production-smoke |

The Clang/default display-smoke veto stopped when the VRAM capture check failed:

```text
DISPLAY_SMOKE_VRAMDUMP status=FAIL: requested vblank 3 produced no VRAM capture
```

Both lld rows reached production-smoke verification, which rejected the link map because
it omitted the expected generated and runtime objects. GCC/default reached cosim-mutants;
five FP conversion mutants were reported no-run; the first linker diagnostic was
`ld.exe: cannot find build/comp: No such file or directory`.
Those are failed vetoes, not compiler passes.
Each row stopped at its first failure; later gates, including native-core-tests and the
VFPU/exception checks, are NOT_RUN for that row rather than passes.

## Measurements

No performance measurement ran. Every row failed its veto, so the harness reported no
compile time, peak memory, link time, object size, executable size, or semantic hash. The
semantic-hash comparison is NOT_COMPARABLE. The configured repetition count was three;
veto failures correctly prevented those repetitions from starting.

| Compiler/linker | Veto | Generated-C compile (s), min/median | Peak working set (bytes), min/median | Link time (s), min/median | Object/executable size (bytes), min/median | Run-output SHA-256 |
| --- | --- | --- | --- | --- | --- | --- |
| GCC/default | FAILED_VETO | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |
| GCC/lld | FAILED_VETO | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |
| Clang/default | FAILED_VETO | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |
| Clang/lld | FAILED_VETO | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |

The harness keeps the linker factor separate. The lld row adds `-fuse-ld=lld`
and checks that every measured corpus link uses it. GCC-only
`-ftrack-macro-expansion=0` is omitted from both comparison rows because it
only changes macro tracking and would bias compile-time and memory measurements. Both rows
use `-O0` and explicitly disable fast-math, reassociation, and FP contraction.
The default Makefile compiler remains GCC; this experiment does not change it.

This single-machine veto result does not establish a winner. Changing the default would
require stable semantic output and passing source-owned gates for each supported toolchain,
plus repeatable measurements across the supported build matrix showing a material gain
without regressions. Hosted CI and maintainer review were NOT_RUN.
