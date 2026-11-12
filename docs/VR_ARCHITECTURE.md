# Steam Frame VR Architecture

The native ARM64 flat build comes first. The upstream Vulkan path is a viable
source for stereo because it captures original KMD models, composed poses and
the channel view before the PS1 bounds check. The renderer still mixes these
with PS1 ordering-table packets. A second view of the current 16:9 output
would lose the geometry that was clipped or culled for that view.

The default third-person view is a room-anchored miniature stage. Its origin
is captured once per scene epoch and it sits 1.7 m in front of the opening
head pose, 0.65 m below it. Scale is 12% of the 1,000-units-per-metre world,
with a fixed 45-degree tabletop tilt.
Every source face is transformed back through its retained channel matrix
before placement. Guest camera pans, cuts and Snake movement cannot change
the room placement. R3 switches to first person and back. Holding View for
one second recentres the stage and panels in front of the wearer. A short
View press sends native Select/Codec on release, using a 50 ms pulse across
the guest's 60 Hz input latch. A long hold sends no Select. Losing focus cancels
a pending hold or pulse and requires release before another press.

## Draw Inventory to Verify at the Dock

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

In first person the eye follows local (0,50,60)mm within head model 6's
rendered pose. Torso, legs, arms and hands use the native animation. Only
head-model faces within 180 game units of the eye are suppressed. I disabled
controller arm IK and hand-driven weapon poses. The old solve and guest
handoff remain covered by tests, but the renderer never activates or publishes
them, including with synthetic hand diagnostics. There’s no controller aim ray.

The diorama uses a fixed 45-degree tabletop tilt. A 1.8m character becomes a
21.6cm figure. I take the initial heading from native pad Up (`PadOrg + 2048`),
then keep it fixed through camera cuts and movement. Stick and D-pad directions
are compensated against the current native pad frame, so up moves away across
the displayed table. This changes only pad input; native animation and collision
still own Snake's movement. A new scene or explicit recenter places the stage.

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

Head and right-stick yaw turn Snake, including the authenticated SOCOM/FAMAS
firing action. HMD pitch is view-only: it never changes body-root pitch. I keep
the native weapon animation, gun aiming and autotarget behavior. Shots use the
original NewBullet matrix, muzzle, ammunition and hit logic; they don't follow
the headset's vertical gaze vector. The firing hook changes only yaw and target
yaw, keeping native pitch, roll, position, step and action transitions.
The right trigger uses Square; the right grip uses R2 for the weapon menu.
Controller poses don't move the gun or its shot origin.

The offscreen `PS1_VR_HEADLESS_AIM_YAW` diagnostic changes facing only.
`PS1_VR_HEADLESS_AIM_PITCH` is ignored. The down60/down85 captures rotate the
view independently, so they can inspect the body without tilting it.

Title and Codec use the latched native scanout on a yaw-only 2 m,
1.9 m-wide panel. World admission requires the producer's authenticated
`MgsPlayerView` and source geometry from its scene epoch. A source mesh alone
isn't enough: Frame title/difficulty captures contained a source mesh at the
32767 coordinate limit without a live Snake witness. That previously admitted
the title as gameplay, replayed incomplete UI and made the menu follow the
head. Such mesh-bearing frames now immediately use the complete native
scanout, including its original portrait shading and menu text. Codec still
forces cinema, and empty interleaved latches keep the short world reuse window.
Native Start pause and item/weapon selectors keep the authenticated world
behind their UI. Their directional input stays native while the menu is open.
For authenticated ordinary Snake idle/run/crouch/prone and SOCOM/FAMAS firing,
I also keep the world through native behind-camera, cut-in and menu-off hints.
Those hints alone don't establish a cutscene. This is a render policy: the
same states still keep their original native input and movement guards.
Unknown handlers don't get that exception. DEMO, PADDEMO, pad masking,
game-over, authored player states, STOP and disc errors keep native presentation
unless the separate live cutscene producer supplies an authenticated theatre view.
Codec always forces cinema. `capture.txt` records `source_faces` and `player_witness`
beside admitted `world_faces`. The Frame title capture at VBlank 3200 shows
the original dark portrait and both menu choices; the difficulty capture at
4400 shows all four choices. Both use native scanout with `world_faces=0`.
Gameplay HUD uses retained ordered UI draws, including
polygon UI without XYZ provenance and recorded TILE1 paint. Snake's radar dot
is the native white point at the radar draw origin, with its original blink
phase. Paint-only latches refresh the HUD too. Full-page opaque paint still
uses background rejection. Its draw-page origin is independent of
the current display page, and partially clipped primitives keep their visible
portion. Gameplay HUD follows the live head midpoint, yaw and pitch without
roll. Title, Codec and theatre panels keep their separate room anchor. Regaining
focus places the cinema panel in front again without moving the toybox or
first-person world. Explicit recentre transfers the current first-person
heading into the camera basis, zeros head/stick deltas and rebuilds geometry
before drawing either eye.

