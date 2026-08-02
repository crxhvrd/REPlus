cmake_minimum_required(VERSION 3.15)

project(RockstarEditorPlus)

# STATIC CRT. Not a preference - a correctness requirement for a .asi.
#
# An ASI is injected into someone else's process and does not get to choose which
# msvcp140.dll is already loaded there. MSVC's STL requires the runtime DLL to be
# AT LEAST AS NEW as the toolset that compiled the headers, and a host is free to
# ship an older one: FiveM carries msvcp140 14.34 in FiveM.app\bin, which wins
# over the system's 14.51, so a build made with the current toolset binds its
# inlined 14.51 STL to a 2022-era runtime. That combination crashed on the first
# std::mutex lock the mod performed - inside MSVCP140!_Mtx_lock, reproducibly,
# and immune to every timing and threading change tried, because it is an ABI
# mismatch rather than a race.
#
# Linking the CRT statically ends the whole class of problem: the mod carries its
# own STL and no longer cares what the host loaded. It also drops the VC++
# redistributable from the install requirements, which is worth having on its
# own for singleplayer users.
#
# Everything linked in must agree, hence CMAKE_MSVC_RUNTIME_LIBRARY set before
# any target is declared (minhook included) rather than per-target.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB_RECURSE PROJECT_SOURCE_FILES "src/*.h" "src/*.cpp")
# Exclude the vendored minhook tree from the ASI target (it builds separately).
list(FILTER PROJECT_SOURCE_FILES EXCLUDE REGEX "src/vendor/")

