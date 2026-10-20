#!/bin/bash
set -euo pipefail

: "${CC:?ClusterFuzzLite must provide CC}"
: "${CXX:?ClusterFuzzLite must provide CXX}"
: "${CFLAGS:?ClusterFuzzLite must provide CFLAGS}"
: "${CXXFLAGS:?ClusterFuzzLite must provide CXXFLAGS}"
: "${LIB_FUZZING_ENGINE:?ClusterFuzzLite must provide LIB_FUZZING_ENGINE}"

mkdir -p "$WORK/obj" "$OUT"

# The OSS-Fuzz flags are intentionally word-split into compiler arguments.
# shellcheck disable=SC2086
$CC $CFLAGS -std=c11 -Isrc/core -c src/core/nk_json.c -o "$WORK/obj/nk_json.o"
$CC $CFLAGS -std=c11 -Isrc/core -c tests/fuzzers/test_nk_json_fuzzer.c -o "$WORK/obj/nk_json_fuzzer.o"
$CXX $CXXFLAGS "$WORK/obj/nk_json.o" "$WORK/obj/nk_json_fuzzer.o" -o "$OUT/nk_json_fuzzer" $LIB_FUZZING_ENGINE

printf '[libfuzzer]\nmax_len = 4096\n' > "$OUT/nk_json_fuzzer.options"

python3 - "$OUT/nk_json_fuzzer_seed_corpus.zip" <<'PY'
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

seeds = (
    ("basic.json", b'{"items":[null,true,false,0,-1.25,"text"]}'),
    ("escape.json", b'"\\u0061"'),
    ("depth-16.json", b"[" * 16 + b"0" + b"]" * 16),
    ("depth-17.json", b"[" * 17 + b"0" + b"]" * 17),
)

with ZipFile(sys.argv[1], "w", compression=ZIP_DEFLATED) as corpus:
    for name, data in seeds:
        item = ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
        item.compress_type = ZIP_DEFLATED
        corpus.writestr(item, data)
PY
