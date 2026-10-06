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
FLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -Isrc"
CORE="src/core/strutil.cpp src/core/status_log.cpp \
      src/audio/transport.cpp src/audio/mixer.cpp src/audio/audio_engine.cpp \
      src/audio/sample.cpp src/audio/drum_synth.cpp src/audio/wav.cpp src/audio/adpcm.cpp \
      src/project/project.cpp src/project/project_io.cpp src/project/session.cpp"

$CXX $FLAGS tests/host/test_main.cpp $CORE -lm -o tests/host/out/core_tests
./tests/host/out/core_tests

if [ "$1" = "preview" ]; then
    mkdir -p tests/host/out/preview
    $CXX $FLAGS tests/host/ui_preview.cpp $CORE src/ui/*.cpp -lm -o tests/host/out/ui_preview
    ./tests/host/out/ui_preview tests/host/out/preview
fi
