#!/usr/bin/env bash
# ci/cross-build.sh
#
# M0 (docs/implementation-plan.md): cross-compile and link the
# stressfs binary for real m68k-amigaos, inside the pinned
# amigadev/crosstools container, driven through the CMake toolchain
# file. This is exactly the docker invocation documented in the plan's
# section 2 ("Build tooling") wrapped in a script so local and CI runs
# are identical, following the pattern of
# unnamed-fs/ci/volamos-m68k-tests.sh.
#
# Usage:
#   ci/cross-build.sh
#
# Produces: build-m68k/stressfs (a real m68k hunk executable).
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="docker.io/amigadev/crosstools@sha256:3120749ec13b18a8f52e6073a484463f46751d69d8a67eaae10397958c05954d"

docker run --rm -v "$ROOT":/work -w /work "$IMAGE" sh -c '
  cmake -S amiga -B build-m68k \
        -DCMAKE_TOOLCHAIN_FILE=../cmake/m68k-amigaos.toolchain.cmake \
  && cmake --build build-m68k
'

echo "== cross-build OK: $ROOT/build-m68k/stressfs =="
file "$ROOT/build-m68k/stressfs" 2>/dev/null || true
ls -la "$ROOT/build-m68k/stressfs"
