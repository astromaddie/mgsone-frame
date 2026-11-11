# Steam Frame VR Architecture

The native ARM64 flat build comes first. The upstream Vulkan path is a viable
source for stereo because it captures original KMD models, composed poses and
the channel view before the PS1 bounds check. The renderer still mixes these
with PS1 ordering-table packets. A second view of the current 16:9 output
would lose the geometry that was clipped or culled for that view.

The default third-person view is a room-anchored miniature stage. Its origin
is captured once per scene epoch and it sits 2.4 m in front of the opening
head pose, 0.9 m below it. Scale is 8% of the 1,000-units-per-metre world.
Every source face is transformed back through its retained channel matrix
before placement. Guest camera pans, cuts and Snake movement cannot change
the room placement. R3 switches to first person and back. Holding View for
one second recentres the stage and panels in front of the wearer. A short
View press sends native Select/Codec on release, using a 50 ms pulse across
the guest's 60 Hz input latch. A long hold sends no Select. Losing focus cancels
a pending hold or pulse and requires release before another press.

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
asymmetric projections. The tracked eye poses retain the runtime's physical
IPD. A scene epoch change places a new toybox; moving the player does not.

In first person the eye follows an offset within the native head model's
rendered pose. Torso,
legs, arms and hands remain visible. Only head-model faces within 180 game
units of the eye are suppressed. The native hands follow Snake's animation;
there is no controller-driven arm IK in this change.

Forward/back/strafe follow the headset's horizontal heading. Right-stick
turning adds up to 90°/s. The rotated analog input still enters the original
pad path. The USA animation callback bridge supplies facing after the native
wall-direction checks. At Snake's call to GM_ActControl, the bridge redirects
the animation's horizontal step toward the requested movement direction,
then the original collision, floor and trap code consumes that step. It never
writes position. Only the authenticated Snake idle/crouch/prone/run/start
handlers accept this override. Scripted, wall, damage and other authored
stances stay native. Third person supplies no override. The host publishes
an atomic sample and no longer writes actor RAM from VBlank.

The bridge installer checks the full USA executable hash and exact retail
instructions. USA addresses differ from the symbolic addresses in the
[Snake source reference](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/chara/snake/sna_init.c).
The actual boundaries are 0x8005CAB4 (animation callbacks) and 0x80026FA4
(GM_ActControl), restricted to the Snake call returning to 0x8005D0FC.

The right controller's OpenXR aim pose supplies weapon facing and pitch
during a shot. A fingerprinted entry hook also adjusts the local matrix passed by
the player weapon actors to the original NewBullet constructor. It preserves
the muzzle position and the original firing, ammunition and collision code.
The three resident player call sites cover SOCOM, FAMAS and rifle bullets.
The right trigger uses the game's Square action, and the right grip uses
R2 for the weapon menu. A short controller ray and level projected marker show the tracked aim.

Title, Codec and video use the latched native scanout on a yaw-only 2 m,
1.9 m-wide panel. World admission requires the producer's authenticated
`MgsPlayerView` and source geometry from its scene epoch. A source mesh alone
isn't enough: Frame title/difficulty captures contained a source mesh at the
32767 coordinate limit without a live Snake witness. That previously admitted
the title as gameplay, replayed incomplete UI and made the menu follow the
head. Such mesh-bearing frames now immediately use the complete native
scanout, including its original portrait shading and menu text. Codec still
forces cinema, and empty interleaved latches keep the short world reuse window.
The player witness rejects paused/scripted/authored views, so those keep their
native presentation. `capture.txt` records `source_faces` and `player_witness`
beside admitted `world_faces`. The Frame title capture at VBlank 3200 shows
the original dark portrait and both menu choices; the difficulty capture at
4400 shows all four choices. Both use native scanout with `world_faces=0`.
Gameplay HUD uses retained ordered UI draws, including
polygon UI without XYZ provenance. Its draw-page origin is independent of
the current display page, and partially clipped primitives keep their visible
portion. Gameplay HUD follows the live head midpoint, yaw and pitch without
roll. Title/Codec/video panels keep their separate room anchor. Regaining
focus places the cinema panel in front again without moving the toybox or
first-person world. Explicit recentre transfers the current first-person
heading into the camera basis, zeros head/stick deltas and rebuilds geometry
before drawing either eye.

