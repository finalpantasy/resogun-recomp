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

#include <algorithm>
#include <atomic>
#include <cstdio>
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

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Everybody's Golf";
enum : UINT { WM_JOB_LINE = WM_APP + 1, WM_JOB_PROGRESS, WM_JOB_DONE, WM_GAME_EXIT, WM_SERVER_STATE };
enum Id : int {
    ID_TABS = 100, ID_PLAY, ID_OPEN_FOLDER, ID_OPEN_LOGS, ID_STATUS,
    ID_BASE, ID_BASE_BROWSE, ID_UPDATE, ID_UPDATE_BROWSE, ID_DLC, ID_DLC_BROWSE, ID_SETUP, ID_SETUP_PROGRESS, ID_SETUP_STATUS,
    ID_WINDOW, ID_SIZE, ID_RENDER, ID_SIXTY, ID_SMAA, ID_SHARPEN, ID_VSYNC, ID_HDR, ID_INTRO, ID_MISSFLASH,
    ID_COLLECT, ID_TEX_COUNT, ID_SCALE, ID_BUILD, ID_TEX_PROGRESS, ID_TEX_STATUS, ID_USE_PACK,
    ID_PROFILES, ID_PROFILE_NAME, ID_PROFILE_USE, ID_PROFILE_NEW, ID_PROFILE_COPY, ID_PROFILE_RENAME,
    ID_PROFILE_DELETE, ID_PROFILE_BACKUP, ID_PROFILE_OPEN, ID_PROFILE_STATUS,
    ID_ONLINE_NAME, ID_ONLINE_MODE, ID_ONLINE_PORT, ID_ONLINE_HOST, ID_SERVER, ID_UPNP, ID_INVITE, ID_ONLINE_STATUS,
    ID_COLLAB,
};
// The pages, in tab order.
enum Page : int { PagePlay, PageDisplay, PageSaves, PageTextures, PageOnline, PageFiles, PageCount };

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

HFONT font, big_font;
int dpi = 96;
int px(int v) { return MulDiv(v, dpi, 96); }
std::vector<std::vector<HWND>> pages;
HWND tabs, status_text, setup_status, setup_progress, tex_status, tex_progress, tex_count;
HWND progress_target, status_target;
std::map<int, HWND> controls;

