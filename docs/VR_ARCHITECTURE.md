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

## Game renderer

`MGS_STEAM_FRAME_VR` builds the eye renderer. Run the game with `--vr`
to use the original scene snapshots. The renderer expands the original
geometry once for both eyes and draws it through two depth-tested,
asymmetric projections. The opening view follows the original third-person
camera. Tracked position and orientation are relative to the first valid
head position and heading after SteamVR gives the game focus. The current
scale is 1,000 game units per metre.

Clicking the right stick switches the gameplay camera. In first person,
the camera follows Snake's live position, head height and facing. The
renderer takes that pose from the same snapshot as the geometry and hides
Snake's body so the camera can see out. Forward input follows the headset's
horizontal heading. The right stick adds smooth turning at up to 90°/s,
and head rotation adds to it. The original movement code reads a rotated
analog sample, with its own pad origin preserved. Scripted views, the original look mode
and vents keep their authored cameras. The head-height calculation follows
the original Snake actor's joint selection. [Snake actor reference](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/chara/snake/sna_init.c#L8129-L8157)

The right controller's OpenXR aim pose supplies the SOCOM and FAMAS shot
direction. A fingerprinted entry hook adjusts the local matrix passed by
the player weapon actors to the original NewBullet constructor. It preserves
the muzzle position and the original firing, ammunition and collision code.

The native scanout supplies a fixed cinema panel for the title screen,
video and Codec. In the world, legacy 2D packets supply the initial HUD.
Scene effects that depend on the flat renderer's feedback or temporal
history still need eye-local implementations.

## Headless dock capture

Use the scripted dock checkpoint in [Steam Frame](STEAM_FRAME.md). Restore
the state in the same isolated profile and point `PS1_MGS_ASSET_CACHE` at the
imported `assets.mgsc`:

```sh
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_IGNORE_INPUT=1 \
PS1_MGS_ASSET_CACHE="$media/assets.mgsc" PS1_AUTOLOAD_VBLANK=1 \
PS1_EXIT_AFTER_LOAD_VBLANKS=120 PS1_VR_CAPTURE_VBLANK=16025 \
build/ps1Runtime/ps1Runtime --config "$profile/runtime.toml" --vr-headless "$capture"
```

The capture writes two 768×768 PPMs per pose from a single dock snapshot.
Compare the identical-pose pair byte for byte. The forward pair has a
64 mm eye separation. The other pairs add a 0.45 radian yaw and a 12 cm
lateral head movement, respectively. Check the scene at each pose for
geometry gaps and depth conflicts. This test uses Vulkan directly and
leaves SteamVR idle.

On 2026-11-11, Turnip rendered the USA v1.0 dock at VBlank 16025 from
2,698 original faces. The coincident eyes matched byte for byte. With a
threshold of 6 on the 8-bit RGB channels, 32.92% of pixels differed between
the forward eyes. The yaw and lean changed 88.22% and 40.16% of the left
eye's pixels, respectively. The turned view showed the edge of the modeled
dock.

The same build opened two 1728×1728 sRGB swapchains in SteamVR and
submitted its first projection frame. SteamVR accepted the suggested
Frame controller bindings. That startup session stayed in
`XR_SESSION_STATE_SYNCHRONIZED`; the guest clock remained paused.

The first-person control check resumed the dock checkpoint before applying
input. Two synthetic headset headings moved Snake 3.608 m and 1.554 m in
the requested directions, with heading agreement of 0.996 and 1.000.
An isolated copy of the same state equipped the original SOCOM. A normal
Square press and release produced a retail NewBullet call whose direction
was +X, matching the supplied tracked aim sample. Coincident first-person
eyes matched byte for byte, with 48.61% stereo pixel difference at a
6/255 RGB threshold.

At startup, the guest waits for focus before dispatching game code. The
standby check submitted the initial projection and completed without CD
timeouts.
