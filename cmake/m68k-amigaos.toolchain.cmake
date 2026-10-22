# cmake/m68k-amigaos.toolchain.cmake
#
# CMake cross-toolchain file for m68k-amigaos (see
# docs/implementation-plan.md section 2, "Build tooling", and milestone
# M0). Adapted from unnamed-fs/cmake/m68k-amigaos.toolchain.cmake. Use it
# with:
#
#   cmake -S amiga -B build-m68k -DCMAKE_TOOLCHAIN_FILE=../cmake/m68k-amigaos.toolchain.cmake
#
# This toolchain is NOT expected to exist on the host. Compilation must
# happen inside the pinned Docker image:
#
#   docker.io/amigadev/crosstools:m68k-amigaos-gcc10
#   pinned by digest sha256:3120749ec13b18a8f52e6073a484463f46751d69d8a67eaae10397958c05954d
#
# NOTE on the image tag: despite the "gcc10" tag name, this image
# actually ships `m68k-amigaos-gcc (AmigaDev) 16.2.1b` — i.e. GCC 16.2,
# matching the "AmigaPorts/bebbo GCC 16.2" reference elsewhere in the
# plan. The tag name is a historical/naming artifact of the image, not a
# statement about the compiler version actually installed; don't be
# fooled by it when reading CI logs or Dockerfiles that reference this
# image.
#
# The image's PATH already includes /opt/m68k-amigaos/bin and it sets
# CMAKE_PREFIX_PATH=/opt/m68k-amigaos:/opt/m68k-amigaos/usr, so the
# compiler and binutils below are found without an explicit prefix path
# here. There is no vlink in this image: linking goes through
# m68k-amigaos-gcc / m68k-amigaos-ld (GNU binutils), not vlink.
#
# Hosted vs. freestanding (the one deliberate difference from
# unnamed-fs's copy of this file): unnamed-fs's core is a freestanding
# library and its toolchain file sets CMAKE_TRY_COMPILE_TARGET_TYPE to
# STATIC_LIBRARY because CMake's internal "can I link a hosted hello
# world" try-compile check cannot succeed for a target with no libc/
# startup assumptions at all. stressfs is different: it is a genuine
# hosted CLI binary, linked against dos.library/exec.library through the
# toolchain's own -noixemul startup objects and libnix, exactly the way
# a normal m68k-amigaos-gcc command-line program is built
# (`m68k-amigaos-gcc -m68020-60 -Os -noixemul foo.c -o foo`). That links
# a complete, runnable (under volamos/UAE/real hardware) executable, so
# CMake's own EXECUTABLE-flavoured try-compile check was tried first in
# the pinned container and it links cleanly — no STATIC_LIBRARY fallback
# was needed. CMAKE_TRY_COMPILE_TARGET_TYPE is therefore left at its
# default (EXECUTABLE). If a future CMake/container upgrade makes that
# try-compile flaky, switch it to STATIC_LIBRARY here and update this
# comment — don't silently carry a workaround nobody documented.

# No AmigaOS-specific CMAKE_SYSTEM_NAME exists upstream; "Generic" is the
# conventional choice here. It disables CMake's normal hosted-platform
# assumptions (searching /usr/lib, /usr/include, etc.) without which the
# cross find_* calls below would wander into the host's own filesystem.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

set(CMAKE_C_COMPILER m68k-amigaos-gcc)
set(CMAKE_AR m68k-amigaos-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB m68k-amigaos-ranlib CACHE FILEPATH "Ranlib")

# Smoke-tested flags (see docs/implementation-plan.md M0 notes):
#   m68k-amigaos-gcc -m68020-60 -Os -noixemul -Wall -Wextra -c foo.c -o foo.o
# -Werror -fno-common are added here to match the host build's
# strictness. Unlike unnamed-fs's freestanding core, stressfs does NOT
# add -ffreestanding here: it is a hosted binary and wants the
# toolchain's normal -noixemul/libnix C runtime underneath it.
set(CMAKE_C_FLAGS_INIT "-m68020-60 -Os -noixemul -Wall -Wextra -Werror -fno-common")

# Don't let CMake go looking for host-side libraries/includes/programs
# under the cross sysroot search paths; only ever use what the
# toolchain's own prefix provides.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
