// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
//
// =============================================================================
//  RE+ Render Settings - editor for Render.ini and its encoder presets
// =============================================================================
//  Two jobs, one window:
//
//    Render tab   every key in Render.ini, with an explanation attached to each
//                 one rather than only in the file
//    Presets tab  builds presets\<name>.ini, the two-line files ffmpeg is
//                 driven by
//
//  Why the preset half exists at all: a container that cannot hold the chosen
//  codec makes ffmpeg refuse, the render produces nothing, and the reason is a
//  line of stderr the user never sees. Here choosing a codec filters the
//  container and pixel-format lists, so the broken combination cannot be
//  expressed. Same for "lossless" that silently subsamples chroma - a lossless
//  codec only offers 4:4:4 or RGB.
//
//  Writing is LINE PRESERVING. Render.ini is heavily commented and those
//  comments are the documentation for anyone editing by hand; rewriting the
//  file (or using WritePrivateProfileString, which drops the trailing comment
//  on any line it touches) would strip them a key at a time.
//
//  Win32, no dependencies, static CRT - same as the .asi, because it ships in
//  the bundle and has to run on a machine with nothing installed.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
//  Settings table - one row per Render.ini key
// ---------------------------------------------------------------------------
enum SType { S_BOOL, S_INT, S_FLOAT, S_TEXT, S_CHOICE, S_PRESET };

struct Setting
{
    const char* key;
    const char* label;
    SType       type;
    const char* choices;   // "Video|Frames"; for S_CHOICE the index is the value
    const char* help;      // tooltip and description strip
    bool        wide;      // lay out full width under the two columns

    // S_CHOICE only: the ini stores the WORD ("Sliding"), not the index.
    //
    // A table flag rather than a name test in the save path, which is what
    // this was. Adding a second word-valued key meant remembering to extend
    // an `if` three hundred lines away - and forgetting writes "1" into a
    // key the mod compares against "Sliding", so the setting reads back as
    // its default and the tool looks like it did nothing.
    bool        wordValue = false;
};

