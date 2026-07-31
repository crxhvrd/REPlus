# Rockstar Editor+

A camera and rendering mod for GTA V's Rockstar Editor.

It gives the editor a proper spline camera, a procedural handheld shake, and its
own render pipeline behind the Export button — video or image sequence, with
real motion blur and sound. It works on both **Legacy** (`GTA5.exe`) and
**Enhanced** (`GTA5_Enhanced.exe`).

---

## Contents

- [What you get](#what-you-get)
- [Requirements](#requirements)
- [Installation](#installation)
- [The in-editor menu](#the-in-editor-menu)
- [Camera](#camera)
- [Shake](#shake)
- [Rendering](#rendering)
- [Depth of field](#depth-of-field)
- [Settings reference](#settings-reference)
- [Where files go](#where-files-go)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Licence](#licence)

---

## What you get

**A spline camera.** Markers are joined by a smooth curve that passes exactly
through each one, instead of the stock straight line. Rotation and zoom get the
same treatment. Shots mounted on a car or a ped are curved in that vehicle's own
frame, so they ride along exactly as stock does; shots locked to a target stay
locked to it, re-aimed from wherever the curve puts the camera.

**Procedural camera shake.** A handheld shake you tune per marker, in real
units — degrees and hertz. It can react to how fast the camera is moving, and it is
identical every time you play the same clip back.

**Your own render pipeline.** Export writes a video or a numbered frame
sequence, at any frame rate, with accumulated motion blur and the project's
audio. No watermark.

**Whole projects.** A render walks every clip in the project, not just the
first.

**The editor's limits lifted.** Fly further than 30 m from the player, pass
through geometry, and zoom past the stock range.

---

## Requirements

| | |
|---|---|
| GTA V | Legacy or Enhanced — singleplayer or **FiveM** |
| An ASI loader | any of the usual ones |
| ReShade | **rendering and DOF only** — must be the build **with full add-on support** |
| `IgcsConnector.addon64` | **included** — the capture bridge, rendering and DOF only |
| `ffmpeg.exe` | **included** — only used for video output |

Smooth Blend and shake need nothing but the ASI loader.

### FiveM

Supported. Drop the files into FiveM's `plugins\` folder — same build as
singleplayer, no separate download, nothing to enable. The mod takes a different
startup route there (it registers through ScriptHookV rather than its usual
deferred-init detour, which cannot be placed in FiveM's address space), but every
feature behaves the same once it is up.

`RockstarEditorPlus.log` sits beside the `.asi`, in
`FiveM.app\plugins\RockstarEditorPlus\`, and its first lines tell you which route
it took.

Two caveats. The build must be one the `.asi` declares — FiveM refuses any plugin
that does not claim the running build id, and says which id it wanted. And a
build whose signatures have moved makes the mod stay dormant rather than
misbehave; `no signature set for this build` in the log is what that looks like.
Builds 3751 and 3889 are the tested ones.

ReShade is only involved in **rendering and depth-of-field sessions**, and it is
there because an ASI cannot read the GPU's frame buffer on its own — the capture
add-on does that part. Install the variant the ReShade setup calls **"with full
add-on support"**. The ordinary build cannot load add-ons at all, so
`IgcsConnector.addon64` is simply ignored and Export falls back to the game's
own watermarked encoder with nothing to indicate why.

---

## Installation

Drag the contents of the download into your game folder, next to `GTA5.exe` /
`GTA5_Enhanced.exe`. You should end up with:

```
<game>\RockstarEditorPlus.asi
<game>\RockstarEditorPlus\                        settings, presets, ffmpeg
<game>\IgcsConnector.addon64                     capture bridge (ReShade add-on)
<game>\reshade-shaders\Shaders\IgcsDof.fx        depth-of-field shader
```

That is everything — nothing to configure before the first launch.

Everything inside `RockstarEditorPlus\` except `ffmpeg.exe` would otherwise be
created on first run. It is shipped instead so you get the commented versions,
which explain what each setting does rather than leaving you a bare list of
keys.

For rendering, also:

Install ReShade **with add-on support**. That is the only extra step — the
capture add-on, its depth-of-field shader and `ffmpeg.exe` are all in the
bundle, and the bundled ffmpeg is used ahead of anything on your PATH so
renders encode the same way on every machine.

ReShade itself is not bundled: it is a general-purpose injector with its own
installer and its own update cycle, and you very likely want to choose your own
version.

---

## The in-editor menu

Everything is reachable from the editor itself. Open a marker's menu and look
for the **Rockstar Editor+** row.

- On the **top-level marker menu** it pages through the global settings. Step
  it to move between **Curve** (spline toggles, curve shape, speed profile) and
  **Limits** (camera collision, distance, zoom). Two pages because the editor's
  menu column draws only sixteen rows and silently drops the rest.
- On the **camera submenu** it becomes a group switcher. Step it left and right
  to move between pages: Spline, Shake, Shake: Motion, and the four advanced
  shake pages.

**Adjust Step** on each page sets how much left/right moves a value, from 0.001
up to 1.

Anything you change is saved automatically — global settings to the ini,
per-marker settings to a side-car file beside it.

---

## Camera

The spline is on by default and takes over markers set to **Smooth** blend.

| Setting | What it does |
|---|---|
| Spline Path / Rotation / Zoom | which parts of the camera the curve drives |
| Curve Shape | Uniform / Centripetal / Chordal — path tension, see below |
| Speed Profile | Continuous / Per Segment / Natural — pacing, see below |

**Curve Shape** is the path's tension: how tightly the curve is pulled around
each marker.

- **Uniform** — long tangents, wide flowing turns. Loose.
- **Centripetal** — tight to your markers. The textbook choice, and the right
  one when a shot needs to hit its marks precisely rather than flow between
  them.
- **Chordal** (default) — knot spacing follows chord length.

The usual advice is that centripetal is always correct, because it provably
never overshoots between markers. That is true and it is protecting against the
wrong thing here: it shortens the tangent wherever a neighbouring marker is
close, and where a shot **doubles back** that leaves the camera no momentum to
sweep around, so it pokes past the marker and hooks back over its own path. On a
shot with a 160° reversal the camera travelled 1.11× the distance it covered at
Uniform and 3.59× at Chordal — so if a shot loops at a marker it doubles back
through, reach for Uniform first.

If your markers are evenly spaced and never double back, all three look
identical.

> **Curve Shape does nothing under Natural pacing**, which is the default. A
> time-parameterised Hermite has no knot spacing to choose. Switch Speed Profile
> to Continuous or Per Segment if you want tension to take effect.

**Speed Profile** is the pacing.

- **Continuous** — one speed curve fitted across every marker. Even and
  metered; the camera never lurches, but keyframes do not breathe.
- **Per Segment** — each segment paced on its own.
- **Natural** (default) — the path is driven straight from the marker times, with no
  arc-length correction, so the curve's own speed carries the camera: it eases
  into tight bends and flows through open ones. This is what makes a shot feel
  loose rather than measured.

Per marker you can override the curve shape, force stock behaviour for one
segment, or add ease-in / ease-out.

---

## Shake

Set a marker's stock **Shake** row to **Rockstar Editor+** to turn it on for
that marker. The shake pages then become available.

### Simple mode (default)

| Setting | |
|---|---|
| Shake Amplitude | **degrees** of rotation (and 5 cm of movement per degree) |
| Shake Frequency | **hertz** — 0.35 is a three-second wander, 0.20 is slower |
| Shake Variation | how much it swells and settles. 0 is perfectly even |

That is usually all you need. Both are real units and match the sibling
Simple Camera project, so a shake you like in one transfers to the other.
A good handheld starting point is **Amplitude 2.0-2.5 at Frequency 0.20**.

### Motion

| Setting | |
|---|---|
| Motion → Intensity | shake harder when the camera moves faster |
| Motion → Speed | shake faster when the camera moves faster |
| Stop When Still | fade the shake out when the camera is parked |

Both are off by default. Turning **Motion → Intensity** up to around 1.0 is the
single biggest step towards a shake that reads as a real operator rather than an
effect.

### Complex mode

Switch **Shake Mode** to Complex to drive the two layers directly — a slow
**Sway** and a fast **Jitter**, each with its own movement, rotation and rate —
plus per-axis weights on all six degrees of freedom.

### Apply to all markers

Each shake page has an **Apply Shake to All** row. Tune one marker, then press
Accept twice on that row to copy its shake to every marker in the project. It
copies only shake settings; curve and easing are left alone.

> The shake is a function of the clip's timeline, so it is frozen while the
> playhead is. **Play or scrub to judge it** — paused, it is just a very slight
> fixed offset.

---

## Rendering

Press **Export** in the editor. That is the only trigger.

**The renderer ships off.** Set `EnableRenderer=1` in `Render.ini` to turn it on;
the game's own encoder is then skipped and this mod renders instead. Everything
else about rendering lives in that file.

It is opt-in because it is the one feature here that cannot share. Diverting the
bake means other editor export tools — EVE, EVER — never fire: they hang off the
game's own bake pipeline, so they do not error, they just silently never run.
Everything else the mod does is additive and coexists fine, so only this needed
a switch you turn on deliberately.

### Output

`RenderMode` chooses what you get:

- **`Video`** (default) — frames are handed to ffmpeg as they finish and
  deleted, so a long render costs a couple of files at a time rather than
  thousands. The project's audio is recorded and muxed in, off the same single
  Export press.
- **`Frames`** — a numbered PNG or JPEG sequence, with an `assemble.txt` next to
  it containing ready-made ffmpeg command lines. No audio; there is nothing to
  mux it into.

The capture is identical either way. Codec settings come from `presets\`, which
ships with `h264`, `h265`, `nvenc_hevc`, `prores_hq` and `lossless` in it —
pick one with `RenderVideoPreset=h265`, or write ffmpeg arguments directly in
`RenderVideoArgs`. Your edits to a preset stick; a missing one is rewritten.

### Motion blur

Defaults are 30 fps with **64 samples** and a 360° shutter. Every sample is a
real render at a real point in time, so this is true accumulated blur rather
than a screen-space smear. More samples cost render time and nothing else — and
64 is genuinely expensive, 64 captures per output frame. Drop `RenderSamples` to
something like 3 while you are setting a shot up, and put it back for the final
pass.

`RenderShutter` is the shutter angle: `1.0` exposes the whole frame interval,
`0.5` is the 180° film convention.

You can also set `RenderSamples=1`, render at a multiple of your target rate,
and blend frames in post — the `assemble.txt` in every frame-sequence render
works out those command lines for you.

### Audio

Set `RenderAudio=1`. One Export press does both passes: the project plays
through once at normal speed to record the sound, then rewinds to the first clip
and renders the frames. The two are muxed at the end. Needs `RenderMode=Video`.

Alternatively, point `AudioFromFile` at a stock GTA export of the same project
and its audio track is used instead.

### While it runs

A render seeks frame by frame, so it is much slower than real time — a few
tenths of a second per frame is normal at 3 samples, and far more at 64.
Progress goes to `RockstarEditorPlus.log` every 15 seconds with an estimate.

---

## Depth of field

The capture add-on can walk the camera around a lens aperture and blend the
result, which gives real optical bokeh rather than a screen-space approximation.
That feature has always been in the add-on; it just had no way to move the
editor's camera. It does now.

**Start it from ReShade's own overlay** — the IgcsConnector depth-of-field panel.
There is no switch in the editor. Like rendering, it needs ReShade with full
add-on support. It also needs:

- the Rockstar Editor open, on a free-camera marker
- **playback paused** — a session integrates one instant
- `IgcsDof.fx` in your ReShade shaders folder (included in the bundle)

The editor's HUD is hidden for the duration and restored afterwards, the same as
during a render.

**It is for static shots**, and there is nothing to configure. The replay clock
is never moved during a session, so every sample sees the same instant from a
different point on the lens and the only thing varying is the aperture.

Motion blur belongs to the renderer, where the shutter is exact, it does not
compete with the bokeh for a sample budget, and it will not smear the highlight
shapes the aperture controls exist to produce.

---

## Settings reference

Two files, both in `RockstarEditorPlus\` beside the game.

### `RockstarEditorPlus.ini` — camera, shake, editor limits

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 disables the mod entirely |
| `SplinePosition` / `SplineOrientation` / `SplineFov` | 1 | which parts the curve drives |
| `SplineAttached` | 1 | also curve shots mounted on a car / bike / ped |
| `LookAtReaim` | 1 | re-aim look-at markers after the curve moves them |
| `LookAtAspect` | 0 | aspect for that re-aim; 0 = the display's |
| `Alpha` | 0 | path tension — 0 loose and flowing, 0.5 centripetal, 1 chordal |
| `SmoothSpeedProfile` | 1 | one speed curve across all markers |
| `NaturalPacing` | 0 | 1 = Simple Camera pacing: Hermite over marker times, natural easing |
| `OnlySmoothBlend` | 1 | 0 also takes over Linear markers |
| `ShakeSimpleMode` | 1 | 0 uses the Sway/Jitter model |
| `ShakeAmplitude` | 1.0 | simple mode: **degrees** of rotation (5 cm of movement per degree) |
| `ShakeFrequency` | 0.35 | simple mode: **hertz** |
| `ShakeVariation` | 0.7 | 0 = perfectly even |
| `ShakeSpeedAmp` / `ShakeSpeedFreq` | 0.0 | motion coupling. Try 1.0 / 0.5 |
| `ShakeStopWhenStill` | 0 | fade out when parked |
| `ShakeAxis*` | 1.0 | per-axis weights, 0 disables an axis |
| `UnlimitedCameraDistance` | 1 | lift the 30 m leash |
| `MaxCameraDistance` | 20000 | how far, in metres |
| `DisableCameraCollision` | 1 | pass through geometry |
| `UncapZoom` | 0 | widen the 0.45x–4.50x range |

### `Render.ini` — everything about rendering

| Key | Default | |
|---|---|---|
| `EnableRenderer` | 1 | master switch. 0 restores the stock exporter |
| `RenderMode` | Frames | `Video` or `Frames` |
| `RenderFps` | 30 | output rate |
| `RenderSamples` | 3 | motion-blur samples. 1 = none |
| `RenderShutter` | 1.0 | shutter angle |
| `RenderJpeg` / `RenderQuality` | 0 / 90 | JPEG instead of PNG |
| `RenderKeepFrames` | 0 | Video mode: keep the frames too |
| `RenderVideoPreset` | | a name from `presets\` |
| `FfmpegPath` | | empty searches `RockstarEditorPlus\` then PATH |
| `RenderAudio` | 0 | record and mux the project's sound |
| `RenderHideHud` | 1 | hide the editor HUD while rendering |
| `RenderOutputFolder` | | empty writes to `RockstarEditorPlus\Captures\` |

---

## Where files go

```
GTA5.exe
RockstarEditorPlus.asi
RockstarEditorPlus\
    RockstarEditorPlus.ini      camera, shake, limits
    Render.ini                  everything about rendering
    RockstarEditorPlus.log
    ffmpeg.exe                  you provide this
    presets\                    codec presets
    markers\                    per-marker settings
    Captures\
        render_0001\            frames, or video.mp4
```

Set `RenderOutputFolder` to an absolute path to write renders elsewhere —
sequences are large and the game drive is often not where you want them.

---

## Troubleshooting

**Export produces a normal watermarked video.** The capture add-on was not
found, so the mod stepped aside rather than silently giving you the wrong thing.
Two causes, in order of likelihood: ReShade was installed **without** add-on
support (the setup offers this as a separate choice, and the ordinary build
cannot load add-ons at all), or `IgcsConnector.addon64` is not beside the game
executable. The log names which.

**Frames come out with red and blue swapped.** Set `RenderChannelOrder` to 1 or
2 in `Render.ini`.

**A video was expected but you got a folder of frames.** `RenderMode=Frames`, or
ffmpeg was not found — the log says which.

**A shake setting seems to do nothing.** Per-marker values override the ini, and
the marker always wins. Check the value in the menu: if it shows a number rather
than `Default`, that marker has its own. Setting `ShakeDebugLog=1` logs the
values that actually reached the camera.

**The shake looks frozen.** The playhead is paused. Play or scrub.

**Nothing happens at all.** Check `RockstarEditorPlus.log`. If it does not
exist, the ASI was not loaded; if it ends early, the line it stops on says why.

---

## Licence

GPL-3.0-only. See [LICENSE](LICENSE).

Bundles [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause), a
GPLv3 build of [FFmpeg](https://ffmpeg.org), and
[IgcsConnector](https://github.com/FransBouma/IgcsConnector) by Frans Bouma
(add-on MIT, `IgcsDof.fx` BSD 2-clause). Licence texts and source information
are in `RockstarEditorPlusfmpeg-LICENSE.txt` and
`RockstarEditorPlus\IgcsConnector-LICENSE.txt`.
