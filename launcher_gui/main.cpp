// SPDX-License-Identifier: GPL-2.0-or-later
// EverybodysGolf.exe: the player's launcher window. It sits in the install folder next to
// everybodys-golf.exe (the game) and keeps everything it and the game write in data\ beside it:
//     data\paths.ini, data\game\     the prepared game (everybodys-golf.exe --prepare-only)
//     data\dump\                      the base game and update 1.17 merged, when given apart
//     data\save\settings.ini          the game's settings (the Display page edits them)
//     data\textures\dump, png, pack   textures collected while playing, and the HD pack built from them
//     data\logs\launcher.log          what the jobs printed
// Pages: Play, Display, Saves, HD textures (collect, upscale with tools\realesrgan through
// tools\python and tools\scripts\texture_pack.py, use), Online (host the title server in
// tools\server or join a friend's), Game files (pick the dump, merge, prepare).
// Nothing is written outside the install folder: no registry, no AppData.

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <oleauto.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "update_logic.h"
#include "../src/invite_code.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#include "miniz.h"
#pragma clang diagnostic pop

#ifndef GOLF_LAUNCHER_VERSION
#define GOLF_LAUNCHER_VERSION "development"
#endif
#include "../src/trophy_record.h"

namespace fs = std::filesystem;

// tools/package.py --version reads this to check the build is the release it names.
extern "C" __attribute__((used)) const char golf_version_marker[] = "GOLF_VERSION=" GOLF_LAUNCHER_VERSION;

namespace {

constexpr wchar_t kTitle[] = L"Everybody's Golf";
// Where the update check asks for the newest release. GOLF_UPDATE_API_URL replaces it for tests only
// (a local server); then plain http to this PC is allowed too.
constexpr wchar_t kReleaseApi[] = L"https://api.github.com/repos/finalpantasy/everybodys-golf-ps4-pc-port/releases/latest";
enum : UINT { WM_JOB_LINE = WM_APP + 1, WM_JOB_PROGRESS, WM_JOB_DONE, WM_GAME_EXIT, WM_SERVER_STATE,
              WM_UPDATE_FOUND, WM_UPDATE_STATUS, WM_UPDATE_PROGRESS, WM_UPDATE_READY };
enum Id : int {
    ID_TABS = 100, ID_PLAY, ID_OPEN_FOLDER, ID_OPEN_LOGS, ID_STATUS,
    ID_BASE, ID_BASE_BROWSE, ID_UPDATE, ID_UPDATE_BROWSE, ID_DLC, ID_DLC_BROWSE, ID_SETUP, ID_SETUP_PROGRESS, ID_SETUP_STATUS,
    ID_WINDOW, ID_SIZE, ID_RENDER, ID_SIXTY, ID_SMAA, ID_SHARPEN, ID_VSYNC, ID_HDR, ID_INTRO, ID_MISSFLASH,
    ID_MONITOR, ID_PRO, ID_SHARPNESS, ID_ANISO, ID_LOD, ID_FRAMEGEN, ID_VOLUME, ID_SPEAKERS, ID_TITLE_BACK,
    ID_DEADZONE, ID_OUTER, ID_TRIGGER, ID_RUMBLE, ID_SWAP, ID_OPEN_SETTINGS, ID_DISPLAY_NOTE, ID_GRAPHICS_NOTE,
    ID_CHECK_UPDATES, ID_CHECK_NOW, ID_VERSION,
    ID_COLLECT, ID_TEX_COUNT, ID_SCALE, ID_BUILD, ID_TEX_PROGRESS, ID_TEX_STATUS, ID_USE_PACK,
    ID_PROFILES, ID_PROFILE_NAME, ID_PROFILE_USE, ID_PROFILE_NEW, ID_PROFILE_COPY, ID_PROFILE_RENAME,
    ID_PROFILE_DELETE, ID_PROFILE_BACKUP, ID_PROFILE_OPEN, ID_PROFILE_STATUS,
    ID_TROPHY_LIST, ID_TROPHY_STATUS,
    ID_ONLINE_NAME, ID_ONLINE_PORT, ID_ONLINE_HOST, ID_SERVER, ID_UPNP, ID_INVITE, ID_INVITE_CODE, ID_FIND, ID_FOUND,
    ID_ONLINE_STATUS,
    ID_NOTES, ID_UPDATE_NOW, ID_UPDATE_LATER, ID_UPDATE_SKIP, ID_UPDATE_BAR, ID_UPDATE_TEXT,
    ID_COLLAB, ID_LANGUAGE,
};
// The pages, in tab order.
enum Page : int { PagePlay, PageDisplay, PageGraphics, PageControls, PageSaves, PageTrophies, PageTextures, PageOnline, PageFiles, PageCount };

// --- paths ------------------------------------------------------------------------------------------

fs::path root, data, save, ini_path, runtime_exe, logs;

fs::path exe_folder() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
        if (n < buffer.size()) { buffer.resize(n); break; }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

size_t count_files(const fs::path& dir, const wchar_t* extension) {
    std::error_code ec;
    size_t n = 0;
    if (!fs::is_directory(dir, ec)) return 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && (!extension || _wcsicmp(it->path().extension().c_str(), extension) == 0)) ++n;
    return n;
}

// --- settings.ini: key=value lines, comments and unknown keys kept as they are -----------------------

struct Ini {
    std::vector<std::string> lines;
    void load(const fs::path& path) {
        lines.clear();
        std::ifstream f(path, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (lines.empty() && line.rfind("\xef\xbb\xbf", 0) == 0) line.erase(0, 3);
            lines.push_back(line);
        }
    }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        for (const auto& line : lines)
            if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
        return fallback;
    }
    void set(const std::string& key, const std::string& value) {
        for (auto& line : lines)
            if (line.rfind(key + "=", 0) == 0) { line = key + "=" + value; return; }
        lines.push_back(key + "=" + value);
    }
    bool save(const fs::path& path) const {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path temp = path; temp += L".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            for (const auto& line : lines) f << line << "\r\n";
            if (!f) return false;
        }
        return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
};
Ini settings;

void load_settings() {
    std::error_code ec;
    if (!fs::exists(ini_path, ec)) {
        fs::create_directories(save, ec);
        for (const fs::path& example : {root / L"settings.example.ini", root / L"prepare" / L"settings.example.ini"})
            if (fs::exists(example, ec)) { fs::copy_file(example, ini_path, ec); break; }
    }
    settings.load(ini_path);
}
void set_setting(const char* key, const std::string& value) {
    settings.load(ini_path); // the game may have saved its Display Settings since
    settings.set(key, value);
    settings.save(ini_path);
}

// The launcher's own choices (data\launcher.ini): which save the game uses.
Ini prefs;
fs::path prefs_path;
void set_pref(const char* key, const std::string& value) {
    prefs.set(key, value);
    prefs.save(prefs_path);
}

// --- saves: each is a folder the game keeps its savedata in. "Main" is data\save (where the game
// has always saved); the others are data\saves\<name>. settings.ini stays in data\save for all.

const std::wstring kMainSave = L"Main";
fs::path save_dir(const std::wstring& name) { return name == kMainSave ? save : data / L"saves" / name; }
std::wstring current_save() {
    std::wstring name = wide(prefs.get("save"));
    std::error_code ec;
    return name.empty() || !fs::is_directory(save_dir(name), ec) ? kMainSave : name;
}
std::vector<std::wstring> list_saves() {
    std::vector<std::wstring> names{kMainSave};
    std::error_code ec;
    std::vector<std::wstring> others;
    for (auto it = fs::directory_iterator(data / L"saves", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) others.push_back(it->path().filename().wstring());
    std::sort(others.begin(), others.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    names.insert(names.end(), others.begin(), others.end());
    return names;
}
// The newest file time under a save's savedata folder; false when it holds no game save yet.
bool last_saved(const std::wstring& name, SYSTEMTIME& local) {
    std::error_code ec;
    FILETIME newest{};
    bool any = false;
    for (auto it = fs::recursive_directory_iterator(save_dir(name) / L"savedata", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!it->is_regular_file(ec) || !GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &info)) continue;
        if (!any || CompareFileTime(&info.ftLastWriteTime, &newest) > 0) { newest = info.ftLastWriteTime; any = true; }
    }
    FILETIME lt;
    return any && FileTimeToLocalFileTime(&newest, &lt) && FileTimeToSystemTime(&lt, &local);
}
// A save name the folder can take: 1 to 32 characters, none of \ / : * ? " < > |, no dot or space at
// the end, not a reserved device name.
bool valid_save_name(const std::wstring& name) {
    if (name.empty() || name.size() > 32 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    if (name.back() == L'.' || name.back() == L' ' || name.front() == L' ') return false;
    for (wchar_t c : name) if (c < 32) return false;
    std::wstring upper = name;
    for (auto& c : upper) c = towupper(c);
    for (const wchar_t* bad : {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"LPT1", L"LPT2", L"LPT3"})
        if (upper == bad) return false;
    return _wcsicmp(name.c_str(), kMainSave.c_str()) != 0;
}
bool copy_tree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (fs::exists(from, ec)) fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}
// Moves a folder to the Recycle Bin, so a deleted save can still be brought back.
bool recycle(const fs::path& folder) {
    std::wstring from = folder.wstring();
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

std::atomic<bool> game_running{false};
void refresh_saves();
void refresh_trophies();
void refresh_online();

// --- jobs: one at a time on a worker thread; their output goes to the page and to launcher.log -------

HWND window;
std::atomic<bool> busy{false};
std::ofstream job_log;

void post_line(const std::wstring& line) {
    PostMessageW(window, WM_JOB_LINE, 0, (LPARAM)new std::wstring(line));
}
// Sets a progress bar at once. Themed bars animate only forward steps, so the bar is pushed one
// past the target (with the range widened for 100) and set back, which draws without animation.
void set_bar(HWND bar, int percent) {
    SendMessageW(bar, PBM_SETRANGE32, 0, 101);
    SendMessageW(bar, PBM_SETPOS, percent + 1, 0);
    SendMessageW(bar, PBM_SETPOS, percent, 0);
    SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    SendMessageW(bar, PBM_SETPOS, percent, 0);
}

void post_progress(int percent) { PostMessageW(window, WM_JOB_PROGRESS, (WPARAM)percent, 0); }

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

// Runs a tool with no console window at below-normal priority; each output line goes to on_line.
// Returns its exit code, or -1 when it could not start.
int run_tool(const std::vector<std::wstring>& argv, const std::function<void(const std::string&)>& on_line) {
    std::wstring command;
    for (const auto& arg : argv) command += (command.empty() ? L"" : L" ") + quote(arg);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    job_log << "> " << utf8(command) << "\n";
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, root.c_str(), &si, &pi);
    CloseHandle(write);
    if (!ok) { CloseHandle(read); return -1; }
    std::string pending;
    char chunk[4096];
    DWORD got;
    while (ReadFile(read, chunk, sizeof chunk, &got, nullptr) && got) {
        pending.append(chunk, got);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("progress ", 0) != 0) job_log << line << "\n";
            on_line(line);
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty()) { job_log << pending << "\n"; on_line(pending); }
    job_log.flush();
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

void start_job(std::function<bool()> body) {
    if (busy.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(logs, ec);
    if (!job_log.is_open()) job_log.open(logs / L"launcher.log", std::ios::app | std::ios::binary);
    std::thread([body] {
        bool ok = false;
        try { ok = body(); } catch (const std::exception& e) { post_line(L"Error: " + wide(e.what())); }
        PostMessageW(window, WM_JOB_DONE, ok, 0);
    }).detach();
}

// --- game files ---------------------------------------------------------------------------------------

std::string sfo_string(const fs::path& sfo, const char* key) {
    std::ifstream f(sfo, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 20 || b.compare(0, 4, "\0PSF", 4)) return {};
    auto u32 = [&](size_t at) { return at + 4 <= b.size() ? *(const uint32_t*)(b.data() + at) : 0u; };
    auto u16 = [&](size_t at) { return at + 2 <= b.size() ? *(const uint16_t*)(b.data() + at) : 0u; };
    uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count; ++i) {
        size_t entry = 20 + i * 16;
        const char* name = b.data() + keys + u16(entry);
        if (keys + u16(entry) >= b.size() || strcmp(name, key)) continue;
        size_t at = values + u32(entry + 12), len = u32(entry + 4);
        if (at + len > b.size()) return {};
        std::string value(b.data() + at, len);
        value.resize(strlen(value.c_str()));
        return value;
    }
    return {};
}

// The base game with the update laid over it, in data\dump: hard links where both are on the same
// drive (no copy, nothing changes in the dump), copies otherwise. An update file of 0 bytes over a base
// file that has data keeps the base's (tools/stage_game.py).
bool merge_dump(const fs::path& base, const fs::path& update, const fs::path& out) {
    std::map<std::wstring, fs::path> files;
    for (const fs::path& from : {base, update}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::wstring relative = fs::relative(it->path(), from, ec).wstring();
            auto old = files.find(relative);
            if (from == update && old != files.end() && fs::file_size(it->path(), ec) == 0 && fs::file_size(old->second, ec) != 0)
                continue;
            files[relative] = it->path();
        }
        if (ec) { post_line(L"Cannot read " + from.wstring()); return false; }
    }
    size_t done = 0, linked = 0;
    for (const auto& [relative, source] : files) {
        std::error_code ec;
        fs::path target = out / relative;
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            if (fs::file_size(target, ec) == fs::file_size(source, ec) &&
                fs::last_write_time(target, ec) == fs::last_write_time(source, ec)) { ++done; continue; }
            fs::remove(target, ec);
        }
        if (CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) ++linked;
        else if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            post_line(L"Cannot copy " + source.wstring());
            return false;
        }
        if (++done % 64 == 0 || done == files.size()) {
            post_progress((int)(done * 100 / files.size()));
            post_line(L"Merging the update: " + std::to_wstring(done) + L" of " + std::to_wstring(files.size()) + L" files");
        }
    }
    post_line(std::to_wstring(files.size()) + L" files in data\\dump (" + std::to_wstring(linked) + L" linked, the rest copied)");
    return true;
}

// --- window -------------------------------------------------------------------------------------------

HFONT font, big_font, title_font, small_font;
int dpi = 96;
int px(int v) { return MulDiv(v, dpi, 96); }
std::vector<std::vector<HWND>> pages;
HWND tabs, status_text, setup_status, setup_progress, tex_status, tex_progress, tex_count;
HWND progress_target, status_target;
std::map<int, HWND> controls;

// The look: a fairway-green band across the top with a flag on a putting green (drawn here, no
// artwork files), the pages on an off-white, dark green text, and the main buttons in green.
namespace Ink {
constexpr COLORREF Band = RGB(0x1f, 0x6b, 0x35), BandDark = RGB(0x17, 0x52, 0x29), Green = RGB(0x7c, 0xc1, 0x5a),
                   Page = RGB(0xf6, 0xf9, 0xf3), Text = RGB(0x1c, 0x2e, 0x21), White = RGB(0xff, 0xff, 0xff),
                   Flag = RGB(0xf2, 0xc9, 0x4c), Button = RGB(0x24, 0x7a, 0x3d), ButtonHot = RGB(0x2c, 0x8f, 0x48),
                   ButtonOff = RGB(0xb8, 0xc4, 0xb9), Hole = RGB(0x10, 0x24, 0x15);
}
HBRUSH page_brush;
constexpr int kBand = 64;     // height of the green band
constexpr int kPageTop = 64;  // how far the pages sit below where they were drawn without the band
// Owner-drawn green buttons (Play, Set up game files, Update now).
bool green_button(int id) { return id == ID_PLAY || id == ID_SETUP || id == ID_UPDATE_NOW; }