Scripted cinema uses a separate read-only witness. I require the live linked
`demothrd.c` producer, `STATE_DEMO`, a loaded DMO header and the source camera
captured with that ordering table. A title mesh or an arbitrary camera flag
can't turn on stereo. The renderer keeps the native camera's composition inside
the same 1.9 m-wide picture at 2 m. Its positive depth approaches 1 m behind
the picture, with a fixed 1,500-unit depth scale. This avoids foreground objects
crossing the caption band. The two eyes use the existing OpenXR projection
layer; head motion never replaces the director's camera. The window and its
0.26 m caption band share one yaw-only room anchor and recentre together.

I keep the native Jimaku text and four outline sprites. The producer checks
the USA `menu_jimaku_act` entry at `0x8004b770`, its `JimState` at `0x800c01d8`,
the live `menuman.c` arena and the complete five-sprite allocation. The native
caption-off setting emits no text packets. Type 1 pause/area labels stay in
the picture; only the original baseline-204 dialogue moves below it. The text
is drawn once, with its original glyph texture, colour and timing. No OCR or
replacement font is involved. The owning source is
[jimaku.c](https://github.com/FoxdieTeam/mgs_reversing/blob/217509eae471629b1a53633b816c61adfef9ed5c/source/menu/jimaku.c).

For the live `movie.c` producer, I retain the original mono scanout picture.
Its authenticated 320-pixel image starts at native Y=24; the supported picture
height is at most 160 pixels, above the separate Jimaku region. Embedded film
titles stay in that image. Unknown layouts keep the complete native panel.
The picture and caption packets share the same display latch. A second UI-only
ordering table preserves the already captured DMO camera, while an empty native
caption submission clears the text without replacing the paused scene. Empty
VBlank latches retain DMO geometry only while the exact visible native picture
is unchanged. A fresh ownerless ordering table still ends theatre immediately.
Movie decoder uploads can change the picture between menu submissions because
that picture is read live; its surrounding native pixels must stay unchanged.
An explicit native clear or reset ends retention too. This keeps the black
caption band from alternating with the full native panel during strip uploads.
`capture.txt` records `cinema`, `cinema_faces`, `cinema_actor`, `caption_packets`
and each caption packet's `subtitle_actor` for paired native/eye verification.
`cinema_native_same` compares the actual visible native pixels across latches.

The d00a water view needs a separate eye image before its native feedback pass.
I render the scene into an sRGB attachment for each eye, then replay the owned
`wt_view.c` strips with their original UVs and colour. Sampling decodes sRGB;
I return to display RGB for the native colour multiply once, then encode the
result normally. The strips sample the physical cinema plane through that eye's
matrix, rather than copying the final native framebuffer over both eyes.
Credits, native letterboxing and captions stay after that pass. A dedicated
water-clear witness identifies the producer's `tile2` at OT255; only source
geometry after that clear and before its water strips enters the eye image.
Unknown ownership/order or an unavailable feedback attachment keeps the full
native panel. The images, sampler, descriptors and framebuffers are released
with the VR renderer. Ordinary gameplay doesn't execute this extra pass.

The C4 d00a check exposed the old feedback problem: its visible scene and credits
both had a 16-pixel eye shift with zero aligned RGB difference, despite 983
submitted source faces. C2 and C4 native VBlank-6150 scanouts were byte-identical
(SHA-256 `c82a4237df6e2008a2395fe2b94cba8a2eeb3b61f14c9370480912e57491a267`).
The offscreen renderer now also saves `forward-scene-left/right.ppm` before the water
pass so the authored camera can be checked directly. `water_actor`,
`water_strips`, `water_feedback` and `water_supported` describe the actual
submission. The same native checkpoint/capture remains the before/after check;
credits should keep plane disparity while geometry behind them has less.

The C5 page capture showed why display selection matters: native page0 held
an upward close-up while displayed page320 still held the dock shot. The stereo
camera matched page0 correctly, but that shot wasn't on the native display yet.
I now keep two bounded scripted histories, one per draw page, with their source
camera, UI and 1MiB native texture/CLUT snapshot. Only the displayed page can be
selected, and its visible native pixels must still match. An unseen page after
a CPU-only load stays on the native panel. A page flip selects the matching
observed history; an ownerless OT, context change, clear or timeline reset
invalidates it. Movie uploads keep their separate live-picture policy.
`cinema_display_page` reports the selected page (or -1 for native fallback),
and `cinema_page_history` reports the observed-page bitmask. The unchanged C5
water pass measured 11-pixel scene/stairs disparity against 16-pixel credits,
with identical coincident eyes. Display-aligned stereo is checked after a native
page flip, rather than treating a paused backbuffer as the displayed shot.

The focused checks are `MgsVrCinema.*`, `MgsCinemaProducer.*` and
`MgsSourceMeshTest.ScriptedCinemaLatchesAuthenticatedProducerWithItsActualSceneCamera`
and `MgsSourceMeshTest.CinemaWaterFeedbackRequiresOwnedClearThenActualSourceThenStrips`,
plus `MgsSourceMeshTest.CinemaHistoryFollowsNativeDisplayPageCameraUiAndTextureSnapshot`.
They cover composition, bounded stereo depth, native glyph ownership on both
framebuffer pages, caption-off/clear and pause-label exclusions, source-camera
latching and title/Codec rejection. The GPU recorder tests compare native VRAM and guest RAM before and after
capture, including the displayed page, camera, UI and texture snapshot.

I checked the retail d00a opening on the Frame from a native VBlank-6100
checkpoint. At VBlank 6150 the renderer selected displayed page 320 with 907
source faces and projection 397. The native scanout stayed byte-identical to the
baseline. Coincident eye images matched exactly. The scene and stairs matched
between separated eyes at an 11-pixel shift, while the credits stayed at the
16-pixel screen plane. A normal-paced continuation to VBlank 6400 selected the
matching page-320 shot with 667 faces and projection 200; its coincident eyes
also matched. This covers the director's framing and real depth behind the
picture. The sampled opening has credits; dialogue glyph ownership and band
placement are covered by the focused tests. Movie strip uploads have their own
GPU-path check.

To repeat the opening check, use an isolated runtime config and card copies.
Point it at the imported USA v1.0 media and keep its savestates outside the
ordinary profile:

```sh
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_HEADLESS=1 \
PS1_MEMORY_CARD_DIR="$profile/cards" \
PS1_DIAGNOSTIC_SAVESTATE_DIR="$profile/savestates" \
PS1_INPUT_SCRIPT=config/mgs-usa-gameplay.input \
PS1_VBLANK_PERIOD_US=1000 PS1_AUTOSAVE_VBLANK=6100 \
PS1_EXIT_AFTER_AUTOSAVE=1 "$runtime" --config "$profile/runtime.toml"

SDL_AUDIODRIVER=dummy PS1_MGS_MESH_REPLAY=1 \
PS1_MGS_ASSET_CACHE="$media/assets.mgsc" \
PS1RECOMP_VULKAN_SHADER_DIR="$shaders" \
PS1_MEMORY_CARD_DIR="$profile/cards" \
PS1_DIAGNOSTIC_SAVESTATE_DIR="$profile/savestates" \
PS1_INPUT_SCRIPT=config/mgs-usa-gameplay.input \
PS1_AUTOLOAD_VBLANK=1 PS1_VBLANK_PERIOD_US=16667 \
PS1_VR_CAPTURE_VBLANK=6400 PS1_EXIT_VBLANK=6500 \
PS1_NATIVE_DISPLAY_DUMP_DIRECTORY="$capture/native" \
PS1_NATIVE_DISPLAY_DUMP_FROM=6398 PS1_NATIVE_DISPLAY_DUMP_TO=6402 \
PS1_NATIVE_DISPLAY_DUMP_STEP=1 \
"$runtime" --config "$profile/runtime.toml" --vr-headless "$capture/eyes"
```

Keep `PS1_VR_HEADLESS_RENDER_HZ` unset for image captures. Check the actual
VBlank in `capture.txt`, match its native image, and require the selected
`cinema_display_page` to match the native scanout. An unseen page should keep
the complete native picture until a matching source has been observed.

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

## Camera and HUD Checks

I checked these changes on 2026-11-12 using an isolated copy of the native
Slot 1 save: `s01a`, story 18. The original memory cards stayed unchanged.
The lower first-person camera retained 3,102 world faces and 260 body faces.
Forward, down60 and down85 eye captures kept the native body visible without
blocking the forward view. The diorama retained 3,183 world faces and showed
the heliport from the corrected native Up direction.

The radar recorder retained the native `C8C8C8` TILE1 at `(589,42)` on page
320. Its projected white point was missing from the old eye image and present
in the new one. The GPU regression also checks page 0 and the native blink.
Holding L2 in the same gameplay process reproduced the selector bug: the old
build switched to cinema, while the new build kept 3,102 world faces behind
the item icons with native pause level 4.

For the alert check, I used
`scripts/fixtures/frame-vr/heliport-alert.mgsr` from the loaded checkpoint.
It walks around the entry crate through normal pad input and stops at the
first native alert. It doesn't change RAM. The route reached alert mode 3
after 909 VBlanks. I replayed its recorded pad intervals from
`scripts/fixtures/frame-vr/heliport-alert.input` for the eye checks. Captures
60 and 120 VBlanks after that alert retained
3,183 world faces. This guard route stayed in 3D in both builds; it didn't
reproduce the reported camera-cut variant. The separate view-policy check
covers ordinary camera/menu hints and keeps native input guards unchanged.
The final build passed all 24 focused view/movement checks on the Frame,
including whole-RAM invariance for both input hooks through those hint states.

Run the route with `scripts/mgs_tas.py record --native-route` using an isolated
profile, copied cards and `mgs_campaign.terminal_checkpoint()` from the load
session. `states/quick_slot_0.ps1s` in that session is its initial boot state.
For repeatable eye checks, use `--vr-headless`, `PS1_MGS_MESH_REPLAY=1`, the
imported asset cache, bundled shader directory and process-local dummy audio.
Use a 16,667 us VBlank period and keep native display dumps around the chosen
capture VBlank. I used a 235-VBlank warmup after the loaded checkpoint, then
held L2 from offsets 185 to 335 and captured the selector at offset 275.
Inspect the reported `panel_latch`, `world_latch` and `player_witness`; loading
clears source snapshots, so an early panel-only capture isn't a gameplay check.

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

The earlier collar correction moved the eye origin to local (0,100,60)mm on head model 6: 100mm above
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
sets that camera bit during wall interaction. For wall actions, I allow it with
authenticated Snake wall status and the USA wall idle/move/crouch callbacks
`0x80055040`, `0x80055134`, `0x800552B8`. I matched those retail callbacks
against the pinned
[`wall helpers`](https://github.com/FoxdieTeam/mgs_reversing/blob/1b25ad9babdbefa2a519d20d0f9c3027e19ae517/source/chara/snake/sna_init.c#L3289-L3420).
The ordinary-action camera-hint policy doesn't broaden wall admission.
Other authored-view bits and pause checks still apply there. Wall actions keep
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
| Left Stick / D-Pad | Table-Relative Move / Native Navigation | Head-Relative Move / Native Navigation |
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
and a rotated presentation head pose. The camera/control baseline passed
14 tests on the Frame; the target now has 26 tests, including the arm and
contact checks below. That baseline runtime suite passed 1,366 tests with
32 optional skips; the controller
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

## Arm IK Checks

The focused host target is `mgsVr_tests`. `MgsVrArms.*` checks native segment
lengths under crossed, extended, raised, lowered and rolled targets; continuous
elbow motion; tracking loss; invalid input; recenter/camera coordinates; rigid
attachments; and the production corner transform used for KMD seams.
`MgsVrMovement.ArmCaptureAuthenticatesHierarchyAndLeavesGuestMemoryAlone`
checks the producer's hierarchy and authored-action gate without guest writes.
`MgsVrMovement.NativeShotUsesWristAndRestoresHazardWorkRegions` exercises the
production bridge, with a dispatcher test hook for clear, clamped and failed
native queries. It checks matrix placement, CPU/GTE, scratch/stack/cursor
restoration, entry rejection and clearing an obstruction without another shot.
This test hook does not prove the retail hazard routine; the saved-game run
must exercise that routine as well. All 26 focused VR tests passed on the Frame.
The retail hazard and rendered arm checks are separate from that result.

For the existing offscreen stereo capture command, add
`PS1_VR_DIAG_HANDS=forward` and `PS1_VR_FIRST_PERSON=1`. The other hand cases are
`crossed`, `up`, `down`, `extended`, `rolled` and `lost`. This input is accepted
only by the offscreen path. Optional `PS1_VR_DIAG_HAND_YAW/PITCH` values
set both grip orientations in radians, using absolute game yaw and upward
pitch. Optional `PS1_VR_DIAG_RIGHT_X/Y/Z` values override individual right-grip
tracking coordinates in metres (+X right, +Y up, -Z forward). Invalid and
nonfinite values are ignored. It drives the production solver and vertex update,
not a separate proxy mesh. `arms.txt` records native and solved joints, owner
addresses, weights and the affected vertex count. The existing body geometry
dump uses the solved positions as well. Compare the same saved game and latch
with the variable unset for the native-animation baseline.

The fade regression is
`MgsVrArms.TrackingFadeKeepsBentJointsAndVerticesContinuousThroughNativeReturn`.
It transports a bent arm around the shoulder at 72, 90 and 120 Hz, then
measures joint and production vertex-delta steps through the final return to
native. The same input also covers a straight native arm. Its `[arm-fade]`
lines report maximum and final joint/vertex steps in game units.
`MgsVrArms.StraightNativeArmHasNoResidualBendAsTrackingWeightVanishes` checks
the small-weight limit and prints `[arm-fade-limit]` errors. Run these unchanged
against the previous solver and the correction for the before/after result.

The compiled production-header comparison measured final bent-arm elbow steps
at 72/90/120 Hz of 318.37/317.368/317.45 mm before the correction and
35.8323/35.8323/13.9196 mm after it. The corrected run had no segment-length
or continuity-bound violations, compared with 23/26/39 before. At a tracking
weight of 0.0001, elbow error fell from 30.0098 to 0.0241655 mm and wrist
error from 3.00041 to 0.069977 mm. The test inputs and acceptance bounds live
in those two unchanged regression cases. Repeat them with:

```sh
./mgsVr_tests --gtest_filter='MgsVrArms.*'
```

## Heliport HUD Check

I reproduced the s01a/story18 Slot 1 plate at VBlank 1626. The native scanout
was normal. All 88 tiles came from the authenticated Panorama actor0x173480,
so I exclude that surface from the VR HUD. I keep the radar's small rectangle
on the HUD. Full-page untextured blends now cover each eye in packet order,
including fadeio.c's subtractive TILE. These dedicated pipelines keep the four
native blend operators; the renderer still blends in linear colour space.
Textured STP, other world translucency and native/cinema output keep their
existing paths.

`MgsVrView.CapturedHeliportFadeCoversEyesButRadarStaysOnHud` covers both draw
pages and the captured fade/radar geometry. `MgsVrView.EyeOverlayPipelineRetainsNativeBlendOperators`
checks the blend configuration used by those pipelines. The source-mesh test
`MgsSourceMeshTest.PanoramaOwnsAffineProjectionAndCompleteTileDomain` checks
HUD rejection after the production sphere actor binder authenticates the tile.
The same Slot 1 checkpoint rendered at VBlanks 1626 and 1686 on Turnip.
Both captures rejected 88 panorama tiles and retained 3,183 source/world
faces, the native scene and the radar. The eye-wide fade count was one at
1626 and zero at 1686. The centre plate was gone in both. Before the fix,
all 79,200 pixels in the centre ROI (X=160..489, Y=270..509 of the 768-pixel
eye) were RGB (10,11,17); neither corrected capture contained that uniform
plate colour there. Most of that region is now the heliport's black sky.
The source-mesh production regression also passed on the Frame.

To repeat, load a copy of the native s01a/story18 save in an isolated profile,
wait for control, and checkpoint at VBlank 1566. Restore that checkpoint with
`PS1_VR_FIRST_PERSON=0` and `PS1_VR_CAPTURE_VBLANK=1626`, then repeat at 1686.
Use the ordinary `--vr-headless` capture and a native scanout capture of the
same latch. Check the panorama/overlay counts, the visible radar and absence
of the centre rectangle. Keep the original memory cards unchanged.

`arms.txt` now also records the actual mapped vertex-buffer count, mismatches
and CPU/submitted hashes. The per-display arm solve updates that buffer after
the native scene upload. A pass has zero mismatches and equal hashes; compare
rendered hand and weapon positions across synthetic hand poses as well.

The Frame dock captures at VBlank 16060 used forward and crossed grip poses
against the same checkpoint. Both submitted 624 arm vertices with zero
CPU/mapped-buffer mismatches and equal hashes. The rendered native forearms
and hands moved with those poses; torso geometry and radar stayed visible.
Coincident eyes matched byte for byte. Forward eye separation changed 41.0%
of RGB channels by more than 6/255, and crossed poses changed 48.8%. Comparing
the two left-eye poses changed 6.2% of channels above that threshold. These
are offscreen production captures, separate from physical controller input.

## Earlier Tracked Firearm Checks

These measurements describe the tracked-arm implementation before I disabled
it. Current production rendering and firing use native arms and muzzle position.

The VBlank 16082 trace exposed a gap in the original movement-only arm gate:
Snake's callback was `0x800599AC` with `PLAYER_ATTACK` (`0x800`), so capture cleared the
hand sample before NewBullet. I matched that USA callback in generated
part048 and the retail EXE to
[`sna_anim_shoot_weapon_helper_80057590`](https://github.com/FoxdieTeam/mgs_reversing/blob/217509eae471629b1a53633b816c61adfef9ed5c/source/chara/snake/sna_init.c).
The match uses the attack check, idle/setup branches, padCross/table8 branch,
A38 countdown and 256-direction threshold followed by the run-gun transition.
The USA entry words are `27bdffd8 afb00018 00808021 24040800 afbf0020
0c0142c1 afb1001c 1440000b` in the same SHA-256 `3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32` USA image used for HZD.

I admit this callback for SOCOM weapon 0 and FAMAS weapon 1, with all eight
entry words checked in both paths. This includes its normal exit: ATTACK
clears before the same callback resets the action and starts idle. I keep the
published wrist and learned muzzle offset through that interval. Pause,
authored status, tracking and context resets still invalidate them. The pinned weapon
factory table gives both the same gun state function and shoot-action setup.
The arm producer and weapon bridge share that predicate. Locomotion keeps its
old allowlist; the control hook can refresh the muzzle constraint without
changing native movement in this action. Other weapons and unverified
callbacks keep their native behavior.

`MgsVrMovement.RetailFirearmFiringKeepsTrackedArmsAndShotWithoutOverridingMovement`
uses the captured control `0x8016CA80`, body `0x8016D4D0`, handler and status through
production skeletal capture and the NewBullet bridge. It also checks wall,
unverified-handler, non-playable, grenade and corrupted-entry rejection. A
continuous firing, attack-cleared, idle sequence checks that a learned muzzle
offset still clamps and rechecks the wall, then recovers clear without another
shot. The FAMAS case uses
the same handler and its authenticated NewBullet caller `0x800682D8`.
The dispatcher remains a test hook here; the retail clear/blocked shot capture
is still the native hazard acceptance check. `mgs.vr.bullet_boundary` now
reports `weapon_gate` separately from the unchanged `snake_gate` movement gate.


The corrected ARM64 runtime is SHA-256
`818a038d404647e50b542c9161b19bfbb2538dc1405c137c3938d2bb2b6a8a8f`.
All 26 focused VR tests passed on the Frame. Native SOCOM and FAMAS shots
used the tracked wrist, with Q12 direction (3969,1013,0) for supplied yaw
pi/2 and pitch 0.25. The real hazard routine detected the wall and accepted
the shortened muzzle after a second, clear segment check. Every saved query
reported restored work regions and CPU context. SOCOM submitted 744 arm
vertices and FAMAS 834, with no CPU/mapped-buffer differences.

The stationary SOCOM run fired once at VBlank 16082. After the normal-exit
correction, 25 clamped muzzle updates continued through VBlank 16129; before
it, they stopped at 16091. The VBlank 16130 render keeps the right hand and
gun on the near side of the rock. A neutral right-grip X=0 run supplied a
clear native shot and 13 unclamped updates, all restored successfully.


## Wall Contact Continuity

The original segment clamp pulled along the line from the animated native
muzzle to the controller target. During back-away movement, its added
correction reached 74.75 mm, including 65.51 mm along the wall. Comparing
accepted minus requested positions cancels camera/body translation; the
163.69 mm world-position step also contained 123.69 mm of requested movement.

I matched the USA wall-hit store in `TestSegment` at `0x80028F9C` and the
floor-hit mask at `0x8002990C`. The former retains a KSEG0 `HZD_SEG` pointer
at scratchpad offset `0x64`; the latter clears bit 31. A valid wall record has
two `{x,z,y,height}` endpoints. I derive its vertical plane, orient its normal
toward the original muzzle, and project only along that normal. The inset
ramps at contact to avoid adding a sudden 20 mm displacement. The native
reach solve and final hazard query still decide whether I can use the pose.
Invalid, degenerate and floor-tagged records keep the previous fallback.

`MgsVrMovement.WallContactPreservesTangentAndCrossesContinuously` replays all
14 captured requests through the production bridge after one learned shot.
Its Frame result was 0.00101 mm maximum tangent error and 0.50012 mm maximum
step during a dense 0.25 mm approach/release, below the 1.25 mm bound. It also
checks the safe side, native recheck and floor-tag fallback.

I repeated the native recovery command with the same dock fixture and inputs:

```sh
printf '16005 16010 start\n16015 16020 square\n' > "$capture/input.txt"
# Use the isolated SOCOM savestate/profile prepared above.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
PS1_AUTOLOAD_VBLANK=1 PS1_EXIT_AFTER_LOAD_VBLANKS=180 \
PS1_DIAGNOSTIC_SAVESTATE_DIR="$profile/savestates" \
PS1_INPUT_SCRIPT="$capture/input.txt" PS1_VBLANK_PERIOD_US=16667 \
PS1_MGS_MESH_REPLAY=1 PS1_MGS_ASSET_CACHE="$media/assets.mgsc" \
PS1_VULKAN_HEADLESS_SURFACE=1 PS1_VULKAN_REQUIRE_HARDWARE=1 \
PS1_VR_FIRST_PERSON=1 PS1_VR_DIAG_HANDS=forward PS1_VR_DIAG_RIGHT_X=0.25 \
PS1_VR_DIAG_HAND_YAW=1.57079632679 PS1_VR_DIAG_HAND_PITCH=0.25 \
PS1_VR_HEADLESS_AIM_YAW=1.57079632679 PS1_VR_HEADLESS_AIM_PITCH=0.25 \
PS1_VR_HEADLESS_MOVE_YAW=0 PS1_VR_HEADLESS_MOVE_X=0 PS1_VR_HEADLESS_MOVE_Y=-1 \
PS1_VR_CAPTURE_VBLANK=16060 PS1_DIAGNOSTIC_IGNORE_INPUT=0 \
PS1_TRACE_EVENTS=mgs.vr.bullet_boundary,mgs.vr.muzzle_segment,mgs.vr.muzzle_resolved,mgs.vr.tracked_shot \
PS1_TRACE_JSONL="$capture/trace.jsonl" \
build/ps1Runtime/ps1Runtime --config "$profile/runtime.toml" --vr-headless "$capture"
```

The runtime's existing movement diagnostic begins at VBlank 16028. This run
fired one tracked SOCOM shot, used eight wall contacts with normal Q12
(0,0,-4096), and finished unclamped after backing away. Ammo went from 12 to
11; every native query restored its work regions and CPU context. The 12
resolved samples had zero tangent correction, compared with 65.51 mm before.
The capture submitted 744 arm vertices with zero mapped-buffer mismatches.
Coincident eyes matched exactly; separated eyes differed in 43.4% of RGB
channels at a 6/255 threshold. The source renderer retained all 276 body faces.

For the final broad runtime gate, run `ps1Runtime_tests` once on the Frame
from this same build, after committing the verified source. A pass has no
failures; optional backend/data-dependent checks report their own skips.
