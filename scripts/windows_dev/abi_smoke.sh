#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
workspace="${MGS_FRAME_WORKSPACE:-/home/astro/MGSOneFrameDev}"
sysroot="${MGS_FRAME_SYSROOT:-$workspace/sysroot/frame}"
probe="$workspace/scratch/frame_abi_probe"

case "$project_root" in
  /home/astro/MGSOneFrameDev/*) ;;
  *) echo "Keep the project checkout in the D-backed WSL workspace." >&2; exit 2 ;;
esac
case "$workspace" in
  /home/astro/MGSOneFrameDev|/home/astro/MGSOneFrameDev/*) ;;
  *) echo "Keep the sysroot and scratch files in the D-backed WSL workspace." >&2; exit 2 ;;
esac
[[ -d "$sysroot/usr/include" ]] || {
  echo "Steam Frame sysroot is missing at $sysroot." >&2; exit 2;
}
mkdir -p "$workspace/scratch"
export TMPDIR="$workspace/scratch"
trap 'rm -f "$probe"' EXIT INT TERM

host_compiler="$(readlink -f "$(command -v clang++-18)")"
host_linker="$(readlink -f "$(command -v ld.lld-18)")"
host_info="$(file "$host_compiler" "$host_linker")"
printf '%s\n' "$host_info"
grep -q 'x86-64' <<<"$host_info"

clang++-18 --target=aarch64-unknown-linux-gnu \
  --sysroot="$sysroot" \
  --gcc-toolchain="$sysroot/usr" \
  -fuse-ld=lld -std=c++20 -O1 \
  "$project_root/scripts/windows_dev/abi_probe.cpp" \
  -Wl,-rpath-link,"$sysroot/usr/lib" \
  -Wl,-rpath-link,"$sysroot/lib" \
  -o "$probe"

file "$probe"
readelf -h "$probe" | grep -E 'Class:|Machine:'
python3 "$project_root/scripts/windows_dev/verify_frame_elf.py" \
  "$probe" --sysroot "$sysroot"