HWND add(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr) {
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
// Next start; Earn in game sells them in the shop's coin tab for 1500 to 2500 coins.
const std::vector<Choice> collab_modes = {{L"All unlocked", "unlocked"}, {L"Earn in game", "earn"}, {L"Original", "original"}};
const std::vector<Choice> miss_flash_modes = {{L"Off", "0"}, {L"On (original)", "1"}};
const std::vector<Choice> window_modes = {{L"Borderless window (recommended)", "borderless"}, {L"Fullscreen", "fullscreen"}, {L"Window", "windowed"}};
const std::vector<Choice> sizes = {{L"1280 x 720", "1280x720"}, {L"1600 x 900", "1600x900"}, {L"1920 x 1080", "1920x1080"},
                                   {L"2560 x 1440", "2560x1440"}, {L"3840 x 2160", "3840x2160"}};
const std::vector<Choice> renders = {{L"Game default (1920 x 1080 at 60 FPS)", "0"}, {L"2560 x 1440", "1440"},
                                     {L"3200 x 1800 (PS4 Pro)", "1800"}, {L"3840 x 2160 (4K)", "2160"}};
const std::vector<Choice> scales = {{L"2x (1024 becomes 2048)", "2"}, {L"4x (1024 becomes 4096, slow)", "4"}};

void fill_combo(int id, const std::vector<Choice>& choices, const std::string& current) {
    HWND c = controls[id];
    int selected = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)choices[i].label);
        if (current == choices[i].value) selected = (int)i;
    }
    SendMessageW(c, CB_SETCURSEL, selected, 0);
}
std::string combo_value(int id, const std::vector<Choice>& choices) {
    int i = (int)SendMessageW(controls[id], CB_GETCURSEL, 0, 0);
    return choices[i < 0 || i >= (int)choices.size() ? 0 : i].value;
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

void set_up_game() {
    fs::path base = text_of(ID_BASE), update = text_of(ID_UPDATE), dlc = text_of(ID_DLC);
    std::error_code ec;
    if (base.empty() || !fs::exists(base / L"eboot.bin", ec)) {
        MessageBoxW(window, L"Choose the folder of your game dump: the one that holds eboot.bin and sce_sys.", kTitle, MB_ICONWARNING);
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
        int code = run_tool(argv, [](const std::string& line) { if (!line.empty()) post_line(wide(line)); });
        post_line(code == 0 ? L"Game files are ready. Press Play." : L"Preparing failed (exit " + std::to_wstring(code) + L"); see data\\logs\\launcher.log.");
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
// Off plays offline. Host runs the bundled title server (tools\server\host.py) on this PC and the
// game uses it; Join uses a friend's. Players on one server share its rankings; each round is still
// played alone, since the game's matchmaking runs inside each PC.

const std::vector<Choice> online_modes = {{L"Off (play offline)", "off"}, {L"Host a server on this PC", "host"},
                                          {L"Join a friend's server", "join"}};
const char* const kDefaultPort = "48765";
HANDLE server_process = nullptr, server_job = nullptr;
std::atomic<bool> server_ready{false};
std::mutex upnp_lock;
std::wstring upnp_note;     // the router's answer, shown on the page (under upnp_lock)
std::wstring upnp_address;  // the router's public address, once a mapping is made (under upnp_lock)
long upnp_port = 0;         // the mapped port, removed again when the server stops (under upnp_lock)

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
// A host name or IPv4 address: letters, digits, dots and dashes.
bool valid_host(const std::string& host) {
    return !host.empty() && host.size() <= 253 &&
           host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos;
}
bool server_running() { return server_process && WaitForSingleObject(server_process, 0) == WAIT_TIMEOUT; }

// IPv4 addresses of this PC's adapters that are up (a VPN such as Tailscale or ZeroTier shows here
// too), with the adapter's name; the one with a default gateway (the home network) first.
struct LocalAddress { std::wstring address, adapter; bool gateway; };
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
            out.push_back({text, a->FriendlyName ? a->FriendlyName : L"", a->FirstGatewayAddress != nullptr});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LocalAddress& x, const LocalAddress& y) { return x.gateway > y.gateway; });
    return out;
}

// UPnP port mappings on the router, through Windows' NAT traversal objects (hnetcfg.dll). MinGW has
// no natupnp.h, so the three interfaces are declared here, in natupnp.idl's order, up to the
// methods used.
struct IStaticPortMapping : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_ExternalIPAddress(BSTR* address) = 0;
};
struct IStaticPortMappingCollection : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** items) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Item(long port, BSTR protocol, IStaticPortMapping** mapping) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(long* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(long port, BSTR protocol) = 0;
    virtual HRESULT STDMETHODCALLTYPE Add(long external_port, BSTR protocol, long internal_port, BSTR client,
                                          VARIANT_BOOL enabled, BSTR description, IStaticPortMapping** mapping) = 0;
};
struct IUPnPNAT : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(IStaticPortMappingCollection** mappings) = 0;
};
const CLSID kUPnPNAT = {0xae1e00aa, 0x3fd5, 0x403c, {0x8a, 0x27, 0x2b, 0xbd, 0xc3, 0x0c, 0xd0, 0xe1}};
const IID kIUPnPNAT = {0xb171c812, 0xcc76, 0x485a, {0x94, 0xd8, 0xb6, 0xb3, 0xa2, 0x79, 0x4e, 0x99}};

