#!/usr/bin/env bash
#
# mayhem/build.sh -- build the fuzz harnesses and the project's test suite (runs in /mayhem as `mayhem`).
# Contract: sanitized library + one fuzz and one -standalone binary per harness in /mayhem; the test suite
# built with the project's normal flags under build/tests for mayhem/test.sh to run.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DWARF must be < 4 (Mayhem triage cannot read newer), so -gdwarf-3 is explicit.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# 1) The project's TEST suite, with its normal flags (a separate, clean build tree), so mayhem/test.sh
#    only runs it. Done first: the sanitized library below is a different tree and cannot disturb it.
cmake -S . -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$COVERAGE_FLAGS"
cmake --build build/tests -j"$MAYHEM_JOBS" --target superslm_tests sslm_inspect sslm_verify tok_verify

# 2) The library itself, with the sanitizers and DWARF < 4, so the fuzzed code is instrumented. Built twice:
#    with libFuzzer coverage instrumentation (for the fuzzers), and without (for the -standalone repros,
#    which link no libFuzzer runtime).
build_lib() {  # <build dir> <extra flags>
  cmake -S . -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=None -DBUILD_TESTING=OFF \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS $2" \
        -DCMAKE_CXX_FLAGS="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS $2"
  cmake --build "$1" -j"$MAYHEM_JOBS" --target superslm
}
build_lib build/fuzz -fsanitize=fuzzer-no-link
build_lib build/standalone ""

# 3) Each harness twice: against libFuzzer, and against the run-once standalone driver. LeakSanitizer is
#    switched off at build time (mayhem/lsan_off.cc); ASan and UBSan stay active.
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o build/standalone/standalone_main.o
$CXX -std=c++20 -O1 $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o build/fuzz/lsan_off.o

CXXFLAGS=(-std=c++20 -O1 $SANITIZER_FLAGS $DEBUG_FLAGS -I"$SRC/include" -I"$SRC/mayhem"
          "-DTOK_ARTIFACT_PATH=\"$SRC/tests/fixtures/qwen2.5-1.5b.tok.sslm\""
          "-DMODEL_FULL_PATH=\"$SRC/mayhem/model_full.sslm\"")
for t in fuzz_artifact fuzz_model fuzz_tokenizer fuzz_seq_ops; do
  $CXX "${CXXFLAGS[@]}" -fsanitize=fuzzer-no-link -c "mayhem/$t.cpp" -o "build/fuzz/$t.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "build/fuzz/$t.o" build/fuzz/lsan_off.o \
      build/fuzz/libsuperslm.a -lpthread -lm -o "/mayhem/$t"
  $CXX "${CXXFLAGS[@]}" -c "mayhem/$t.cpp" -o "build/standalone/$t.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "build/standalone/$t.o" build/standalone/standalone_main.o build/fuzz/lsan_off.o \
      build/standalone/libsuperslm.a -lpthread -lm -o "/mayhem/$t-standalone"
done