HWND add(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr) {
    if (page >= 0) y += kPageTop;
    if (green_button(id)) style = (style & ~(BS_DEFPUSHBUTTON | BS_PUSHBUTTON)) | BS_OWNERDRAW;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, px(x), px(y), px(w), px(h), window,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : font), TRUE);
    if (page >= 0) pages[page].push_back(c);
    if (id) controls[id] = c;
    return c;
}
std::wstring text_of(int id) {
    HWND c = controls[id];
    std::wstring s(GetWindowTextLengthW(c) + 1, L'\0');
    GetWindowTextW(c, s.data(), (int)s.size());
    s.resize(wcslen(s.c_str()));
    return s;
}
bool checked(int id) { return SendMessageW(controls[id], BM_GETCHECK, 0, 0) == BST_CHECKED; }
void check(int id, bool on) { SendMessageW(controls[id], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

struct Choice { const wchar_t* label; const char* value; };
// Next start; Earn in game sells them in the shop's coin tab for 1500 to 2500 coins. The cup modes
// run the 2018 collab cups on the title server (tools\server\collab_cups.py), whose prizes are the
// outfits, and sell the parts for 500 coins (Classic) or 1500 to 2500 (Hardcore).
const std::vector<Choice> collab_modes = {{L"All unlocked", "unlocked"}, {L"Earn in game (shop)", "earn"},
                                          {L"Classic cups (win or buy for 500)", "classic"},
                                          {L"Hardcore cups (cut -3, 3 tries a day)", "hardcore"}, {L"Original", "original"}};
// settings.ini language: the four the disc has (gpu/shim/resogun_settings.h), for the next start.
const std::vector<Choice> languages = {{L"English", "english"}, {L"Fran\u00e7ais", "french"},
                                       {L"Portugu\u00eas (Brasil)", "portuguese"},
                                       {L"Espa\u00f1ol (Latinoam\u00e9rica)", "spanish"}};
const std::vector<Choice> miss_flash_modes = {{L"Off", "0"}, {L"On (original)", "1"}};
const std::vector<Choice> window_modes = {{L"Borderless window (recommended)", "borderless"}, {L"Fullscreen", "fullscreen"}, {L"Window", "windowed"}};
const std::vector<Choice> sizes = {{L"1280 x 720", "1280x720"}, {L"1600 x 900", "1600x900"}, {L"1920 x 1080", "1920x1080"},
                                   {L"2560 x 1440", "2560x1440"}, {L"3840 x 2160", "3840x2160"}};
const std::vector<Choice> renders = {{L"Game's own (1920 x 1080 in 60 FPS mode)", "0"}, {L"1920 x 1080", "1080"},
                                     {L"2560 x 1440", "1440"}, {L"3200 x 1800", "1800"}, {L"3840 x 2160 (4K)", "2160"},
                                     {L"5120 x 2880 (5K, very demanding)", "2880"}};
// settings.ini render_resolution: the game in PS4 Pro mode (its 4K output) or as on a PS4.
const std::vector<Choice> pro_modes = {{L"PS4 Pro (recommended)", "4k"}, {L"PS4 (1080p, no 60 FPS mode)", "1080p"}};
const std::vector<Choice> monitors = {{L"Primary display", ""}, {L"Display 1", "0"}, {L"Display 2", "1"},
                                      {L"Display 3", "2"}, {L"Display 4", "3"}};
// VSync: one list for the vsync and tearing keys.
const std::vector<Choice> vsync_modes = {{L"On (no tearing)", "1,0"}, {L"Off (newest frame each refresh)", "0,0"},
                                         {L"Off, tearing allowed (least delay)", "0,1"}};
const std::vector<Choice> sharpness = {{L"Soft", "soft"}, {L"Default", "default"}, {L"Strong", "strong"}};
const std::vector<Choice> anisotropy = {{L"Game's own", "off"}, {L"4x", "4"}, {L"8x", "8"}, {L"16x", "16"}};
const std::vector<Choice> lod_modes = {{L"Off (game's own)", "off"}, {L"Auto (sharper above 1080 lines)", "auto"}};
const std::vector<Choice> volumes = {{L"100% (game's own level)", "100"}, {L"90%", "90"}, {L"80%", "80"}, {L"70%", "70"},
                                     {L"60%", "60"}, {L"50%", "50"}, {L"40%", "40"}, {L"30%", "30"}, {L"20%", "20"},
                                     {L"10%", "10"}, {L"Muted", "0"}};
const std::vector<Choice> speakers = {{L"Same as Windows", "auto"}, {L"Stereo", "stereo"}, {L"5.1", "5.1"}, {L"7.1", "7.1"}};
const std::vector<Choice> deadzones = {{L"None (raw stick)", "0"}, {L"3%", "3"}, {L"6% (worn sticks)", "6"}, {L"10%", "10"},
                                       {L"15%", "15"}, {L"20%", "20"}};
const std::vector<Choice> outer_zones = {{L"None", "0"}, {L"2%", "2"}, {L"5%", "5"}, {L"10%", "10"}};
const std::vector<Choice> triggers = {{L"Light (pressed at 6%)", "15"}, {L"Default (12%)", "30"}, {L"Half (50%)", "128"},
                                      {L"Deep (80%)", "204"}};
const std::vector<Choice> rumbles = {{L"100%", "100"}, {L"75%", "75"}, {L"50%", "50"}, {L"25%", "25"}, {L"Off", "0"}};
const std::vector<Choice> scales = {{L"2x (1024 becomes 2048)", "2"}, {L"4x (1024 becomes 4096, slow)", "4"}};

void fill_combo(int id, const std::vector<Choice>& choices, const std::string& current) {
    HWND c = controls[id];
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t i = 0; i < choices.size(); ++i) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)choices[i].label);
        if (current == choices[i].value) selected = (int)i;
    }
    if (selected < 0 && !current.empty()) {
        // A value typed into settings.ini by hand: shown as it is, and kept until another is picked.
        std::wstring own = wide(current) + L" (from settings.ini)";
        selected = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)own.c_str());
    }
    SendMessageW(c, CB_SETCURSEL, selected < 0 ? 0 : selected, 0);
}
// The picked choice's value; false for the settings.ini value fill_combo kept.
bool combo_pick(int id, const std::vector<Choice>& choices, std::string& value) {
    int i = (int)SendMessageW(controls[id], CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= (int)choices.size()) return false;
    value = choices[i].value;
    return true;
}
std::string combo_value(int id, const std::vector<Choice>& choices) {
    std::string value;
    return combo_pick(id, choices, value) ? value : choices[0].value;
}

struct Paths { std::wstring game, dlc, image; };
Paths saved_paths() {
    Paths p;
    std::ifstream f(data / L"paths.ini", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("\xef\xbb\xbf", 0) == 0) line.erase(0, 3);
        if (line.rfind("game=", 0) == 0) p.game = wide(line.substr(5));
        else if (line.rfind("dlc=", 0) == 0) p.dlc = wide(line.substr(4));
        else if (line.rfind("image=", 0) == 0) p.image = wide(line.substr(6));
    }
    return p;
}
bool game_ready() {
    Paths p = saved_paths();
    std::error_code ec;
    return !p.game.empty() && !p.image.empty() && fs::is_directory(data / L"game" / p.image, ec);
}

void refresh_status() {
    std::wstring s;
    Paths p = saved_paths();
    s += game_ready() ? L"Game files: ready (" + p.game + L")" : L"Game files: not set up yet. Open Game files to choose your dump.";
    s += L"\r\nSave in use: " + current_save();
    s += L"\r\n\r\nRender size: ";
    std::string h = settings.get("render_height", "0");
    for (const auto& r : renders) if (h == r.value) s += r.label;
    size_t packed = count_files(data / L"textures" / L"pack", nullptr);
    s += L"\r\nHD textures: ";
    s += settings.get("texture_pack").empty() || settings.get("hd_textures", "1") == "0" ? L"off" : L"on, " + std::to_wstring(packed) + L" textures";
    s += L"\r\n\r\nEverything the game writes stays in " + data.wstring();
    SetWindowTextW(status_text, s.c_str());
    EnableWindow(controls[ID_PLAY], game_ready() && !busy);
    size_t dumped = count_files(data / L"textures" / L"dump", L".dds");
    std::wstring t = std::to_wstring(dumped) + L" textures collected, " + std::to_wstring(packed) + L" in the HD pack.";
    SetWindowTextW(tex_count, t.c_str());
}

void show_page(int page) {
    for (size_t i = 0; i < pages.size(); ++i)
        for (HWND c : pages[i]) ShowWindow(c, (int)i == page ? SW_SHOW : SW_HIDE);
    if (page == PageSaves) refresh_saves();
    if (page == PageTrophies) refresh_trophies();
    if (page == PageOnline) refresh_online();
    refresh_status();
}

void set_busy(bool on) {
    for (int id : {ID_SETUP, ID_BUILD, ID_BASE_BROWSE, ID_UPDATE_BROWSE, ID_DLC_BROWSE}) EnableWindow(controls[id], !on);
    EnableWindow(controls[ID_PLAY], !on && game_ready());
}