// The router's port mapping list, or null with the reason in note. Runs on its own thread (the
// router can take seconds to answer).
IStaticPortMappingCollection* upnp_mappings(std::wstring& note) {
    IUPnPNAT* nat = nullptr;
    if (FAILED(CoCreateInstance(kUPnPNAT, nullptr, CLSCTX_INPROC_SERVER, kIUPnPNAT, (void**)&nat)) || !nat) {
        note = L"Windows' UPnP support is not available.";
        return nullptr;
    }
    IStaticPortMappingCollection* mappings = nullptr;
    HRESULT hr = nat->get_StaticPortMappingCollection(&mappings);
    nat->Release();
    if (FAILED(hr) || !mappings) note = L"No UPnP router answered (it may be off in the router's settings).";
    return mappings;
}
void upnp_map(long port, std::wstring client) {
    std::thread([port, client] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring note, address;
        if (IStaticPortMappingCollection* mappings = upnp_mappings(note)) {
            BSTR tcp = SysAllocString(L"TCP"), host = SysAllocString(client.c_str()),
                 what = SysAllocString(L"Everybody's Golf server");
            IStaticPortMapping* mapping = nullptr;
            HRESULT hr = mappings->Add(port, tcp, port, host, VARIANT_TRUE, what, &mapping);
            if (SUCCEEDED(hr) && mapping) {
                BSTR external = nullptr;
                if (SUCCEEDED(mapping->get_ExternalIPAddress(&external)) && external) address = external;
                SysFreeString(external);
                mapping->Release();
                note = L"The router forwards port " + std::to_wstring(port) + L" to this PC.";
                std::lock_guard<std::mutex> hold(upnp_lock);
                upnp_port = port;
            } else {
                note = L"The router refused the port mapping; forward TCP port " + std::to_wstring(port) + L" to " + client + L" in its settings.";
            }
            SysFreeString(tcp); SysFreeString(host); SysFreeString(what);
            mappings->Release();
        }
        {
            std::lock_guard<std::mutex> hold(upnp_lock);
            upnp_note = note;
            upnp_address = address;
        }
        CoUninitialize();
        PostMessageW(window, WM_SERVER_STATE, 0, 0);
    }).detach();
}
// Removes this launcher's mapping, waiting for the router (used when the server stops).
void upnp_unmap() {
    long port;
    {
        std::lock_guard<std::mutex> hold(upnp_lock);
        port = upnp_port;
        upnp_port = 0;
        upnp_address.clear();
        upnp_note.clear();
    }
    if (!port) return;
    std::thread([port] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring note;
        if (IStaticPortMappingCollection* mappings = upnp_mappings(note)) {
            BSTR tcp = SysAllocString(L"TCP");
            mappings->Remove(port, tcp);
            SysFreeString(tcp);
            mappings->Release();
        }
        CoUninitialize();
    }).join();
}

void stop_server() {
    if (server_process) {
        TerminateProcess(server_process, 0);
        WaitForSingleObject(server_process, 5000);
        CloseHandle(server_process);
        server_process = nullptr;
    }
    server_ready = false;
    upnp_unmap();
}

