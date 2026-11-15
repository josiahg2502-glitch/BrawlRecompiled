# Cross-compile for 64-bit ARM Linux (aarch64) from an x86_64 Linux PC with clang and lld.
#
# Needs an aarch64 sysroot holding the development packages the build links against. One way, with podman and
# qemu-user-static (arm64 emulation) installed:
#   podman run --platform linux/arm64 --name sysroot docker.io/library/ubuntu:24.04 bash -c 'apt-get update &&
#     apt-get install -y --no-install-recommends libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev
#     libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev libxtst-dev
#     libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgles-dev
#     libgl-dev libvulkan-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev
#     libdbus-1-dev libusb-1.0-0-dev zlib1g-dev'
#   mkdir sysroot && podman export sysroot | tar -x -C sysroot --anchored --exclude='dev/*' --exclude='proc/*' --exclude='sys/*'
#   then turn the sysroot's absolute symlinks into relative ones (usr/lib/aarch64-linux-gnu/libm.so -> /lib/...).
#   (--anchored matters: an unanchored 'sys/*' also empties usr/include/aarch64-linux-gnu/sys.)
#
# Configure:
#   cmake -S runtime -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=runtime/cmake/toolchains/aarch64-linux-gnu.cmake -DMKW_AARCH64_SYSROOT=/path/to/sysroot \
#     -DAURORA_SDL3_PROVIDER=vendor -DAURORA_DAWN_PROVIDER=package
#
# The result needs the sysroot's glibc (2.38 for Ubuntu 24.04) or newer on the device, and a Vulkan driver.
# Save states are not available on ARM (save_states.cpp).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(NOT MKW_AARCH64_SYSROOT AND DEFINED ENV{MKW_AARCH64_SYSROOT})
    set(MKW_AARCH64_SYSROOT "$ENV{MKW_AARCH64_SYSROOT}")
endif()
if(NOT MKW_AARCH64_SYSROOT)
    message(FATAL_ERROR "Set MKW_AARCH64_SYSROOT to an aarch64 sysroot (see the comment at the top of this file)")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MKW_AARCH64_SYSROOT)
set(CMAKE_SYSROOT "${MKW_AARCH64_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)
# Also in the default flags: steps that run the compiler themselves (libpng's pnglibconf preprocessing) pass
# CMAKE_C_FLAGS but not the target or sysroot.
set(CMAKE_C_FLAGS_INIT "--target=aarch64-linux-gnu --sysroot=${CMAKE_SYSROOT}")
set(CMAKE_CXX_FLAGS_INIT "--target=aarch64-linux-gnu --sysroot=${CMAKE_SYSROOT}")
set(CMAKE_ASM_FLAGS_INIT "--target=aarch64-linux-gnu --sysroot=${CMAKE_SYSROOT}")
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
