# Native x86_64 WSL tools cross-compile Linux binaries for Steam Frame ARM64.
# Keep host programs (cmake, glslc, generators) outside the target sysroot.

if(NOT DEFINED MGS_FRAME_SYSROOT)
    if(DEFINED ENV{MGS_FRAME_SYSROOT})
        set(_frame_sysroot_default "$ENV{MGS_FRAME_SYSROOT}")
    else()
        set(_frame_sysroot_default
            "/home/astro/MGSOneFrameDev/sysroot/frame")
    endif()
    set(MGS_FRAME_SYSROOT "${_frame_sysroot_default}" CACHE PATH
        "Relocated Steam Frame Linux sysroot stored on D:")
endif()

if(NOT MGS_FRAME_SYSROOT MATCHES "^/home/astro/MGSOneFrameDev/")
    message(FATAL_ERROR
        "The Steam Frame sysroot must be in the D-backed WSL ext4 workspace; got '${MGS_FRAME_SYSROOT}'")
endif()
if(NOT IS_DIRECTORY "${MGS_FRAME_SYSROOT}/usr/include")
    message(FATAL_ERROR
        "Frame sysroot headers are missing at ${MGS_FRAME_SYSROOT}/usr/include")
endif()
if(NOT IS_DIRECTORY "${MGS_FRAME_SYSROOT}/usr/lib")
    message(FATAL_ERROR
        "Frame ARM64 libraries are missing at ${MGS_FRAME_SYSROOT}/usr/lib")
endif()
if(NOT EXISTS
        "${MGS_FRAME_SYSROOT}/usr/lib/gcc/aarch64-unknown-linux-gnu/15.2.1/crtbegin.o")
    message(FATAL_ERROR
        "Frame GCC startup files are missing from the sysroot")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER clang-18)
set(CMAKE_CXX_COMPILER clang++-18)
set(CMAKE_C_COMPILER_TARGET aarch64-unknown-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)
set(CMAKE_SYSROOT "${MGS_FRAME_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${MGS_FRAME_SYSROOT}")

# Clang and LLD run natively on x86_64. The Frame GCC 15.2.1 installation in
# the sysroot supplies target headers, startup objects and runtime libraries.
# No ARM64 compiler executable is run on the host.
set(CMAKE_C_FLAGS_INIT
    "--gcc-toolchain=${MGS_FRAME_SYSROOT}/usr -fuse-ld=lld")
set(CMAKE_CXX_FLAGS_INIT
    "--gcc-toolchain=${MGS_FRAME_SYSROOT}/usr -fuse-ld=lld")
set(_frame_link_search
    "-Wl,-rpath-link,${MGS_FRAME_SYSROOT}/usr/lib "
    "-Wl,-rpath-link,${MGS_FRAME_SYSROOT}/lib")
string(JOIN " " _frame_link_search ${_frame_link_search})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld ${_frame_link_search}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld ${_frame_link_search}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld ${_frame_link_search}")

# CMake must find host build tools such as glslc, but every target dependency
# must resolve inside the Frame sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CMake's compiler probe shouldn't need to link a target executable before
# project-specific target libraries have been located.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MGS_ABI_LIBRARY_DIR
    "${MGS_FRAME_SYSROOT}/usr/lib" CACHE PATH
    "Frame ARM64 runtime libraries used for the final link")