static Setting kSettings[] =
{
// --- left column ---
{ "EnableRenderer", "Enable renderer", S_BOOL, nullptr,
  "The master switch. Export is the only trigger, and this is what diverts it away from the game's own encoder.\r\n"
  "Ships OFF: diverting the bake stops other export tools (EVE, EVER) from running - they do not error, they simply never fire.", false },

{ "RenderMode", "Output", S_CHOICE, "Video|Frames",
  "Video   - frames are handed to ffmpeg as they finish and deleted, so a long render costs a couple of files at a time. Audio is muxed in.\r\n"
  "Frames  - numbered PNG/JPEG sequence with an assemble.txt of ready-made ffmpeg commands. No audio.\r\n"
  "The capture itself is identical either way.", false, true },

{ "RenderCaptureMode", "Capture mode", S_CHOICE, "Walking|Sliding",
  "Sliding - the clip PLAYS in slow motion: it advances to each frame's mark, then exposes Motion blur samples consecutive frames with the world still simulating between them. Particles step, and anything with temporal history (TAA, SSR, ray tracing) stays warm instead of being reset at every sample.\r\n"
  "The default, and roughly 3x faster than Walking at a 360-degree shutter - it needs one frame per sample where Walking needs a settle frame too. That edge is shutter-dependent: it needs Samples/Shutter frames, so at 0.5 the two are level and below that Walking wins.\r\n"
  "Walking - pause, seek to each sub-sample, grab, average. Exact shutter placement, deterministic frame times, and a more obvious failure mode: a repeated frame rather than a smeared one. Use it if a sliding render looks wrong, or for short shutters.", false, true },

{ "RenderFps", "Frame rate", S_INT, nullptr,
  "Output frame rate, independent of the rate the game is running at. Halving it halves the render time.", false },

{ "RenderSamples", "Motion blur samples", S_INT, nullptr,
  "Sub-frames averaged into each output frame. 1 = no blur.\r\n"
  "Every sample is a real render at a real instant, so this is true accumulation - and it is the dominant cost. "
  "64 means 64 captures per output frame; drop to 3 while setting a shot up.\r\n"
  "Exact in both capture modes. Walking seeks to each instant; sliding advances the clip to the frame's mark and then takes this many consecutive presents, with the world still simulating between them.", false },

{ "RenderShutter", "Shutter angle", S_FLOAT, nullptr,
  "1.0 exposes the whole frame interval (360 degrees). 0.5 is the 180-degree film convention.\r\n"
  "Free in Walking - it only changes how the samples are spaced.\r\n"
  "In Sliding it also sets the target the playback speed is tuned to, so a shorter shutter means a slower playback and a longer render.", false },

{ "RenderSettleFrames", "Settle frames", S_INT, nullptr,
  "Frames to let the game redraw after seeking to a new output frame. Paid once per frame, so it barely matters at high sample counts "
  "but is most of the cost when samples = 1.", false },

{ "RenderSettleSubFrames", "Settle sub-frames", S_INT, nullptr,
  "Redraw wait between motion-blur sub-samples. NEVER 0 - at zero every sample captures the same image and averages back to one sharp frame.\r\n"
  "Each extra frame here is multiplied by the sample count, so raising it is expensive.", false },

{ "RenderHighlightBoost", "Highlight boost", S_FLOAT, nullptr,
  "Lifts bright areas while samples are accumulated in linear light, so speculars streak instead of averaging down to grey. 0 disables it.", false },

// --- right column ---
{ "RenderAudio", "Record audio", S_BOOL, nullptr,
  "One Export press does two passes: the project plays through once at normal speed to capture sound, then rewinds and renders the frames. "
  "Muxed at the end. Requires Output = Video.", false },

{ "RenderHideHud", "Hide HUD while rendering", S_BOOL, nullptr,
  "Hides the editor HUD and cursor for the duration and restores them afterwards.", false },

{ "ExportCloseWhenDone", "Return to menus when done", S_BOOL, nullptr,
  "Go back to the editor menus once the render finishes, rather than staying in playback.", false },

{ "RenderKeepFrames", "Keep frames (Video mode)", S_BOOL, nullptr,
  "Normally the frames are deleted as ffmpeg consumes them. Turn this on to keep both the video and the image sequence.", false },

{ "RenderJpeg", "Write JPEG instead of PNG", S_BOOL, nullptr,
  "PNG is lossless and the default. JPEG is smaller and faster to write, and lossy.", false },

{ "RenderQuality", "JPEG quality", S_INT, nullptr,
  "1 to 100. Only used when JPEG is on.", false },

{ "RenderVideoPreset", "Encoder preset", S_PRESET, nullptr,
  "A name from the presets folder - see the Encoder presets tab. A preset supplies both the ffmpeg arguments and the file extension, "
  "overriding the two boxes below.\r\nLeave empty to use those boxes directly.", false },

// --- full width ---
{ "RenderOutputFolder", "Output folder", S_TEXT, nullptr,
  "Where renders are written. Empty means RockstarEditorPlus\\Captures\\ next to the .asi.\r\n"
  "Sequences are large - point this at another drive if the game is on a small one.", true },

{ "FfmpegPath", "ffmpeg path", S_TEXT, nullptr,
  "Empty searches the bundled RockstarEditorPlus\\ffmpeg.exe first, then beside the game, then PATH.\r\n"
  "The bundled copy is used ahead of any other so renders encode the same way on every machine.", true },

{ "RenderVideoArgs", "ffmpeg arguments", S_TEXT, nullptr,
  "Used when no preset is named. The extension below must match the codec or ffmpeg refuses and the render produces nothing.", true },

{ "RenderVideoExt", "Container", S_TEXT, nullptr,
  "File extension for the video, without the dot. Must be able to hold the codec in the arguments above.", true },

{ "AudioFromFile", "Borrow audio from file", S_TEXT, nullptr,
  "Use the audio track from an existing file - normally a stock export of the same project - instead of recording it. "
  "Ignored while Record audio is on.", true },
};
static const int kSettingCount = (int)(sizeof(kSettings) / sizeof(kSettings[0]));

// ---------------------------------------------------------------------------
//  Codec table for the preset tab
// ---------------------------------------------------------------------------
struct Quality { const char* label; const char* args; };

struct Codec
{
    const char* name;
    const char* video;
    const char* qualityFlag;
    Quality     quality[5];
    const char* exts[5];
    const char* pixFmts[5];
    const char* note;
};