// Starts tools\server\host.py on every network adapter of this PC; its output goes to
// data\logs\server.log. The process is in a job that ends it when the launcher closes.
bool start_server(std::wstring& error) {
    if (server_running()) return true;
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
    const std::wstring port = wide(online_port());
    // server_bind in launcher.ini: one adapter's address only (say a VPN's); every adapter by default.
    std::string bind = prefs.get("server_bind", "0.0.0.0");
    if (bind.empty() || bind.size() > 15 || bind.find_first_not_of("0123456789.") != std::string::npos) bind = "0.0.0.0";
    std::wstring command = quote(python.wstring()) + L" -I " + quote(host.wstring()) + L" --eboot " + quote(ready.wstring()) +
                           L" --libcrypto " + quote(crypto.wstring()) + L" --state " + quote((data / L"server").wstring()) +
                           L" --port " + port + L" --bind " + wide(bind);
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

// The text a host sends to friends: where to connect, and how.
std::wstring invite_text() {
    std::wstring port = wide(online_port()), where;
    {
        std::lock_guard<std::mutex> hold(upnp_lock);
        if (!upnp_address.empty()) where += L"  " + upnp_address + L" (over the internet)\r\n";
    }
    for (const auto& a : local_addresses())
        where += L"  " + a.address + L" (" + (a.gateway ? L"on my home network" : L"on my " + a.adapter) + L")\r\n";
    return L"Join my Everybody's Golf PC server.\r\n"
           L"In the launcher, open Online, choose \"Join a friend's server\", enter the address and port, then press Play.\r\n"
           L"Address, the first one that works for you:\r\n" + where +
           L"Port: " + port + L"\r\n";
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

void refresh_online() {
    const std::string mode = online_mode();
    const bool host = mode == "host", join = mode == "join", running = server_running();
    EnableWindow(controls[ID_ONLINE_HOST], join);
    EnableWindow(controls[ID_SERVER], host);
    EnableWindow(controls[ID_UPNP], host);
    EnableWindow(controls[ID_INVITE], host);
    SetWindowTextW(controls[ID_SERVER], running ? L"Stop server" : L"Start server");
    std::wstring s;
    const std::wstring port = wide(online_port());
    if (host) {
        s += !running ? L"Server: stopped. Play starts it." : server_ready ? L"Server: running on port " + port + L"." : L"Server: starting...";
        if (!running && fs::exists(logs / L"server.log")) s += L" Its output is in data\\logs\\server.log.";
        s += L"\r\nFriends connect to: ";
        auto addresses = local_addresses();
        for (size_t i = 0; i < addresses.size() && i < 3; ++i)
            s += (i ? L", " : L"") + addresses[i].address + L" (" + addresses[i].adapter + L")";
        {
            std::lock_guard<std::mutex> hold(upnp_lock);
            if (!upnp_address.empty()) s += L", " + upnp_address + L" (internet)";
            if (!upnp_note.empty()) s += L"\r\n" + upnp_note;
        }
        s += L"\r\nFriends outside your home network need the port open on your router (the box above, or forward TCP "
             + port + L" to this PC by hand) or a shared VPN such as Tailscale or ZeroTier. Windows may ask once to let "
               L"Python through the firewall: allow it.";
    } else if (join) {
        s += L"Play connects to your friend's server at the address and port above (their invite has both).";
    } else {
        s += L"Play runs offline: the game's online menus cannot connect.";
    }
    s += L"\r\n\r\nPlayers on one server share its rankings (the Open Course hole scores). Each round is still "
         L"played alone: the game's matchmaking runs inside each PC, so friends do not appear on your course.";
    SetWindowTextW(controls[ID_ONLINE_STATUS], s.c_str());
}

// Command line options for the game, or false (with a message shown) when the Online page is not filled in.
bool online_arguments(std::wstring& command) {
    const std::string mode = online_mode();
    // Signed in with no server, the game reports "Server connection failed" on the island; signed
    // out it plays offline without asking.
    if (mode == "off") { command += L" --np-mode offline"; return true; }
    const std::string name = prefs.get("online_name", "Golfer"), friend_host = prefs.get("online_host");
    if (!valid_online_name(name)) {
        MessageBoxW(window, L"On the Online page, enter a name of 3 to 16 letters, digits, - or _.", kTitle, MB_ICONWARNING);
        return false;
    }
    if (mode == "join" && !valid_host(friend_host)) {
        MessageBoxW(window, L"On the Online page, enter your friend's server address (from their invite).", kTitle, MB_ICONWARNING);
        return false;
    }
    if (mode == "host") {
        std::wstring error;
        if (!start_server(error)) { MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING); return false; }
    }
    command += L" --online-id " + wide(name) + L" --local-auth 1 --discovery-port " + wide(online_port());
    if (mode == "join") command += L" --server-host " + wide(friend_host);
    return true;
}

void online_command(int id, int code) {
    switch (id) {
    case ID_ONLINE_MODE:
        if (code != CBN_SELCHANGE) return;
        set_pref("online", combo_value(ID_ONLINE_MODE, online_modes));
        break;
    case ID_ONLINE_NAME: if (code == EN_KILLFOCUS) set_pref("online_name", utf8(text_of(ID_ONLINE_NAME))); return;
    case ID_ONLINE_HOST: if (code == EN_KILLFOCUS) set_pref("online_host", utf8(text_of(ID_ONLINE_HOST))); return;
    case ID_ONLINE_PORT:
        if (code != EN_KILLFOCUS) return;
        set_pref("online_port", utf8(text_of(ID_ONLINE_PORT)));
        SetWindowTextW(controls[ID_ONLINE_PORT], wide(online_port()).c_str());
        if (server_running()) stop_server(); // the next Start or Play uses the new port
        break;
    case ID_UPNP: set_pref("upnp", checked(ID_UPNP) ? "1" : "0"); break;
    case ID_SERVER:
        if (server_running()) {
            stop_server();
        } else {
            std::wstring error;
            if (!start_server(error)) MessageBoxW(window, error.c_str(), kTitle, MB_ICONWARNING);
        }
        break;
    case ID_INVITE:
        copy_text(invite_text());
        MessageBoxW(window, (L"Copied to the clipboard; paste it to your friends:\r\n\r\n" + invite_text()).c_str(), kTitle, MB_ICONINFORMATION);
        break;
    }
    refresh_online();
}
// The server became ready or stopped: open the router port when asked, and show the state.
void on_server_state() {
    if (server_ready && prefs.get("upnp", "0") == "1") {
        bool mapped;
        {
            std::lock_guard<std::mutex> hold(upnp_lock);
            mapped = upnp_port != 0 || !upnp_note.empty();
        }
        auto addresses = local_addresses();
        if (!mapped && !addresses.empty() && addresses[0].gateway) {
            {
                std::lock_guard<std::mutex> hold(upnp_lock);
                upnp_note = L"Asking the router to open the port...";
            }
            upnp_map(std::stol(online_port()), addresses[0].address);
        }
    }
    refresh_online();
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
        MessageBoxW(window, L"Cannot start everybodys-golf.exe. Reinstall the game.", kTitle, MB_ICONERROR);
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

// --- the Saves page ---------------------------------------------------------------------------------

std::vector<std::wstring> shown_saves;

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

void create_controls() {
    pages.assign(PageCount, {});
    // Only the tab strip: the pages draw on the window's own background, under it.
    tabs = add(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, 10, 8, 620, 30, ID_TABS);
    const wchar_t* names[PageCount] = {L"Play", L"Display", L"Saves", L"HD textures", L"Online", L"Game files"};
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
    add(PageSaves, L"STATIC", L"Collab outfits", SS_LEFT, 30, 342, 160, 22);
    add(PageSaves, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 338, 300, 200, ID_COLLAB);
    add(PageSaves, L"STATIC", L"Back up copies a save to data\\backups. Saves stay in data\\save (Main) and data\\saves.",
        SS_LEFT, 30, 376, 580, 40);
    // Game files
    add(PageFiles, L"STATIC", L"Your decrypted dump of Everybody's Golf (CUSA04687): the folder with eboot.bin and sce_sys.",
        SS_LEFT, 30, 50, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 72, 470, 24, ID_BASE);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 71, 100, 26, ID_BASE_BROWSE);
    add(PageFiles, L"STATIC", L"Update 1.17, when your dump has it in its own folder (else leave empty):", SS_LEFT, 30, 110, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 132, 470, 24, ID_UPDATE);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 131, 100, 26, ID_UPDATE_BROWSE);
    add(PageFiles, L"STATIC", L"DLC: a folder of extracted add-ons (optional; the game's own content is unlocked anyway):", SS_LEFT, 30, 170, 580, 20);
    add(PageFiles, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 192, 470, 24, ID_DLC);
    add(PageFiles, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 191, 100, 26, ID_DLC_BROWSE);
    add(PageFiles, L"BUTTON", L"Set up game files", BS_PUSHBUTTON | WS_TABSTOP, 30, 240, 180, 32, ID_SETUP);
    setup_progress = add(PageFiles, PROGRESS_CLASSW, L"", 0, 30, 290, 580, 18, ID_SETUP_PROGRESS);
    setup_status = add(PageFiles, L"STATIC", L"Your dump is only read. With the update in its own folder, the two are merged into data\\dump "
                                     L"(hard links on the same drive, so no extra space; a copy otherwise).",
                       SS_LEFT, 30, 316, 580, 80, ID_SETUP_STATUS);
    // Display
    add(PageDisplay, L"STATIC", L"Window", SS_LEFT, 30, 56, 160, 22);
    add(PageDisplay, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 52, 300, 200, ID_WINDOW);
    add(PageDisplay, L"STATIC", L"Window size", SS_LEFT, 30, 92, 160, 22);
    add(PageDisplay, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 88, 300, 200, ID_SIZE);
    add(PageDisplay, L"STATIC", L"Render resolution", SS_LEFT, 30, 128, 160, 22);
    add(PageDisplay, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 124, 300, 200, ID_RENDER);
    add(PageDisplay, L"STATIC", L"Higher render resolutions are sharper and need a fast GPU; at 4K some courses drop below 60 FPS.",
        SS_LEFT, 200, 152, 410, 34);
    add(PageDisplay, L"BUTTON", L"60 FPS mode (the game's frame rate priority option)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 196, 560, 22, ID_SIXTY);
    add(PageDisplay, L"BUTTON", L"Anti-aliasing (SMAA)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 224, 560, 22, ID_SMAA);
    add(PageDisplay, L"BUTTON", L"Sharpening and upscaling to the window (AMD FSR 1)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 252, 560, 22, ID_SHARPEN);
    add(PageDisplay, L"BUTTON", L"VSync", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 280, 560, 22, ID_VSYNC);
    add(PageDisplay, L"BUTTON", L"HDR (needs Windows HDR on)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 308, 560, 22, ID_HDR);
    add(PageDisplay, L"BUTTON", L"Skip the start-up logos", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 336, 560, 22, ID_INTRO);
    // settings.ini miss_shot_flash: the game's inverted full-screen flash on a missed swing (gpu/shim/resogun_miss_flash.h).
    add(PageDisplay, L"STATIC", L"Miss-shot flash", SS_LEFT, 30, 368, 160, 22);
    add(PageDisplay, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 364, 300, 200, ID_MISSFLASH);
    add(PageDisplay, L"STATIC", L"Changes are saved at once. In the game, press F10 or hold Options for 2 seconds for Display Settings: window, "
        L"vsync, render resolution (up to 1800 lines), SMAA, sharpening and HD textures change at once; 60 FPS mode and HDR "
        L"take effect at the next start.",
        SS_LEFT, 30, 400, 580, 52);
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
    add(PageOnline, L"STATIC", L"Play against one title server with friends: host it on this PC, or join a friend's.",
        SS_LEFT, 30, 50, 580, 20);
    add(PageOnline, L"STATIC", L"Your name", SS_LEFT, 30, 82, 160, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 200, 78, 200, 24, ID_ONLINE_NAME);
    SendMessageW(controls[ID_ONLINE_NAME], EM_SETLIMITTEXT, 16, 0);
    add(PageOnline, L"STATIC", L"Online", SS_LEFT, 30, 116, 160, 22);
    add(PageOnline, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 112, 300, 200, ID_ONLINE_MODE);
    add(PageOnline, L"STATIC", L"Port", SS_LEFT, 30, 150, 160, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 200, 146, 80, 24, ID_ONLINE_PORT);
    SendMessageW(controls[ID_ONLINE_PORT], EM_SETLIMITTEXT, 5, 0);
    add(PageOnline, L"STATIC", L"Friend's address", SS_LEFT, 300, 150, 110, 22);
    add(PageOnline, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 410, 146, 200, 24, ID_ONLINE_HOST);
    SendMessageW(controls[ID_ONLINE_HOST], EM_SETLIMITTEXT, 253, 0);
    add(PageOnline, L"BUTTON", L"Start server", BS_PUSHBUTTON | WS_TABSTOP, 30, 184, 130, 28, ID_SERVER);
    add(PageOnline, L"BUTTON", L"Copy invite for friends", BS_PUSHBUTTON | WS_TABSTOP, 170, 184, 170, 28, ID_INVITE);
    add(PageOnline, L"BUTTON", L"Open the port on my router (UPnP)", BS_AUTOCHECKBOX | WS_TABSTOP, 350, 188, 260, 22, ID_UPNP);
    add(PageOnline, L"STATIC", L"", SS_LEFT, 30, 224, 580, 196, ID_ONLINE_STATUS);
    fill_combo(ID_ONLINE_MODE, online_modes, online_mode());
    SetWindowTextW(controls[ID_ONLINE_NAME], wide(prefs.get("online_name", "Golfer")).c_str());
    SetWindowTextW(controls[ID_ONLINE_PORT], wide(online_port()).c_str());
    SetWindowTextW(controls[ID_ONLINE_HOST], wide(prefs.get("online_host")).c_str());
    check(ID_UPNP, prefs.get("upnp", "0") == "1");

    fill_combo(ID_WINDOW, window_modes, settings.get("display_mode", "borderless"));
    fill_combo(ID_MISSFLASH, miss_flash_modes, settings.get("miss_shot_flash", "0") == "1" ? "1" : "0");
    fill_combo(ID_COLLAB, collab_modes, settings.get("collab_items", "unlocked"));
    fill_combo(ID_SIZE, sizes, settings.get("output_resolution", "1920x1080"));
    fill_combo(ID_RENDER, renders, settings.get("render_height", "0"));
    fill_combo(ID_SCALE, scales, "2");
    check(ID_SIXTY, settings.get("frame_rate_priority", "1") != "0");
    check(ID_SMAA, settings.get("smaa", "0") == "1");
    check(ID_SHARPEN, settings.get("sharpening", "1") != "0");
    check(ID_VSYNC, settings.get("vsync", "1") != "0");
    check(ID_HDR, settings.get("hdr", "0") == "1");
    check(ID_INTRO, settings.get("skip_intro", "1") != "0");
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
    case ID_ONLINE_NAME: case ID_ONLINE_MODE: case ID_ONLINE_PORT: case ID_ONLINE_HOST: case ID_SERVER: case ID_UPNP:
    case ID_INVITE:
        online_command(id, code);
        return;
    case ID_MISSFLASH: if (code == CBN_SELCHANGE) set_setting("miss_shot_flash", combo_value(ID_MISSFLASH, miss_flash_modes)); break;
    case ID_COLLAB: if (code == CBN_SELCHANGE) set_setting("collab_items", combo_value(ID_COLLAB, collab_modes)); break;
    case ID_WINDOW: if (code == CBN_SELCHANGE) set_setting("display_mode", combo_value(ID_WINDOW, window_modes)); break;
    case ID_SIZE: if (code == CBN_SELCHANGE) set_setting("output_resolution", combo_value(ID_SIZE, sizes)); break;
    case ID_RENDER:
        if (code == CBN_SELCHANGE) {
            std::string h = combo_value(ID_RENDER, renders);
            set_setting("render_height", h);
            if (h != "0") set_setting("render_resolution", "4k"); // the size patch works in the game's Pro mode
        }
        break;
    case ID_SIXTY: set_setting("frame_rate_priority", checked(ID_SIXTY) ? "1" : "0"); break;
    case ID_SMAA: set_setting("smaa", checked(ID_SMAA) ? "1" : "0"); break;
    case ID_SHARPEN: set_setting("sharpening", checked(ID_SHARPEN) ? "1" : "0"); break;
    case ID_VSYNC: set_setting("vsync", checked(ID_VSYNC) ? "1" : "0"); break;
    case ID_HDR: set_setting("hdr", checked(ID_HDR) ? "1" : "0"); break;
    case ID_INTRO: set_setting("skip_intro", checked(ID_INTRO) ? "1" : "0"); break;
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

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: on_command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == ID_TABS && ((NMHDR*)lp)->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(tabs));
        return 0;
    case WM_CTLCOLORSTATIC: {
        // Labels on the tab page: the page's color, not the dialog gray.
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
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
        refresh_saves();
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        refresh_status();
        return 0;
    case WM_SERVER_STATE: on_server_state(); return 0;
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
    CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"EverybodysGolfLauncher", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
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

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"EverybodysGolfLauncher";
    RegisterClassExW(&wc);
    RECT r{0, 0, px(640), px(462)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    window = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, instance, nullptr);
    create_controls();
    int page = -1, argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (!wcscmp(argv[i], L"--page")) page = _wtoi(argv[i + 1]);
    if (argv) LocalFree(argv);
    open_first_page(page);
    ShowWindow(window, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
