#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="${MGS_FRAME_WORKSPACE:-/home/astro/MGSOneFrameDev}"
sysroot="${MGS_FRAME_SYSROOT:-$workspace/sysroot/frame}"
jobs="${BUILD_JOBS:-2}"

case "$project_root" in
  /home/astro/MGSOneFrameDev/*) ;;
  *) echo "Keep the project checkout in the D-backed WSL workspace." >&2; exit 2 ;;
esac
case "$workspace" in
  /home/astro/MGSOneFrameDev|/home/astro/MGSOneFrameDev/*) ;;
  *) echo "Keep build, sysroot and scratch files in the D-backed WSL workspace." >&2; exit 2 ;;
esac

mkdir -p "$workspace/build/host" "$workspace/build/frame-smoke" \
  "$workspace/build/frame-runtime" "$workspace/scratch"
export TMPDIR="$workspace/scratch"

configure_frame() {
  local build_dir="$1"
  local use_stub="$2"
  cmake -S "$project_root/engine" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$project_root/engine/cmake/toolchains/linux-aarch64-frame-wsl.cmake" \
    -DMGS_FRAME_SYSROOT="$sysroot" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMGS_STEAM_FRAME=ON \
    -DMGS_STEAM_FRAME_VR=ON \
    -DPS1RECOMP_USE_STUB="$use_stub" \
    -DPS1RECOMP_BUILD_TESTS=OFF \
    -DPS1RECOMP_BUILD_INTERFACE=OFF \
    -DBUILD_SDK_TESTS=OFF \
    -DPRESENTATION_BACKEND=wayland \
    -DPS1RECOMP_VULKAN_HEADERS_ROOT="$sysroot/usr/include" \
    -DPS1RECOMP_VULKAN_LIBRARY="$sysroot/usr/lib/libvulkan.so" \
    -DPS1RECOMP_GLSLC_EXECUTABLE="$(command -v glslc)" \
    -DMGS_ABI_LIBRARY_DIR="$sysroot/usr/lib"
}

command="${1:-}"
case "$command" in
  abi-smoke)
    exec bash "$project_root/scripts/windows_dev/abi_smoke.sh"
    ;;
  host-tools)
    cmake -S "$project_root/engine" -B "$workspace/build/host" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPS1RECOMP_USE_STUB=ON \
      -DPS1RECOMP_BUILD_TESTS=OFF \
      -DPS1RECOMP_BUILD_INTERFACE=OFF \
      -DMGS_STEAM_FRAME=OFF \
      -DMGS_STEAM_FRAME_VR=OFF
    cmake --build "$workspace/build/host" \
      --target ps1Analyzer ps1Recomp --parallel "$jobs"
    file "$workspace/build/host/ps1Analyzer/ps1Analyzer" \
      "$workspace/build/host/ps1Recomp/ps1Recomp"
    ;;
  frame-smoke)
    [[ -d "$sysroot/usr/include" ]] || {
      echo "Steam Frame sysroot is missing at $sysroot." >&2; exit 2;
    }
    configure_frame "$workspace/build/frame-smoke" ON
    cmake --build "$workspace/build/frame-smoke" \
      --target ps1Runtime --parallel "$jobs"
    python3 "$project_root/scripts/windows_dev/verify_frame_elf.py" \
      "$workspace/build/frame-smoke/ps1Runtime/ps1Runtime" --sysroot "$sysroot"
    ;;
  frame-runtime)
    [[ -f "$project_root/engine/ps1Runtime/src/recompiled_out.cpp" ]] || {
      echo "Generated MGS translations are missing from this checkout." >&2
      exit 2
    }
    [[ -f "$project_root/engine/ps1Runtime/src/recompiled_out.generated.h" ]] || {
      echo "The generated MGS translation manifest is missing." >&2
      exit 2
    }
    configure_frame "$workspace/build/frame-runtime" OFF
    cmake --build "$workspace/build/frame-runtime" \
      --target ps1Runtime --parallel "$jobs"
    python3 "$project_root/scripts/windows_dev/verify_frame_elf.py" \
      "$workspace/build/frame-runtime/ps1Runtime/ps1Runtime" --sysroot "$sysroot"
    ;;
  *)
    cat >&2 <<'USAGE'
Usage: scripts/build_windows_frame.sh {abi-smoke|host-tools|frame-smoke|frame-runtime}

  abi-smoke     Compile and ABI-check a small AArch64 C++ program.
  host-tools    Build native x86_64 ps1Analyzer and ps1Recomp generators.
  frame-smoke   Cross-build the source-only ARM64 runtime against the Frame sysroot.
  frame-runtime Cross-build generated MGS translation shards already in this checkout.

Set BUILD_JOBS=2 to limit compiler parallelism, or MGS_FRAME_WORKSPACE and
MGS_FRAME_SYSROOT to use the existing D-backed WSL workspace.
USAGE
    exit 2
    ;;
esac
