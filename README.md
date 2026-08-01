# Rockstar Editor+

Camera and rendering mod for GTA V's Rockstar Editor. Legacy (`GTA5.exe`) and
Enhanced (`GTA5_Enhanced.exe`), singleplayer and FiveM, one build.

- **Spline camera** — markers joined by a curve through every point instead of
  the stock straight line. Position, rotation and FOV. Attached markers curve in
  the parent entity's frame; look-at markers re-aim from the splined position.
- **Procedural shake** — six-axis, per marker, in degrees and hertz.
  Deterministic: same clip, same output.
- **Render pipeline** on the Export button — video or image sequence, arbitrary
  frame rate, accumulated motion blur, project audio, no watermark. Renders every
  clip in the project.
- **Editor limits lifted** — camera distance, collision, profanity check,
  timeline spinner, zoom range.

---

## Requirements

| | |
|---|---|
| GTA V | Legacy or Enhanced. Singleplayer or FiveM |
| ASI loader | any |
| ReShade | **rendering and DOF only**, must be the build **with full add-on support** |
| `IgcsConnector.addon64` | bundled — capture bridge, rendering and DOF only |
| `ffmpeg.exe` | bundled — video output only |

Camera and shake need only the ASI loader.

ReShade is required for rendering because an ASI cannot read the GPU frame
buffer; the add-on does that. The ordinary ReShade build cannot load add-ons at
all — `IgcsConnector.addon64` is then ignored and Export falls back to the game's
watermarked encoder with no indication why. ReShade itself is not bundled.

### FiveM