macro(GroupSources curdir groupindex)
    file(GLOB children RELATIVE ${curdir} ${curdir}/*)
    foreach(child ${children})
        if(IS_DIRECTORY ${curdir}/${child})
            GroupSources(${curdir}/${child} ${groupindex}/${child})
        else()
            string(REPLACE "/" "\\" groupname ${groupindex})
            source_group(${groupname} FILES ${curdir}/${child})
        endif()
    endforeach()
endmacro()

GroupSources(${CMAKE_SOURCE_DIR}/src "Source Files")

add_subdirectory("src/vendor/minhook")

add_library(RockstarEditorPlus SHARED ${PROJECT_SOURCE_FILES} fx_asi_build.rc)
add_dependencies(RockstarEditorPlus minhook)

include_directories(
    ./src
)

set_target_properties(RockstarEditorPlus PROPERTIES
    OUTPUT_NAME "RockstarEditorPlus"
    SUFFIX ".asi"
    CXX_STANDARD 20
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)

# =============================================================================
#  The shipped .ini files
# =============================================================================
#  GENERATED, not tracked. The .ini is a build output like the .asi: it belongs
#  beside the binary, which is where the mod reads it and where the in-editor
#  menu writes toggles back to. Keeping a copy in the repo as well meant two
#  files that drift, and a working tree that collects edited settings from
#  whatever was last tested.
#
#  So the authoritative text lives HERE, is written into the build tree at
#  configure time, and is copied next to the .asi on every build. Nothing
#  ini-shaped exists in the source tree at all.
#
#  Editing note: these are bracket arguments, so everything inside is literal -
#  no escaping needed for the ";" comment markers or for backslashes in paths.
# =============================================================================
set(REP_INI_MAIN [==[
; RockstarEditorPlus - editor settings.   Render settings are in Render.ini.
; Most of this is also in the in-game menu ("Rockstar Editor+" row).
; README.md explains the why; this file is just the what.
;
; Inline "; ..." comments are safe on numbers and 0/1 only - on a PATH or STRING
; key Windows' ini API keeps the comment as part of the value.

[RockstarEditorPlus]

; --- Camera -----------------------------------------------------------------
Enabled=1                  ; 0 = stock game, no hooks at all
SplinePosition=1           ; curved path instead of the straight-line blend
SplineOrientation=1        ; splined rotation (look-at markers re-aim instead)
SplineFov=1                ; monotone zoom curve, no overshoot
SplineAttached=1           ; also spline shots mounted on a car/bike/ped
LookAtReaim=1              ; re-aim look-at markers after moving the camera
LookAtAspect=0             ; aspect for that re-aim; 0 = the display's
OnlySmoothBlend=1          ; 1 = only "Smooth" markers, 0 = "Linear" too

; Pacing. Natural is the default and overrides the two below it.
NaturalPacing=1            ; path driven by marker times - eases into bends
SmoothSpeedProfile=1       ; one speed curve across markers (Continuous)
ArcLengthRemap=1           ; per-segment fallback, only used when both are 0
Alpha=1                    ; tension: 0 uniform / 0.5 centripetal / 1 chordal
                           ; has no effect while NaturalPacing=1

; --- Shake ------------------------------------------------------------------
; Per-marker; set a marker's Shake row to "Rockstar Editor+" to use it. These
; are the defaults each marker inherits. Handheld starting point: 2.0 @ 0.20.
ShakeSimpleMode=1          ; 1 = Amplitude/Frequency, 0 = the Sway/Jitter model
ShakeAmplitude=1.0         ; degrees of rotation, 0.05 m of travel per degree
ShakeFrequency=0.35        ; Hz
ShakeVariation=0.7         ; 0 = metronomic, higher = detuned and less regular
ShakeRoughness=3
ShakeSeed=0                ; reroll without changing the character

; Motion coupling - shake harder when the camera is moving.
ShakeSpeedAmp=0.0          ; extra amplitude at reference speed. Try 1.0
ShakeSpeedFreq=0.0         ; extra rate at reference speed. Try 0.5
ShakeSpeedRef=30.0         ; m/s that counts as "fully moving"
ShakeStopWhenStill=0       ; 1 = fade out when parked

; Complex mode only - ignored while ShakeSimpleMode=1.
SwayMove=0.0               ; slow, larger - the "handheld" layer
SwayRotate=0.0
SwayFrequency=1.8
JitterMove=0.0             ; fast, small - engine buzz and rattle
JitterRotate=0.0
JitterFrequency=9.0

; Per-axis weights. 0 disables an axis, e.g. Roll=0 for a locked horizon.
ShakeAxisLateral=1.0
ShakeAxisForward=0.6
ShakeAxisVertical=1.0
ShakeAxisPitch=1.0
ShakeAxisRoll=1.0
ShakeAxisYaw=1.0

; --- Editor limits ----------------------------------------------------------
UnlimitedCameraDistance=1  ; lift the 30m leash off the player
MaxCameraDistance=20000    ; the world streams near the player - expect LOD pop
DisableCameraCollision=1   ; camera passes through geometry
BypassProfanityFilter=1    ; naming/export still works with Social Club offline
UnlockCameraRestrictions=1 ; free camera on first-person / cutscene clips
HideEditorSpinner=1        ; no spinner ring while scrubbing
UncapZoom=1                ; widen the stock 0.45x..4.50x range
ZoomMinFov=1               ; 1..130 is the engine's own clamp
ZoomMaxFov=130             ; readout is 45/fov: 1 = 45x in, 130 = 0.35x out

; --- Recording length --------------------------------------------------------
; HOW LONG A SINGLE CLIP CAN BE, in 4 MB blocks.
;
; The recording ring IS the clip: it fills, the clip is saved, and recording
; rolls straight into a new one. So length = ring size / how fast the scene
; fills it - which is exactly why a street full of traffic gives you seconds
; where an empty road gives you a minute.
;
; The game ships 30 blocks (120 MB), and that is not its own preference: its
; settings code force-clamps the count to 30 whenever the replay heap is under
; ~196 MB, and the heap is 172 MB. We widen the heap first, so the clamp stops
; firing and the count can go up.
;
; Measured in dense city traffic:
;    30 blocks / 120 MB   ~17 seconds   (stock)
;    64 blocks / 256 MB   ~44 seconds
;   128 blocks / 512 MB   ~1m 45s
;
; COSTS, because 128 is the default and it is not free. The heap is sized for
; the blocks plus a 384 KB thumbnail each, so this reserves about 604 MB of RAM
; for the session, and clips grow to match - a 128-block clip is roughly 230 MB
; on disk. Lower it if memory is tight; check the editor's own disk allowance
; under Settings -> Saving & Startup if you shoot long clips.
;
; 3..128. Above 30 needs the heap widening to succeed - if it cannot (the sites
; did not resolve on your build, or this machine cannot reserve the address
; space) the value is clamped back to 30 and the log says which. Takes effect at
; the next recording setup, so restart before recording.
ReplayBlocks=128

; --- Menu state (written for you) -------------------------------------------
MenuGroup=0
MenuStep=2                 ; 0=.001 1=.005 2=.01 3=.05 4=.1 5=.5 6=1
MenuExpanded=0             ; 0 closed / 1 curve / 2 limits

; --- Diagnostics ------------------------------------------------------------
ShakeDebugLog=0            ; log the shake amplitudes that reached the camera
SplineDebugLog=0           ; log which curve path each frame took, once a second
SplineTraceLog=0           ; per-frame camera trace -> spline-trace.log
SplineDebugGain=1          ; exaggerate the curve's bow. 10 = obvious
]==])

set(REP_INI_RENDER [==[
; RockstarEditorPlus - render settings.   Editor settings are in
; RockstarEditorPlus.ini. README.md covers the workflow.
;
; Inline "; ..." comments are safe on numbers and 0/1 only - on a PATH or STRING
; key Windows' ini API keeps the comment as part of the value.

[Render]

; --- Capture ----------------------------------------------------------------
; EnableRenderer is the master switch: Export is the only trigger, and this is
; what diverts it from the game's own encoder. Ships OFF because diverting the
; bake means other export tools (EVE, EVER) silently never run.
EnableRenderer=0

RenderFps=30               ; output rate, independent of your actual framerate
RenderSamples=64           ; sub-frames averaged per frame. 1 = no motion blur
RenderShutter=1            ; 1.0 = 360 degrees, 0.5 = the 180 film convention
RenderSettleFrames=3       ; redraw wait after seeking to a new output frame
RenderSettleSubFrames=1    ; ...between blur sub-samples. Never 0
RenderHighlightBoost=0.9   ; keeps specular streaks bright through accumulation
RenderJpeg=0               ; 0 = PNG
RenderQuality=90           ; JPEG only
RenderChannelOrder=0       ; 0 auto / 1 RGBA / 2 BGRA - only if red and blue swap
RenderHideHud=1            ; hide the editor HUD and cursor while rendering
ExportCloseWhenDone=1      ; return to the editor menus when the render finishes
; Empty = RockstarEditorPlus\Captures\
RenderOutputFolder=

; --- Output -----------------------------------------------------------------
; Video  = streamed to ffmpeg as frames finish, then deleted
; Frames = numbered PNG/JPEG sequence with an assemble.txt beside it
; The capture is identical either way; this is only the deliverable.
RenderMode=Video
RenderKeepFrames=0         ; Video mode: keep the frames as well
; Empty = RockstarEditorPlus\ffmpeg.exe, then beside the exe, then PATH.
FfmpegPath=

; Named preset from presets\ (h264, h265, nvenc_hevc, prores_hq, lossless).
; A preset supplies both args and extension, overriding the two below.
RenderVideoPreset=

; Used when no preset is named. Ext must match the codec or ffmpeg refuses.
RenderVideoArgs=-c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p
RenderVideoExt=mp4

; --- Audio ------------------------------------------------------------------
; One Export press does both passes: the project plays through once to capture
; sound, then rewinds and renders the frames. Needs RenderMode=Video.
RenderAudio=1
; Or borrow the track from an existing file, normally a stock export of the same
; project. Ignored while RenderAudio is on.
AudioFromFile=

; --- Depth of field ---------------------------------------------------------
; Driven entirely from ReShade's overlay - nothing to configure here. Needs the
; editor open, a free-camera marker, playback PAUSED, and IgcsDof.fx installed.
; For STATIC shots: the replay clock never moves during a session. Motion blur
; is the renderer's job above, where the shutter is exact.
]==])

# Staged inside the build tree, never in the source tree.
file(WRITE "${CMAKE_BINARY_DIR}/ini/RockstarEditorPlus.ini" "${REP_INI_MAIN}")
file(WRITE "${CMAKE_BINARY_DIR}/ini/Render.ini"             "${REP_INI_RENDER}")

# copy_if_different, so a user's edited settings beside the .asi are not
# clobbered on every rebuild - only replaced when the shipped text really changed.
add_custom_command(TARGET RockstarEditorPlus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ini/RockstarEditorPlus.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/RockstarEditorPlus.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ini/Render.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/Render.ini"
    COMMENT "Writing the generated .ini files next to the .asi")

set_target_properties( minhook
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/LIB"
)

# smoothblend.cpp guards the detour body with __try/__except; MSVC needs SEH
# enabled and it must not be compiled with /EHsc-only semantics there.
target_compile_options(RockstarEditorPlus PRIVATE /utf-8)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
if(ENABLE_LOGS)
    target_compile_definitions(RockstarEditorPlus PRIVATE _LOG)
endif()

target_link_directories(RockstarEditorPlus PUBLIC "${CMAKE_BINARY_DIR}/LIB")
target_link_libraries(RockstarEditorPlus version.lib shell32.lib debug minhook.x64d.lib optimized minhook.x64.lib)

# =============================================================================
#  Preset Maker - a small GUI for authoring encoder presets
# =============================================================================
#  Separate executable, not part of the .asi. Encoder presets are two-line inis
#  that are easy to write and easy to get subtly wrong: a container that cannot
#  hold the chosen codec makes ffmpeg refuse, and the render then produces
#  nothing for a reason the user never sees. The tool makes that pairing
#  impossible to express.
#
#  Win32 with no dependencies and the same static CRT as the .asi, so it runs
#  from the bundle on a machine with nothing installed.
# =============================================================================
add_executable(PresetMaker WIN32 tools/presetmaker/presetmaker.cpp)
set_target_properties(PresetMaker PROPERTIES
    OUTPUT_NAME "RE+ Preset Maker"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)
target_compile_options(PresetMaker PRIVATE /utf-8)
