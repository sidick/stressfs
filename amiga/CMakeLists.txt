# amiga/CMakeLists.txt
#
# Build entry point for the stressfs Amiga binary and its portable
# lib/ helper code (see docs/implementation-plan.md section 2, "Build
# tooling", and milestone M0 in section 3). Mirrors
# unnamed-fs/core/CMakeLists.txt's structure, adapted for a hosted
# binary rather than a freestanding library:
#
#   cmake -S amiga -B build-host [-DSTRESSFS_SANITIZERS=ON]
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Cross build for m68k-amigaos (inside the pinned amigadev/crosstools
# container; see cmake/m68k-amigaos.toolchain.cmake for details), via
# ci/cross-build.sh, or by hand:
#
#   cmake -S amiga -B build-m68k \
#         -DCMAKE_TOOLCHAIN_FILE=../cmake/m68k-amigaos.toolchain.cmake
#   cmake --build build-m68k

cmake_minimum_required(VERSION 3.25)
project(stressfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ASan/UBSan only make sense for the host build: there is no sanitizer
# runtime for the bare m68k-amigaos target, so this is forced off when
# cross-compiling regardless of the cache value (mirrors unnamed-fs's
# PHX_SANITIZERS logic).
option(STRESSFS_SANITIZERS "Build the host lib/tests with ASan+UBSan" ON)
if(CMAKE_CROSSCOMPILING)
  set(STRESSFS_SANITIZERS OFF)
endif()

# ---------------------------------------------------------------------------
# lib/: the portable helper library (packet sending, model, seeded
# generator, report writer, quirks parser as later milestones add them).
# Only the *portable* pieces live here and only they are listed as
# sources below — Amiga-API-touching sources (DoPkt, console I/O) belong
# in src/ and are excluded from this target, which is what lets lib/
# build and unit-test on the host with ASan/UBSan. At M0 that's just the
# PRNG.
# ---------------------------------------------------------------------------
add_library(stressfs-lib STATIC
  lib/prng.c
  lib/conform.c
)

target_include_directories(stressfs-lib PUBLIC include)
target_compile_options(stressfs-lib PRIVATE -Wall -Wextra -Werror)

if(STRESSFS_SANITIZERS)
  target_compile_options(stressfs-lib PRIVATE -fsanitize=address,undefined -g)
  target_link_options(stressfs-lib PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# src/: the stressfs binary itself. Amiga-only — touches dos.library/
# exec.library directly — so it is only built when cross-compiling for
# m68k-amigaos. There is nothing to run it against on the host build.
# ---------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING)
  add_executable(stressfs
    src/main.c
    src/packet.c
    src/handler_id.c
    src/conform.c
  )
  target_link_libraries(stressfs PRIVATE stressfs-lib)
endif()

# ---------------------------------------------------------------------------
# tests/: host-runnable C unit tests of the portable lib/ pieces. Not
# built when cross-compiling — there is no host to run an m68k binary
# on here (M0's volamos/Copperline runs are separate, scripted
# executions of the real cross-built stressfs binary, not a ctest
# target).
# ---------------------------------------------------------------------------
if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()

  add_executable(prng_test tests/prng_test.c)
  target_link_libraries(prng_test PRIVATE stressfs-lib)
  target_compile_options(prng_test PRIVATE -Wall -Wextra -Werror)

  if(STRESSFS_SANITIZERS)
    target_compile_options(prng_test PRIVATE -fsanitize=address,undefined -g)
    target_link_options(prng_test PRIVATE -fsanitize=address,undefined)
  endif()

  add_test(NAME prng_test COMMAND prng_test)

  add_executable(conform_test tests/conform_test.c)
  target_link_libraries(conform_test PRIVATE stressfs-lib)
  target_compile_options(conform_test PRIVATE -Wall -Wextra -Werror)

  if(STRESSFS_SANITIZERS)
    target_compile_options(conform_test PRIVATE -fsanitize=address,undefined -g)
    target_link_options(conform_test PRIVATE -fsanitize=address,undefined)
  endif()

  add_test(NAME conform_test COMMAND conform_test)
endif()