Supported. Same build, `plugins\` folder, nothing to enable. Startup routes
through ScriptHookV there (the usual deferred-init detour cannot be placed in
FiveM's address space); behaviour is otherwise identical.

Two constraints. FiveM refuses any plugin that does not declare the running game
build — the shipped ids cover every build it currently offers. And a build whose
signatures have moved leaves the mod dormant rather than misbehaving:
`no signature set for this build` in the log. Builds 3751 and 3889 are tested.

---

## Installation

Extract into the game folder:

```
<game>\RockstarEditorPlus.asi
<game>\RockstarEditorPlus\                     settings, presets, ffmpeg
<game>\IgcsConnector.addon64                   capture bridge (ReShade add-on)
<game>\reshade-shaders\Shaders\IgcsDof.fx      depth-of-field shader
```

No configuration needed before first launch. The `.ini` files are shipped rather
than generated so you get the commented versions.

For rendering, additionally install ReShade **with add-on support**. The bundled
`ffmpeg.exe` takes precedence over any on PATH, so renders encode identically on
every machine.

---

## Menu

All settings are in the editor. Open a marker's menu and find the
**Rockstar Editor+** row.

| Where | Behaviour |
|---|---|
| Top-level marker menu | pages through global settings: **Curve**, **Limits** |
| Camera submenu | group switcher: Spline, Shake, Shake Motion, 4 advanced pages |

Two pages rather than one because the editor's Scaleform column draws 16 rows
and silently discards the rest.

**Adjust Step** sets the increment for left/right, 0.001 to 1. Changes save
automatically — global to the ini, per-marker to a side-car file.

---

## Camera

On by default; takes over markers set to **Smooth** blend.

| Setting | |
|---|---|
| Spline Path / Rotation / Zoom | which channels the curve drives |
| Curve Shape | Uniform / Centripetal / Chordal — tension |
| Speed Profile | Natural / Continuous / Per Segment — pacing |

**Speed Profile**

| | |
|---|---|
| **Natural** (default) | path driven from marker times, no arc-length correction. The curve's own parameter speed carries the camera — eases into tight bends, flows through open ones |
| Continuous | one speed curve fitted across all markers. Even and metered |
| Per Segment | each segment paced independently |

**Curve Shape** — how tightly the curve is pulled around each marker. Uniform
gives long tangents and wide turns; Chordal (default) spaces knots by chord
length; Centripetal sits between them.

Centripetal is the usual textbook recommendation because it provably never
overshoots between markers. That guards against the wrong failure here: it
shortens the tangent wherever a neighbouring marker is close, which at a
**reversal** leaves the camera no momentum to sweep around, so it overshoots the
marker and hooks back across its own path. Measured on a 160° reversal, distance
travelled over distance covered was **1.11 at Uniform, 1.56 at Centripetal, 3.59
at Chordal**. If a shot loops at a marker it doubles back through, try Uniform.

With evenly spaced markers that never reverse, all three are indistinguishable.

> **Curve Shape has no effect under Natural pacing**, the default — a
> time-parameterised Hermite has no knot spacing to choose. Switch to Continuous
> or Per Segment for tension to apply.

Per marker: override curve shape, add ease-in/ease-out, or force stock behaviour
for one segment.

---

## Shake

Set a marker's stock **Shake** row to **Rockstar Editor+**.

**Simple mode** (default) — two real units, shared with the sibling Simple
Camera project, so values transfer between them.

| Setting | |
|---|---|
| Shake Amplitude | degrees of rotation; 5 cm of translation per degree |
| Shake Frequency | hertz. 0.35 ≈ a three-second wander, 0.20 slower |
| Shake Variation | swell and settle over time. 0 = statistically flat |

Handheld starting point: **amplitude 2.0–2.5 at frequency 0.20**.

**Motion coupling** — off by default. `Motion → Intensity` at about 1.0 is the
single biggest step towards a shake that reads as an operator rather than an
effect. `Motion → Speed` scales rate the same way; `Stop When Still` fades out
when the camera is parked.

**Complex mode** drives the two layers directly — slow **Sway**, fast
**Jitter**, each with movement, rotation and rate — plus per-axis weights on all
six degrees of freedom.

**Apply Shake to All** on any shake page copies that marker's shake to every
marker in the project. Shake only; curve and easing are untouched.

> Shake is a function of clip time, so it is frozen while the playhead is.
> Play or scrub to judge it.

---

## Rendering

Triggered by **Export**. Nothing else starts it.

**Ships off** — set `EnableRenderer=1` in `Render.ini`. It is opt-in because it
is the one feature that cannot coexist: diverting the bake means other export
tools (EVE, EVER) never fire. They do not error, they silently never run.
Everything else the mod does is additive.

**Output** — `RenderMode=Video` (default) hands frames to ffmpeg as they finish
and deletes them, so a long render costs a couple of files at a time rather than
thousands; project audio is recorded and muxed from the same Export press.
`RenderMode=Frames` writes a numbered PNG/JPEG sequence with an `assemble.txt`
of ready-made ffmpeg command lines, and no audio.

The capture is identical either way. Codecs come from `presets\` — `h264`,
`h265`, `nvenc_hevc`, `prores_hq`, `lossless` — selected with
`RenderVideoPreset=h265`, or write ffmpeg arguments directly into
`RenderVideoArgs`. Edited presets persist; a deleted one is rewritten.

**Motion blur** — 64 samples at a 360° shutter by default. Every sample is a
real render at a real instant, so this is true accumulation, not a screen-space
smear. Samples cost render time and nothing else, and 64 is genuinely expensive
— 64 captures per output frame. Drop `RenderSamples` to 3 while setting a shot
up. `RenderShutter=0.5` is the 180° film convention.

**Audio** — `RenderAudio=1` (default). One Export press does two passes: the
project plays through once at normal speed to record sound, then rewinds to clip
one and renders frames. Muxed at the end. Requires `RenderMode=Video`.
Alternatively point `AudioFromFile` at a stock export of the same project.

**Duration** — rendering seeks frame by frame and is far slower than real time.
Progress and an estimate go to the log every 15 seconds.

---

## Depth of field

The capture add-on can walk the camera around a lens aperture and blend the
result — real optical bokeh rather than a screen-space approximation.

Started from **ReShade's own overlay**, the IgcsConnector depth-of-field panel.
There is no switch in the editor. Requires:

- the editor open, on a free-camera marker
- **playback paused** — a session integrates one instant
- `IgcsDof.fx` in the ReShade shaders folder (bundled)

For static shots, and there is nothing to configure. The replay clock never
moves during a session, so every sample sees the same instant from a different
point on the lens. Motion blur belongs to the renderer, where the shutter is
exact and does not compete for a sample budget.

---

## Settings

Both files live in `RockstarEditorPlus\`, beside the `.asi`.

### `RockstarEditorPlus.ini`

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = no hooks, stock game |
| `SplinePosition` `SplineOrientation` `SplineFov` | 1 | channels the curve drives |
| `SplineAttached` | 1 | also curve shots mounted on an entity |
| `LookAtReaim` | 1 | re-aim look-at markers after the curve moves them |
| `LookAtAspect` | 0 | aspect for that re-aim; 0 = display's |
| `OnlySmoothBlend` | 1 | 0 also takes over Linear markers |
| `NaturalPacing` | 1 | overrides the two below |
| `SmoothSpeedProfile` | 1 | one speed curve across markers |
| `ArcLengthRemap` | 1 | per-segment fallback, used when both above are 0 |
| `Alpha` | 1 | tension: 0 uniform / 0.5 centripetal / 1 chordal |
| `ShakeSimpleMode` | 1 | 0 = Sway/Jitter model |
| `ShakeAmplitude` | 1.0 | degrees |
| `ShakeFrequency` | 0.35 | hertz |
| `ShakeVariation` | 0.7 | 0 = flat |
| `ShakeSpeedAmp` `ShakeSpeedFreq` | 0.0 | motion coupling. Try 1.0 / 0.5 |
| `ShakeStopWhenStill` | 0 | fade out when parked |
| `ShakeAxis*` | 1.0 | per-axis weights; 0 disables an axis |
| `UnlimitedCameraDistance` | 1 | lift the 30 m leash |
| `MaxCameraDistance` | 20000 | metres |
| `DisableCameraCollision` | 1 | pass through geometry |
| `BypassProfanityFilter` | 1 | naming/export works with Social Club offline |
| `HideEditorSpinner` | 1 | no spinner while scrubbing |
| `UncapZoom` | 0 | widen the 0.45x–4.50x range |
| `ShakeDebugLog` `SplineDebugLog` `SplineTraceLog` | 0 | diagnostics |

### `Render.ini`

| Key | Default | |
|---|---|---|
| `EnableRenderer` | 0 | master switch; 0 restores the stock exporter |
| `RenderMode` | Video | `Video` or `Frames` |
| `RenderFps` | 30 | output rate |
| `RenderSamples` | 64 | motion-blur samples; 1 = none |
| `RenderShutter` | 1 | shutter angle; 0.5 = 180° |
| `RenderJpeg` `RenderQuality` | 0 / 90 | JPEG instead of PNG |
| `RenderKeepFrames` | 0 | Video mode: keep frames too |
| `RenderVideoPreset` | | a name from `presets\` |
| `RenderChannelOrder` | 0 | 0 auto / 1 RGBA / 2 BGRA |
| `FfmpegPath` | | empty = bundled, then beside the exe, then PATH |
| `RenderAudio` | 1 | record and mux project sound |
| `RenderHideHud` | 1 | hide the editor HUD while rendering |
| `RenderOutputFolder` | | empty = `RockstarEditorPlus\Captures\` |

---

## File layout

```
GTA5.exe
RockstarEditorPlus.asi
RockstarEditorPlus\
    RockstarEditorPlus.ini      camera, shake, limits
    Render.ini                  rendering
    RockstarEditorPlus.log
    ffmpeg.exe                  bundled
    presets\                    codec presets
    markers\                    per-marker settings
    Captures\render_0001\       frames, or video.mp4
