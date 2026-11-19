// SPDX-License-Identifier: GPL-2.0-or-later
// EverybodysGolf.exe: the player's launcher window. It sits in the install folder next to
// everybodys-golf.exe (the game) and keeps everything it and the game write in data\ beside it:
//     data\paths.ini, data\game\     the prepared game (everybodys-golf.exe --prepare-only)
//     data\dump\                      the base game and update 1.17 merged, when given apart
//     data\save\settings.ini          the game's settings (the Display page edits them)
//     data\textures\dump, png, pack   textures collected while playing, and the HD pack built from them
//     data\logs\launcher.log          what the jobs printed
// Pages: Play, Game files (pick the dump, merge, prepare), Display, HD textures (collect, upscale
// with tools\realesrgan through tools\python and tools\scripts\texture_pack.py, use).
// Nothing is written outside the install folder: no registry, no AppData.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Everybody's Golf";
enum : UINT { WM_JOB_LINE = WM_APP + 1, WM_JOB_PROGRESS, WM_JOB_DONE, WM_GAME_EXIT };
enum Id : int {
    ID_TABS = 100, ID_PLAY, ID_OPEN_FOLDER, ID_OPEN_LOGS, ID_STATUS,
    ID_BASE, ID_BASE_BROWSE, ID_UPDATE, ID_UPDATE_BROWSE, ID_DLC, ID_DLC_BROWSE, ID_SETUP, ID_SETUP_PROGRESS, ID_SETUP_STATUS,
    ID_WINDOW, ID_SIZE, ID_RENDER, ID_SIXTY, ID_SMAA, ID_SHARPEN, ID_VSYNC, ID_HDR, ID_INTRO,
    ID_COLLECT, ID_TEX_COUNT, ID_SCALE, ID_BUILD, ID_TEX_PROGRESS, ID_TEX_STATUS, ID_USE_PACK,
};

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

// --- jobs: one at a time on a worker thread; their output goes to the page and to launcher.log -------

HWND window;
std::atomic<bool> busy{false};
std::ofstream job_log;

void post_line(const std::wstring& line) {
    PostMessageW(window, WM_JOB_LINE, 0, (LPARAM)new std::wstring(line));
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
            job_log << line << "\n";
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
    s += L"\r\n\r\nRender size: ";
    std::string h = settings.get("render_height", "0");
    for (const auto& r : renders) if (h == r.value) s += r.label;
    size_t packed = count_files(data / L"textures" / L"pack", nullptr);
    s += L"\r\nHD textures: ";
    s += settings.get("texture_pack").empty() ? L"off" : L"on, " + std::to_wstring(packed) + L" textures";
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
        size_t total = count_files(png, L".png");
        std::atomic<bool> running{true};
        std::thread watch([&] {
            while (running) {
                size_t done = count_files(pack, nullptr);
                post_progress(total ? (int)std::min<size_t>(100, done * 100 / total) : 0);
                post_line(L"Upscaling with Real-ESRGAN: " + std::to_wstring(done) + L" of about " + std::to_wstring(total) + L" textures");
                Sleep(1500);
            }
        });
        std::vector<std::wstring> argv = {python.wstring(), L"-I", script.wstring(), L"upscale", png.wstring(), pack.wstring(),
                                          L"--scale", wide(scale), L"--format", L"dds", L"--jobs", L"4"};
        if (fs::exists(esrgan, e)) { argv.push_back(L"--esrgan"); argv.push_back(esrgan.wstring()); }
        int code = run_tool(argv, [](const std::string& line) { if (!line.empty()) post_line(wide(line)); });
        running = false;
        watch.join();
        post_progress(100);
        post_line(code == 0 ? L"HD texture pack built: " + std::to_wstring(count_files(pack, nullptr)) + L" textures. It is used from the next start."
                            : L"Building the pack failed (exit " + std::to_wstring(code) + L"); see data\\logs\\launcher.log.");
        return code == 0;
    });
}

