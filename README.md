# MGSOne VR for Steam Frame

A native ARM64 Steam Frame port of *Metal Gear Solid* for PlayStation, with
stereo rendering, head tracking and two ways to play in VR. It runs on the
headset without a streaming PC.

This is my VR fork of
[coccofresco/mgsone-recomp](https://github.com/coccofresco/mgsone-recomp).
The original game logic and assets sit underneath a Vulkan renderer and
OpenXR presentation.

## What It Does

| View | Behaviour |
| --- | --- |
| Third Person | A fixed, tilted toybox diorama. Movement follows the diorama's orientation, even when the original game camera changes. |
| First Person | A head-tracked camera attached to Snake, with his body, arms and weapon visible. Movement follows your horizontal head direction; the right stick turns Snake. |
| Scripted Cutscenes | A fixed cinema screen with stereo depth behind the picture and native subtitles below it. |
| Prerecorded Video | The original mono picture on a cinema screen. |
| Menus and Codec | The complete original panel. Gameplay HUD, radar and item selectors stay over the game view. |

Arms and weapons use their original animations and aiming, including
autotarget. Tracked-controller aiming and arm IK are disabled.

## Before You Start

This is an experimental source release. There isn't a ready-to-install
Steam Frame download in this repository yet. The upstream Windows and x86-64
Linux packages are separate desktop builds.

You'll need:

- A Steam Frame with SteamVR/OpenXR and Vulkan support.
- Your own **USA v1.0** copies of both discs: **SLUS-00594** and **SLUS-00776**.
- An ARM64 Linux build environment, or an x86-64 Windows/WSL cross-build setup.

The importer accepts raw BIN images or single-track MODE2/2352 CUE/BIN pairs.
Extract archives first. **USA v1.1 / Rev 1, other regions and Integral aren't
supported.** The executable patches and overlay addresses target v1.0.

No BIOS download is required. Disc images, extracted assets, saves and
generated game translations stay local and aren't included here.

## Build

### ARM64 Linux

The simplest build path uses ARM64 Linux, including the Frame itself if its
development tools are already installed. You'll need Git, Python 3.11+, CMake
3.20+, Ninja, Clang with C++20 support, SDL2 2.26+, Vulkan development files
and `glslc`. CMake fetches the pinned OpenXR SDK.

On ARM64 Ubuntu, install the dependencies with:

```sh
sudo apt-get update
sudo apt-get install git cmake ninja-build clang g++ libsdl2-dev libvulkan-dev glslc python3
```

Then clone and build:

```sh
git clone https://github.com/astromaddie/mgsone-frame.git
cd mgsone-frame
BUILD_JOBS=2 bash scripts/build_alpha.sh --steam-frame \
  '/path/to/disc1.cue' '/path/to/disc2.cue'
```

The build verifies both discs, prepares the local asset cache, generates the
native game translation and builds the runtime. It also runs the controller,
runtime and Python checks. Keep the whole checkout together after building;
the launcher uses its runtime, shaders and scripts.

See [Steam Frame Setup](docs/STEAM_FRAME.md) for the full build details.

### Windows / WSL

The [Windows Cross-Build Guide](docs/MGS_WINDOWS_FRAME_DEV.md) describes the
D:-backed WSL setup used for development. It uses native x86-64 host tools
and cross-compiles the runtime for ARM64 against a Frame sysroot. Its workspace
paths describe that development setup; the ARM64 recipe above doesn't require
that layout.

On Apple Silicon, use ARM64 Linux in a VM or on a remote host. This is a Linux
OpenXR port, not a native macOS VR application.

## Launch on the Frame

Copy the built checkout to the Frame if you built elsewhere. If you haven't
imported the discs on the Frame yet, run this from that checkout first:

```sh
scripts/launch_steam_frame.sh import '/path/to/disc1.cue' '/path/to/disc2.cue'
```

This creates the Frame profile described below. An existing verified import
can be reused.

1. In Steam's Desktop Mode, add `scripts/launch_steam_frame.sh` from the
   checkout as a **Non-Steam Game**.
2. Set **Start In** to the checkout directory.
3. Launch it from Steam. With no arguments, the launcher opens VR directly.

Launch through Steam so the Frame controllers are available. `--vr` selects
VR explicitly; `--flat` starts the desktop version.

## Controls

| Input | Action |
| --- | --- |
| Left Stick / D-Pad | Move Snake; navigate native menus |
| Right Stick | Turn Snake in first person |
| Right-Stick Click | Switch between first person and the diorama |
| A / B / X / Y | Original Cross / Circle / Square / Triangle buttons |
| Left / Right Bumper | Original L1 / R1 buttons |
| Left Trigger | Original L2 item selector |
| Right Trigger, Third Person | Original R2 weapon selector |
| Right Trigger, First Person | Fire |
| Right Grip, First Person | Open the weapon selector |
| Menu | Original Start button |
| View, Tap | Original Select / Codec |
| View, Hold | Recentre the view |

In first person, looking up or down changes the camera, not the gun's aim.
The original item and weapon selectors keep their normal navigation.

## Saves and Settings

VR and flat mode share the same profile:

```text
${XDG_DATA_HOME:-$HOME/.local/share}/mgs-recomp-frame/
├── library/       Verified disc import and asset cache
├── memory_cards/  Original in-game saves
├── savestates/    Quick states
└── config/        Launcher settings
```

Keep normal in-game Codec saves as well as quick states. Quick-state
compatibility can change between builds. Back up the profile before updating.
Changing or removing the checkout doesn't remove this profile.

## Status and Bug Reports

The latest recorded Frame checks passed **39 VR tests** and **1,373 runtime
tests**, with **32 optional skips**. Offscreen captures checked cinema depth,
native camera composition and identical images for coincident eyes. These
checks cover specific scenes and behaviours, not a complete campaign.

The [VR Notes](docs/VR_ARCHITECTURE.md) record the implementation and
repeatable checks. [Playback Notes](docs/FRAME_PLAYBACK.md) cover timing and
audio work. [Public Source Checks](docs/PUBLIC_SOURCE.md) describe the publication
boundary.

Please [report problems](https://github.com/astromaddie/mgsone-frame/issues)
with the commit, disc version, location, selected VR view and steps to
reproduce. Mention whether you loaded an in-game save or a quick state.
Screenshots help. Don't upload discs, extracted assets, memory cards or full
savestates to public issues.

## Credits and Licensing

The Frame port builds on
[Midnight Ghost Stone / mgsone-recomp](https://github.com/coccofresco/mgsone-recomp)
and its Vulkan presentation. The runtime comes from
[PS1Recomp](https://github.com/Dellareti/ps1-recomp).
[FoxdieTeam's reversing work](https://github.com/FoxdieTeam/mgs_reversing)
provides game research and the separately attributed texture adaptation.
The original game is the work of Konami and the Metal Gear Solid team.

The engine is GPL-3.0; see [LICENSE](LICENSE). Dependencies and adapted code
retain their own terms. The inherited FoxdieTeam texture adaptation has a
separate provenance and licensing limitation, documented in the
[Third-Party Notices](packaging/THIRD_PARTY_NOTICES.md); the engine's licence
doesn't grant a new licence to that code.

This is an unofficial project, with no affiliation with or endorsement from
Konami. Metal Gear Solid and its assets belong to their respective owners.