static const Codec kCodecs[] =
{
    { "H.264 (x264)", "libx264", "-crf",
      { {"Visually lossless (16)","16"}, {"High (18)","18"}, {"Good (20)","20"},
        {"Smaller (23)","23"}, {nullptr,nullptr} },
      { "mp4","mov","mkv",nullptr }, { "yuv420p","yuv444p",nullptr },
      "The safe default. Plays and edits anywhere." },

    { "H.265 (x265)", "libx265", "-crf",
      { {"Visually lossless (18)","18"}, {"High (20)","20"}, {"Good (23)","23"}, {nullptr,nullptr} },
      { "mp4","mov","mkv",nullptr }, { "yuv420p","yuv444p",nullptr },
      "Smaller than H.264 at the same quality, slower to encode." },

    { "HEVC (NVIDIA NVENC)", "hevc_nvenc", "-cq",
      { {"High (20)","20"}, {"Good (23)","23"}, {nullptr,nullptr} },
      { "mp4","mov","mkv",nullptr }, { "yuv420p",nullptr },
      "GPU encode - much faster, needs an NVIDIA card." },

    { "ProRes 422 HQ", "prores_ks", "-profile:v",
      { {"422 HQ (3)","3"}, {"422 (2)","2"}, {"4444 (4)","4"}, {nullptr,nullptr} },
      { "mov","mkv",nullptr }, { "yuv422p10le","yuva444p10le",nullptr },
      "The editing-timeline choice. Large files, grades well." },

    { "H.264 lossless", "libx264 -qp 0 -preset veryslow", "",
      { {"Mathematically lossless",""}, {nullptr,nullptr} },
      { "mkv","mov","mp4",nullptr }, { "yuv444p",nullptr },
      "Bit-exact and very large. 4:4:4 only - 4:2:0 would discard colour before the encoder ever saw it." },

    { "QuickTime RLE (lossless)", "qtrle", "",
      { {"Mathematically lossless",""}, {nullptr,nullptr} },
      { "mov",nullptr }, { "rgb24",nullptr },
      "Lossless MOV that every editor reads. Enormous files." },

    { "FFV1 (lossless)", "ffv1 -level 3", "",
      { {"Mathematically lossless",""}, {nullptr,nullptr} },
      { "mkv","avi",nullptr }, { "yuv444p","rgb24",nullptr },
      "Archival lossless, far smaller than RLE." },
};
static const int kCodecCount = (int)(sizeof(kCodecs) / sizeof(kCodecs[0]));

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum { ID_TAB = 900, ID_SAVE, ID_RELOAD, ID_BROWSE, ID_HELP, ID_PATH,
       ID_PLIST = 1200, ID_PNAME, ID_PCODEC, ID_PQUAL, ID_PEXT, ID_PPIX,
       ID_PARGS, ID_PNOTE, ID_PSAVE, ID_PDEL, ID_SETTING = 1400 };

static HWND g_tab, g_help, g_pathLabel, g_tip;
static HWND g_ctl[kSettingCount];          // one control per setting
static HWND g_ctlLabel[kSettingCount];
static HWND g_plist, g_pname, g_pcodec, g_pqual, g_pext, g_ppix, g_pargs, g_pnote;
static std::vector<HWND> g_presetTabCtls;
static std::string g_iniPath, g_presetDir;
static std::vector<std::string> g_lines;   // Render.ini, verbatim

// ---------------------------------------------------------------------------
//  Paths
// ---------------------------------------------------------------------------
static std::string exeDir()
{
    char p[MAX_PATH]{};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    if (char* s = strrchr(p, '\\')) *(s + 1) = '\0';
    return p;
}