std::wstring browse_folder(const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// The file a "Prepare: <path>: <why>" line names. The path may hold a drive colon ("A:/x"), so the
// split is at the first ": " (colon and space), not the first ':'.
std::string problem_file(const std::string& problem) { return problem.substr(0, problem.find(": ")); }

// A pasted folder: surrounding spaces and quotes ("D:\dumps\golf") removed.
fs::path pasted_folder(std::wstring text) {
    const wchar_t* trim = L" \t\r\n\"";
    size_t a = text.find_first_not_of(trim), b = text.find_last_not_of(trim);
    return a == std::wstring::npos ? fs::path() : fs::path(text.substr(a, b - a + 1));
}

// What to do about a failed preparation, in plain words, from the preparer's last complaint.
std::wstring prepare_advice(const std::string& problem, int code) {
    const std::wstring log = L" Details are in data\\logs\\launcher.log.";
    auto has = [&](const char* text) { return problem.find(text) != std::string::npos; };
    if (has("encrypted") || has("expected a plaintext"))
        return L"A game file in your dump is still encrypted (" + wide(problem_file(problem)) +
               L"). The port needs a decrypted dump: dump the game again with a tool that decrypts it." + log;
    if (has("EBOOT hash differs"))
        return L"This eboot.bin is not the one from update 1.17. Choose the 1.17 update folder (or a dump with the update "
               L"merged in), then press Set up game files again." + log;
    if (has("expected CUSA04687"))
        return L"This dump is not Everybody's Golf (CUSA04687) with update 1.17. Choose that dump, or add the 1.17 update "
               L"folder." + log;
    if (has("cannot open") || has("cannot read") || has("empty or oversized"))
        return L"A file the game needs is missing or unreadable in your dump (" + wide(problem_file(problem)) +
               L"). Choose the whole game folder, the one with eboot.bin, sce_module and sce_sys." + log;
    if (code == -1) return L"everybodys-golf.exe could not be started. Unpack the whole release again into a new folder.";
    if (!problem.empty())
        return L"Setting up failed: " + wide(problem) + L". Check that the dump is complete and decrypted." + log;
    return L"Setting up failed (exit " + std::to_wstring(code) + L"). Check that the dump is complete and decrypted." + log;
}

void set_up_game() {
    fs::path base = pasted_folder(text_of(ID_BASE)), update = pasted_folder(text_of(ID_UPDATE)), dlc = pasted_folder(text_of(ID_DLC));
    std::error_code ec;
    if (base.empty() || !fs::exists(base / L"eboot.bin", ec)) {
        MessageBoxW(window, L"Choose the folder of your game dump: the one that holds eboot.bin and sce_sys.", kTitle, MB_ICONWARNING);
        return;
    }
    if (!fs::is_directory(base / L"sce_module", ec)) {
        MessageBoxW(window, L"This folder has eboot.bin but no sce_module folder, so it is not the whole game. Choose the complete dump "
                            L"(eboot.bin, sce_module, sce_sys and the game's data).", kTitle, MB_ICONWARNING);
        return;
    }
    if (!update.empty() && !fs::exists(update / L"eboot.bin", ec)) {
        MessageBoxW(window, L"The update folder has no eboot.bin. Choose the folder of the 1.17 update, or leave it empty when your dump already has it.", kTitle, MB_ICONWARNING);
        return;
    }
    fs::path have = update.empty() ? base : update;
    std::string title = sfo_string(have / L"sce_sys" / L"param.sfo", "TITLE_ID"), version = sfo_string(have / L"sce_sys" / L"param.sfo", "APP_VER");
    if (!title.empty() && (title != "CUSA04687" || version != "01.17")) {
        std::wstring ask = L"This dump is " + wide(title) + L" version " + wide(version) +
                           L". The port is made for CUSA04687 with update 1.17 and may not start with other files. Go on anyway?";
        if (MessageBoxW(window, ask.c_str(), kTitle, MB_YESNO | MB_ICONWARNING) != IDYES) return;
    }
    progress_target = setup_progress; status_target = setup_status;
    set_busy(true);
    bool any = !title.empty() && (title != "CUSA04687" || version != "01.17");
    start_job([base, update, dlc, any] {
        fs::path game = base;
        if (!update.empty()) {
            game = data / L"dump";
            if (!merge_dump(base, update, game)) return false;
        }
        post_progress(-1);
        post_line(L"Preparing the game's code for this PC (a few minutes the first time)...");
        std::vector<std::wstring> argv = {runtime_exe.wstring(), L"--data-dir", data.wstring(), L"--game", game.wstring(), L"--prepare-only", L"--console"};
        if (!dlc.empty()) { argv.push_back(L"--dlc"); argv.push_back(dlc.wstring()); }
        if (any) argv.push_back(L"--any-build");
        std::string problem; // the preparer's "Prepare: <file>: <why>" or "ERROR: <why>"
        int code = run_tool(argv, [&problem](const std::string& line) {
            if (line.rfind("Prepare: ", 0) == 0 || line.rfind("ERROR: ", 0) == 0) problem = line.substr(line.find(' ') + 1);
            if (!line.empty()) post_line(wide(line));
        });
        post_line(code == 0 ? L"Game files are ready. Press Play." : prepare_advice(problem, code));
        return code == 0;
    });
}

void build_textures() {
    fs::path python = root / L"tools" / L"python" / L"python.exe", script = root / L"tools" / L"scripts" / L"texture_pack.py",
             esrgan = root / L"tools" / L"realesrgan" / L"realesrgan-ncnn-vulkan.exe";
    fs::path dump = data / L"textures" / L"dump", png = data / L"textures" / L"png", pack = data / L"textures" / L"pack";
    std::error_code ec;
    if (!fs::exists(python, ec) || !fs::exists(script, ec)) {
        MessageBoxW(window, L"The texture tools are missing from tools\\. Reinstall the game to get them back.", kTitle, MB_ICONERROR);
        return;
    }
    size_t dumped = count_files(dump, L".dds");
    if (!dumped) {
        MessageBoxW(window, L"No textures collected yet. Tick \"Collect textures while playing\", play a while (each place you visit adds its textures), then come back.", kTitle, MB_ICONINFORMATION);
        return;
    }
    std::string scale = combo_value(ID_SCALE, scales);
    progress_target = tex_progress; status_target = tex_status;
    set_busy(true);
    start_job([=] {
        std::error_code e;
        fs::create_directories(png, e);
        post_line(L"Converting " + std::to_wstring(dumped) + L" collected textures...");
        post_progress(-1);
        if (run_tool({python.wstring(), L"-I", script.wstring(), L"convert", dump.wstring(), png.wstring()},
                     [](const std::string& line) { if (!line.empty()) post_line(wide(line)); }) != 0) return false;
        post_line(L"Upscaling...");
        std::vector<std::wstring> argv = {python.wstring(), L"-I", script.wstring(), L"upscale", png.wstring(), pack.wstring(),
                                          L"--scale", wide(scale), L"--format", L"dds", L"--jobs", L"4"};
        if (fs::exists(esrgan, e)) { argv.push_back(L"--esrgan"); argv.push_back(esrgan.wstring()); }
        // texture_pack.py prints "progress upscaling|writing DONE/TOTAL": Real-ESRGAN is the first
        // 90% of the bar, writing the block-compressed files the rest.
        int code = run_tool(argv, [](const std::string& line) {
            char stage[32];
            unsigned done = 0, total = 0;
            if (sscanf(line.c_str(), "progress %31s %u/%u", stage, &done, &total) == 3 && total) {
                bool ai = strcmp(stage, "upscaling") == 0;
                post_progress(ai ? (int)(done * 90ull / total) : 90 + (int)(done * 10ull / total));
                post_line((ai ? L"Upscaling with Real-ESRGAN: " : L"Writing the pack: ") + std::to_wstring(done) +
                          L" of " + std::to_wstring(total) + L" textures");
            } else if (!line.empty()) {
                post_line(wide(line));
            }
        });
        post_progress(100);
        post_line(code == 0 ? L"HD texture pack built: " + std::to_wstring(count_files(pack, nullptr)) + L" textures. It is used from the next start."
                            : L"Building the pack failed (exit " + std::to_wstring(code) + L"); see data\\logs\\launcher.log.");
        return code == 0;
    });
}


// --- the Online page ----------------------------------------------------------------------------------
// Host a game runs the bundled title server (tools\server\host.py --online) on this PC: it keeps a
// room key, asks the router to open its one TCP port (UPnP, NAT-PMP or PCP), and writes invite codes
// to data\server\online.txt. A friend pastes the code into Join and presses Play. Every packet
// between the players goes through the host's server, so only the host may need an open port.
// Find games on my network asks the home network for hosts (UDP broadcast on port 48765).

const char* const kDefaultPort = "48765";
constexpr unsigned short kLanDiscoveryPort = 48765;
constexpr UINT_PTR kOnlineTimer = 7;
HANDLE server_process = nullptr, server_job = nullptr;
// What the running server was started for: loopback only (the collab cups of an offline Play) or
// hosting, and its collab_items cup mode ("" when none).
bool server_loopback = false;
std::string server_cups;
std::atomic<bool> server_ready{false};
std::mutex lan_lock;
std::vector<std::pair<std::wstring, std::wstring>> lan_games; // (code, host name), under lan_lock
std::atomic<bool> lan_searching{false};
bool lan_searched = false;
int lan_generation = 0, lan_listed = -1; // the list box is rebuilt only after a new search

// "host" (Host a game was pressed and not stopped), "join" or "off".
std::string online_mode() {
    std::string mode = prefs.get("online", "off");
    return mode == "host" || mode == "join" ? mode : "off";
}
// The port in launcher.ini when it is 1 to 65535, else 48765.
std::string online_port() {
    std::string port = prefs.get("online_port", kDefaultPort);
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return kDefaultPort;
    long n = std::stol(port);
    return n >= 1 && n <= 65535 ? std::to_string(n) : kDefaultPort;
}
// A PSN-style online ID: 3 to 16 letters, digits, - or _.
bool valid_online_name(const std::string& name) {
    return name.size() >= 3 && name.size() <= 16 &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
// The player's name; a new install gets "Golfer-" and four random digits, so two friends who never
// change it still get separate accounts.
std::string online_name() {
    std::string name = prefs.get("online_name");
    if (name.empty()) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        char text[16];
        snprintf(text, sizeof text, "Golfer-%04u", (unsigned)((t.QuadPart ^ GetCurrentProcessId() * 2654435761u) % 10000));
        name = text;
        set_pref("online_name", name);
    }
    return name;
}
// What the Join box holds: an invite code, or HOST[:PORT] (home network or VPN address).
bool valid_join(const std::string& text) {
    GolfInvite code;
    if (golf_invite_decode(text.c_str(), &code)) return true;
    if (golf_invite_shaped(text.c_str())) return false; // a mistyped code
    const size_t colon = text.find(':');
    const std::string host = text.substr(0, colon);
    if (host.empty() || host.size() > 253 ||
        host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
        return false;
    if (colon == std::string::npos) return true;
    const std::string port = text.substr(colon + 1);
    return !port.empty() && port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos &&
           std::stol(port) >= 1 && std::stol(port) <= 65535;
}
std::string trimmed(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
bool server_running() { return server_process && WaitForSingleObject(server_process, 0) == WAIT_TIMEOUT; }

// IPv4 addresses of this PC's adapters that are up (a VPN such as Tailscale or ZeroTier shows here
// too), with the adapter's name and directed broadcast address; the one with a default gateway (the
// home network) first.
struct LocalAddress { std::wstring address, adapter; bool gateway; unsigned long broadcast; };
std::vector<LocalAddress> local_addresses() {
    std::vector<LocalAddress> out;
    ULONG size = 32768;
    std::vector<BYTE> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    }
    if (result != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buffer.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        // Hyper-V's and WSL's virtual switches reach only this PC's own virtual machines.
        if ((a->FriendlyName && !wcsncmp(a->FriendlyName, L"vEthernet", 9)) || (a->Description && wcsstr(a->Description, L"Hyper-V")))
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* b = (const unsigned char*)&((const sockaddr_in*)u->Address.lpSockaddr)->sin_addr;
            if (b[0] == 169 && b[1] == 254) continue; // no DHCP answer: nobody can reach it
            wchar_t text[16];
            swprintf(text, 16, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            const unsigned long host = (unsigned long)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
            const unsigned prefix = u->OnLinkPrefixLength <= 32 ? u->OnLinkPrefixLength : 24;
            const unsigned long mask = prefix ? 0xFFFFFFFFul << (32 - prefix) & 0xFFFFFFFFul : 0;
            out.push_back({text, a->FriendlyName ? a->FriendlyName : L"", a->FirstGatewayAddress != nullptr,
                           (host & mask) | (~mask & 0xFFFFFFFFul)});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LocalAddress& x, const LocalAddress& y) { return x.gateway > y.gateway; });
    return out;
}

// data\server\online.txt, written by host.py --online: port=, key=, lan=/vpn=/internet= "ADDRESS CODE",
// portmap= and note= lines.
struct HostStatus {
    std::wstring internet, internet_code, note, portmap;
    std::vector<std::pair<std::wstring, std::wstring>> local; // (address, code)
};
HostStatus host_status() {
    HostStatus s;
    std::ifstream f(data / L"server" / L"online.txt", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const size_t space = value.find(' ');
        if (key == "note") s.note = wide(value);
        else if (key == "portmap") s.portmap = wide(value);
        else if (space != std::string::npos && (key == "lan" || key == "vpn" || key == "internet")) {
            const std::wstring address = wide(value.substr(0, space)), code = wide(value.substr(space + 1));
            if (key == "internet") { s.internet = address; s.internet_code = code; }
            else s.local.push_back({address, code});
        }
    }
    return s;
}
// The code to hand out: the internet one when the router told us the public address, else the
// home network one.
std::wstring main_invite_code(const HostStatus& s) {
    if (!s.internet_code.empty()) return s.internet_code;
    return s.local.empty() ? L"" : s.local.front().second;
}

// Asks host.py to stop (it closes the router port on the way out), then ends it.
void stop_server() {
    if (server_process) {
        std::ofstream(data / L"server" / L"stop").put('1');
        if (WaitForSingleObject(server_process, 4000) == WAIT_TIMEOUT) TerminateProcess(server_process, 0);
        WaitForSingleObject(server_process, 5000);
        CloseHandle(server_process);
        server_process = nullptr;
    }
    server_ready = false;
    KillTimer(window, kOnlineTimer);
    std::error_code ec;
    fs::remove(data / L"server" / L"online.txt", ec);
}

// settings.ini collab_items when it is a cup mode (classic or hardcore), else "".
std::string cup_mode() {
    const std::string mode = settings.get("collab_items", "unlocked");
    return mode == "classic" || mode == "hardcore" ? mode : "";
}

// Starts tools\server\host.py --online on every network adapter of this PC, or with `loopback` a
// server only this PC can reach (no router port, no invite codes) for the collab cups of an offline
// Play. Its output goes to data\logs\server.log. The process is in a job that ends it when the
// launcher closes.
bool start_server(std::wstring& error, bool loopback = false) {
    if (server_running() && server_loopback == loopback && server_cups == cup_mode()) return true;
    stop_server();
    Paths p = saved_paths();
    const fs::path eboot = data / L"game" / p.image / L"eboot.elf", tools = root / L"tools";
    const fs::path python = tools / L"python" / L"python.exe", host = tools / L"server" / L"host.py",
                   crypto = tools / L"server" / L"libcrypto-4-x64.dll";
    std::error_code ec;
    // Images set up by v0.1.3 and earlier lack eboot.elf: the runtime writes it on its next start.
    if (!p.image.empty() && !fs::exists(eboot, ec))
        run_tool({runtime_exe.wstring(), L"--data-dir", data.wstring(), L"--prepare-only", L"--console"}, [](const std::string&) {});
    p = saved_paths();
    const fs::path ready = data / L"game" / p.image / L"eboot.elf";
    if (p.image.empty() || !fs::exists(ready, ec)) { error = L"Set up the game files first (Game files page)."; return false; }
    if (!fs::exists(python, ec) || !fs::exists(host, ec) || !fs::exists(crypto, ec)) {
        error = L"The server files in tools\\ are missing. Reinstall the game.";
        return false;
    }
    fs::create_directories(logs, ec);
    fs::create_directories(data / L"server", ec);
    fs::remove(data / L"server" / L"online.txt", ec);
    const std::wstring port = wide(online_port());
    // server_bind in launcher.ini: one adapter's address only (say a VPN's); every adapter by default.
    std::string bind = prefs.get("server_bind", "0.0.0.0");
    if (bind.empty() || bind.size() > 15 || bind.find_first_not_of("0123456789.") != std::string::npos) bind = "0.0.0.0";
    std::wstring command = quote(python.wstring()) + L" -I " + quote(host.wstring()) + L" --eboot " + quote(ready.wstring()) +
                           L" --libcrypto " + quote(crypto.wstring()) + L" --state " + quote((data / L"server").wstring()) +
                           L" --port " + port + L" --stop-file " + quote((data / L"server" / L"stop").wstring());
    if (loopback) command += L" --bind 127.0.0.1";
    else command += L" --online --bind " + wide(bind) + L" --name " + wide(online_name());
    if (!loopback && prefs.get("upnp", "1") == "0") command += L" --no-port-mapping";
    if (!cup_mode().empty()) command += L" --collab-cups " + wide(cup_mode());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    const fs::path log = logs / L"server.log";
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { error = L"Cannot write " + log.wstring(); return false; }
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr, root.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) { error = L"Cannot start the server (" + python.wstring() + L")."; return false; }
    if (!server_job) {
        server_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (server_job) SetInformationJobObject(server_job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    if (server_job) AssignProcessToJobObject(server_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    server_process = pi.hProcess;
    server_loopback = loopback;
    server_cups = cup_mode();
    SetTimer(window, kOnlineTimer, 1000, nullptr); // online.txt fills in as the router answers
    // Ready once the log holds "ready ADDRESS:PORT" (the first start also builds the fixtures).
    HANDLE watched = server_process;
    std::thread([watched, log] {
        for (int i = 0; i < 600; ++i) {
            if (WaitForSingleObject(watched, 100) != WAIT_TIMEOUT) break;
            std::ifstream f(log, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (text.find("ready ") != std::string::npos) { server_ready = true; break; }
        }
        PostMessageW(window, WM_SERVER_STATE, 1, 0);
    }).detach();
    return true;
}

// The text a host sends to friends.
std::wstring invite_text(const std::wstring& code) {
    return L"Join my Everybody's Golf game: in the launcher's Online tab, paste " + code +
           L" into Join, press Play, then pick Online > Open Course.";
}
void copy_text(const std::wstring& text) {
    if (!OpenClipboard(window)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

// Find games on my network: one broadcast to every adapter, answers for 1.5 seconds.
void find_lan_games() {
    if (lan_searching.exchange(true)) return;
    std::vector<unsigned long> targets = {0xFFFFFFFFul, 0x7F000001ul};
    for (const auto& a : local_addresses())
        if (std::find(targets.begin(), targets.end(), a.broadcast) == targets.end()) targets.push_back(a.broadcast);
    std::thread([targets] {
        std::vector<std::pair<std::wstring, std::wstring>> found;
        WSADATA wsa;
        if (!WSAStartup(MAKEWORD(2, 2), &wsa)) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s != INVALID_SOCKET) {
                BOOL on = TRUE;
                DWORD wait = 200;
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof wait);
                static const char kFind[] = "EGOLF-FIND 1", kReply[] = "EGOLF-HOST 1 ";
                for (unsigned long target : targets) {
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_port = htons(kLanDiscoveryPort);
                    to.sin_addr.s_addr = htonl(target);
                    sendto(s, kFind, (int)sizeof kFind - 1, 0, (const sockaddr*)&to, sizeof to);
                }
                const ULONGLONG end = GetTickCount64() + 1500;
                while (GetTickCount64() < end) {
                    char buffer[160];
                    const int n = recv(s, buffer, (int)sizeof buffer - 1, 0);
                    if (n <= (int)sizeof kReply - 1 || memcmp(buffer, kReply, sizeof kReply - 1)) continue;
                    buffer[n] = 0;
                    std::string rest(buffer + sizeof kReply - 1);
                    const size_t space = rest.find(' ');
                    const std::string code = rest.substr(0, space), name = space == std::string::npos ? "" : rest.substr(space + 1);
                    GolfInvite parsed;
                    if (!golf_invite_decode(code.c_str(), &parsed)) continue;
                    std::wstring wcode = wide(code), wname;
                    for (char c : name) wname += (c >= 32 && c < 127) ? (wchar_t)c : L'?';
                    // Answers from this PC arrive once per adapter; keep one per host name and port.
                    bool seen = false;
                    for (const auto& g : found) {
                        GolfInvite other;
                        golf_invite_decode(utf8(g.first).c_str(), &other);
                        seen |= g.second == wname && other.port == parsed.port && other.key == parsed.key;
                    }
                    if (!seen) found.push_back({wcode, wname});
                }
                closesocket(s);
            }
            WSACleanup();
        }
        {
            std::lock_guard<std::mutex> hold(lan_lock);
            lan_games = found;
            ++lan_generation;
        }
        lan_searching = false;
        PostMessageW(window, WM_SERVER_STATE, 2, 0);
    }).detach();
}

void refresh_online() {
    const bool host = online_mode() == "host", running = server_running();
    const std::string join = trimmed(prefs.get("online_join"));
    SetWindowTextW(controls[ID_SERVER], host ? L"Stop hosting" : L"Host a game");
    EnableWindow(controls[ID_ONLINE_HOST], !host);
    EnableWindow(controls[ID_FIND], !host && !lan_searching);
    EnableWindow(controls[ID_FOUND], !host);
    SetWindowTextW(controls[ID_FIND], lan_searching ? L"Looking..." : L"Find games on my network");
    if (lan_listed != lan_generation) {
        std::lock_guard<std::mutex> hold(lan_lock);
        lan_listed = lan_generation;
        SendMessageW(controls[ID_FOUND], LB_RESETCONTENT, 0, 0);
        for (const auto& g : lan_games)
            SendMessageW(controls[ID_FOUND], LB_ADDSTRING, 0, (LPARAM)(g.second + L"   " + g.first).c_str());
        if (lan_games.empty() && lan_searched)
            SendMessageW(controls[ID_FOUND], LB_ADDSTRING, 0, (LPARAM)L"No game found on this network.");
    }
    std::wstring code_line, s;
    const std::wstring port = wide(online_port());
    if (host) {
        const HostStatus st = running ? host_status() : HostStatus{};
        const std::wstring code = main_invite_code(st);
        code_line = code.empty() ? L"Invite code: ..." : L"Invite code: " + code;
        EnableWindow(controls[ID_INVITE], !code.empty());
        if (!running) s += L"The server is stopped. Press Play or Stop hosting, then Host a game again.";
        else if (!server_ready) s += L"Starting the server (the first time takes a minute)...";
        else {
            s += L"Send the invite code to your friends. ";
            if (!st.note.empty()) s += st.note;
        }
        if (running && server_ready && !st.local.empty()) {
            // Codes for the home network and VPNs (the adapters the launcher sees, by name).
            s += L"\r\nSame network or VPN:";
            const auto adapters = local_addresses();
            int shown = 0;
            for (const auto& entry : st.local) {
                auto a = std::find_if(adapters.begin(), adapters.end(), [&](const LocalAddress& x) { return x.address == entry.first; });
                if (a == adapters.end() || shown == 3) continue;
                s += L"\r\n  " + entry.second + L"  (" + (a->gateway ? std::wstring(L"home network") : a->adapter) + L")";
                ++shown;
            }
        }
        s += L"\r\nOnly you need the port (TCP " + port + L"); friends never do. Windows may ask once to let Python "
             L"through the firewall: allow it.";
    } else if (!join.empty()) {
        code_line = L"";
        EnableWindow(controls[ID_INVITE], FALSE);
        s += valid_join(join) ? L"Play joins your friend's game. In the game, pick Online, then Open Course."
                              : L"That is not an invite code or address. Codes look like R2M0-255Y-FPT7-WP1X.";
        if (valid_join(join) && !cup_mode().empty()) s += L" The collab cups are the host's.";
    } else {
        EnableWindow(controls[ID_INVITE], FALSE);
        s += cup_mode().empty() ? L"Play runs offline." : L"Play runs a server only this PC can reach, for the collab cups.";
        s += L" To play with friends, one of you presses Host a game and sends the invite code; the others paste it into Join.";
    }
    SetWindowTextW(controls[ID_INVITE_CODE], code_line.c_str());
    SetWindowTextW(controls[ID_ONLINE_STATUS], s.c_str());
}

// Command line options for the game, or false (with a message shown) when the Online page is not filled in.
bool online_arguments(std::wstring& command) {
    const std::string mode = online_mode(), join = trimmed(prefs.get("online_join")), name = online_name();
    if (mode != "host" && join.empty() && cup_mode().empty()) {
        // Signed in with no server, the game reports "Server connection failed" on the island; signed
        // out it plays offline without asking.
        command += L" --np-mode offline";
        return true;
    }
    if (!valid_online_name(name)) {
        MessageBoxW(window, L"On the Online page, enter a name of 3 to 16 letters, digits, - or _.", kTitle, MB_ICONWARNING);
        return false;
    }
    if (mode == "host" || join.empty()) {
        // Hosting, or an offline Play with the collab cups on: this PC's server (loopback only when
        // offline). A joined game's cups come from the host's server instead.
        std::wstring error;
        if (!start_server(error, mode != "host")) { MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING); return false; }
        command += L" --online-id " + wide(name) + L" --join 127.0.0.1:" + wide(online_port());
        return true;
    }
    if (!valid_join(join)) {
        MessageBoxW(window, L"On the Online page, paste your friend's invite code into Join (or clear it to play offline).",
                    kTitle, MB_ICONWARNING);
        return false;
    }
    command += L" --online-id " + wide(name) + L" --join " + quote(wide(join));
    return true;
}

void host_toggle() {
    if (online_mode() == "host") {
        stop_server();
        set_pref("online", "off");
        return;
    }
    set_pref("online", "host");
    std::wstring error;
    if (!start_server(error)) {
        set_pref("online", "off");
        MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING);
    }
}

void online_command(int id, int code) {
    switch (id) {
    case ID_ONLINE_NAME: if (code == EN_KILLFOCUS) set_pref("online_name", utf8(text_of(ID_ONLINE_NAME))); return;
    case ID_ONLINE_HOST:
        if (code != EN_CHANGE) return;
        set_pref("online_join", utf8(text_of(ID_ONLINE_HOST)));
        break;
    case ID_ONLINE_PORT:
        if (code != EN_KILLFOCUS) return;
        set_pref("online_port", utf8(text_of(ID_ONLINE_PORT)));
        SetWindowTextW(controls[ID_ONLINE_PORT], wide(online_port()).c_str());
        if (server_running()) { // the new port needs a new code
            stop_server();
            std::wstring error;
            if (!start_server(error)) MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING);
        }
        break;
    case ID_UPNP: set_pref("upnp", checked(ID_UPNP) ? "1" : "0"); break;
    case ID_SERVER: host_toggle(); break;
    case ID_FIND: lan_searched = true; find_lan_games(); break;
    case ID_FOUND: {
        if (code != LBN_SELCHANGE && code != LBN_DBLCLK) return;
        const LRESULT row = SendMessageW(controls[ID_FOUND], LB_GETCURSEL, 0, 0);
        std::lock_guard<std::mutex> hold(lan_lock);
        if (row >= 0 && (size_t)row < lan_games.size()) {
            SetWindowTextW(controls[ID_ONLINE_HOST], lan_games[row].first.c_str()); // EN_CHANGE saves it
            return;
        }
        return;
    }
    case ID_INVITE: {
        const std::wstring invite = main_invite_code(host_status());
        if (invite.empty()) return;
        copy_text(invite_text(invite));
        SetWindowTextW(controls[ID_INVITE], L"Copied");
        SetTimer(window, kOnlineTimer + 1, 2000, nullptr);
        return;
    }
    }
    refresh_online();
}
// The server became ready or stopped, or a LAN search finished.
void on_server_state() { refresh_online(); }
void on_online_timer(UINT_PTR id) {
    if (id == kOnlineTimer + 1) {
        KillTimer(window, id);
        SetWindowTextW(controls[ID_INVITE], L"Copy");
        return;
    }
    if (!server_running()) KillTimer(window, id);
    refresh_online();
}
// EverybodysGolf.exe --host: open the Online page and host straight away (a PC that keeps a game up
// for friends without playing).
void host_from_command_line() {
    if (online_mode() != "host") host_toggle();
    else {
        std::wstring error;
        if (!start_server(error)) MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING);
    }
}

