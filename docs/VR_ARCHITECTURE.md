# Steam Frame VR Architecture

The native ARM64 flat build comes first. The upstream Vulkan path is a viable
source for stereo because it captures original KMD models, composed poses and
the channel view before the PS1 bounds check. The renderer still mixes these
with PS1 ordering-table packets. A second view of the current 16:9 output
would lose the geometry that was clipped or culled for that view.

The first stereo target is the opening dock: keep Snake's original controls
and the third-person framing, with head tracking relative to the camera
anchor. Check the gameplay camera and world scale against the original room
and character dimensions before choosing a default. Keep authored camera cuts
and pre-rendered video on a cinema screen while the gameplay view uses stereo.

## Draw inventory to verify at the dock

| Source | Flat behavior | Stereo rule to verify |
| --- | --- | --- |
| `games/mgs_scene.cpp`, `games/mgs_mesh_replay.cpp`, `shaders/games/mgs_original_mesh.glsl` | `captureSceneBeforeBounds` retains channel view and world transforms; replay clips reconstructed faces to the PS1 view and projects with positive view Z | Prepare the snapshot once. Cull for both eyes and clip in the eye passes. Convert the OpenXR camera basis before feeding the scene's positive Z path. Keep the guest camera and its gameplay visibility rules intact. |
| `gpu/gpu.cpp`, `gpu/modern_replay_recorder.cpp` | PS1 packets supply ordering, materials, native fallbacks and some screen-space content | Keep guest VRAM work single, and classify the replay draws. Rebuild world fallbacks from their source positions; keep genuinely 2D content on the panel. |
| `games/mgs_surface_effects.cpp`, `games/mgs_screen_effects.cpp` | Particles, water and camouflage use a mix of scene depth and copies of the flat image | Reproject positioned effects. Water, screen copies, depth and temporal history must be eye-local. Audit HUD and video pixels out of the scene history. |
| Native GPU overlays and `gpu/renderer_vulkan.cpp` | Radar, subtitles and Codec are 2D; the host pause menu draws ImGui after the scene | Separate HUD and menus from the eye render, with no head roll. Put full-screen Codec, subtitles and video on a stable plane. |

## Frame ownership

The existing runtime preserves the original NTSC game cadence with 30 and 20
Hz gameplay divisors. Its 60 Hz presentation interpolation is optional. Keep
guest timing and the input latch independent of the XR refresh rate. Use
`xrWaitFrame` and locate both eyes at `predictedDisplayTime`. Prepare the game
frame once, record a complete pass for each eye, and submit one projection
layer with the poses and fields of view used to draw it. An XR session in
standby keeps servicing events without advancing gameplay.

The standalone colour probe uses `XR_KHR_vulkan_enable2`, prints the formats
offered by the runtime, and submits red in the left eye and blue in the right.
It uses live tracked poses and skips rendering in standby. Build it separately
from the game and launch it as a Steam shortcut with SteamVR active:

```sh
cmake -S engine/tools/steam-frame-probe -B build-frame-probe -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-frame-probe --parallel 2
```

On 2026-11-11 the native probe opened a SteamVR OpenXR session using Turnip
on the Adreno 750. The runtime offered `VK_FORMAT_R8G8B8A8_SRGB`, allocated
three 512×512 images per eye, and accepted one red-left, blue-right projection
frame before the session stopped. This checks the Vulkan binding and submission
path. It does not check the appearance of the game in stereo.

After the flat gameplay gate, compare the opening dock's original view with
identical eyes, then check both eyes at several yaw angles for missing draws
and depth conflicts before enabling the tracked camera for play.