```

Paths resolve from the `.asi`, not from the game executable — under FiveM that
puts everything in `plugins\RockstarEditorPlus\`. Set `RenderOutputFolder` to an
absolute path to write renders off the game drive.

Per-marker settings are stored outside the `.clip` file, so projects stay
loadable without this mod installed.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| Export gives a normal watermarked video | Capture add-on not found — the mod stepped aside rather than produce the wrong thing. Either ReShade was installed without add-on support, or `IgcsConnector.addon64` is not beside the executable. The log names which |
| Red and blue swapped | Set `RenderChannelOrder` to 1 or 2 |
| Got frames, expected video | `RenderMode=Frames`, or ffmpeg not found — the log says which |
| A shake setting does nothing | Per-marker values override the ini and always win. If the menu shows a number rather than `Default`, that marker has its own. `ShakeDebugLog=1` logs what reached the camera |
| Shake looks frozen | The playhead is paused. Play or scrub |
| Nothing happens at all | Check the log. No file = the ASI never loaded; a log that stops early names the reason on its last line |

---

## Building

CMake, MSVC v143, C++20, x64. Static CRT — required, not preference: an ASI does
not choose which runtime its host already loaded, and MSVC's STL demands a
runtime at least as new as the compiling toolset. FiveM ships an older
`msvcp140` than current, which produced a crash on the first `std::mutex` lock
until the CRT was linked statically.

```
cmake -S . -B build.vs
cmake --build build.vs --config Release
```

Output in `build.vs/BIN/Release/`. The `.ini` files are generated from
`CMakeLists.txt` at configure time and copied next to the `.asi` — they are build
outputs and are not tracked.

---

## Licence

GPL-3.0-only. See [LICENSE](LICENSE).

Bundles [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause), a
GPLv3 build of [FFmpeg](https://ffmpeg.org), and
[IgcsConnector](https://github.com/FransBouma/IgcsConnector) by Frans Bouma
(add-on MIT, `IgcsDof.fx` BSD 2-clause). Licence texts and source information
ship in `RockstarEditorPlus\`.