void play() {
    std::wstring command = quote(runtime_exe.wstring()) + L" --data-dir " + quote(data.wstring());
    const std::wstring chosen = current_save();
    if (chosen != kMainSave) // another save folder; the one settings.ini for every save
        command += L" --save " + quote(save_dir(chosen).wstring()) + L" --config " + quote(ini_path.wstring());
    if (!online_arguments(command)) return;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
        MessageBoxW(window, L"everybodys-golf.exe could not be started. Unpack the whole release zip again, into a new folder.", kTitle, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    game_running = true;
    refresh_saves();
    ShowWindow(window, SW_MINIMIZE);
    std::thread([process = pi.hProcess] {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        PostMessageW(window, WM_GAME_EXIT, 0, 0);
    }).detach();
}

// --- updates ------------------------------------------------------------------------------------------
// At start (at most once a day, unless turned off on the Play page) the launcher asks GitHub for the
// newest release. When its tag is newer than this build's version, a window shows its notes and
// offers Update now, Later or Skip this version. Update now downloads the release's Windows zip and
// its .sha256 into data\update, checks the hash, unpacks it to data\update\staging, then swaps the
// files in: each old file is renamed to <name>.old (Windows lets a running exe be renamed) and the
// new one moved in, with every step written to data\update\journal.txt first. A failure moves the old
// files back; a journal left by a crash is undone at the next start. data\ (saves, settings, logs,
// the prepared game) is never written. A new runtime that needs the game prepared again does that
// itself on the next Play (the prepared image's key includes the preparation format).

const std::string kVersion = GOLF_LAUNCHER_VERSION;
HANDLE instance_mutex;
HWND update_window;
std::atomic<bool> update_checking{false}, update_cancel{false};
struct UpdateOffer { GolfUpdate::Release release; GolfUpdate::Asset zip, sha; bool manual = false; };
std::unique_ptr<UpdateOffer> offer;
fs::path update_dir() { return data / L"update"; }

std::wstring env(const wchar_t* name) {
    wchar_t value[2048];
    DWORD n = GetEnvironmentVariableW(name, value, 2048);
    return n && n < 2048 ? std::wstring(value, n) : std::wstring();
}
std::wstring release_api() {
    std::wstring test = env(L"GOLF_UPDATE_API_URL");
    return test.empty() ? kReleaseApi : test;
}
// HTTPS only; plain http only to this PC, and only when a test names its own server.
bool url_allowed(const std::wstring& url) {
    if (url.rfind(L"https://", 0) == 0) return true;
    if (env(L"GOLF_UPDATE_API_URL").empty()) return false;
    for (const wchar_t* local : {L"http://127.0.0.1:", L"http://127.0.0.1/", L"http://localhost:", L"http://localhost/"})
        if (url.rfind(local, 0) == 0) return true;
    return false;
}

// GET `url` into `body` (at most `cap` bytes) or, with `file`, into that file while hashing it into
// `hash`. Short timeouts; `progress` gets bytes so far and the total (0 when unknown).
bool http_get(const std::wstring& url, std::string* body, const fs::path* file, GolfUpdate::Sha256* hash, uint64_t cap,
              bool api, std::wstring& error, const std::function<void(uint64_t, uint64_t)>& progress = {}) {
    const std::wstring offline = L"Could not reach GitHub. Check the internet connection and try again later.";
    if (!url_allowed(url)) { error = L"The download address is not a secure (https) address, so it was not used."; return false; }
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256], path[4096], extra[4096];
    parts.lpszHostName = host; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { error = L"The download address could not be read."; return false; }
    const std::wstring agent = L"EverybodysGolfLauncher/" + wide(kVersion);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = offline; return false; }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, api ? 10000 : 30000);
    HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    const std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    auto close = [&] {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
    };
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28"
                                 : L"Accept: application/octet-stream";
    if (!request || !WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        close();
        error = offline;
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        close();
        error = status == 404 ? (api ? L"GitHub has no published release yet." : L"The update file is no longer on GitHub. Try again later.") :
                status == 403 || status == 429 ? L"GitHub is limiting requests from this network right now. Try again in an hour." :
                L"GitHub answered with an error (HTTP " + std::to_wstring(status) + L"). Try again later.";
        return false;
    }
    wchar_t length_text[32] = L"";
    DWORD length_size = sizeof length_text;
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length_text, nullptr, 10);
    std::ofstream out;
    if (file) {
        out.open(*file, std::ios::binary | std::ios::trunc);
        if (!out) { close(); error = L"Cannot write to " + file->parent_path().wstring() + L". Check that the folder is not read-only."; return false; }
    }
    uint64_t got = 0;
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), (DWORD)buffer.size(), &read)) { close(); error = offline; return false; }
        if (!read) break;
        got += read;
        if (got > cap || update_cancel) {
            close();
            error = update_cancel ? L"Update canceled." : L"The download is larger than expected, so it was stopped.";
            return false;
        }
        if (file) {
            out.write(buffer.data(), read);
            if (hash) hash->update(buffer.data(), read);
            if (!out) { close(); error = L"Writing the download failed. Check that the drive has free space."; return false; }
        } else {
            body->append(buffer.data(), read);
        }
        if (progress) progress(got, total);
    }
    close();
    if (total && got != total) { error = L"The download stopped before it was complete. Try again."; return false; }
    return true;
}

void start_update_check(bool manual) {
    if (update_checking.exchange(true)) return;
    if (!manual) set_pref("update_last_check", std::to_string((long long)time(nullptr)));
    SetWindowTextW(controls[ID_VERSION], (L"Version " + wide(kVersion) + L"\r\nChecking for updates...").c_str());
    std::thread([manual] {
        auto found = std::make_unique<UpdateOffer>();
        found->manual = manual;
        std::string body;
        std::wstring message;
        GolfUpdate::Version current;
        if (!GolfUpdate::ParseVersion(kVersion, current)) {
            message = L"This is a development build (" + wide(kVersion) + L"), so it does not update itself.";
        } else if (!http_get(release_api(), &body, nullptr, nullptr, 2 << 20, true, message)) {
        } else if (!GolfUpdate::ParseRelease(body, found->release)) {
            message = L"GitHub's answer could not be read. Try again later.";
        } else if (!GolfUpdate::Offerable(found->release) || !GolfUpdate::IsNewer(found->release.tag, kVersion)) {
            message = L"You have the newest version.";
        } else if (!GolfUpdate::PickWindowsAssets(found->release, found->zip, found->sha)) {
            message = L"Version " + wide(found->release.tag) + L" is out, but its Windows download is not ready yet. Try again later.";
        } else {
            PostMessageW(window, WM_UPDATE_FOUND, 0, (LPARAM)found.release());
            return;
        }
        PostMessageW(window, WM_UPDATE_STATUS, manual, (LPARAM)new std::wstring(message));
    }).detach();
}

void update_status(const std::wstring& text) {
    if (update_window) SetWindowTextW(GetDlgItem(update_window, ID_UPDATE_TEXT), text.c_str());
}