void play() {
    std::wstring command = quote(runtime_exe.wstring()) + L" --data-dir " + quote(data.wstring());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
        MessageBoxW(window, L"Cannot start everybodys-golf.exe. Reinstall the game.", kTitle, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    ShowWindow(window, SW_MINIMIZE);
    std::thread([process = pi.hProcess] {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        PostMessageW(window, WM_GAME_EXIT, 0, 0);
    }).detach();
}

void create_controls() {
    pages.assign(4, {});
    // Only the tab strip: the pages draw on the window's own background, under it.
    tabs = add(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, 10, 8, 620, 30, ID_TABS);
    const wchar_t* names[] = {L"Play", L"Game files", L"Display", L"HD textures"};
    for (int i = 0; i < 4; ++i) {
        TCITEMW item{TCIF_TEXT};
        item.pszText = (LPWSTR)names[i];
        TabCtrl_InsertItem(tabs, i, &item);
    }
    // Play
    add(0, L"STATIC", L"Everybody's Golf", SS_LEFT, 30, 50, 580, 40, 0, big_font);
    status_text = add(0, L"STATIC", L"", SS_LEFT, 30, 100, 580, 140, ID_STATUS);
    add(0, L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 30, 260, 200, 60, ID_PLAY, big_font);
    add(0, L"BUTTON", L"Open install folder", BS_PUSHBUTTON | WS_TABSTOP, 30, 370, 170, 30, ID_OPEN_FOLDER);
    add(0, L"BUTTON", L"Open logs", BS_PUSHBUTTON | WS_TABSTOP, 210, 370, 120, 30, ID_OPEN_LOGS);
    // Game files
    add(1, L"STATIC", L"Your decrypted dump of Everybody's Golf (CUSA04687): the folder with eboot.bin and sce_sys.",
        SS_LEFT, 30, 50, 580, 20);
    add(1, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 72, 470, 24, ID_BASE);
    add(1, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 71, 100, 26, ID_BASE_BROWSE);
    add(1, L"STATIC", L"Update 1.17, when your dump has it in its own folder (else leave empty):", SS_LEFT, 30, 110, 580, 20);
    add(1, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 132, 470, 24, ID_UPDATE);
    add(1, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 131, 100, 26, ID_UPDATE_BROWSE);
    add(1, L"STATIC", L"DLC: a folder of extracted add-ons (optional; the game's own content is unlocked anyway):", SS_LEFT, 30, 170, 580, 20);
    add(1, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, 192, 470, 24, ID_DLC);
    add(1, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 510, 191, 100, 26, ID_DLC_BROWSE);
    add(1, L"BUTTON", L"Set up game files", BS_PUSHBUTTON | WS_TABSTOP, 30, 240, 180, 32, ID_SETUP);
    setup_progress = add(1, PROGRESS_CLASSW, L"", 0, 30, 290, 580, 18, ID_SETUP_PROGRESS);
    setup_status = add(1, L"STATIC", L"Your dump is only read. With the update in its own folder, the two are merged into data\\dump "
                                     L"(hard links on the same drive, so no extra space; a copy otherwise).",
                       SS_LEFT, 30, 316, 580, 80, ID_SETUP_STATUS);
    // Display
    add(2, L"STATIC", L"Window", SS_LEFT, 30, 56, 160, 22);
    add(2, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 52, 300, 200, ID_WINDOW);
    add(2, L"STATIC", L"Window size", SS_LEFT, 30, 92, 160, 22);
    add(2, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 88, 300, 200, ID_SIZE);
    add(2, L"STATIC", L"Render resolution", SS_LEFT, 30, 128, 160, 22);
    add(2, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 124, 300, 200, ID_RENDER);
    add(2, L"STATIC", L"Higher render resolutions are sharper and need a fast GPU; at 4K some courses drop below 60 FPS.",
        SS_LEFT, 200, 152, 410, 34);
    add(2, L"BUTTON", L"60 FPS mode (the game's frame rate priority option)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 196, 560, 22, ID_SIXTY);
    add(2, L"BUTTON", L"Anti-aliasing (SMAA)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 224, 560, 22, ID_SMAA);
    add(2, L"BUTTON", L"Sharpening and upscaling to the window (AMD FSR 1)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 252, 560, 22, ID_SHARPEN);
    add(2, L"BUTTON", L"VSync", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 280, 560, 22, ID_VSYNC);
    add(2, L"BUTTON", L"HDR (needs Windows HDR on)", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 308, 560, 22, ID_HDR);
    add(2, L"BUTTON", L"Skip the start-up logos", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 336, 560, 22, ID_INTRO);
    add(2, L"STATIC", L"Changes are saved at once and used from the next start. F10 in the game changes the window settings too.",
        SS_LEFT, 30, 372, 580, 34);
    // HD textures
    add(3, L"STATIC", L"HD textures are made on this PC from your own game: the game saves each texture it uses while you play, "
                      L"then Real-ESRGAN upscales them on your GPU into a pack the game loads instead.",
        SS_LEFT, 30, 50, 580, 50);
    add(3, L"BUTTON", L"1. Collect textures while playing", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 108, 560, 22, ID_COLLECT);
    tex_count = add(3, L"STATIC", L"", SS_LEFT, 52, 132, 560, 20, ID_TEX_COUNT);
    add(3, L"STATIC", L"2. Upscale", SS_LEFT, 30, 166, 100, 22);
    add(3, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 162, 280, 200, ID_SCALE);
    add(3, L"BUTTON", L"Build HD texture pack", BS_PUSHBUTTON | WS_TABSTOP, 420, 160, 190, 30, ID_BUILD);
    tex_progress = add(3, PROGRESS_CLASSW, L"", 0, 30, 204, 580, 18, ID_TEX_PROGRESS);
    tex_status = add(3, L"STATIC", L"Building takes a few seconds per texture; it can run again later to add new ones.",
                     SS_LEFT, 30, 228, 580, 60, ID_TEX_STATUS);
    add(3, L"BUTTON", L"3. Use the HD texture pack", BS_AUTOCHECKBOX | WS_TABSTOP, 30, 300, 560, 22, ID_USE_PACK);
    add(3, L"STATIC", L"Packs are made from your copy of the game: they stay on this PC and are never shared.",
        SS_LEFT, 30, 336, 580, 40);

    fill_combo(ID_WINDOW, window_modes, settings.get("display_mode", "borderless"));
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
    check(ID_USE_PACK, !settings.get("texture_pack").empty());
    Paths p = saved_paths();
    SetWindowTextW(controls[ID_BASE], p.game.c_str());
    SetWindowTextW(controls[ID_DLC], p.dlc.c_str());
}

// The first page: Game files until the game is set up, else Play; --page N (0-3) picks one.
void open_first_page(int page) {
    if (page < 0 || page > 3) page = game_ready() ? 0 : 1;
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
    case ID_BUILD: build_textures(); break;
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
        set_setting("texture_pack", checked(ID_USE_PACK) ? utf8(pack.wstring()) : "");
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
                SendMessageW(progress_target, PBM_SETPOS, percent, 0);
            }
        }
        return 0;
    case WM_JOB_DONE:
        busy = false;
        set_busy(false);
        if (progress_target) {
            SendMessageW(progress_target, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(progress_target, GWL_STYLE, GetWindowLongPtrW(progress_target, GWL_STYLE) & ~PBS_MARQUEE);
            SendMessageW(progress_target, PBM_SETPOS, wp ? 100 : 0, 0);
        }
        refresh_status();
        return 0;
    case WM_GAME_EXIT:
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        refresh_status();
        return 0;
    case WM_CLOSE:
        if (busy && MessageBoxW(hwnd, L"A job is still running. Close anyway? It stops where it is.", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
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
    RECT r{0, 0, px(640), px(430)};
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
