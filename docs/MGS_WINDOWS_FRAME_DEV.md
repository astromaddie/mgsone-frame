# Windows WSL Development

I use Ubuntu 24.04 WSL2 on an x86_64 Windows PC as the build host. The Ubuntu
VHDX is at `D:\WSL\Ubuntu\ext4.vhdx`. `D:\MGSOneFrameDev` is the Windows
entry point; the project checkout, build output, temporary files, toolchain
files and Frame sysroot live at `/home/astro/MGSOneFrameDev` inside that
D-backed ext4 VHDX. Run `D:\MGSOneFrameDev\Open-Dev-Shell.ps1` to open the
project shell. This keeps compiler and CMake file work on ext4 while all of the
storage stays on D:.

Clang, LLD and the MGS analyzers run as native x86_64 programs. Clang targets
`aarch64-unknown-linux-gnu` and links with Frame GCC 15.2.1 startup/runtime
files from the sysroot. The setup does not run an ARM64 compiler through QEMU.

## Install the Host Tools

From Ubuntu 24.04 WSL2, run:

```sh
bash scripts/windows_dev/bootstrap_wsl.sh
```

The script installs CMake, Ninja, native Clang and LLD, the x86_64-hosted
AArch64 GCC cross package, `glslc`, SDL2 and Vulkan development files, and the
Wayland protocol XML used by OpenXR's Linux configure step. It checks that the
Ubuntu WSL disk is on D: and keeps project builds and temporary files inside
the D-backed WSL workspace.

The Windows PC currently has CMake 3.28.3, Ninja 1.11.1, Clang/LLD 18.1.3,
the AArch64 GCC 13.3.0 cross packages, `glslc` 2023.8 and
`wayland-protocols` 1.45. Ubuntu's Clang compiler binary is x86_64. The cross
toolchain finds target headers and libraries only in the Frame sysroot.

## Prepare the Frame Sysroot

The sysroot is at `/home/astro/MGSOneFrameDev/sysroot/frame`. It contains
Frame ARM64 headers, startup and compiler runtime files, glibc and libstdc++,
plus the target SDL2 and Vulkan loader dependencies. The host SDL2/Vulkan
packages are only for native generator configuration; they must never be used
by the ARM64 runtime link.

The read-only Frame probe reported SteamOS `aarch64`, glibc 2.43, GCC/G++
15.2.1, Clang 21.1.8, SDL2 2.32.64 and Vulkan 1.4.309. Target headers are in
`/usr/include` and `/usr/include/c++/15.2.1`; GCC startup/runtime files are in
`/usr/lib` and `/usr/lib/gcc/aarch64-unknown-linux-gnu/15.2.1`. SDL2's CMake
config is `/usr/lib/cmake/SDL2`, with pkg-config files under
`/usr/lib/pkgconfig`. The sysroot contains that header tree and only the
runtime/linker files used by the C++ probe and MGS runtime. It doesn't contain
the Frame home directory, game data or GPU drivers. The Frame's
`libpthread_nonshared.a` wasn't present; its integrated glibc 2.43
`libpthread.a` and `libpthread.so.0` are present.

Copy only the target system files needed by the runtime. Do not copy discs,
private game imports, the Frame home directory or GPU drivers. After extraction,
relocate Frame's absolute symlinks and check that every target stays inside the
sysroot:

```sh
python3 scripts/windows_dev/normalize_frame_sysroot.py \
  /home/astro/MGSOneFrameDev/sysroot/frame
```

The CMake toolchain rejects sysroots outside the D-backed ext4 workspace,
keeps host programs outside target search, and resolves target headers and
libraries only inside the Frame sysroot.

## Check the Compiler and Target ABI

Run the small C++ cross-compile before building the project:

```sh
bash scripts/build_windows_frame.sh abi-smoke
```

It checks that Clang and LLD are x86-64 programs, emits an AArch64 PIE with the
Frame GCC headers and runtime files, and compares the ELF's direct dependencies
and symbol-version requirements against the Frame sysroot. The probe on this
PC linked with GLIBC through 2.34, GLIBCXX through 3.4.32, CXXABI through
1.3, and GCC through 3.0; all were present in the Frame libraries.

## Build the Host Generators

Run this from a project checkout inside the WSL ext4 workspace. It puts the
host build under `/home/astro/MGSOneFrameDev/build/host`:

```sh
BUILD_JOBS=2 bash scripts/build_windows_frame.sh host-tools
file /home/astro/MGSOneFrameDev/build/host/ps1Analyzer/ps1Analyzer \
  /home/astro/MGSOneFrameDev/build/host/ps1Recomp/ps1Recomp
```

Both programs report x86-64. The source-only host build does not need disc
images or generated game data. I keep the verified programs at
`/home/astro/MGSOneFrameDev/tools/ps1Analyzer` and
`/home/astro/MGSOneFrameDev/tools/ps1Recomp`.

## Cross-Build the Runtime

First verify the Frame sysroot is present, then build the source-only runtime:

```sh
BUILD_JOBS=2 bash scripts/build_windows_frame.sh frame-smoke
```

This uses the native Clang/LLD toolchain and target libraries from the Frame
snapshot. It compiles the ARM64 runtime with the stub game translation, so it
checks project configuration and linking without importing game data.
OpenXR's upstream CMake processes a Linux presentation backend even when its
SDK samples and tests are disabled. The build selects Wayland for that
configure step, so the host needs the protocol XML package. MGS still links
the OpenXR loader and its Steam Frame VR runtime code stays enabled. This
selection doesn't add a desktop window backend to MGS or link host Wayland
libraries into the ARM64 runtime.

For a checkout that already contains the private generated MGS translation
shards, use:

```sh
BUILD_JOBS=2 bash scripts/build_windows_frame.sh frame-runtime
```

That command leaves `PS1RECOMP_USE_STUB=OFF`, compiles the generated ARM64
runtime and verifies its ELF architecture, direct dependencies and GLIBC,
GLIBCXX, CXXABI and GCC symbol-version requirements against the Frame sysroot.
Generated translations and game data stay outside Git.

The generator keeps each overlay's normal and resume lookup in a separate
function. Startup registrations use data arrays and short insertion loops.
This keeps the shared support file out of one large optimizer graph without
changing the guest translation shards or the first matching overlay identity.
The synthetic regression exercises lookup aliases, resume entries, identity
precedence and registration through the generated C++:

```sh
python3 -m unittest scripts.tests.test_native_codegen -v
```

The test expects `build/ps1Recomp/ps1Recomp`; link that path to the native host
generator when using the external WSL build folder. For the USA campaign I
compared every generated registration and overlay condition before and after
the split: 2,257 entry points, 19,140 resume entries, 31 aliases and 258,208
overlay cases matched in order. All 160 guest shards were byte identical.
Clang 18 finished the replacement support file and link in 9 minutes; the
earlier support compile was still running when I stopped it after 43 minutes.
The shared PC load differed, so those times aren't a controlled speed comparison.

The project build is a smoke check only. Launch, controller input, Vulkan and
performance still need their own Frame runs.

## What I Checked

I started from source revision `30434f943f7e4f3d2e9a104a5d3a2e60c6c8ab29`.
The source-only stub now forwards resume dispatch to its existing no-op guest
dispatch, so the runtime links without generated game code. The standalone
ABI command compiled an ARM64 PIE with native x86-64 Clang 18 and LLD 18. Its
direct libraries were `libc.so.6`, `libgcc_s.so.1`, `libm.so.6` and
`libstdc++.so.6`; their version tables accept the requirements through GLIBC
2.34, GLIBCXX 3.4.32, CXXABI 1.3 and GCC 3.0. I also ran that probe on the
Frame. It printed `Frame ABI probe` and exited with status 0.

The host-generator build produced x86-64 programs with Build IDs
`e6be5080b2f741c24202ef62701b6b1a51cbca86` (`ps1Analyzer`) and
`e059eacbfc7213211dfae2f55e277599c2550c7c` (`ps1Recomp`).

The ARM64 project smoke build keeps `MGS_STEAM_FRAME_VR=ON` and builds the
OpenXR loader and handwritten MGS VR sources. It uses the project's no-game
stub translation, so it checks the full runtime compile and link without
private game data. I keep the resulting binary and its shaders in
`/home/astro/MGSOneFrameDev/artifacts/frame-smoke`. Its Build ID is
`0cd58dceff6087f2` and its SHA-256 is
`5d2788ebf6dee3185b20d06b5a41eb32033263c2f6d308f7e4b7206cdc3abd4e`. The
ELF links against Frame `libSDL2-2.0.so.0`, `libvulkan.so.1`, libc, libgcc_s,
libm and libstdc++; Frame accepts its requirements through GLIBC 2.43,
GLIBCXX 3.4.32, CXXABI 1.3.13 and GCC 4.5.0.