// The release zip's files, less the player's, into `staging`. False (with why) on a bad zip or path.
bool unpack(const fs::path& zip_file, const fs::path& staging, std::wstring& error) {
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, utf8(zip_file.wstring()).c_str(), 0)) { error = L"The downloaded zip could not be opened."; return false; }
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; ok && i < count && !update_cancel; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ok = false; error = L"The downloaded zip is damaged."; break; }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        const std::string path = GolfUpdate::InstallPath(stat.m_filename);
        if (path.empty()) { ok = false; error = L"The downloaded zip holds a file outside its folder, so it was not used."; break; }
        if (GolfUpdate::PlayerOwned(path)) continue;
        const fs::path target = staging / fs::path(wide(path));
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        auto write = [](void* opaque, mz_uint64, const void* data, size_t n) -> size_t {
            static_cast<std::ofstream*>(opaque)->write(static_cast<const char*>(data), (std::streamsize)n);
            return *static_cast<std::ofstream*>(opaque) ? n : 0;
        };
        if (!out || !mz_zip_reader_extract_to_callback(&zip, i, write, &out, 0)) {
            ok = false;
            error = L"Unpacking the update failed. Check that the drive has free space.";
        }
        if ((i & 31) == 0) PostMessageW(update_window, WM_UPDATE_PROGRESS, 90 + i * 10 / std::max<mz_uint>(count, 1), 0);
    }
    mz_zip_reader_end(&zip);
    if (ok && update_cancel) { ok = false; error = L"Update canceled."; }
    if (ok && (!fs::exists(staging / L"EverybodysGolf.exe", ec) || !fs::exists(staging / L"everybodys-golf.exe", ec))) {
        ok = false;
        error = L"The downloaded zip is not an Everybody's Golf PC release.";
    }
    return ok;
}

// Download, check and unpack on a worker thread; WM_UPDATE_READY (1 ready, 0 failed) when done.
void download_update() {
    std::thread([zip = offer->zip, sha = offer->sha] {
        std::error_code ec;
        const fs::path dir = update_dir();
        fs::create_directories(dir, ec);
        std::wstring error;
        auto fail = [&](const std::wstring& why) {
            GolfUpdate::CleanUpdateDir(dir);
            PostMessageW(update_window, WM_UPDATE_STATUS, 0, (LPARAM)new std::wstring(why));
            PostMessageW(window, WM_UPDATE_READY, 0, 0);
        };
        if (zip.name.find_first_of("/\\:") != std::string::npos) return fail(L"The release's file name is not valid.");
        PostMessageW(update_window, WM_UPDATE_STATUS, 0, (LPARAM)new std::wstring(L"Downloading the checksum..."));
        std::string sha_text;
        if (!http_get(wide(sha.url), &sha_text, nullptr, nullptr, 4096, false, error)) return fail(error);
        const std::string expected = GolfUpdate::ParseSha256File(sha_text, zip.name);
        if (expected.empty()) return fail(L"The release's checksum file could not be read, so nothing was downloaded. Nothing changed.");
        const fs::path part = dir / (wide(zip.name) + L".part");
        GolfUpdate::Sha256 hash;
        DWORD last = 0;
        if (!http_get(wide(zip.url), nullptr, &part, &hash, 2ull << 30, false, error, [&](uint64_t got, uint64_t total) {
                if (GetTickCount() - last < 200) return;
                last = GetTickCount();
                PostMessageW(update_window, WM_UPDATE_PROGRESS, total ? (WPARAM)(got * 90 / total) : 0, 0);
                std::wstring line = L"Downloading: " + std::to_wstring(got >> 20) + L" MB";
                if (total) line += L" of " + std::to_wstring(total >> 20) + L" MB";
                PostMessageW(update_window, WM_UPDATE_STATUS, 0, (LPARAM)new std::wstring(line));
            })) {
            fs::remove(part, ec);
            return fail(error + L" Nothing changed.");
        }
        if (!GolfUpdate::HashMatches(expected, hash.hex())) {
            fs::remove(part, ec);
            return fail(L"The download does not match its checksum, so it was deleted and not used. Nothing changed. Try again later.");
        }
        PostMessageW(update_window, WM_UPDATE_STATUS, 0, (LPARAM)new std::wstring(L"Checksum OK. Unpacking..."));
        if (!unpack(part, dir / L"staging", error)) {
            fs::remove_all(dir / L"staging", ec);
            fs::remove(part, ec);
            return fail(error + L" Nothing changed.");
        }
        fs::remove(part, ec);
        PostMessageW(window, WM_UPDATE_READY, 1, 0); // staging stays for apply_update, which cleans up
    }).detach();
}

// The journal of an update being applied: "R path" for a file renamed to path.old and replaced,
// "N path" for a file the update added.
fs::path journal_path() { return update_dir() / L"journal.txt"; }
fs::path cleanup_path() { return update_dir() / L"cleanup.txt"; }

void note_cleanup(const fs::path& file) {
    std::ofstream(cleanup_path(), std::ios::binary | std::ios::app) << utf8(file.wstring()) << "\n";
}

// Puts back what the journal says was replaced or added, newest first.
void roll_back() {
    std::ifstream in(journal_path(), std::ios::binary);
    std::vector<std::string> steps;
    for (std::string line; std::getline(in, line);) if (line.size() > 2) steps.push_back(line);
    in.close();
    for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
        const fs::path target = root / fs::path(wide(step->substr(2)));
        fs::path old = target; old += L".old";
        fs::path failed = target; failed += L".failed";
        std::error_code ec;
        if ((*step)[0] == 'R' && !fs::exists(old, ec)) continue; // not moved yet
        if (fs::exists(target, ec)) {
            if (!DeleteFileW(target.c_str()) && MoveFileExW(target.c_str(), failed.c_str(), MOVEFILE_REPLACE_EXISTING)) note_cleanup(failed);
        }
        if ((*step)[0] == 'R') MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::error_code ec;
    fs::remove(journal_path(), ec);
}

// Moves the staged files over the install. False (everything put back) when one cannot be moved.
bool apply_update(std::wstring& error) {
    const fs::path staging = update_dir() / L"staging";
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(fs::relative(it->path(), staging, ec));
    if (ec || files.empty()) { error = L"The unpacked update could not be read."; GolfUpdate::CleanUpdateDir(update_dir()); return false; }
    // The launcher last, so the rest is in place before the program that starts next.
    std::stable_partition(files.begin(), files.end(), [](const fs::path& p) { return _wcsicmp(p.c_str(), L"EverybodysGolf.exe") != 0; });
    std::ofstream journal(journal_path(), std::ios::binary | std::ios::trunc);
    for (const fs::path& rel : files) {
        const std::string name = utf8(rel.generic_wstring());
        if (GolfUpdate::PlayerOwned(name)) continue;
        const fs::path target = root / rel, source = staging / rel;
        fs::path old = target; old += L".old";
        const bool replace = fs::exists(target, ec);
        journal << (replace ? "R " : "N ") << name << "\n";
        journal.flush();
        if (!journal) { error = L"Cannot write data\\update\\journal.txt."; break; }
        if (replace) {
            DeleteFileW(old.c_str());
            if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                error = L"Cannot replace " + rel.wstring() + L": it is in use. Close the game and any program running from the "
                        L"install folder, then try again.";
                break;
            }
        }
        fs::create_directories(target.parent_path(), ec);
        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            error = L"Cannot move the new " + rel.wstring() + L" into the install folder.";
            break;
        }
    }
    journal.close();
    if (!error.empty()) {
        roll_back();
        GolfUpdate::CleanUpdateDir(update_dir());
        return false;
    }
    std::ifstream in(journal_path(), std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("R ", 0) != 0) continue;
        fs::path old = root / fs::path(wide(line.substr(2)));
        old += L".old";
        note_cleanup(old);
    }
    in.close();
    fs::remove(journal_path(), ec);
    GolfUpdate::CleanUpdateDir(update_dir());
    return true;
}

// At start: undo an update a crash left half done, and delete the old files of the last one (the old
// launcher may still be closing, so this retries for a few seconds).
bool recover_update() {
    std::error_code ec;
    const bool rolled_back = fs::exists(journal_path(), ec);
    if (rolled_back) roll_back();
    GolfUpdate::CleanUpdateDir(update_dir());
    if (fs::exists(cleanup_path(), ec)) {
        std::thread([] {
            std::vector<fs::path> left;
            std::ifstream in(cleanup_path(), std::ios::binary);
            for (std::string line; std::getline(in, line);) if (!line.empty()) left.push_back(wide(line));
            in.close();
            for (int attempt = 0; attempt < 20 && !left.empty(); ++attempt) {
                std::erase_if(left, [](const fs::path& p) {
                    std::error_code e;
                    return !fs::exists(p, e) || DeleteFileW(p.c_str());
                });
                if (!left.empty()) Sleep(500);
            }
            std::error_code e;
            if (left.empty()) { fs::remove(cleanup_path(), e); GolfUpdate::CleanUpdateDir(update_dir()); }
        }).detach();
    }
    return rolled_back;
}

void close_update_window() {
    if (!update_window) return;
    EnableWindow(window, TRUE);
    DestroyWindow(update_window);
    update_window = nullptr;
    SetForegroundWindow(window);
}

