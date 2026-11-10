# Steam Frame

The first milestone is the original game running flat as a native ARM64 Linux
program on Steam Frame. The upstream x86-64 AppImage and Flatpak are separate
builds. See [VR architecture](VR_ARCHITECTURE.md) for the stereo work.

## Build the flat baseline

Use a dedicated writable checkout and two original **USA v1.0** disc images
(`SLUS-00594` and `SLUS-00776`). The build checks the image hashes before
generating any game code. Archives must be extracted; pass each raw BIN or its
single-track MODE2/2352 CUE. The dumps labelled `Rev 1` are USA v1.1. The
executable patches and overlay addresses are specific to v1.0.

On a Steam Frame with the build tools already installed, use a terminal in
Desktop Mode. The build defaults to Clang and needs CMake, Ninja, Python 3.11 or newer,
SDL2 2.26 or newer (including sdl2-compat), Vulkan headers and loader, and
`glslc`. No system changes are needed on a Frame that already has these tools.

For a separate ARM64 Ubuntu 24.04 environment, install the same tools there:

```sh
sudo apt-get update
sudo apt-get install git cmake ninja-build clang g++ libsdl2-dev libvulkan-dev glslc python3
```

On Apple Silicon, use an ARM64 Ubuntu VM (or an ARM64 Linux host over SSH).
On ARM64 Windows, use ARM64 Ubuntu in WSL2. An x86-64 Linux or Windows host
needs an ARM64 VM or remote ARM64 Linux host for this recipe; the analyzer and
recompiler run during the build.

```sh
bash scripts/build_alpha.sh --steam-frame '/path/to/disc1.cue' '/path/to/disc2.cue'
readelf -h build/ps1Runtime/ps1Runtime | grep 'Machine:'
```

Expect `AArch64`. The build imports the discs into
`${XDG_DATA_HOME:-$HOME/.local/share}/mgs-recomp-frame` and reuses the verified
import on later runs. This profile stores the cache, settings and saves outside
the checkout. Generated translations and extracted analysis data stay in the
private build checkout. Keep the discs and those files out of Git.

## Run from Steam

In the checkout on the Frame, set an initial resolution for the flat test:

```sh
scripts/launch_steam_frame.sh settings --height 720
```

Add `scripts/launch_steam_frame.sh` as a Non-Steam Game and use the checkout as
**Start In**. Use Steam Input's Gamepad layout. The first gameplay check starts
a fresh game, reaches the dock, moves Snake, opens and closes the Codec, saves,
and reloads.
Check the simulation pace and Vulkan output at the dock before raising the
resolution. A source-only build with `PS1RECOMP_USE_STUB=ON` checks the compiler
and runtime dependencies; it cannot start the retail game.

## Evidence

On 2026-11-10, the Frame reported `aarch64`, GCC 15.2.1, Python 3.14.3,
SDL2 2.32.64 and Vulkan 1.4.309 development files, with CMake, Ninja and `glslc`
available. Its native build environment uses glibc 2.43; a binary built there
is a local test build. A portable build needs its own ELF dependency and ABI
check against the intended Steam Linux Runtime.

On 2026-11-11, Clang 21.1.8 compiled the generated game and runtime. Both the
game and launcher were `AArch64`, with glibc 2.43 and GLIBCXX 3.4.32 requirements
that resolved on the Frame. The controller suite passed 25 tests, the runtime
suite passed 1,363 with 32 skips, and the Python suite ran 210 with 8 skips.
From a fresh profile and verified v1.0
discs, `scripts/launch_steam_frame.sh run --headless-frames 1800` reached the
requested VBlank and exited cleanly. That run used accelerated guest pacing
and no Vulkan presentation.

The stock `config/mgs-usa-gameplay.input` route reached the dock without
interpreter fallbacks. I ran the runtime with `PS1_DIAGNOSTIC_HEADLESS=1`,
`PS1_INPUT_SCRIPT` set to that route, `PS1_VBLANK_PERIOD_US=1000`, and
`PS1_AUTOSAVE_VBLANK=16000 PS1_EXIT_AFTER_AUTOSAVE=1`. I then restored the
checkpoint with `PS1_AUTOLOAD_VBLANK=1 PS1_EXIT_AFTER_LOAD_VBLANKS=90` and the
original asset replay enabled (`PS1_MGS_MESH_REPLAY=1`,
`PS1_MGS_ASSET_CACHE` pointing to the imported cache). The Vulkan run used
`PS1_VULKAN_HEADLESS_SURFACE=1 PS1_VULKAN_REQUIRE_HARDWARE=1` and SDL's dummy
video and audio drivers. Turnip rendered at 1280×720, recorded 46 scene
snapshots, and reached VBlank 16091. This covers a scripted checkpoint and
offscreen rendering. Steam Input and the sustained in-game frame rate need
separate checks.
