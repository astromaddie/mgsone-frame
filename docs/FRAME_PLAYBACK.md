# Frame Playback Checks

The Frame launcher opens VR with no arguments. `--flat` runs the flat game.
The existing `MGSOneFrame` library entry forwards to the `MGSOneVR` package,
so both entries use the same VR runtime. Each session log starts with its
presentation mode and records that mode in `last-session.json`.

## Audio Queue

I found short runs of extra silence in SDL's output while the game held close
to 60 guest ticks per second. The nonzero samples matched the SPU's generated
audio byte for byte. The missing reserve between guest audio bursts and SDL
callbacks caused the gaps.

The playback queue now waits for three callback buffers before its first read.
With the default 1,024-frame buffer at 44.1 kHz, that's about 70 ms of audio.
It primes again after loading a state or exhausting the queue. Guest audio
generation and the game clock keep running independently of playback.

The metrics distinguish initial buffering from an underrun:

- `spu.playback_priming_sample` counts silence while the reserve fills.
- `spu.playback_underrun_sample` counts missing samples after playback starts.
- `spu.playback_queued_sample` gives the remaining queue depth in stereo frames.

## Prepare a Dock Checkpoint

Use the isolated profile and scripted dock route in [Steam Frame](STEAM_FRAME.md).
Keep tests out of the player's save folder. The accelerated VBlank 16000
checkpoint can contain unconsumed SPU clock credit. Don't use that backlog to
measure normal audio playback. Restore it at normal speed, resume with
`16005 16010 start`, and save again after it has settled:

```sh
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_HEADLESS=1 \
PS1_DIAGNOSTIC_IGNORE_INPUT=1 PS1_MGS_ASSET_CACHE="$media/assets.mgsc" \
PS1_AUTOLOAD_VBLANK=1 PS1_INPUT_SCRIPT="$scratch/resume.input" \
PS1_AUTOSAVE_VBLANK=16620 PS1_EXIT_AFTER_AUTOSAVE=1 \
"$runtime" --config "$profile/runtime.toml"
```

## Render Both Eyes Without the Headset

`--vr-headless` normally captures four pairs of 768×768 images and exits.
`PS1_VR_HEADLESS_RENDER_HZ` instead renders both 1728×1728 eyes continuously
at 72, 90 or 120 Hz. It uses the normal eye drawing code without copying
images back to the CPU. This is an offscreen workload check; it doesn't
include SteamVR compositor cost.

```sh
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_IGNORE_INPUT=1 \
PS1_MGS_ASSET_CACHE="$media/assets.mgsc" PS1_MGS_MESH_REPLAY=1 \
PS1_AUTOLOAD_VBLANK=1 PS1_EXIT_AFTER_LOAD_VBLANKS=600 \
PS1_VR_HEADLESS_RENDER_HZ=90 PS1_BENCHMARK_REPORT=1 \
"$runtime" --config "$profile/runtime.toml" --vr-headless "$scratch/eyes"
```

The guest should hold close to 60 ticks per second. For counter checks,
`PS1_METRICS` records `vr.headless_world_faces` and `vr.headless_stereo_pair`.
Leave global metrics off when measuring cadence: instrumenting the guest's
hot paths adds work. The tap below measures actual Vulkan submissions after
an initial two-second warmup, including viewport size and vertices per submit.
Initial audio priming is expected; steady playback underruns aren't. Keep
the separate identical-eye, separated-eye and turned-view image checks from
[VR Architecture](VR_ARCHITECTURE.md).

## Capture the Actual Audio Callback

The callback tap records the stereo S16 samples delivered to SDL, including
any queue padding. `PS1_SPU_CAPTURE_PCM` records generated audio before the
playback queue. Neither capture changes the device's speaker mute state.

Build the tap on Linux:

```sh
cc -shared -fPIC -O2 engine/tools/steam-frame-probe/audio_tap.c \
  -o "$scratch/audio_tap.so" $(pkg-config --cflags sdl2) -ldl
```

For the same bounded offscreen run, route PulseAudio to an owned null sink:

```sh
audio_module=$(pactl load-module module-null-sink sink_name=mgsone_audio_check)
trap 'pactl unload-module "$audio_module"' EXIT
export SDL_AUDIODRIVER=pulseaudio PULSE_SINK=mgsone_audio_check
export LD_PRELOAD="$scratch/audio_tap.so"
export MGS_AUDIO_TAP="$scratch/callback.pcm"
export PS1_SPU_CAPTURE_PCM="$scratch/generated.pcm"
```

Compare complete stereo frames. After allowing for initial buffering and the
unplayed tail, the nonzero sample sequence and the silence between those
samples should match the generated PCM. A zero-only frame is two zero-valued
16-bit samples. Matching only the nonzero samples would miss inserted silence.
The tap also reports the callback count and its longest execution time.
Remove the null sink and diagnostic environment after the run.

```sh
python3 scripts/compare_audio_capture.py "$scratch/generated.pcm" "$scratch/callback.pcm"
```

This requires nonzero output, matching signal samples and no added or missing
silence after playback starts. It allows initial priming and the unplayed tail.

## Results

I used the same warmed dock state and 1728×1728 eye images for these runs.
Global guest metrics were off. Both runs drew 32,376 vertices per stereo
submission and used the real PulseAudio callback through a private null sink.

| Requested Eye Rate | Measured Stereo Pairs/s | Guest Ticks/s | Extra Silent Frames |
| --- | --- | --- | --- |
| 120 Hz | 113.08 | 59.25 | 4,151 |
| 90 Hz | 88.99 | 59.97 | 0 |

The 90 Hz run's delivered signal and silence intervals matched the generated
PCM. I chose 90 Hz for both MGS Steam entries. The global headset setting
stays at 120 Hz. This preserves the eye resolution and geometry.

The first-person image check captured 2,368 original faces at VBlank 16700.
Identical-pose eyes matched byte for byte. The separated eyes differed on
48.67% of pixels at a 6/255 RGB threshold. I checked the forward and turned
views for missing surfaces and depth conflicts.

The runtime suite passed 1,366 tests with 32 optional skips. The controller
suite passed all 25 tests. Python ran 211 tests with eight optional skips.