void update_now() {
    if (game_running) { MessageBoxW(update_window, L"Close the game first, then press Update now.", kTitle, MB_ICONINFORMATION); return; }
    if (busy) { MessageBoxW(update_window, L"Wait until the running job finishes, then press Update now.", kTitle, MB_ICONINFORMATION); return; }
    if (server_running()) {
        if (MessageBoxW(update_window, L"Updating stops your online server. Go on?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        stop_server();
    }
    update_cancel = false;
    EnableWindow(GetDlgItem(update_window, ID_UPDATE_NOW), FALSE);
    EnableWindow(GetDlgItem(update_window, ID_UPDATE_SKIP), FALSE);
    SetWindowTextW(GetDlgItem(update_window, ID_UPDATE_LATER), L"Cancel");
    busy = true;
    download_update();
}

LRESULT CALLBACK update_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void show_update_window() {
    const auto& r = offer->release;
    RECT rect{0, 0, px(600), px(470)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&rect, style, FALSE);
    RECT owner;
    GetWindowRect(window, &owner);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    update_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EverybodysGolfUpdate", L"Update available", style,
                                    owner.left + (owner.right - owner.left - w) / 2, owner.top + (owner.bottom - owner.top - h) / 2,
                                    w, h, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int x, int y, int cw, int ch, int id, HFONT f) {
        if (green_button(id)) s = (s & ~BS_PUSHBUTTON) | BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | s, px(x), px(y), px(cw), px(ch), update_window,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    std::wstring heading = L"Version " + wide(r.tag) + L" is available";
    child(L"STATIC", heading.c_str(), SS_LEFT, 20, kBand + 12, 560, 30, 0, big_font);
    child(L"STATIC", (L"You have " + wide(kVersion) + L". The update keeps your saves, settings and prepared game.").c_str(),
          SS_LEFT, 20, kBand + 44, 560, 20, 0, font);
    std::wstring notes;
    for (wchar_t c : wide(r.notes.empty() ? "This release has no notes." : r.notes)) {
        if (c == L'\n' && (notes.empty() || notes.back() != L'\r')) notes += L'\r';
        notes += c;
    }
    child(L"EDIT", notes.c_str(), WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, kBand + 72, 560, 220,
          ID_NOTES, font);
    child(PROGRESS_CLASSW, L"", 0, 20, kBand + 302, 560, 16, ID_UPDATE_BAR, font);
    child(L"STATIC", L"", SS_LEFT, 20, kBand + 322, 560, 34, ID_UPDATE_TEXT, font);
    child(L"BUTTON", L"Update now", BS_PUSHBUTTON | WS_TABSTOP, 20, kBand + 360, 150, 34, ID_UPDATE_NOW, font);
    child(L"BUTTON", L"Later", BS_PUSHBUTTON | WS_TABSTOP, 180, kBand + 362, 110, 30, ID_UPDATE_LATER, font);
    child(L"BUTTON", L"Skip this version", BS_PUSHBUTTON | WS_TABSTOP, 300, kBand + 362, 150, 30, ID_UPDATE_SKIP, font);
    EnableWindow(window, FALSE);
    ShowWindow(update_window, SW_SHOW);
}

// --- the Saves page ---------------------------------------------------------------------------------

std::vector<std::wstring> shown_saves;

// The trophies of the save in use: each trophy in the game's list with its grade and the day it was
// earned. Ids come from the game's trophy00.trp (TROPnnn.PNG entries); 31 when it cannot be read.
std::vector<int> trophy_ids() {
    std::vector<int> ids;
    Paths p = saved_paths();
    if (!p.image.empty()) {
        std::ifstream f(data / L"game" / p.image / L"sce_sys" / L"trophy" / L"trophy00.trp", std::ios::binary);
        unsigned char head[0x60];
        if (f.read((char*)head, sizeof head) && head[0] == 0xDC && head[1] == 0xA2 && head[2] == 0x4D) {
            unsigned count = head[0x10] << 24 | head[0x11] << 16 | head[0x12] << 8 | head[0x13];
            unsigned elem = head[0x14] << 24 | head[0x15] << 16 | head[0x16] << 8 | head[0x17];
            if (elem >= 0x30 && elem <= 0x100 && count < 4096) {
                std::vector<unsigned char> entry(elem);
                for (unsigned i = 0; i < count && f.read((char*)entry.data(), elem); ++i) {
                    const char* n = (const char*)entry.data();
                    if (!strncmp(n, "TROP", 4) && isdigit((unsigned char)n[4]) && isdigit((unsigned char)n[5]) &&
                        isdigit((unsigned char)n[6]) && !strncmp(n + 7, ".PNG", 4))
                        ids.push_back((n[4] - '0') * 100 + (n[5] - '0') * 10 + (n[6] - '0'));
                }
            }
        }
    }
    if (ids.empty()) for (int i = 0; i < 31; ++i) ids.push_back(i);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// A trophy's name and grade from trophy_names.txt ("ID=Name | Description | Grade"): the list beside the
// program first, then the save's own, whose lines replace it.
struct TrophyText { std::wstring name; wchar_t grade = 0; };
std::wstring utf8_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
void read_trophy_texts(const fs::path& path, std::map<int, TrophyText>& out) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        char* end = nullptr;
        long id = strtol(line.c_str(), &end, 10);
        if (end == line.c_str() || id < 0 || id >= 256) continue;
        auto trim = [](std::string v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            size_t b = v.find_first_not_of(' ');
            return b == std::string::npos ? std::string() : v.substr(b);
        };
        std::string rest = line.substr(eq + 1);
        size_t bar = rest.find('|');
        std::string name = trim(rest.substr(0, bar));
        if (name.empty()) continue;
        TrophyText t;
        t.name = utf8_wide(name);
        if (bar != std::string::npos) {
            size_t bar2 = rest.find('|', bar + 1);
            if (bar2 != std::string::npos) {
                std::string g = trim(rest.substr(bar2 + 1));
                if (!g.empty()) t.grade = (wchar_t)toupper((unsigned char)g[0]);
            }
        }
        out[(int)id] = t;
    }
}

void refresh_trophies() {
    HWND list = controls[ID_TROPHY_LIST];
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    TrophyLog log;
    log.n = 0;
    if (FILE* f = _wfopen((save_dir(current_save()) / L"trophies.txt").c_str(), L"rb")) { trophy_log_read(&log, f); fclose(f); }
    std::map<int, TrophyText> texts;
    read_trophy_texts(root / L"trophy_names.txt", texts);
    read_trophy_texts(save_dir(current_save()) / L"trophy_names.txt", texts);
    auto grade_name = [](wchar_t g) { return g == L'P' ? L"Platinum" : g == L'G' ? L"Gold" : g == L'S' ? L"Silver" : g == L'B' ? L"Bronze" : L"?"; };
    std::vector<int> ids = trophy_ids();
    for (int i = 0; i < log.n; ++i) // a recorded id the list lacks still shows
        if (!std::count(ids.begin(), ids.end(), log.e[i].id)) ids.push_back(log.e[i].id);
    std::sort(ids.begin(), ids.end());
    int earned = 0;
    for (int id : ids) {
        const TrophyRec* rec = nullptr;
        for (int i = 0; i < log.n; ++i) if (log.e[i].id == id) rec = &log.e[i];
        auto text = texts.find(id);
        const wchar_t listed = text == texts.end() ? 0 : text->second.grade; // the shipped list knows every grade
        std::wstring line = (text == texts.end() ? L"Trophy " + std::to_wstring(id) : text->second.name) + L"\t";
        if (rec) {
            ++earned;
            const wchar_t* grade = grade_name(listed ? listed : (wchar_t)rec->grade);
            __time64_t when = rec->when;
            tm local{};
            wchar_t date[48] = L"";
            if (!_localtime64_s(&local, &when)) wcsftime(date, 48, L"%Y-%m-%d %H:%M", &local);
            line += std::wstring(grade) + L"\t" + date;
        } else {
            line += std::wstring(grade_name(listed)) + L"\tnot earned yet";
        }
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    std::wstring status = std::to_wstring(earned) + L" of " + std::to_wstring(ids.size()) + L" trophies earned on the save \"" + current_save() + L"\".";
    SetWindowTextW(controls[ID_TROPHY_STATUS], status.c_str());
}

void refresh_saves() {
    HWND list = controls[ID_PROFILES];
    if (!list) return;
    int keep = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    shown_saves = list_saves();
    const std::wstring chosen = current_save();
    for (size_t i = 0; i < shown_saves.size(); ++i) {
        std::wstring line = shown_saves[i];
        SYSTEMTIME t;
        wchar_t when[64];
        if (last_saved(shown_saves[i], t)) {
            swprintf(when, 64, L"   last saved %04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
            line += when;
        } else {
            line += L"   (new: the game starts from the beginning)";
        }
        if (shown_saves[i] == chosen) line += L"   [in use]";
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        if (keep < 0 && shown_saves[i] == chosen) keep = (int)i;
    }
    SendMessageW(list, LB_SETCURSEL, std::clamp(keep, 0, (int)shown_saves.size() - 1), 0);
    const bool idle = !game_running;
    for (int id : {ID_PROFILE_USE, ID_PROFILE_NEW, ID_PROFILE_COPY, ID_PROFILE_RENAME, ID_PROFILE_DELETE})
        EnableWindow(controls[id], idle);
    std::wstring status = L"The game uses \"" + chosen + L"\". Every save keeps its own progress; display "
                          L"and texture settings are shared.";
    if (game_running) status = L"The game is running: saves can be changed after it closes.";
    SetWindowTextW(controls[ID_PROFILE_STATUS], status.c_str());
}

std::wstring selected_save() {
    int i = (int)SendMessageW(controls[ID_PROFILES], LB_GETCURSEL, 0, 0);
    return i >= 0 && i < (int)shown_saves.size() ? shown_saves[i] : kMainSave;
}

// New, Copy and Rename take the name typed in the box; a bad or taken name is refused with why.
std::wstring typed_save_name() {
    std::wstring name = text_of(ID_PROFILE_NAME);
    while (!name.empty() && name.back() == L' ') name.pop_back();
    std::error_code ec;
    if (!valid_save_name(name)) {
        MessageBoxW(window, L"Type a name for the save in the box first: up to 32 characters, without \\ / : * ? \" < > |.", kTitle, MB_ICONINFORMATION);
        return {};
    }
    if (fs::exists(save_dir(name), ec)) {
        MessageBoxW(window, (L"There is already a save called \"" + name + L"\".").c_str(), kTitle, MB_ICONINFORMATION);
        return {};
    }
    return name;
}

void save_action(int id) {
    if (game_running && id != ID_PROFILE_BACKUP && id != ID_PROFILE_OPEN) return;
    const std::wstring picked = selected_save();
    std::error_code ec;
    switch (id) {
    case ID_PROFILE_USE: set_pref("save", picked == kMainSave ? "" : utf8(picked)); break;
    case ID_PROFILE_NEW: {
        std::wstring name = typed_save_name();
        if (name.empty()) return;
        fs::create_directories(save_dir(name), ec);
        set_pref("save", utf8(name));
        SetWindowTextW(controls[ID_PROFILE_NAME], L"");
        break;
    }
    case ID_PROFILE_COPY: {
        std::wstring name = typed_save_name();
        if (name.empty()) return;
        if (!copy_tree(save_dir(picked) / L"savedata", save_dir(name) / L"savedata")) {
            MessageBoxW(window, L"Copying the save failed.", kTitle, MB_ICONERROR);
            return;
        }
        SetWindowTextW(controls[ID_PROFILE_NAME], L"");
        break;
    }
    case ID_PROFILE_RENAME: {
        if (picked == kMainSave) {
            MessageBoxW(window, L"Main is the game's own save folder and keeps its name. Copy it to a save with another name instead.", kTitle, MB_ICONINFORMATION);
            return;
        }
        std::wstring name = typed_save_name();
        if (name.empty()) return;
        fs::rename(save_dir(picked), save_dir(name), ec);
        if (ec) { MessageBoxW(window, L"Renaming failed: is a file of that save open?", kTitle, MB_ICONERROR); return; }
        if (wide(prefs.get("save")) == picked) set_pref("save", utf8(name));
        SetWindowTextW(controls[ID_PROFILE_NAME], L"");
        break;
    }
    case ID_PROFILE_DELETE: {
        if (picked == kMainSave) {
            MessageBoxW(window, L"Main is the game's own save folder and is not deleted from here.", kTitle, MB_ICONINFORMATION);
            return;
        }
        std::wstring ask = L"Move the save \"" + picked + L"\" to the Recycle Bin? It can be restored from there.";
        if (MessageBoxW(window, ask.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        if (!recycle(save_dir(picked))) { MessageBoxW(window, L"The save could not be moved to the Recycle Bin.", kTitle, MB_ICONERROR); return; }
        if (wide(prefs.get("save")) == picked) set_pref("save", "");
        break;
    }
    case ID_PROFILE_BACKUP: {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path to = data / L"backups" / (picked + L"-" + stamp);
        if (!copy_tree(save_dir(picked) / L"savedata", to / L"savedata")) {
            MessageBoxW(window, L"The backup failed.", kTitle, MB_ICONERROR);
            return;
        }
        SetWindowTextW(controls[ID_PROFILE_STATUS], (L"Backed up to " + to.wstring()).c_str());
        return; // keep the message
    }
    case ID_PROFILE_OPEN:
        fs::create_directories(save_dir(picked), ec);
        ShellExecuteW(window, L"open", save_dir(picked).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    SendMessageW(controls[ID_PROFILES], LB_SETCURSEL, (WPARAM)-1, 0);
    refresh_saves();
}

// --- options: the settings.ini keys the game reads (gpu/shim/resogun_settings.cpp, src/runtime_pad.c) ---

// A switch as the game reads it: 1, true, yes or on; the default when the key is missing or empty.
bool setting_on(const char* key, bool fallback) {
    std::string v = settings.get(key);
    for (char& c : v) c = (char)tolower((unsigned char)c);
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Greys out what does nothing with the other choices: the window size outside Window mode, render
// resolution and 60 FPS mode outside PS4 Pro mode, sharpening strength with sharpening off.
void update_option_states() {
    const bool pro = combo_value(ID_PRO, pro_modes) == "4k";
    EnableWindow(controls[ID_SIZE], combo_value(ID_WINDOW, window_modes) == "windowed");
    EnableWindow(controls[ID_RENDER], pro);
    EnableWindow(controls[ID_SIXTY], pro);
    EnableWindow(controls[ID_SHARPNESS], checked(ID_SHARPEN));
    std::error_code ec;
    const bool streamline = fs::exists(root / L"sl.interposer.dll", ec);
    EnableWindow(controls[ID_FRAMEGEN], streamline);
    std::wstring note =
        L"Changes are saved at once and used from the next start of the game. While playing, press F10 (or hold "
        L"Options for 2 seconds) for the game's Display Settings, where window, VSync, render resolution up to 3200 x "
        L"1800 and the picture options change at once.";
    if (!pro) note += L"\r\nConsole mode PS4 runs the game as on a base PS4: 30 FPS and its own 1080p size. Pick PS4 Pro for "
                      L"60 FPS mode and render resolution.";
    else note += L" Console mode, 60 FPS mode and render sizes above 3200 x 1800 apply at the next start.";
    SetWindowTextW(controls[ID_DISPLAY_NOTE], note.c_str());
    if (!streamline) SetWindowTextW(controls[ID_FRAMEGEN], L"Frame generation (NVIDIA DLSS): not included in this copy");
}

void load_option_controls() {
    fill_combo(ID_WINDOW, window_modes, settings.get("display_mode", "borderless"));
    fill_combo(ID_SIZE, sizes, settings.get("output_resolution", "1920x1080"));
    fill_combo(ID_MONITOR, monitors, settings.get("monitor"));
    std::string pro = settings.get("render_resolution", "4k");
    for (char& c : pro) c = (char)tolower((unsigned char)c);
    fill_combo(ID_PRO, pro_modes, pro == "1080p" ? "1080p" : "4k");
    fill_combo(ID_RENDER, renders, settings.get("render_height", "0"));
    const bool vsync = setting_on("vsync", true), tearing = setting_on("tearing", false);
    fill_combo(ID_VSYNC, vsync_modes, vsync ? "1,0" : tearing ? "0,1" : "0,0");
    check(ID_SIXTY, setting_on("frame_rate_priority", true));
    check(ID_SMAA, setting_on("smaa", false));
    check(ID_SHARPEN, setting_on("sharpening", true));
    std::string strength = settings.get("sharpening_strength", "default");
    if (strength == "low") strength = "soft";
    if (strength == "high") strength = "strong";
    fill_combo(ID_SHARPNESS, sharpness, strength);
    const int aniso = atoi(settings.get("anisotropy", "0").c_str());
    fill_combo(ID_ANISO, anisotropy, aniso >= 16 ? "16" : aniso >= 8 ? "8" : aniso >= 2 ? "4" : "off");
    fill_combo(ID_LOD, lod_modes, settings.get("texture_lod_bias") == "auto" || setting_on("texture_lod_bias", false) ? "auto" : "off");
    check(ID_HDR, setting_on("hdr", false));
    check(ID_FRAMEGEN, setting_on("frame_generation", false));
    fill_combo(ID_VOLUME, volumes, settings.get("audio_volume", "100"));
    fill_combo(ID_SPEAKERS, speakers, settings.get("audio_channels", "auto"));
    fill_combo(ID_DEADZONE, deadzones, settings.get("pad_deadzone", "0"));
    fill_combo(ID_OUTER, outer_zones, settings.get("pad_outer_zone", "0"));
    fill_combo(ID_TRIGGER, triggers, settings.get("pad_trigger_threshold", "30"));
    fill_combo(ID_RUMBLE, rumbles, settings.get("rumble_strength", "100"));
    check(ID_SWAP, settings.get("pad_swap_cross_circle", "0") == "1");
    check(ID_INTRO, setting_on("skip_intro", true));
    check(ID_TITLE_BACK, setting_on("title_back_exits", true));
    fill_combo(ID_MISSFLASH, miss_flash_modes, settings.get("miss_shot_flash", "0") == "1" ? "1" : "0");
    fill_combo(ID_COLLAB, collab_modes, settings.get("collab_items", "unlocked"));
    std::string language = settings.get("language", "english");
    for (char& c : language) c = (char)tolower((unsigned char)c);
    if (language == "2" || language == "fr") language = "french";
    if (language == "17" || language == "pt") language = "portuguese";
    if (language == "20" || language == "es") language = "spanish";
    fill_combo(ID_LANGUAGE, languages, language);
    update_option_states();
}

// A changed option control: its settings.ini key(s). True when `id` is an option.
bool option_command(int id, int code) {
    auto pick = [&](const char* key, const std::vector<Choice>& choices) {
        std::string value;
        if (code == CBN_SELCHANGE && combo_pick(id, choices, value)) set_setting(key, value);
    };
    auto toggle = [&](const char* key) { set_setting(key, checked(id) ? "1" : "0"); };
    switch (id) {
    case ID_WINDOW: pick("display_mode", window_modes); break;
    case ID_SIZE: pick("output_resolution", sizes); break;
    case ID_MONITOR: pick("monitor", monitors); break;
    case ID_PRO: pick("render_resolution", pro_modes); break;
    case ID_RENDER: pick("render_height", renders); break;
    case ID_VSYNC: {
        std::string value;
        if (code == CBN_SELCHANGE && combo_pick(id, vsync_modes, value)) {
            set_setting("vsync", value.substr(0, 1));
            set_setting("tearing", value.substr(2, 1));
        }
        break;
    }
    case ID_SIXTY: toggle("frame_rate_priority"); break;
    case ID_SMAA: toggle("smaa"); break;
    case ID_SHARPEN: toggle("sharpening"); break;
    case ID_SHARPNESS: pick("sharpening_strength", sharpness); break;
    case ID_ANISO: pick("anisotropy", anisotropy); break;
    case ID_LOD: pick("texture_lod_bias", lod_modes); break;
    case ID_HDR: toggle("hdr"); break;
    case ID_FRAMEGEN: toggle("frame_generation"); break;
    case ID_VOLUME: pick("audio_volume", volumes); break;
    case ID_SPEAKERS: pick("audio_channels", speakers); break;
    case ID_DEADZONE: pick("pad_deadzone", deadzones); break;
    case ID_OUTER: pick("pad_outer_zone", outer_zones); break;
    case ID_TRIGGER: pick("pad_trigger_threshold", triggers); break;
    case ID_RUMBLE: pick("rumble_strength", rumbles); break;
    case ID_SWAP: toggle("pad_swap_cross_circle"); break;
    case ID_INTRO: toggle("skip_intro"); break;
    case ID_TITLE_BACK: toggle("title_back_exits"); break;
    case ID_MISSFLASH: pick("miss_shot_flash", miss_flash_modes); break;
    case ID_COLLAB: pick("collab_items", collab_modes); break;
    case ID_LANGUAGE: pick("language", languages); break;
    default: return false;
    }
    update_option_states();
    return true;
}

void create_controls() {
    pages.assign(PageCount, {});
    // Only the tab strip: the pages draw on the window's own background, under it.
    tabs = add(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, 10, kPageTop + 8, 680, 30, ID_TABS);
    const wchar_t* names[PageCount] = {L"Play", L"Display", L"Graphics", L"Sound && controls", L"Saves", L"Trophies",
                                       L"HD textures", L"Online", L"Game files"};
    for (int i = 0; i < PageCount; ++i) {
        TCITEMW item{TCIF_TEXT};
        item.pszText = (LPWSTR)names[i];
        TabCtrl_InsertItem(tabs, i, &item);
    }
    // Play
    add(PagePlay, L"STATIC", L"Everybody's Golf", SS_LEFT, 30, 50, 580, 40, 0, big_font);
    status_text = add(PagePlay, L"STATIC", L"", SS_LEFT, 30, 100, 580, 140, ID_STATUS);
    add(PagePlay, L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 30, 260, 200, 60, ID_PLAY, big_font);
    add(PagePlay, L"BUTTON", L"Open install folder", BS_PUSHBUTTON | WS_TABSTOP, 30, 370, 170, 30, ID_OPEN_FOLDER);
    add(PagePlay, L"BUTTON", L"Open logs", BS_PUSHBUTTON | WS_TABSTOP, 210, 370, 120, 30, ID_OPEN_LOGS);
    add(PagePlay, L"BUTTON", L"Check for updates when the launcher starts", BS_AUTOCHECKBOX | WS_TABSTOP, 300, 262, 360, 22,
        ID_CHECK_UPDATES);
    add(PagePlay, L"BUTTON", L"Check now", BS_PUSHBUTTON | WS_TABSTOP, 300, 290, 110, 28, ID_CHECK_NOW);
    add(PagePlay, L"STATIC", L"", SS_LEFT, 300, 324, 380, 40, ID_VERSION);
    // settings.ini language: the game's text and voices, from the next start.
    add(PagePlay, L"STATIC", L"Language", SS_LEFT, 30, 334, 90, 22);
    add(PagePlay, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, 330, 180, 200, ID_LANGUAGE);
    // Saves
    add(PageSaves, L"STATIC", L"Saves: each has its own golfer and progress. The game uses the one marked [in use].",
        SS_LEFT, 30, 50, 580, 20);
    add(PageSaves, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, 30, 74, 440, 170, ID_PROFILES);
    add(PageSaves, L"BUTTON", L"Use this save", BS_PUSHBUTTON | WS_TABSTOP, 480, 74, 130, 30, ID_PROFILE_USE);
    add(PageSaves, L"BUTTON", L"Back up", BS_PUSHBUTTON | WS_TABSTOP, 480, 112, 130, 30, ID_PROFILE_BACKUP);
    add(PageSaves, L"BUTTON", L"Move to Recycle Bin", BS_PUSHBUTTON | WS_TABSTOP, 480, 150, 130, 30, ID_PROFILE_DELETE);
    add(PageSaves, L"BUTTON", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, 480, 188, 130, 30, ID_PROFILE_OPEN);
    add(PageSaves, L"STATIC", L"Name:", SS_LEFT, 30, 262, 50, 22);
    add(PageSaves, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 80, 258, 190, 24, ID_PROFILE_NAME);
    SendMessageW(controls[ID_PROFILE_NAME], EM_SETLIMITTEXT, 32, 0);
    add(PageSaves, L"BUTTON", L"New empty save", BS_PUSHBUTTON | WS_TABSTOP, 280, 256, 120, 28, ID_PROFILE_NEW);
    add(PageSaves, L"BUTTON", L"Copy selected", BS_PUSHBUTTON | WS_TABSTOP, 406, 256, 100, 28, ID_PROFILE_COPY);
    add(PageSaves, L"BUTTON", L"Rename", BS_PUSHBUTTON | WS_TABSTOP, 512, 256, 98, 28, ID_PROFILE_RENAME);
    add(PageSaves, L"STATIC", L"", SS_LEFT, 30, 296, 580, 36, ID_PROFILE_STATUS);
    // settings.ini collab_items: the tie-in outfits no add-on grants (src/runtime_game.c, runtime_collab.c).
    add(PageSaves, L"STATIC", L"Collab outfits and cups", SS_LEFT, 30, 342, 165, 22);
    add(PageSaves, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 338, 300, 200, ID_COLLAB);
    add(PageSaves, L"STATIC", L"Back up copies a save to data\\backups. Saves stay in data\\save (Main) and data\\saves.",
        SS_LEFT, 30, 376, 580, 40);
    // Trophies
    add(PageTrophies, L"STATIC", L"", SS_LEFT, 30, 50, 580, 22, ID_TROPHY_STATUS);
    add(PageTrophies, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOSEL | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT, 30, 78, 580, 270, ID_TROPHY_LIST);
    {
        int stops[2] = {120, 190}; // dialog units: Trophy | Grade | Earned
        SendMessageW(controls[ID_TROPHY_LIST], LB_SETTABSTOPS, 2, (LPARAM)stops);
    }
    add(PageTrophies, L"STATIC", L"Trophies are recorded per save, in its trophies.txt, when the game unlocks them. This list is read-only. "
        L"Names and grades come from trophy_names.txt; they show as Trophy n and ? where that file is missing.", SS_LEFT, 30, 358, 580, 50);
    // Game files
    add(PageFiles, L"STATIC", L"1. Your decrypted dump of Everybody's Golf (CUSA04687): the folder with eboot.bin and sce_sys.",
        SS_LEFT, 30, 50, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 72, 470, 24, ID_BASE);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 71, 100, 26, ID_BASE_BROWSE);
    add(PageFiles, L"STATIC", L"2. Update 1.17, when your dump has it in its own folder (else leave empty):", SS_LEFT, 30, 110, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 132, 470, 24, ID_UPDATE);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 131, 100, 26, ID_UPDATE_BROWSE);
    add(PageFiles, L"STATIC", L"3. DLC: a folder of extracted add-ons (optional; the game's own content is unlocked anyway):", SS_LEFT, 30, 170, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 192, 470, 24, ID_DLC);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 191, 100, 26, ID_DLC_BROWSE);
    add(PageFiles, L"BUTTON", L"Set up game files", BS_PUSHBUTTON | WS_TABSTOP, 30, 240, 180, 32, ID_SETUP);
    setup_progress = add(PageFiles, PROGRESS_CLASSW, L"", 0, 30, 290, 580, 18, ID_SETUP_PROGRESS);
    setup_status = add(PageFiles, L"STATIC", L"Your dump is only read. With the update in its own folder, the two are merged into data\\dump "
                                     L"(hard links on the same drive, so no extra space; a copy otherwise).",
                       SS_LEFT, 30, 316, 580, 80, ID_SETUP_STATUS);
    // Display
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    auto row = [&](int page, const wchar_t* label, int y, int id) {
        add(page, L"STATIC", label, SS_LEFT, 30, y + 4, 200, 22);
        add(page, WC_COMBOBOXW, L"", combo, 240, y, 320, 300, id);
    };
    row(PageDisplay, L"Window mode", 50, ID_WINDOW);
    row(PageDisplay, L"Window size (Window mode)", 84, ID_SIZE);
    row(PageDisplay, L"Monitor", 118, ID_MONITOR);
    row(PageDisplay, L"Console mode", 152, ID_PRO);
    row(PageDisplay, L"Render resolution", 186, ID_RENDER);
    add(PageDisplay, L"STATIC", L"The size the game draws its scenes at, scaled to the window. Higher is sharper and needs a faster "
                                L"GPU; at 3840 x 2160 some course flyovers drop below 60 FPS.",
        SS_LEFT, 240, 214, 440, 36);
    row(PageDisplay, L"VSync", 254, ID_VSYNC);
    // settings.ini miss_shot_flash: the game's inverted full-screen flash on a missed swing (gpu/shim/resogun_miss_flash.h).
    row(PageDisplay, L"Miss-shot flash", 288, ID_MISSFLASH);
    add(PageDisplay, L"BUTTON", L"60 FPS mode (the game's frame rate priority, held on in every scene)", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 324, 640, 22, ID_SIXTY);
    add(PageDisplay, L"STATIC", L"", SS_LEFT, 30, 354, 650, 76, ID_DISPLAY_NOTE);

    // Graphics
    add(PageGraphics, L"BUTTON", L"Anti-aliasing (SMAA 1x at the render size)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 50, 640, 22, ID_SMAA);
    add(PageGraphics, L"BUTTON", L"Sharpening (AMD FSR 1 upscaling and RCAS at the window's size)", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 78, 640, 22, ID_SHARPEN);
    row(PageGraphics, L"Sharpening strength", 108, ID_SHARPNESS);
    row(PageGraphics, L"Anisotropic filtering", 142, ID_ANISO);
    row(PageGraphics, L"High-res texture detail", 176, ID_LOD);
    add(PageGraphics, L"BUTTON", L"HDR (turn on Use HDR in Windows display settings first)", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 212, 640, 22, ID_HDR);
    add(PageGraphics, L"BUTTON", L"Frame generation (NVIDIA DLSS, GeForce RTX 40 and 50 series)", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 240, 640, 22, ID_FRAMEGEN);
    add(PageGraphics, L"STATIC",
        L"SMAA smooths jagged edges; off keeps the game's own look. Anisotropic filtering keeps fairways and greens sharp "
        L"at a slant. High-res texture detail picks sharper texture levels above 1080 lines and can add a little shimmer. "
        L"Frame generation adds a generated frame between each two of the game's, for up to twice the frames on a high "
        L"refresh rate display; fast-moving objects can smear. HDR and turning frame generation on apply at the next "
        L"start; the rest also change in the game's Display Settings (F10).",
        SS_LEFT, 30, 272, 650, 110, ID_GRAPHICS_NOTE);

    // Sound & controls
    row(PageControls, L"Master volume", 50, ID_VOLUME);
    row(PageControls, L"Speakers", 84, ID_SPEAKERS);
    add(PageControls, L"STATIC", L"Controller", SS_LEFT, 30, 124, 300, 22, 0, big_font);
    row(PageControls, L"Stick dead zone", 152, ID_DEADZONE);
    row(PageControls, L"Stick outer zone", 186, ID_OUTER);
    row(PageControls, L"L2 / R2 press point", 220, ID_TRIGGER);
    row(PageControls, L"Rumble", 254, ID_RUMBLE);
    add(PageControls, L"BUTTON", L"Swap Cross and Circle", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 288, 300, 22, ID_SWAP);
    add(PageControls, L"BUTTON", L"Skip the start-up logos", BS_AUTOCHECKBOX | WS_TABSTOP, 340, 288, 300, 22, ID_INTRO);
    add(PageControls, L"BUTTON", L"Back on the title screen asks to close the game", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 316, 640, 22, ID_TITLE_BACK);
    add(PageControls, L"STATIC",
        L"A dead zone ignores small stick movement near the center (try 6% for a worn stick); the outer zone reads the "
        L"stick's rim as full. Controller and sound settings apply at the next start. Keyboard keys are set with "
        L"key_<button> lines in settings.ini.",
        SS_LEFT, 30, 344, 500, 60);
    add(PageControls, L"BUTTON", L"Open settings.ini", BS_PUSHBUTTON | WS_TABSTOP, 540, 350, 140, 28, ID_OPEN_SETTINGS);
    // HD textures
    add(PageTextures, L"STATIC", L"HD textures are made on this PC from your own game: the game saves each texture it uses while you play, "
                      L"then Real-ESRGAN upscales them on your GPU into a pack the game loads instead.",
        SS_LEFT, 30, 50, 580, 50);
    add(PageTextures, L"BUTTON", L"1. Collect textures while playing", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 108, 560, 22, ID_COLLECT);
    tex_count = add(PageTextures, L"STATIC", L"", SS_LEFT, 52, 132, 560, 20, ID_TEX_COUNT);
    add(PageTextures, L"STATIC", L"2. Upscale", SS_LEFT, 30, 166, 100, 22);
    add(PageTextures, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 162, 280, 200, ID_SCALE);
    add(PageTextures, L"BUTTON", L"Build HD texture pack", BS_PUSHBUTTON | WS_TABSTOP, 420, 160, 190, 30, ID_BUILD);
    tex_progress = add(PageTextures, PROGRESS_CLASSW, L"", 0, 30, 204, 580, 18, ID_TEX_PROGRESS);
    tex_status = add(PageTextures, L"STATIC", L"Building takes a few seconds per texture; it can run again later to add new ones.",
                     SS_LEFT, 30, 228, 580, 60, ID_TEX_STATUS);
    add(PageTextures, L"BUTTON", L"3. Use the HD texture pack", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 300, 560, 22, ID_USE_PACK);
    add(PageTextures, L"STATIC", L"Packs are made from your copy of the game: they stay on this PC and are never shared.",
        SS_LEFT, 30, 336, 580, 40);

    // Online
    add(PageOnline, L"STATIC", L"Play with friends: one of you hosts, the others join with the invite code.",
        SS_LEFT, 30, 50, 580, 20);
    add(PageOnline, L"STATIC", L"Your name", SS_LEFT, 30, 82, 90, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 120, 78, 160, 24, ID_ONLINE_NAME);
    SendMessageW(controls[ID_ONLINE_NAME], EM_SETLIMITTEXT, 16, 0);
    add(PageOnline, L"STATIC", L"Port", SS_LEFT, 300, 82, 40, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 340, 78, 70, 24, ID_ONLINE_PORT);
    SendMessageW(controls[ID_ONLINE_PORT], EM_SETLIMITTEXT, 5, 0);
    add(PageOnline, L"BUTTON", L"Open the port on my router", BS_AUTOCHECKBOX | WS_TABSTOP, 425, 80, 200, 22, ID_UPNP);
    add(PageOnline, L"BUTTON", L"Host a game", BS_PUSHBUTTON | WS_TABSTOP, 30, 116, 130, 30, ID_SERVER);
    add(PageOnline, L"STATIC", L"", SS_LEFT, 175, 119, 340, 26, ID_INVITE_CODE, big_font);
    add(PageOnline, L"BUTTON", L"Copy", BS_PUSHBUTTON | WS_TABSTOP, 520, 116, 90, 30, ID_INVITE);
    add(PageOnline, L"STATIC", L"Join", SS_LEFT, 30, 164, 90, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 120, 160, 290, 24, ID_ONLINE_HOST);
    SendMessageW(controls[ID_ONLINE_HOST], EM_SETLIMITTEXT, 253, 0);
    add(PageOnline, L"BUTTON", L"Find games on my network", BS_PUSHBUTTON | WS_TABSTOP, 420, 158, 190, 28, ID_FIND);
    add(PageOnline, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
        120, 192, 490, 52, ID_FOUND);
    add(PageOnline, L"STATIC", L"", SS_LEFT, 30, 254, 580, 166, ID_ONLINE_STATUS);
    // Older launchers kept a friend's address in online_host with online=join.
    if (prefs.get("online_join").empty() && prefs.get("online") == "join" && !prefs.get("online_host").empty())
        set_pref("online_join", prefs.get("online_host"));
    if (online_mode() == "join") set_pref("online", "off");
    SetWindowTextW(controls[ID_ONLINE_NAME], wide(online_name()).c_str());
    SetWindowTextW(controls[ID_ONLINE_PORT], wide(online_port()).c_str());
    SetWindowTextW(controls[ID_ONLINE_HOST], wide(prefs.get("online_join")).c_str());
    check(ID_UPNP, prefs.get("upnp", "1") != "0");

    fill_combo(ID_SCALE, scales, "2");
    load_option_controls();
    check(ID_COLLECT, !settings.get("texture_dump").empty());
    check(ID_USE_PACK, !settings.get("texture_pack").empty() && settings.get("hd_textures", "1") != "0");
    Paths p = saved_paths();
    SetWindowTextW(controls[ID_BASE], p.game.c_str());
    SetWindowTextW(controls[ID_DLC], p.dlc.c_str());
}

// The first page: Game files until the game is set up, else Play; --page N picks one.
void open_first_page(int page) {
    if (page < 0 || page >= PageCount) page = game_ready() ? PagePlay : PageFiles;
    TabCtrl_SetCurSel(tabs, page);
    show_page(page);
}

void on_command(int id, int code) {
    auto folder = [](const fs::path& p) { ShellExecuteW(window, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    std::error_code ec;
    if (option_command(id, code)) { refresh_status(); return; }
    switch (id) {
    case ID_PLAY: play(); break;
    case ID_OPEN_FOLDER: folder(root); break;
    case ID_OPEN_LOGS: fs::create_directories(logs, ec); folder(logs); break;
    case ID_BASE_BROWSE: case ID_UPDATE_BROWSE: case ID_DLC_BROWSE: {
        std::wstring picked = browse_folder(id == ID_BASE_BROWSE ? L"Game dump (eboot.bin, sce_sys)" : id == ID_UPDATE_BROWSE ? L"Update 1.17" : L"DLC folder");
        if (!picked.empty()) SetWindowTextW(controls[id - 1], picked.c_str());
        break;
    }
    case ID_SETUP: set_up_game(); break;
    case ID_PROFILE_USE: case ID_PROFILE_NEW: case ID_PROFILE_COPY: case ID_PROFILE_RENAME:
    case ID_PROFILE_DELETE: case ID_PROFILE_BACKUP: case ID_PROFILE_OPEN:
        save_action(id);
        break;
    case ID_PROFILES: if (code == LBN_DBLCLK) save_action(ID_PROFILE_USE); return;
    case ID_BUILD: build_textures(); break;
    case ID_ONLINE_NAME: case ID_ONLINE_PORT: case ID_ONLINE_HOST: case ID_SERVER: case ID_UPNP:
    case ID_INVITE: case ID_FIND: case ID_FOUND:
        online_command(id, code);
        return;
    case ID_OPEN_SETTINGS:
        load_settings();
        ShellExecuteW(window, L"open", L"notepad.exe", quote(ini_path.wstring()).c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case ID_CHECK_UPDATES: set_pref("update_check", checked(ID_CHECK_UPDATES) ? "1" : "0"); break;
    case ID_CHECK_NOW: start_update_check(true); break;
    case ID_COLLECT: {
        fs::path dump = data / L"textures" / L"dump";
        if (checked(ID_COLLECT)) fs::create_directories(dump, ec);
        set_setting("texture_dump", checked(ID_COLLECT) ? utf8(dump.wstring()) : "");
        break;
    }
    case ID_USE_PACK: {
        fs::path pack = data / L"textures" / L"pack";
        if (checked(ID_USE_PACK) && !count_files(pack, nullptr)) {
            MessageBoxW(window, L"There is no HD texture pack yet: build one first (step 2).", kTitle, MB_ICONINFORMATION);
            check(ID_USE_PACK, false);
            break;
        }
        // The pack stays named while off, so Display Settings in the game can turn it back on.
        if (count_files(pack, nullptr)) set_setting("texture_pack", utf8(pack.wstring()));
        set_setting("hd_textures", checked(ID_USE_PACK) ? "1" : "0");
        break;
    }
    }
    refresh_status();
}

// The green band: a putting green with its hole, the pin and a yellow flag, then the title.
void paint_band(HWND hwnd, HDC dc, const wchar_t* title, const std::wstring& note) {
    RECT client;
    GetClientRect(hwnd, &client);
    RECT band{0, 0, client.right, px(kBand)};
    HBRUSH fill = CreateSolidBrush(Ink::Band);
    FillRect(dc, &band, fill);
    DeleteObject(fill);
    RECT rule{0, px(kBand) - px(3), client.right, px(kBand)};
    HBRUSH dark = CreateSolidBrush(Ink::BandDark);
    FillRect(dc, &rule, dark);
    DeleteObject(dark);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH green = CreateSolidBrush(Ink::Green), hole = CreateSolidBrush(Ink::Hole), flag = CreateSolidBrush(Ink::Flag);
    HGDIOBJ old_brush = SelectObject(dc, green);
    Ellipse(dc, px(12), px(38), px(76), px(58));
    SelectObject(dc, hole);
    Ellipse(dc, px(38), px(44), px(50), px(50));
    HBRUSH white = CreateSolidBrush(Ink::White);
    SelectObject(dc, white);
    Rectangle(dc, px(43), px(10), px(46), px(48));
    SelectObject(dc, flag);
    POINT pennant[3] = {{px(46), px(10)}, {px(66), px(16)}, {px(46), px(22)}};
    Polygon(dc, pennant, 3);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(green); DeleteObject(hole); DeleteObject(flag); DeleteObject(white);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Ink::White);
    HGDIOBJ old_font = SelectObject(dc, title_font);
    RECT text{px(88), px(8), client.right - px(16), px(40)};
    DrawTextW(dc, title, -1, &text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, small_font);
    SetTextColor(dc, RGB(0xd8, 0xee, 0xcf));
    RECT under{px(90), px(38), client.right - px(16), px(58)};
    DrawTextW(dc, note.c_str(), -1, &under, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

// A green button: lighter while pressed, grey when off.
void paint_button(const DRAWITEMSTRUCT* item) {
    const bool off = item->itemState & ODS_DISABLED, down = item->itemState & ODS_SELECTED;
    HBRUSH back = CreateSolidBrush(Ink::Page);
    FillRect(item->hDC, &item->rcItem, back);
    DeleteObject(back);
    HBRUSH fill = CreateSolidBrush(off ? Ink::ButtonOff : down ? Ink::ButtonHot : Ink::Button);
    HPEN pen = CreatePen(PS_SOLID, 1, off ? Ink::ButtonOff : Ink::BandDark);
    HGDIOBJ old_brush = SelectObject(item->hDC, fill), old_pen = SelectObject(item->hDC, pen);
    const RECT& r = item->rcItem;
    RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, px(12), px(12));
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(fill);
    DeleteObject(pen);
    wchar_t label[64];
    GetWindowTextW(item->hwndItem, label, 64);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, Ink::White);
    HGDIOBJ old_font = SelectObject(item->hDC, (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    RECT text = r;
    DrawTextW(item->hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, old_font);
    if (item->itemState & ODS_FOCUS) {
        RECT focus = r;
        InflateRect(&focus, -px(4), -px(4));
        DrawFocusRect(item->hDC, &focus);
    }
}

// Text on the page color, dark green; the ones every window shares.
LRESULT themed(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        if (GetWindowLongPtrW((HWND)lp, GWL_STYLE) & ES_READONLY && GetDlgCtrlID((HWND)lp) == ID_NOTES) {
            SetBkColor((HDC)wp, Ink::White);
            SetTextColor((HDC)wp, Ink::Text);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, Ink::Text);
        return (LRESULT)page_brush;
    case WM_CTLCOLORBTN:
        return (LRESULT)page_brush;
    case WM_DRAWITEM:
        paint_button((const DRAWITEMSTRUCT*)lp);
        return TRUE;
    }
    handled = false;
    return 0;
}

std::wstring band_note() {
    return L"Version " + wide(kVersion) + L"  \x2022  runs your own PS4 copy natively on Windows";
}

LRESULT CALLBACK update_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    LRESULT themed_result = themed(msg, wp, lp, handled);
    if (handled) return themed_result;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_band(hwnd, dc, L"Update", L"Everybody's Golf PC");
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_UPDATE_NOW: update_now(); return 0;
        case ID_UPDATE_SKIP:
            set_pref("update_skip", offer->release.tag);
            close_update_window();
            return 0;
        case ID_UPDATE_LATER:
        case IDCANCEL:
            if (busy && update_cancel.exchange(true) == false) { update_status(L"Canceling..."); return 0; }
            if (!busy) close_update_window();
            return 0;
        }
        return 0;
    case WM_CLOSE: SendMessageW(hwnd, WM_COMMAND, ID_UPDATE_LATER, 0); return 0;
    case WM_UPDATE_STATUS: {
        std::unique_ptr<std::wstring> text((std::wstring*)lp);
        update_status(*text);
        return 0;
    }
    case WM_UPDATE_PROGRESS: set_bar(GetDlgItem(hwnd, ID_UPDATE_BAR), (int)wp); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The new launcher, started by the old one once the files are in place.
void restart_updated() {
    const std::wstring command = quote((root / L"EverybodysGolf.exe").wstring()) + L" --updated " + quote(wide(kVersion));
    if (instance_mutex) { CloseHandle(instance_mutex); instance_mutex = nullptr; }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(update_window, L"The update is installed. Start EverybodysGolf.exe again.", kTitle, MB_ICONINFORMATION);
    }
    close_update_window();
    DestroyWindow(window);
}

void on_update_ready(bool ready) {
    busy = false;
    if (ready) {
        update_status(L"Installing...");
        std::wstring error;
        if (apply_update(error)) { restart_updated(); return; }
        update_status(error + L" The previous version was kept.");
    } else if (update_cancel) {
        GolfUpdate::CleanUpdateDir(update_dir());
        close_update_window();
        return;
    }
    if (!update_window) return;
    EnableWindow(GetDlgItem(update_window, ID_UPDATE_NOW), TRUE);
    EnableWindow(GetDlgItem(update_window, ID_UPDATE_SKIP), TRUE);
    SetWindowTextW(GetDlgItem(update_window, ID_UPDATE_LATER), L"Later");
    set_bar(GetDlgItem(update_window, ID_UPDATE_BAR), 0);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    LRESULT themed_result = themed(msg, wp, lp, handled);
    if (handled) return themed_result;
    switch (msg) {
    case WM_COMMAND: on_command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == ID_TABS && ((NMHDR*)lp)->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(tabs));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_band(hwnd, dc, L"Everybody's Golf PC", band_note());
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_UPDATE_FOUND: {
        std::unique_ptr<UpdateOffer> found((UpdateOffer*)lp);
        update_checking = false;
        SetWindowTextW(controls[ID_VERSION], (L"Version " + wide(kVersion) + L"\r\n" + wide(found->release.tag) + L" is available.").c_str());
        if (update_window || (!found->manual && prefs.get("update_skip") == found->release.tag)) return 0;
        offer = std::move(found);
        show_update_window();
        return 0;
    }
    case WM_UPDATE_STATUS: {
        std::unique_ptr<std::wstring> text((std::wstring*)lp);
        update_checking = false;
        // A failed automatic check (offline, rate limit) says nothing: the player just plays.
        SetWindowTextW(controls[ID_VERSION], (L"Version " + wide(kVersion) + (wp || text->rfind(L"You have", 0) == 0 ? L"\r\n" + *text : std::wstring())).c_str());
        if (wp) MessageBoxW(hwnd, text->c_str(), kTitle, MB_ICONINFORMATION);
        return 0;
    }
    case WM_UPDATE_READY: on_update_ready(wp != 0); return 0;
    case WM_JOB_LINE: {
        std::unique_ptr<std::wstring> line((std::wstring*)lp);
        if (status_target) SetWindowTextW(status_target, line->c_str());
        return 0;
    }
    case WM_JOB_PROGRESS:
        if (progress_target) {
            int percent = (int)wp;
            LONG_PTR style = GetWindowLongPtrW(progress_target, GWL_STYLE);
            if (percent < 0) {
                SetWindowLongPtrW(progress_target, GWL_STYLE, style | PBS_MARQUEE);
                SendMessageW(progress_target, PBM_SETMARQUEE, TRUE, 30);
            } else {
                SendMessageW(progress_target, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrW(progress_target, GWL_STYLE, style & ~PBS_MARQUEE);
                set_bar(progress_target, percent);
            }
        }
        return 0;
    case WM_JOB_DONE:
        busy = false;
        set_busy(false);
        if (progress_target) {
            SendMessageW(progress_target, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(progress_target, GWL_STYLE, GetWindowLongPtrW(progress_target, GWL_STYLE) & ~PBS_MARQUEE);
            set_bar(progress_target, wp ? 100 : 0);
        }
        if (wp && progress_target == tex_progress) {  // a new pack is wanted in game: step 3 on
            check(ID_USE_PACK, true);
            set_setting("texture_pack", utf8((data / L"textures" / L"pack").wstring()));
            set_setting("hd_textures", "1");
        }
        refresh_status();
        return 0;
    case WM_GAME_EXIT:
        game_running = false;
        if (server_loopback) stop_server(); // the collab cups' private server ends with the game
        load_settings(); // what the game's Display Settings saved
        load_option_controls();
        refresh_saves();
        refresh_trophies();
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        refresh_status();
        return 0;
    case WM_SERVER_STATE: on_server_state(); return 0;
    case WM_TIMER: on_online_timer(wp); return 0;
    case WM_CLOSE:
        if (busy && MessageBoxW(hwnd, L"A job is still running. Close anyway? It stops where it is.", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        if (server_running() && MessageBoxW(hwnd, L"Closing the launcher stops your server for your friends. Close anyway?", kTitle,
                                            MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        stop_server();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    root = exe_folder();
    data = root / L"data";
    save = data / L"save";
    ini_path = save / L"settings.ini";
    logs = data / L"logs";
    prefs_path = data / L"launcher.ini";
    runtime_exe = root / L"everybodys-golf.exe";
    // One launcher per install folder.
    std::wstring mutex_name = L"EverybodysGolfLauncher-" + std::to_wstring(std::hash<std::wstring>{}(root.wstring()));
    instance_mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"EverybodysGolfLauncher", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    const bool update_undone = recover_update();
    load_settings();
    prefs.load(prefs_path);

    HDC screen = GetDC(nullptr);
    dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW metrics{sizeof metrics};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW large = metrics.lfMessageFont;
    large.lfHeight = -MulDiv(16, dpi, 72);
    large.lfWeight = FW_SEMIBOLD;
    big_font = CreateFontIndirectW(&large);
    LOGFONTW title = large;
    title.lfHeight = -MulDiv(20, dpi, 72);
    title.lfWeight = FW_BOLD;
    title_font = CreateFontIndirectW(&title);
    LOGFONTW small = metrics.lfMessageFont;
    small.lfHeight = -MulDiv(9, dpi, 72);
    small_font = CreateFontIndirectW(&small);
    page_brush = CreateSolidBrush(Ink::Page);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = page_brush;
    wc.lpszClassName = L"EverybodysGolfLauncher";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = update_proc;
    wc.lpszClassName = L"EverybodysGolfUpdate";
    RegisterClassExW(&wc);
    RECT r{0, 0, px(700), px(430 + kPageTop + 6)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    window = CreateWindowExW(0, L"EverybodysGolfLauncher", kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, instance, nullptr);
    create_controls();
    int page = -1, argc = 0;
    std::wstring updated_from;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--page")) page = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--updated")) updated_from = argv[i + 1];
    }
    bool host_now = false;
    for (int i = 1; argv && i < argc; ++i) host_now |= !wcscmp(argv[i], L"--host");
    if (argv) LocalFree(argv);
    open_first_page(page);
    check(ID_CHECK_UPDATES, prefs.get("update_check", "1") != "0");
    std::wstring version_text = L"Version " + wide(kVersion);
    if (!updated_from.empty()) version_text += L"\r\nUpdated from " + updated_from + L".";
    SetWindowTextW(controls[ID_VERSION], version_text.c_str());
    if (host_now) {
        TabCtrl_SetCurSel(tabs, PageOnline);
        show_page(PageOnline);
        host_from_command_line();
    }
    ShowWindow(window, show);
    if (update_undone)
        MessageBoxW(window, L"The last update did not finish, so the previous version was put back. You can try it again "
                            L"from the Play tab (Check now).", kTitle, MB_ICONWARNING);
    if (GolfUpdate::CheckDue(prefs.get("update_check", "1") != "0", atoll(prefs.get("update_last_check", "0").c_str()),
                             (long long)time(nullptr)))
        start_update_check(false);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (update_window && IsDialogMessageW(update_window, &msg)) continue;
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
