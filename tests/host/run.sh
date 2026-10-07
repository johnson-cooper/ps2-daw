#!/bin/sh
# Host-side checks for the platform-independent code (needs a desktop g++).
# The PS2 build itself is always just: ps2build build
#
#   tests/host/run.sh            core unit tests (ASan + UBSan)
#   tests/host/run.sh preview    also render UI screenshots to tests/host/out/preview/
set -e
cd "$(dirname "$0")/../.."
mkdir -p tests/host/out
CXX="${CXX:-g++}"
# ASan/UBSan where the toolchain provides them (Linux, macOS, MSYS2 clang64);
# MinGW g++ has no sanitizer runtimes, so fall back to assertions-only there.
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
echo 'int main(){return 0;}' > tests/host/out/san_probe.cpp
if ! $CXX $SAN tests/host/out/san_probe.cpp -o tests/host/out/san_probe >/dev/null 2>&1; then
    echo "note: sanitizers unavailable with $CXX; running without ASan/UBSan"
    SAN="-D_GLIBCXX_ASSERTIONS"
fi
FLAGS="-std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN -Isrc"
CORE="src/core/strutil.cpp src/core/status_log.cpp \
      src/audio/transport.cpp src/audio/mixer.cpp src/audio/audio_engine.cpp \
      src/audio/sample.cpp src/audio/drum_synth.cpp src/audio/wav.cpp src/audio/adpcm.cpp src/audio/sample_ref.cpp src/audio/sample_import.cpp \
      src/project/project.cpp src/project/project_io.cpp src/project/session.cpp src/project/sample_library.cpp src/project/slot_store.cpp"

$CXX $FLAGS tests/host/test_main.cpp tests/host/test_samples.cpp tests/host/test_workflow.cpp $CORE -lm -o tests/host/out/core_tests
./tests/host/out/core_tests

if [ "$1" = "preview" ]; then
    mkdir -p tests/host/out/preview
    $CXX $FLAGS tests/host/ui_preview.cpp $CORE src/ui/*.cpp -lm -o tests/host/out/ui_preview
    ./tests/host/out/ui_preview tests/host/out/preview
fi