`[MGS VR] Panel placement` logs once on focus, presentation-context changes
and explicit recentre. Its reason bits are 1/2/4 respectively. It records the
head position/forward, active panel anchor, and panel-centre projected X/Y/W
for both eyes. These are diagnostics for live placement, not evidence that
the title pixels have rendered correctly.

The page convention comes from the pinned primary source:
[`CopyDrawEnv`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/libdg/chanl.c#L70-L95)
shifts X, [`InitWork`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/menu/menuman.c#L120-L128)
sets pages at X=0 and X=320, and
[`set_radar_pos`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/menu/radar.c#L1016-L1030)
puts the radar clip at X=235/555 with a distinct local origin at 269/589.
I use the retained draw area as the page witness, retaining the enclosing
page for an edge-crossing subrect. I don't treat the radar's local offset as
the framebuffer origin. The Frame dock captures confirmed full draw pages
at X=0/320 and radar clips at X=235/555 with offsets 269/589.
`capture.txt` records each draw area, offset, selected page and packet count.
Screen effects that depend on the flat renderer's
feedback or temporal history still need eye-local implementations.

The VBlank 16060 first-person capture showed a black HUD-sized plate over
Snake's body. I found that `GPU::executeRectangle` retains MGS TILE commands
as `DrawPacket`s, so ignoring `FillRect` operations wasn't enough. The pinned
[`channel setup`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/libdg/chanl.c#L117-L137)
enables background clearing for channel 0. I now exclude opaque, untextured,
flat TILE/POLY_F4 rectangles that cover the entire draw page from gameplay
HUD replay. The predicate checks the actual native corners and draw area.
It keeps smaller status backgrounds, radar subrects, textures and blended
fades. It doesn't change cinema scanout.

`capture.txt` now records each HUD candidate's opcode, blend mode, colours,
native corners, draw page, source tokens and background decision. The matching
Frame capture identified the opaque `0x60` TILE at native corners
(320,0)..(640,224). Excluding it removed the rectangular plate while keeping
the radar and SOCOM ammo display. Credits also use
paired subtractive/additive sprites in
[`telop_800DD550`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/takabe/telop.c#L40-L70).
Those draws remain eligible, but the existing half-alpha VR blend path
doesn't reproduce those blend modes exactly.

First-person headless captures also write `body-geometry.txt`. It records
the captured eye origin, source model bounds relative to that origin, and
the exact submitted world triangles with their lifetime, asset, local model
and face identities. The forward/down60/down85 eye matrices accompany them.
I use this to identify near-camera occluders before changing body visibility
or eye placement. Source bounds include the suppressed head; submitted
triangles include only the geometry actually sent to the world passes.
The dump doesn't sample texture transparency, so geometric intersections
still need comparison with the matching PNG.

The matching VBlank 16060 geometry dump identified the remaining dark strip
as Snake's torso, not another HUD item: epoch 4, lifetime 26, asset 370,
model 1, face 28, material 2052, triangles 4104/4105. Its submitted vertices
were only about 2–11cm from the joint camera. Projection reproduced the
down60 strip's right edge at pixels (448.39,331.56) and (414.92,635.97).
The torso reached 23.7mm above the joint and the neck reached 45.5mm above
it. The camera was sitting inside the collar region.

I moved the eye origin to local (0,100,60)mm on head model 6: 100mm above
the joint and 60mm toward the face. The native head's local bounds are
Y=-60..165mm and Z=-90..93mm, so this sits within the head instead of the
collar. The offset follows the same presentation matrix used by the body
geometry. I kept every torso, neck, arm and hand face and left the existing
head-only near-eye suppression in place. The matching Frame down60/down85
captures show the torso and arms without the enlarged collar strip. The
SOCOM pose also shows the native weapon hand. The capture retains 276 body
faces and removes 54 head faces.

Normal wall presses keep the world presentation. The native Up capture at
VBlank 16115 reached Snake's wall callback `0x80055040`, player status
`0x10000` and game status `0x10`, with pause level zero. The pinned
[`status definitions`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/game/g_define.h#L29-L61)
name these `PLAYER_CAUTION` and `STATE_BEHIND_CAMERA`.
[`GM_CheckBehindCamera`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/game/camera.c#L1154-L1183)
sets that camera bit during wall interaction. I allow that bit only with
authenticated Snake wall status and the USA wall idle/move/crouch callbacks
`0x80055040`, `0x80055134`, `0x800552B8`. I matched those retail callbacks
against the pinned
[`wall helpers`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/chara/snake/sna_init.c#L3289-L3420).
All other authored-view bits and pause checks still apply. Wall actions keep
their native control logic; admission doesn't enable the movement override
for them. The matching native Up capture at VBlank 16115 contains 2,926 world
faces and a live player witness. Its stage epoch and centre match the
stationary VBlank 16060 capture: epoch 4, centre
(-5623.75,99.3409,-1252.21). The left cave region at pixels
X=75..311, Y=446..521 differs in four of 18,012 pixels; two exceed 6/255 at
the bottom border. This checks a fixed background through the wall-camera change.

The affected control map is:

| Physical Control | Third Person | First Person |
| --- | --- | --- |
| Left Stick / D-Pad | Native Movement / Navigation | Head-Relative Move / Native Navigation |
| Right Stick X | Native | Turn Heading |
| R3 | First Person | Toybox |
| A / B / X / Y | Cross / Circle / Square / Triangle | Same |
| Left / Right Bumper | L1 / R1 | Same |
| Left Trigger | L2 | Same |
| Right Trigger | R2 | Square (Fire) |
| Right Grip | Unassigned | R2 (Weapon Menu) |
| Menu | Start | Same |
| View, Short Release | Select / Codec | Same |
| View, Hold 1s | Recentre Stage and Panels | Recentre Without Turning |

The first-person trigger and grip map stays selected through native menu and
pause transitions, including while the gameplay camera witness is absent.

## Current Correction Checks

The standalone `mgsVr_tests` target covers inverse
channel transforms at 0/90/180°, frozen scene
placement, no-roll HUD tracking, horizontal HUD pages, View hold/release and
focus reset, Select pulses through 60 Hz guest latches at 72/90/120 Hz, heading
preservation on recentre, native-step redirection without position writes,
and byte-exact no-op behavior in third person and authored states. It also
exercises the actual player-view producer with authenticated wall callbacks
and a rotated presentation head pose. All 14 tests passed on the Frame.
The runtime suite passed 1,366 tests with 32 optional skips; the controller
suite passed 25. The native generator's eight synthetic checks and four
bridge checks also passed, including the verified USA executable/overlays.

Headless capture no longer requires a world mesh, so title/menu/Codec/video
can be captured at an explicit `PS1_VR_CAPTURE_VBLANK`. Each capture includes
`coincident`, `forward`, `yaw90`, `yaw180` and `leaned` eye pairs. First person
also writes `down60` and `down85` pairs from the same snapshot with the same
64 mm eye separation, to inspect native torso, arms and hands. `capture.txt`
records scanout origin, native nonblack count, world/body/panel counts and
toybox placement. Identity eyes must match exactly. A stationary
world point must retain the same room position across guest camera changes.

For movement, restore the same dock state for all four runs, use the unpause
input below, and capture at VBlank 16115. Keep
`PS1_VR_HEADLESS_MOVE_YAW=0` and vary
`PS1_VR_HEADLESS_MOVE_X/Y` through `0/1`, `0/-1`, `-1/0`, `1/0`.
Set `PS1_DIAGNOSTIC_IGNORE_INPUT=0`: suppressing host input also suppresses
the production VR analog movement path. The native input script still runs
with input suppression, so an unpaused checkpoint alone doesn't prove this.
Record `mgs.vr.control` and final RAM. Compare signed X/Z displacement and
actor facing against the stationary run. Native collision must still stop
Snake at a wall. In third person, use native scripted Up with those diagnostic
variables and request the same control trace: native movement should occur
without any VR control-override event. Repeat the SOCOM native shot check
below after movement passes.

From the same isolated dock state, the Frame runs moved +721 Z forward,
-312 Z backward, +1,554 X left and -2,437 X right, in game units. Native wall
contact limited several directions. A longer forward run stopped just one
unit beyond the short run, with `CONTROL.n_touches=1` and a zero horizontal
step after native collision. Walking traces retained the supplied facing;
wall stances kept their native callbacks and rotation. The third-person Up
run moved Snake with no VR control events. A cross-process exact-image and
post-readback RAM comparison was inconclusive because it didn't establish
the same guest state; I use the production trace and byte-exact hook checks
for the control no-op claim.

The isolated SOCOM fixture consumed one round (12 to 11) and the original
NewBullet call used Q12 direction (3969,1013,0), matching controller yaw
pi/2 and pitch 0.25. The title, difficulty and Codec captures use complete
native scanout; identical-pose eyes matched byte for byte, while separated
eyes differed. The corrected native ARM64 runtime's SHA-256 is
`212d5898a84cd72a3d2016ea6e83c5c400eec720794f2f7b9966704ef89640af`.
Earlier measurements below describe the superseded camera.

## Headless Dock Capture

Use the scripted dock checkpoint in [Steam Frame](STEAM_FRAME.md). Restore
the state in the same isolated profile and point `PS1_MGS_ASSET_CACHE` at the
imported `assets.mgsc`. Resume its native pause flag before capturing gameplay:

```sh
mkdir -p "$capture"
printf '16005 16010 start\n' > "$capture/unpause.input"
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_IGNORE_INPUT=1 \
PS1_MGS_ASSET_CACHE="$media/assets.mgsc" PS1_AUTOLOAD_VBLANK=1 \
PS1_INPUT_SCRIPT="$capture/unpause.input" \
PS1_EXIT_AFTER_LOAD_VBLANKS=120 PS1_VR_CAPTURE_VBLANK=16060 \
build/ps1Runtime/ps1Runtime --config "$profile/runtime.toml" --vr-headless "$capture"
```

The capture writes two 768×768 PPMs per pose from a single snapshot.
Compare the identical-pose pair byte for byte. The forward pair has a
64 mm eye separation. The other pairs add 90° and 180° yaw, and a 12 cm lateral head movement. Check the scene at each pose for
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

The runtime suite passed 1,365 tests with 32 optional skips. The bridge
installer's identity and idempotence checks also passed against the imported
USA executable. A second SOCOM check added 0.25 radians of upward controller
aim. The retail shot's Q12 direction was (3969, 1013, 0). The aim marker
covered 13 pixels in each 768×768 eye, increased from a four-pixel thin
marker that lost its horizontal bar at that size.

To repeat the first-person check, restore the VBlank 16001 dock state in an
isolated profile. That checkpoint has the original pause flag set. Use this
input file to resume before measuring movement:

```sh
printf '16005 16010 start\n' > "$capture/unpause.input"
SDL_AUDIODRIVER=dummy PS1_MGS_ASSET_CACHE="$media/assets.mgsc" \
PS1_AUTOLOAD_VBLANK=1 PS1_EXIT_AFTER_LOAD_VBLANKS=180 \
PS1_INPUT_SCRIPT="$capture/unpause.input" PS1_VR_FIRST_PERSON=1 \
PS1_VR_CAPTURE_VBLANK=16060 \
build/ps1Runtime/ps1Runtime --config "$profile/runtime.toml" --vr-headless "$capture/stationary"
```

Repeat at VBlank 16115 with `PS1_VR_HEADLESS_MOVE_YAW=0`, then
`PS1_VR_HEADLESS_MOVE_YAW=1.57079632679`. The diagnostic supplies forward
input through the production head-relative analog mapping after VBlank
16028. Set `PS1_RAM_DUMP_PATH` to a separate file for each run and decode it
with `mgs_tas.game_state`. A pass has pause level zero and displacement
along the requested +Z and +X directions.

For the SOCOM check, use an isolated NSV12 copy with `mgs_fixture.patch_ram`
and `checkpoint_bytes`. Set the signed 16-bit linked weapon field at
`0x800B752C` to 0 and the SOCOM inventory field at `0x800B7532` to 12.
Check their original values before applying the fixture. Add
`16055 16080 square` to the input file and capture at VBlank 16130.
Use `PS1_VR_HEADLESS_AIM_YAW=1.57079632679`,
`PS1_VR_HEADLESS_AIM_PITCH=0.25`, `PS1_TRACE_EVENTS=mgs.vr.aimed_shot`
and a fresh `PS1_TRACE_JSONL` path. A pass contains a retail shot event with
positive X and Y direction, near-zero Z, plus a visible marker in both eyes.
These diagnostic pose inputs are read only by `--vr-headless`.
