#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -m)" != x86_64 || ! "$(uname -r)" =~ [Mm]icrosoft ]]; then
  echo "Run this from Ubuntu 24.04 x86_64 under WSL2." >&2
  exit 2
fi
if [[ "$(. /etc/os-release; echo "$ID:$VERSION_ID")" != ubuntu:24.04 ]]; then
  echo "This setup is pinned to Ubuntu 24.04; found $(. /etc/os-release; echo "$PRETTY_NAME")." >&2
  exit 2
fi
if [[ $EUID -eq 0 ]]; then
  echo "Run as your normal WSL user so D: workspace files keep your ownership." >&2
  exit 2
fi
if [[ ! -f /mnt/d/WSL/Ubuntu/ext4.vhdx ]]; then
  echo "Expected the Ubuntu WSL disk at D:\\WSL\\Ubuntu\\ext4.vhdx. Check its registered BasePath before continuing." >&2
  exit 2
fi

workspace="${MGS_FRAME_WORKSPACE:-/home/astro/MGSOneFrameDev}"
case "$workspace" in
  /home/astro/MGSOneFrameDev|/home/astro/MGSOneFrameDev/*) ;;
  *) echo "Keep project, build, sysroot and scratch files in the D-backed WSL ext4 workspace." >&2; exit 2 ;;
esac
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$project_root" in
  "$workspace"/*) ;;
  *) echo "Keep the project checkout inside $workspace." >&2; exit 2 ;;
esac

mkdir -p "$workspace"/build/{host,frame-smoke,frame-runtime} \
  "$workspace"/scratch "$workspace"/sysroot/frame "$workspace"/toolchain
export TMPDIR="$workspace/scratch"

sudo -v
sudo env TMPDIR="$TMPDIR" apt-get update
sudo env TMPDIR="$TMPDIR" apt-get install -y --no-install-recommends \
  binutils-aarch64-linux-gnu \
  clang-18 \
  cmake \
  g++-aarch64-linux-gnu \
  gcc-aarch64-linux-gnu \
  git \
  glslc \
  libvulkan-dev \
  libsdl2-dev \
  lld-18 \
  ninja-build \
  pkg-config \
  python3 \
  wayland-protocols

printf 'Workspace: %s\n' "$workspace"
printf 'D-backed WSL ext4 disk: '
df -h "$workspace" | awk 'NR == 2 { print $1 " (" $4 " free)" }'
for command in cmake ninja clang-18 clang++-18 ld.lld-18 \
  aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ \
  aarch64-linux-gnu-readelf glslc; do
  printf '%-30s' "$command"
  command -v "$command"
done
