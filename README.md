# Rockstar Editor+

Camera and rendering mod for GTA V's Rockstar Editor. Legacy (`GTA5.exe`) and
Enhanced (`GTA5_Enhanced.exe`), singleplayer and FiveM, one build.

**1 — Camera and scene**

- **Spline camera** — markers joined by a curve through every point instead of
  the stock straight line. Position, rotation and FOV. Attached markers curve in
  the parent entity's frame; look-at markers re-aim from the splined position.
- **Procedural shake** — six-axis, per marker, in degrees and hertz.
  Deterministic: same clip, same output.
- **Time of day and weather** — relight a clip at any hour, or replace the
  weather it was shot in. A clip normally replays its own clock, weather *and*
  lighting, so none of it can be changed; these are substituted as the frame
  plays and nothing is written to the `.clip`.

**2 — Limits removed**

- **Longer recordings** — the clip length limit is a memory budget, not a timer,
  which is why a busy street gives you seconds where an empty road gives a
  minute. Lifting it takes ~17 seconds of dense city to **1m 45s**. On by
  default; costs ~600 MB of reserved address space.
- **Free camera on first-person clips** — the editor normally greys out the whole
  Camera submenu on anything recorded in first person, leaving the clip stuck
  with the recorded view.
- **The rest of the guard rails** — camera distance, collision, zoom range,
  profanity check.

**3 — Rendering**

- **Render pipeline** on the Export button — video or image sequence, arbitrary
  frame rate, accumulated motion blur, project audio, no watermark. Renders every
  clip in the project.

---

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
build — the shipped ids cover every build it currently offers. And an older build
loses whichever features its patterns no longer match, keeping the rest: every
address resolves independently and each hook is guarded, so a miss disables that
one feature and says so in the log. On b3258, for instance, world-collision
disable and the profanity bypass are inert while the spline, shake, menu,
distance leash and export all work. Builds 3751 and 3889 are tested.

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
| Top-level marker menu | pages through global settings: **Curve**, **Limits**, **Scene** |
| Camera submenu | group switcher: Spline, Shake, Shake Motion, 4 advanced pages |

Pages rather than one long list because the editor's Scaleform column draws 16
rows and silently discards the rest.

**Adjust Step** sets the increment for left/right, 0.001 to 1. Changes save
automatically — global to the ini, per-marker to a side-car file.

---

## 1 — Camera and scene

How the shot moves and how it is lit: the curve between markers, the shake on
top of it, and the time of day and weather it plays in. Everything here is
authored per project — the mod's own settings, not the game's.

### Spline

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

### Shake

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

### Time of day and weather

**Rockstar Editor+ → Scene**, in the top-level marker menu. Ships off.

A clip records the clock and the weather on every frame and replays them, which
is why setting the time or forcing weather from a trainer does not survive a
single frame in the editor. These rows substitute different values as the frame
is played instead, so the change holds for the whole clip and shows up live.

| Row | |
|---|---|
| **Timecycle** | the page's master switch. **Live** re-lights the clip for the settings below; **As Recorded** keeps the clip's own lighting and greys the rest out |
| **Time of Day** | As Recorded, or any quarter hour. Relights the whole clip |
| **Weather** | As Recorded, or one of the game's 15 types |
| **Weather Blend To** | a second type to sit between |
| **Weather Blend** | 0–1 between the two. This is the engine's own transition, so half-way is real weather rather than a cross-fade |
| **Wetness** | wet roads and puddles, independent of the type. As Recorded is not the same as 0 — a clip shot in the rain has wet ground |

Weather is handed to the game's own weather code rather than forced field by
field, so clouds, wind, puddles and the snow effects all follow the type you
pick.

**Why the Timecycle row exists.** A clip does not just record the clock and the
weather — it records the *fully resolved timecycle keyframe*, every lighting
variable already evaluated for the hour and weather it was shot in. The engine
still evaluates the timecycle live on every replayed frame, then throws it away
variable by variable and puts the recorded one back. So on its own, moving the
clock to midnight moves the sun and leaves an afternoon sky; switching to RAIN
gives you rain particles over sunshine. **Live** stops that replacement, so the
freshly evaluated lighting survives.

The trade is that any timecycle *modifier* the clip carried goes with it — an
interior grade, a mission colour grade, script post FX. Set the row to
**As Recorded** to keep those and accept the baked lighting.

That is why Timecycle is the master switch rather than one setting among six:
with the recorded lighting standing, a new time of day would only swing the sun
across an unchanged sky and a new weather would drop rain into sunshine. On
**As Recorded** the other rows are greyed *and* inert — the clip plays exactly
as shot, and what the rows say matches what playback does.