static bool fileExists(const std::string& p)
{ return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The tool may sit beside the .asi, inside RockstarEditorPlus\, or one level up
// if someone moved it. Try each before giving up.
static void resolvePaths()
{
    const std::string b = exeDir();
    const char* roots[] = { "RockstarEditorPlus\\", "", "..\\RockstarEditorPlus\\", "..\\" };
    for (const char* r : roots)
    {
        if (fileExists(b + r + "Render.ini"))
        {
            g_iniPath   = b + r + "Render.ini";
            g_presetDir = b + r + "presets\\";
            return;
        }
    }
    g_iniPath   = b + "RockstarEditorPlus\\Render.ini";
    g_presetDir = b + "RockstarEditorPlus\\presets\\";
}

// ---------------------------------------------------------------------------
//  Line-preserving ini
// ---------------------------------------------------------------------------
static void trimR(std::string& s)
{ while (!s.empty() && (s.back()=='\r'||s.back()=='\n'||s.back()==' '||s.back()=='\t')) s.pop_back(); }

static void loadIni()
{
    g_lines.clear();
    HANDLE h = CreateFileA(g_iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string all;
    char buf[4096]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) all.append(buf, got);
    CloseHandle(h);

    size_t start = 0;
    while (start <= all.size())
    {
        size_t nl = all.find('\n', start);
        if (nl == std::string::npos) { g_lines.push_back(all.substr(start)); break; }
        std::string line = all.substr(start, nl - start);
        trimR(line);
        g_lines.push_back(line);
        start = nl + 1;
    }
}

// Value of a key, ignoring any trailing "; comment".
static std::string iniGet(const char* key)
{
    const size_t klen = strlen(key);
    for (const std::string& l : g_lines)
    {
        if (l.size() <= klen || _strnicmp(l.c_str(), key, klen) != 0) continue;
        size_t i = klen;
        while (i < l.size() && (l[i]==' '||l[i]=='\t')) ++i;
        if (i >= l.size() || l[i] != '=') continue;
        std::string v = l.substr(i + 1);
        const size_t c = v.find(';');
        if (c != std::string::npos) v = v.substr(0, c);
        while (!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
        trimR(v);
        return v;
    }
    return "";
}

// Replace only the value, keeping the key's own spelling and any trailing
// comment exactly as it was. Appends the key if the file does not have it.
static void iniSet(const char* key, const std::string& val)
{
    const size_t klen = strlen(key);
    for (std::string& l : g_lines)
    {
        if (l.size() <= klen || _strnicmp(l.c_str(), key, klen) != 0) continue;
        size_t i = klen;
        while (i < l.size() && (l[i]==' '||l[i]=='\t')) ++i;
        if (i >= l.size() || l[i] != '=') continue;

        const std::string head = l.substr(0, i + 1);
        std::string tail;
        const size_t c = l.find(';', i);
        if (c != std::string::npos)
        {
            // Keep the comment where it sat, padded so the file stays aligned.
            const std::string comment = l.substr(c);
            std::string pad = " ";
            const size_t want = head.size() + val.size();
            if (want < 38) pad = std::string(38 - want, ' ');
            tail = pad + comment;
        }
        l = head + val + tail;
        return;
    }
    g_lines.push_back(std::string(key) + "=" + val);
}

static bool saveIni()
{
    std::string out;
    for (size_t i = 0; i < g_lines.size(); ++i)
    {
        out += g_lines[i];
        if (i + 1 < g_lines.size()) out += "\r\n";
    }
    HANDLE h = CreateFileA(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
static std::string editText(HWND h)
{ char b[1024]{}; GetWindowTextA(h, b, sizeof(b)); return b; }

static std::string comboText(HWND h)
{
    const int i = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
    if (i < 0) return "";
    char b[128]{}; SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b); return b;
}

static void addTip(HWND ctl, const char* text)
{
    TOOLINFOA ti{};
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = GetParent(ctl);
    ti.uId      = (UINT_PTR)ctl;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void fillPresetCombo(HWND h)
{
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)"(none - use the arguments below)");
    WIN32_FIND_DATAA fd{};
    HANDLE f = FindFirstFileA((g_presetDir + "*.ini").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        const size_t d = n.rfind('.');
        if (d != std::string::npos) n = n.substr(0, d);
        SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

// ---------------------------------------------------------------------------
//  Settings <-> controls
// ---------------------------------------------------------------------------
static void settingsToUi()
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        const Setting& s = kSettings[i];
        const std::string v = iniGet(s.key);
        switch (s.type)
        {
        case S_BOOL:
            SendMessageA(g_ctl[i], BM_SETCHECK, (v == "1") ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case S_CHOICE:
        {
            int idx = 0;
            if (s.choices && strchr(s.choices, '|') && !isdigit((unsigned char)(v.empty()?'x':v[0])))
            {
                // Textual choice, e.g. RenderMode=Video
                const int n = (int)SendMessageA(g_ctl[i], CB_GETCOUNT, 0, 0);
                for (int k = 0; k < n; ++k)
                {
                    char b[64]{}; SendMessageA(g_ctl[i], CB_GETLBTEXT, k, (LPARAM)b);
                    if (_stricmp(b, v.c_str()) == 0) { idx = k; break; }
                }
            }
            else idx = atoi(v.c_str());
            SendMessageA(g_ctl[i], CB_SETCURSEL, idx, 0);
            break;
        }
        case S_PRESET:
        {
            fillPresetCombo(g_ctl[i]);
            int idx = 0;
            const int n = (int)SendMessageA(g_ctl[i], CB_GETCOUNT, 0, 0);
            for (int k = 1; k < n; ++k)
            {
                char b[128]{}; SendMessageA(g_ctl[i], CB_GETLBTEXT, k, (LPARAM)b);
                if (_stricmp(b, v.c_str()) == 0) { idx = k; break; }
            }
            SendMessageA(g_ctl[i], CB_SETCURSEL, idx, 0);
            break;
        }
        default:
            SetWindowTextA(g_ctl[i], v.c_str());
            break;
        }
    }
}

static void uiToSettings()
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        const Setting& s = kSettings[i];
        switch (s.type)
        {
        case S_BOOL:
            iniSet(s.key, SendMessageA(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            break;
        case S_CHOICE:
        {
            const int idx = (int)SendMessageA(g_ctl[i], CB_GETCURSEL, 0, 0);
            if (s.wordValue) iniSet(s.key, comboText(g_ctl[i]));
            else { char b[16]; sprintf_s(b, "%d", idx < 0 ? 0 : idx); iniSet(s.key, b); }
            break;
        }
        case S_PRESET:
        {
            const int idx = (int)SendMessageA(g_ctl[i], CB_GETCURSEL, 0, 0);
            iniSet(s.key, idx <= 0 ? "" : comboText(g_ctl[i]));
            break;
        }
        default:
            iniSet(s.key, editText(g_ctl[i]));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  Preset tab
// ---------------------------------------------------------------------------
static int selCodec()
{
    const int i = (int)SendMessageA(g_pcodec, CB_GETCURSEL, 0, 0);
    return (i < 0 || i >= kCodecCount) ? 0 : i;
}

static std::string buildArgs()
{
    const Codec& c = kCodecs[selCodec()];
    const int qi = (int)SendMessageA(g_pqual, CB_GETCURSEL, 0, 0);
    std::string s = "-c:v "; s += c.video;
    if (c.qualityFlag && *c.qualityFlag && qi >= 0 && c.quality[qi].label)
    {
        s += " "; s += c.qualityFlag; s += " "; s += c.quality[qi].args;
        if (strstr(c.video, "libx26")) s += " -preset slow";
    }
    const std::string pf = comboText(g_ppix);
    if (!pf.empty()) { s += " -pix_fmt "; s += pf; }
    return s;
}

static void previewPreset()
{
    const std::string s = "[Preset]\r\nArgs=" + buildArgs() + "\r\nExt=" + comboText(g_pext);
    SetWindowTextA(g_pargs, s.c_str());
}

static void codecChanged()
{
    const Codec& c = kCodecs[selCodec()];
    SendMessageA(g_pqual, CB_RESETCONTENT, 0, 0);
    for (int i = 0; c.quality[i].label; ++i)
        SendMessageA(g_pqual, CB_ADDSTRING, 0, (LPARAM)c.quality[i].label);
    SendMessageA(g_pqual, CB_SETCURSEL, 0, 0);
    EnableWindow(g_pqual, c.quality[1].label != nullptr);

    SendMessageA(g_pext, CB_RESETCONTENT, 0, 0);
    for (int i = 0; c.exts[i]; ++i) SendMessageA(g_pext, CB_ADDSTRING, 0, (LPARAM)c.exts[i]);
    SendMessageA(g_pext, CB_SETCURSEL, 0, 0);

    SendMessageA(g_ppix, CB_RESETCONTENT, 0, 0);
    for (int i = 0; c.pixFmts[i]; ++i) SendMessageA(g_ppix, CB_ADDSTRING, 0, (LPARAM)c.pixFmts[i]);
    SendMessageA(g_ppix, CB_SETCURSEL, 0, 0);

    SetWindowTextA(g_pnote, c.note);
}

static void refreshPresetList()
{
    SendMessageA(g_plist, LB_RESETCONTENT, 0, 0);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((g_presetDir + "*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        const size_t d = n.rfind('.');
        if (d != std::string::npos) n = n.substr(0, d);
        SendMessageA(g_plist, LB_ADDSTRING, 0, (LPARAM)n.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void savePreset()
{
    std::string name = editText(g_pname);
    if (name.empty())
    {
        MessageBoxA(nullptr, "Give the preset a name first.\n\nThat name is what goes in "
            "Render.ini as the encoder preset.", "RE+ Render Settings", MB_ICONINFORMATION);
        return;
    }
    for (char& c : name)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';

    CreateDirectoryA(g_presetDir.c_str(), nullptr);
    const Codec& c = kCodecs[selCodec()];
    const std::string text =
        "; " + std::string(c.note) + "\r\n"
        "; Use with RenderVideoPreset=" + name + " in Render.ini\r\n\r\n"
        "[Preset]\r\nArgs=" + buildArgs() + "\r\nExt=" + comboText(g_pext) + "\r\n";

    HANDLE h = CreateFileA((g_presetDir + name + ".ini").c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        char m[512];
        sprintf_s(m, "Could not write into:\n%s\n\nError %lu. Nothing here needs "
            "administrator rights - if the game is under Program Files, the mod keeps "
            "its files in LocalAppData instead.", g_presetDir.c_str(), GetLastError());
        MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONERROR);
        return;
    }
    DWORD w = 0; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr); CloseHandle(h);

    refreshPresetList();
    for (int i = 0; i < kSettingCount; ++i)
        if (kSettings[i].type == S_PRESET) fillPresetCombo(g_ctl[i]);

    char m[256];
    sprintf_s(m, "Saved %s.ini\n\nPick it under Encoder preset on the Render tab to use it.",
              name.c_str());
    MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONINFORMATION);
}

static void loadPreset()
{
    const int sel = (int)SendMessageA(g_plist, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    char n[128]{}; SendMessageA(g_plist, LB_GETTEXT, sel, (LPARAM)n);
    const std::string file = g_presetDir + n + ".ini";

    char args[1024]{}, ext[64]{};
    GetPrivateProfileStringA("Preset", "Args", "", args, sizeof(args), file.c_str());
    GetPrivateProfileStringA("Preset", "Ext",  "", ext,  sizeof(ext),  file.c_str());

    SetWindowTextA(g_pname, n);
    for (int i = 0; i < kCodecCount; ++i)
        if (strstr(args, kCodecs[i].video))
        { SendMessageA(g_pcodec, CB_SETCURSEL, i, 0); codecChanged(); break; }

    const int n2 = (int)SendMessageA(g_pext, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n2; ++i)
    { char b[32]{}; SendMessageA(g_pext, CB_GETLBTEXT, i, (LPARAM)b);
      if (_stricmp(b, ext) == 0) { SendMessageA(g_pext, CB_SETCURSEL, i, 0); break; } }

    const std::string s = "[Preset]\r\nArgs=" + std::string(args) + "\r\nExt=" + ext;
    SetWindowTextA(g_pargs, s.c_str());
}

// ---------------------------------------------------------------------------
//  Tabs
// ---------------------------------------------------------------------------
static void showTab(int t)
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        ShowWindow(g_ctl[i],      t == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(g_ctlLabel[i], t == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND h : g_presetTabCtls) ShowWindow(h, t == 1 ? SW_SHOW : SW_HIDE);
}

// ---------------------------------------------------------------------------
//  Window
// ---------------------------------------------------------------------------
static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
               HWND p, int id)
{
    return CreateWindowA(cls, text, WS_CHILD | style, x, y, w, h, p, (HMENU)(INT_PTR)id,
                         nullptr, nullptr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_tip = CreateWindowExA(0, TOOLTIPS_CLASSA, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, 460);
        SendMessageA(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

        g_tab = CreateWindowA(WC_TABCONTROLA, nullptr, WS_CHILD | WS_VISIBLE,
            8, 8, 700, 24, hwnd, (HMENU)ID_TAB, nullptr, nullptr);
        TCITEMA ti{}; ti.mask = TCIF_TEXT;
        ti.pszText = (LPSTR)"Render";          SendMessageA(g_tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
        ti.pszText = (LPSTR)"Encoder presets"; SendMessageA(g_tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);

        // --- Render tab: two columns, then the wide text rows ---------------
        const int colX[2] = { 16, 372 };
        int rowY[2] = { 44, 44 };
        int wideY = 0, col = 0, placed = 0;

        // Split the columns evenly for whatever the table holds rather than
        // at a fixed eight. Both this and wideY below were hardcoded to the
        // row count of the day, so adding a setting overflowed the left
        // column straight through the wide text boxes underneath it.
        int nonWide = 0;
        for (int i = 0; i < kSettingCount; ++i) if (!kSettings[i].wide) ++nonWide;
        const int perCol = (nonWide + 1) / 2;

        for (int i = 0; i < kSettingCount; ++i)
        {
            const Setting& s = kSettings[i];
            if (s.wide) continue;
            col = (placed < perCol) ? 0 : 1;
            const int x = colX[col], y = rowY[col];

            g_ctlLabel[i] = mk("STATIC", s.label, WS_VISIBLE, x, y + 4, 150, 18, hwnd, 0);

            if (s.type == S_BOOL)
                g_ctl[i] = mk("BUTTON", "", WS_VISIBLE | BS_AUTOCHECKBOX,
                              x + 156, y + 3, 20, 18, hwnd, ID_SETTING + i);
            else if (s.type == S_CHOICE || s.type == S_PRESET)
            {
                g_ctl[i] = mk("COMBOBOX", "", WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              x + 156, y, 160, 200, hwnd, ID_SETTING + i);
                if (s.choices)
                {
                    std::string all = s.choices, cur;
                    for (char ch : all + "|")
                    { if (ch == '|') { if (!cur.empty()) SendMessageA(g_ctl[i], CB_ADDSTRING, 0, (LPARAM)cur.c_str()); cur.clear(); }
                      else cur += ch; }
                }
            }
            else
                g_ctl[i] = mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              x + 156, y, 90, 22, hwnd, ID_SETTING + i);

            addTip(g_ctl[i], s.help);
            addTip(g_ctlLabel[i], s.help);
            rowY[col] += 30;
            ++placed;
        }

        wideY = 44 + perCol * 30 + 10;
        for (int i = 0; i < kSettingCount; ++i)
        {
            const Setting& s = kSettings[i];
            if (!s.wide) continue;
            g_ctlLabel[i] = mk("STATIC", s.label, WS_VISIBLE, 16, wideY + 4, 150, 18, hwnd, 0);
            g_ctl[i] = mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                          172, wideY, 516, 22, hwnd, ID_SETTING + i);
            addTip(g_ctl[i], s.help);
            addTip(g_ctlLabel[i], s.help);
            wideY += 28;
        }

        // --- Preset tab -----------------------------------------------------
        auto keep = [&](HWND h) { g_presetTabCtls.push_back(h); return h; };
        keep(mk("STATIC", "Presets", WS_VISIBLE, 16, 44, 90, 18, hwnd, 0));
        g_plist = keep(mk("LISTBOX", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                          16, 64, 120, 200, hwnd, ID_PLIST));

        struct { const char* l; int id; } rows[] = {
            {"Name", ID_PNAME}, {"Codec", ID_PCODEC}, {"Quality", ID_PQUAL},
            {"Container", ID_PEXT}, {"Pixel format", ID_PPIX} };
        int py = 44;
        for (int i = 0; i < 5; ++i)
        {
            keep(mk("STATIC", rows[i].l, WS_VISIBLE, 152, py + 4, 90, 18, hwnd, 0));
            HWND h = (i == 0)
                ? mk("EDIT", "my_preset", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     248, py, 250, 22, hwnd, rows[i].id)
                : mk("COMBOBOX", "", WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     248, py, 250, 240, hwnd, rows[i].id);
            keep(h);
            switch (rows[i].id)
            {
            case ID_PNAME:  g_pname  = h; break;
            case ID_PCODEC: g_pcodec = h; break;
            case ID_PQUAL:  g_pqual  = h; break;
            case ID_PEXT:   g_pext   = h; break;
            case ID_PPIX:   g_ppix   = h; break;
            }
            py += 30;
        }
        g_pnote = keep(mk("STATIC", "", WS_VISIBLE, 152, py + 4, 540, 34, hwnd, ID_PNOTE));
        g_pargs = keep(mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY,
                          152, py + 42, 540, 60, hwnd, ID_PARGS));
        keep(mk("BUTTON", "Save preset", WS_VISIBLE | BS_PUSHBUTTON, 16, 272, 120, 26, hwnd, ID_PSAVE));
        keep(mk("BUTTON", "Delete", WS_VISIBLE | BS_PUSHBUTTON, 16, 302, 120, 26, hwnd, ID_PDEL));

        // --- Shared bottom ---------------------------------------------------
        //
        // Positioned from where the tallest tab actually ended, not from
        // constants. Hardcoding these put the help and path labels straight
        // through the last two wide rows.
        const int presetBottom = py + 42 + 60;          // args box on the preset tab
        int bottom = (wideY > presetBottom ? wideY : presetBottom) + 10;

        // A read-only multiline EDIT, not a STATIC.
        //
        // A static clips whatever does not fit and there is no height that is
        // safe: it was one line, then two, and RenderMode alone needs four once
        // its first line wraps. An edit scrolls instead, so the panel cannot
        // clip no matter how long a future help string gets - the failure mode
        // becomes "scroll a little", not "the last sentence is invisible".
        g_help = CreateWindowA("EDIT", "Hover any setting, or click into it, for an explanation.",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            16, bottom, 690, 76, hwnd, (HMENU)ID_HELP, nullptr, nullptr);
        bottom += 82;

        g_pathLabel = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
            16, bottom, 690, 18, hwnd, (HMENU)ID_PATH, nullptr, nullptr);
        bottom += 24;

        CreateWindowA("BUTTON", "Save Render.ini", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            576, bottom, 130, 28, hwnd, (HMENU)ID_SAVE, nullptr, nullptr);
        CreateWindowA("BUTTON", "Reload", WS_CHILD | WS_VISIBLE,
            488, bottom, 80, 28, hwnd, (HMENU)ID_RELOAD, nullptr, nullptr);
        bottom += 28 + 12;

        // Fit the window to the content rather than guessing a height.
        RECT want{ 0, 0, 722, bottom };
        AdjustWindowRect(&want, (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE), FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(hwnd, [](HWND c, LPARAM p) -> BOOL
            { SendMessageA(c, WM_SETFONT, (WPARAM)p, TRUE); return TRUE; }, (LPARAM)f);

        for (int i = 0; i < kCodecCount; ++i)
            SendMessageA(g_pcodec, CB_ADDSTRING, 0, (LPARAM)kCodecs[i].name);
        SendMessageA(g_pcodec, CB_SETCURSEL, 0, 0);
        codecChanged(); previewPreset();

        resolvePaths();
        loadIni();
        settingsToUi();
        refreshPresetList();
        SetWindowTextA(g_pathLabel, ("Editing: " + g_iniPath).c_str());
        showTab(0);
        return 0;
    }

    case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == ID_TAB && ((LPNMHDR)lp)->code == TCN_SELCHANGE)
            showTab((int)SendMessageA(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id >= ID_SETTING && id < ID_SETTING + kSettingCount)
        {
            const int i = id - ID_SETTING;
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == EN_SETFOCUS)
                SetWindowTextA(g_help, kSettings[i].help);
            return 0;
        }
        switch (id)
        {
        case ID_PCODEC: if (HIWORD(wp) == CBN_SELCHANGE) { codecChanged(); previewPreset(); } return 0;
        case ID_PQUAL: case ID_PEXT: case ID_PPIX:
            if (HIWORD(wp) == CBN_SELCHANGE) previewPreset(); return 0;
        case ID_PLIST:  if (HIWORD(wp) == LBN_SELCHANGE) loadPreset(); return 0;
        case ID_PSAVE:  savePreset(); return 0;
        case ID_PDEL:
        {
            const int sel = (int)SendMessageA(g_plist, LB_GETCURSEL, 0, 0);
            if (sel < 0) return 0;
            char n[128]{}; SendMessageA(g_plist, LB_GETTEXT, sel, (LPARAM)n);
            char q[256]; sprintf_s(q, "Delete %s.ini?\n\nThe renderer rewrites the five "
                                      "shipped presets if they go missing.", n);
            if (MessageBoxA(nullptr, q, "RE+ Render Settings", MB_YESNO | MB_ICONQUESTION) == IDYES)
            { DeleteFileA((g_presetDir + n + ".ini").c_str()); refreshPresetList(); }
            return 0;
        }
        case ID_SAVE:
            uiToSettings();
            if (saveIni())
                SetWindowTextA(g_help, "Saved. Comments in the file were left as they were.");
            else
            {
                char m[512];
                sprintf_s(m, "Could not write:\n%s\n\nError %lu.", g_iniPath.c_str(), GetLastError());
                MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONERROR);
            }
            return 0;
        case ID_RELOAD:
            loadIni(); settingsToUi();
            SetWindowTextA(g_help, "Reloaded from disk.");
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSA wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "REPlusRenderSettings";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    HWND w = CreateWindowA(wc.lpszClassName, "Rockstar Editor+ - Render Settings",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 736, 500, nullptr, nullptr, hInst, nullptr);
    if (!w) return 1;

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageA(w, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