> **Nothing is written to the `.clip`.** The substitution happens as the frame
> is played, so the file on disk is untouched and a clip opened without the mod
> is exactly as it was shot. That also means the look is a setting rather than
> an edit: set a row back to *As Recorded* and it is gone.

### Per-marker settings

A marker's curve, shake and per-axis values are stored beside the project rather
than inside it, because the marker struct is serialised straight into the `.clip`
file and has nowhere to put extra fields without breaking the format the game's
own loader validates. Keeping them outside means a project still opens on a
machine without this mod.

They are scoped to the **project and clip** you set them in.
`markers\<project>.txt` holds one project, with its clips as sections inside:

```
RockstarEditorPlus v6
clip 0
marker 7138 shake=1 intensity=1.35 freqMul=0.15
marker 7968 shake=1 intensity=1.35
clip 1
marker 0 orient=2
```

Fields are named and only written when set, so the file stays readable and a
lightly-edited marker is one short line. Plain text, safe to hand-edit if a
project needs rescuing.

> Scoping is by clip *index*, so reordering a project's clips carries the
> settings with the slot rather than with the clip.

---

## 2 — Limits removed

The editor is built for clips, not for shots, and most of what stops you is a
guard rail rather than a technical bound. Each of these is lifted independently
and each is a single ini key, so any of them can go back to stock on its own.

Every one resolves separately too: if a pattern does not match on your build the
mod disables **that** limit and says so in the log, keeping the rest.

### Recording length

`ReplayBlocks` in `RockstarEditorPlus.ini`. The recording ring **is** the clip:
it fills, the clip saves, and recording rolls straight into a new one — so
length is ring size divided by how fast the scene fills it. That is why the
number moves with traffic density rather than being a fixed duration.

The game ships 30 blocks, and not by preference: its settings code force-clamps
the count to 30 whenever the replay heap is under ~196 MB, and the heap is
172 MB. The heap is widened first, so the clamp stops firing.

Measured in dense city traffic:

| | ring | clip |
|---|---|---|
| 30 *(the game's own)* | 120 MB | ~17 s |
| 64 | 256 MB | ~44 s |
| **128** *(default)* | 512 MB | **~1m 45s** |

**It costs RAM.** Each block needs 4 MB plus a 384 KB thumbnail, so the default
reserves about 604 MB for the session, and clips grow to match — a 128-block
clip is roughly 230 MB on disk. That is the right trade for a tool whose whole
job is capturing footage, but lower it if memory is tight. If you shoot long,
check the editor's own disk allowance under Settings → Saving & Startup.

Above 30 depends on the heap widening succeeding. If it cannot — the patterns
did not resolve on your build, or the process cannot reserve that much address
space — the value is clamped back to 30 and the log says which. Takes effect at
the next recording, so restart before shooting.

**Under FiveM this has to happen much earlier.** The pool is sized once at
startup and can only be changed before that, and FiveM defers the mod's init to
ScriptHookV — 20 to 90 seconds later, long after the pool is committed. So the
widening runs at DLL attach instead, before anything else. Same result; the log
reports it separately (`replay heap 172 -> 604 MB ... patched at attach`) so you
can tell which path did the work.

### First-person clips

Record anything from the first-person view and the editor greys out the whole
Camera submenu — the clip keeps the recorded view forever.

`UnlockCameraRestrictions=1` (default) removes it, and those clips take a free
camera like any other.

There are two locks, not one. The menu greys the row, and separately the camera
director ignores whatever the marker asks for and forces the recorded camera —
so unlocking only the menu gives you a free camera that will not move, because
there is no free camera. Both are lifted.

The same pair of locks covers clips recorded during a cutscene or with camera
movement disabled, and those are lifted too. In practice you are unlikely to have
such a clip: recording stops while controls are disabled, which is the same
condition that flags them.


### Camera distance

`UnlimitedCameraDistance=1` (default), `MaxCameraDistance=20000`.

Stock leashes the free camera to **30 m** from the player and yanks it back —
the same value feeds the out-of-range warning and the fallback to the recorded
camera, so lifting it once covers all three.

The world still streams around the *player*, not the camera, so expect LOD to
drop off at distance. That is the trade, and it is why the leash exists.

### Camera collision

`DisableCameraCollision=1` (default).

Stock pushes the camera off geometry. Pushing through a doorframe, a windscreen
or a fence is most of what a cinematic move is, and the push-off does not just
block those — it silently bends the path away from the markers you placed, so
the mod would be fighting its own spline.

The cost is that you can end up inside solid map with no visual reference. That
is recoverable in a frame; a shot the push-off quietly ruined is not obvious at
all.

### Zoom range

`UncapZoom=1` (default), `ZoomMinFov=1`, `ZoomMaxFov=130`.

Stock allows a 10x span, 0.45x to 4.50x. That is a gameplay-camera choice, and
this is not a gameplay camera — a long lens is ordinary cinematography and the
range is the first thing anyone runs into.

The ends default to the engine's own clamp, 1° to 130° of FOV, so nothing
outside them was ever reachable. The editor's readout is `45 / fov`: 1° reads as
45x in, 130° as 0.35x out.

### Profanity check

`BypassProfanityFilter=1` (default).

Naming or exporting a project polls Social Club, and the poll times out to a
refusal when it cannot be reached — so offline you cannot name a project at all,
which is a network check standing between you and a local file.

---

## 3 — Rendering

A complete replacement for the editor's own export, driven from the same button:
arbitrary frame rate, true accumulated motion blur, project audio, no watermark,
every clip in the project.

### The renderer

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

### Capture modes

`RenderCaptureMode` in `Render.ini`. Both average `RenderSamples` real renders
into every output frame — the difference is what the world does between them.

| | Sliding *(default)* | Walking |
|---|---|---|
| the clip | plays, in slow motion | paused, seeked per sample |
| particles, TAA, SSR, RT | keep simulating | reset at every sample |
| shutter | approximate | exact midpoints |
| frames per output frame | `samples / shutter` | `2·samples + 2` |
| 64 samples @ 360° | **~1.6 s/frame** | ~4.6 s/frame |

Sliding advances to each frame's mark, then exposes N consecutive frames with
the world still running. That keeps anything with temporal history warm, which a
seeked frame cannot — every walking sample starts cold.

It is also faster at the default 360° shutter, because it spends one frame per
sample where walking also needs a settle frame. **That edge is shutter-dependent**
— it needs `samples / shutter` frames, so at 0.5 the two are level and below that
walking wins.

Walking is still the more predictable one: exact shutter placement, deterministic
frame times, and it fails obviously — a repeated frame rather than a smeared one.
Reach for it if a sliding render looks wrong, or for short shutters.

The playback speed sliding uses is measured, not configured: it times one frame
during a warm-up and solves for the speed that makes N samples span the shutter,
then holds it. The log reports what it settled on.

### Depth of field

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
| `UncapZoom` | 1 | widen the 0.45x–4.50x range to the engine's own 1–130° |
| `ReplayBlocks` | 128 | recording length, in 4 MB blocks. 3–128 |
| `UnlockCameraRestrictions` | 1 | free camera on first-person clips |
| `OverrideTimeOfDay` | 0 | relight the clip at `TimeOfDay` instead of its recorded clock |
| `TimeOfDay` | 720 | minutes past midnight. 720 = 12:00 |
| `OverrideWeather` | 0 | replace the clip's recorded weather |
| `WeatherType` | 0 | index into weather.xml's order. 0 EXTRASUNNY … 7 THUNDER … |
| `WeatherBlendTo` | -1 | second type to blend towards; -1 = none |
| `WeatherBlend` | 0.0 | 0–1 between the two types |
| `WeatherWetness` | -1 | 0–1 wet roads and puddles; -1 = as recorded |
| `LiveTimecycle` | 1 | re-light for the overridden time/weather instead of replaying the clip's baked keyframe |
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
    RockstarEditorPlus.ini          camera, shake, limits
    Render.ini                      rendering
    RE+ Render Settings.exe         editor for both of the above
    RockstarEditorPlus.log
    ffmpeg.exe                      bundled
    presets\                        codec presets
    markers\                        per-marker settings, one file per project
    Captures\render_0001\           frames, or video.mp4
```

**RE+ Render Settings** is a standalone editor for the render settings and the
encoder presets, sitting next to the files it edits. Every setting carries its
own explanation, and choosing a codec narrows the container and pixel-format
lists to the ones that actually work — a container that cannot hold the codec
makes ffmpeg refuse, and that failure is otherwise invisible. It writes
line-by-line, so the comments in the ini survive being edited through it.

Paths resolve from the `.asi`, not from the game executable — under FiveM that
puts everything in `plugins\RockstarEditorPlus\`. Set `RenderOutputFolder` to an
absolute path to write renders off the game drive.

Per-marker settings are stored outside the `.clip` file, so projects stay
loadable without this mod installed. See [Per-marker settings](#per-marker-settings)
for how they are scoped.

An ini written by an older build will not contain keys added since. Those fall
back to their defaults, so nothing breaks — but a key has to be present before
you can change it. Delete the ini to have a current one written.

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
