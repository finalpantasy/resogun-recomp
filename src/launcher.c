/* The executable configures its own portable state before the loader starts. */
#include "launcher.h"
#include "prepare.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#include <direct.h>
#else
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TITLE "Everybody's Golf"
#define PATH_CHARS GOLF_PATH
#define SEP "/"
#ifndef GOLF_LAUNCHER_VERSION
#define GOLF_LAUNCHER_VERSION "development"
#endif

static char exe_dir[PATH_CHARS], data_dir[PATH_CHARS], save[PATH_CHARS], log_path[PATH_CHARS];
static char game[PATH_CHARS], dlc[PATH_CHARS], boot[PATH_CHARS], profile[PATH_CHARS], isa[PATH_CHARS];
static char absolute_boot[PATH_CHARS], explicit_profile[PATH_CHARS], explicit_isa[PATH_CHARS];
static char *runtime_args[128];
static int unattended, show_dialog;

static int complain(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    if (show_dialog) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, message, NULL);
    return LAUNCH_ERROR;
}

static int exists(const char *path) {
    struct stat st;
    return !stat(path, &st);
}
static int is_directory(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
static int is_absolute(const char *path) {
#ifdef _WIN32
    return (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
           (path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/');
#else
    return path[0] == '/';
#endif
}
static int copy_path(char out[PATH_CHARS], const char *value) {
    size_t length = strlen(value);
    if (length >= PATH_CHARS || strpbrk(value, "\r\n")) return 0;
    memmove(out, value, length + 1);
#ifdef _WIN32
    for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
#endif
    return 1;
}
static int join_path(char out[PATH_CHARS], const char *dir, const char *leaf) {
    char buffer[PATH_CHARS];
    int n = snprintf(buffer, sizeof buffer, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", leaf);
    return n >= 0 && n < PATH_CHARS && copy_path(out, buffer);
}
static int rooted(char out[PATH_CHARS], const char *dir, const char *value) {
    return is_absolute(value) ? copy_path(out, value) : join_path(out, dir, value);
}
static int from_cwd(char out[PATH_CHARS], const char *value) {
    if (!value || !*value) return 0;
#ifdef _WIN32
    char buffer[PATH_CHARS];
    DWORD n = GetFullPathNameA(value, sizeof buffer, buffer, NULL);
    return n && n < sizeof buffer && copy_path(out, buffer);
#else
    if (is_absolute(value)) return copy_path(out, value);
    char cwd[PATH_CHARS];
    return getcwd(cwd, sizeof cwd) && join_path(out, cwd, value);
#endif
}
static int new_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}
static int make_dirs(const char *path) {
    if (is_directory(path)) return 1;
    char part[PATH_CHARS];
    if (!is_absolute(path) || !copy_path(part, path)) return 0;
    char *start = part + 1;
#ifdef _WIN32
    if (part[0] == '/' && part[1] == '/') {
        start = strchr(part + 2, '/');
        if (start) start = strchr(start + 1, '/');
        if (!start) return is_directory(path);
        ++start;
    } else start = part + 3;
#endif
    for (char *p = start; *p; ++p) {
        if (*p != '/' || p == start || p[-1] == '/') continue;
        *p = 0;
        int ok = new_directory(part) || is_directory(part);
        *p = '/';
        if (!ok) return 0;
    }
    return new_directory(part) || is_directory(part);
}
static void remove_dir(const char *path) {
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}
static int find_exe_dir(void) {
#ifdef _WIN32
    wchar_t wide[PATH_CHARS];
    char utf8[PATH_CHARS];
    DWORD n = GetModuleFileNameW(NULL, wide, PATH_CHARS);
    if (!n || n >= PATH_CHARS ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, NULL, NULL) ||
        !copy_path(exe_dir, utf8)) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);
    if (n <= 0 || n == (ssize_t)sizeof exe_dir - 1) return 0;
    exe_dir[n] = 0;
#endif
    char *last = strrchr(exe_dir, '/');
    if (!last) return 0;
    if (last == exe_dir || (last == exe_dir + 2 && exe_dir[1] == ':')) ++last;
    *last = 0;
    return 1;
}
static int set_env(const char *name, const char *value) {
#ifdef _WIN32
    return !_putenv_s(name, value);
#else
    return !setenv(name, value, 1);
#endif
}

const char *launcher_save_dir(void) { return save; }

typedef struct { char chosen[PATH_CHARS]; SDL_AtomicInt done; } FolderChoice;
static void SDLCALL folder_selected(void *data, const char *const *files, int filter) {
    (void)filter;
    FolderChoice *choice = data;
    if (files && files[0]) copy_path(choice->chosen, files[0]);
    SDL_SetAtomicInt(&choice->done, 1);
}
static int pick_folder(char out[PATH_CHARS]) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 0;
    FolderChoice choice = {0};
    SDL_ShowOpenFolderDialog(folder_selected, &choice, NULL, NULL, false);
    while (!SDL_GetAtomicInt(&choice.done)) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return choice.chosen[0] && from_cwd(out, choice.chosen);
}

/* The crash watch (RESOGUN_CRASH_WATCH, on unless 0): an unattended start runs the game as a
 * child of this process and waits for it. Whatever ends the game with an exit code other than 0 (a
 * fault the runtime reports, an error exit, or an end no handler sees: a fast fail, abort(), a
 * driver ending the process) is added to the log, the log is kept as log/crash-<time>.log and the
 * player is told where it is. The next start no longer overwrites the only record of a crash, and
 * every crash keeps its own copy (none is ever deleted here: the player may delete them). */

/* What an exit code means, "" when nothing is known about it. */
static void describe_exit(unsigned long code, char *out, size_t size) {
    const char *name = "";
    switch (code) {
    case 1: name = "an error, see the ERROR: line"; break;
    case 3: name = "a fault the runtime reported, see the Fault: lines, or abort()"; break;
    case 20: name = "the runtime stopped, see the STOP: line"; break;
#ifdef _WIN32
    case 0xC0000005: name = "access violation"; break;
    case 0xC000001D: name = "illegal instruction"; break;
    case 0xC0000094: name = "integer divide by zero"; break;
    case 0xC0000096: name = "privileged instruction"; break;
    case 0xC00000FD: name = "stack overflow"; break;
    case 0xC0000135: name = "a DLL is missing"; break;
    case 0xC0000142: name = "a DLL failed to start"; break;
    case 0xC0000374: name = "heap corruption"; break;
    case 0xC0000409: name = "fast fail: a security check or abort()"; break;
    case 0x80000003: name = "breakpoint"; break;
#else
    default:
        if (code > 128 && code < 128 + 65) {
            snprintf(out, size, "signal %lu (%s)", code - 128, strsignal((int)(code - 128)));
            return;
        }
#endif
    }
    snprintf(out, size, "%s", name);
}

/* The game ended with `code`: the end goes at the bottom of the log, the log is copied to
 * log/crash-<time>.log and a message box names that file. */
static void report_crash(const char *log_dir, unsigned long code) {
    char meaning[160], stamp[32], crash[PATH_CHARS], message[3 * PATH_CHARS];
    describe_exit(code, meaning, sizeof meaning);
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    make_dirs(log_dir);
    FILE *log = fopen(log_path, "a");
    if (log) {
        fprintf(log, "Launcher: the game ended with exit code %lu (0x%08lx)%s%s\n", code, code, meaning[0] ? ": " : "", meaning);
        fclose(log);
    }
    snprintf(crash, sizeof crash, "%s" SEP "crash-%s.log", log_dir, stamp);
    FILE *from = fopen(log_path, "rb"), *to = from ? fopen(crash, "wb") : NULL;
    if (to) {
        char buffer[65536];
        for (size_t n; (n = fread(buffer, 1, sizeof buffer, from));) fwrite(buffer, 1, n, to);
        fclose(to);
    }
    if (from) fclose(from);
    if (!to) snprintf(crash, sizeof crash, "%s", log_path);
    snprintf(message, sizeof message,
             "The game closed unexpectedly (exit code 0x%08lx%s%s).\n\nIts log was kept as:\n%s\n\n"
             "Please attach that file when you report the problem.",
             code, meaning[0] ? ": " : "", meaning, crash);
    fprintf(stderr, "ERROR: %s\n", message);
#ifdef _WIN32
    strncat(message, "\n\nOpen the log's folder now?", sizeof message - strlen(message) - 1);
    if (MessageBoxA(NULL, message, TITLE, MB_YESNO | MB_ICONERROR) == IDYES) {
        char command[PATH_CHARS + 64];
        snprintf(command, sizeof command, "explorer.exe /select,\"%s\"", crash);
        STARTUPINFOA startup = {.cb = sizeof startup};
        PROCESS_INFORMATION explorer;
        if (CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &explorer)) {
            CloseHandle(explorer.hThread);
            CloseHandle(explorer.hProcess);
        }
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, message, NULL);
#endif
}

/* Starts the game as a child and waits for it: 1 with its exit code in *code once it has ended,
 * 0 when this process is to run the game itself (the child, or no child could be started). */
static int watch_game(unsigned long *code) {
#ifdef _WIN32
    /* The same command line again, in a job that also takes in the process a restart starts
     * (runtime_restart, whose exit is then the one that counts) and that ends with this process. */
    HANDLE job = CreateJobObjectW(NULL, NULL), port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!job || !port) return 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT link = {.CompletionKey = job, .CompletionPort = port};
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &link, sizeof link)) return 0;
    _putenv_s("EG_LAUNCH_WATCH_CHILD", "1");
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION child;
    wchar_t *line = _wcsdup(GetCommandLineW());
    if (!line || !CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child)) {
        _putenv_s("EG_LAUNCH_WATCH_CHILD", "");
        return 0;
    }
    if (!AssignProcessToJobObject(job, child.hProcess)) {
        TerminateProcess(child.hProcess, 0);
        _putenv_s("EG_LAUNCH_WATCH_CHILD", "");
        return 0;
    }
    AllowSetForegroundWindow(child.dwProcessId);
    ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    HANDLE last = child.hProcess;
    DWORD message;
    ULONG_PTR key;
    LPOVERLAPPED data;
    while (GetQueuedCompletionStatus(port, &message, &key, &data, INFINITE)) {
        DWORD pid = (DWORD)(ULONG_PTR)data;
        if (message == JOB_OBJECT_MSG_NEW_PROCESS && pid != child.dwProcessId) {
            HANDLE next = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (next) last = next;
        } else if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) break;
    }
    WaitForSingleObject(last, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(last, &exit_code);
    *code = exit_code;
    return 1;
#else
    /* The child returns to run the game and ends with this process (a restart execs in place). */
    setenv("EG_LAUNCH_WATCH_CHILD", "1", 1);
    pid_t pid = fork();
    if (pid < 0) unsetenv("EG_LAUNCH_WATCH_CHILD");
    if (pid <= 0) {
        if (!pid) prctl(PR_SET_PDEATHSIG, SIGTERM);
        return 0;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *code = WIFSIGNALED(status) ? 128 + (unsigned long)WTERMSIG(status) : (unsigned long)WEXITSTATUS(status);
    return 1;
#endif
}

/* An exit code that is not a crash: 0, or the player or the system ending the game. */
static int quiet_exit(unsigned long code) {
#ifdef _WIN32
    return !code || code == 0xC000013A /* STATUS_CONTROL_C_EXIT */;
#else
    return !code || code == 128 + SIGTERM || code == 128 + SIGINT;
#endif
}


typedef struct { const char *flag, *suffix; int path; } Setting;
static const Setting settings[] = {
    {"--amd-paths-off", "AMD_PATHS_OFF", 0},
    {"--aspect-ratio", "ASPECT_RATIO", 0},
    {"--async-fences", "ASYNC_FENCES", 0},
    {"--audio-channels", "AUDIO_CHANNELS", 0},
    {"--audio-dump", "AUDIO_DUMP", 1},
    {"--audio-stats", "AUDIO_STATS", 0},
    {"--audio", "AUDIO", 0},
    {"--background-warmup", "BACKGROUND_WARMUP", 0},
    {"--buffer-stats", "BUFFER_STATS", 0},
    {"--capture-dir", "CAPTURE_DIR", 1},
    {"--capture-trigger", "CAPTURE_TRIGGER", 1},
    {"--config-default", "CONFIG_DEFAULT", 1},
    {"--config", "CONFIG", 1},
    {"--copy-gpu-buffers", "COPY_GPU_BUFFERS", 0},
    {"--copy-merge-kb", "COPY_MERGE_KB", 0},
    {"--copy-threads", "COPY_THREADS", 0},
    {"--cpu-check", "CPU_CHECK", 0},
    {"--crash-watch", "CRASH_WATCH", 0},
    {"--deferred-readback", "DEFERRED_READBACK", 0},
    {"--diag-dir", "DIAG_DIR", 1},
    {"--diag-images", "DIAG_IMAGES", 0},
    {"--direct-memory-access", "DIRECT_MEMORY_ACCESS", 0},
    {"--disk-shaders", "DISK_SHADERS", 0},
    {"--display-mode", "DISPLAY_MODE", 0},
    {"--dmem-mb", "DMEM_MB", 0},
    {"--draw-pipe", "DRAW_PIPE", 0},
    {"--dump-dir", "DUMP_DIR", 1},
    {"--dump-shaders", "DUMP_SHADERS", 0},
    {"--fail-guest-memory", "FAIL_GUEST_MEMORY", 0},
    {"--fault-peek", "FAULT_PEEK", 0},
    {"--fault-window", "FAULT_WINDOW", 0},
    {"--file-trace-match", "FILE_TRACE_MATCH", 0},
    {"--file-trace", "FILE_TRACE", 0},
    {"--frame-dump-trigger", "FRAME_DUMP_TRIGGER", 1},
    {"--frame-rate", "FRAME_RATE", 0},
    {"--frame-stats", "FRAME_STATS", 0},
    {"--full-barriers", "FULL_BARRIERS", 0},
    {"--gc-budget-mb", "GC_BUDGET_MB", 0},
    {"--gpu-id", "GPU_ID", 0},
    {"--gpu-log", "GPU_LOG", 0},
    {"--gpu-profile", "GPU_PROFILE", 0},
    {"--gpu-dir", "GPU_USER_DIR", 1},
    {"--guest-commit-on-map", "GUEST_COMMIT_ON_MAP", 0},
    {"--hang-dump", "HANG_DUMP", 0},
    {"--hdr", "HDR", 0},
    {"--hot-pages", "HOT_PAGES", 0},
    {"--ime-text", "IME_TEXT", 0},
    {"--internal-height", "INTERNAL_HEIGHT", 0},
    {"--internal-width", "INTERNAL_WIDTH", 0},
    {"--interp-int-fix", "INTERP_INT_FIX", 0},
    {"--isa-rewrite-mode", "ISA_REWRITE", 0},
    {"--language", "LANGUAGE", 0},
    {"--low-handles", "LOW_HANDLES", 0},
    {"--log-missing", "LOG_MISSING", 0},
    {"--monitor", "MONITOR", 0},
    {"--no-msaa", "NO_MSAA", 0},
    {"--null-gpu", "NULL_GPU", 0},
    {"--resolution", "OUTPUT_RESOLUTION", 0},
    {"--pad-file", "PAD_FILE", 1},
    {"--pad-record", "PAD_RECORD", 1},
    {"--pad-replay", "PAD_REPLAY", 1},
    {"--parallel-warmup", "PARALLEL_WARMUP", 0},
    {"--partial-height-views", "PARTIAL_HEIGHT_VIEWS", 0},
    {"--perf-diag", "PERF_DIAG", 0},
    {"--pipe-spin-us", "PIPE_SPIN_US", 0},
    {"--pipe-verify", "PIPE_VERIFY", 0},
    {"--pipeline-binaries", "PIPELINE_BINARIES", 0},
    {"--pipeline-binary-isolated", "PIPELINE_BINARY_ISOLATED", 0},
    {"--pipeline-cache", "PIPELINE_CACHE", 0},
    {"--pipeline-capture-only", "PIPELINE_CAPTURE_ONLY", 0},
    {"--prep-workers", "PREP_WORKERS", 0},
    {"--present-thread", "PRESENT_THREAD", 0},
    {"--readback-linear", "READBACK_LINEAR", 0},
    {"--readbacks", "READBACKS", 0},
    {"--reload-trigger", "RELOAD_TRIGGER", 1},
    {"--render-resolution", "RENDER_RESOLUTION", 0},
    {"--rwlock-front", "RWLOCK_FRONT", 0},
    {"--save-trace", "SAVE_TRACE", 0},
    {"--sema-front", "SEMA_FRONT", 0},
    {"--shader-dir", "SHADER_DIR", 1},
    {"--shader-notes", "SHADER_NOTES", 0},
    {"--shader-prefetch", "SHADER_PREFETCH", 0},
    {"--sharpening", "SHARPENING", 0},
    {"--skip-intro", "SKIP_INTRO", 0},
    {"--slow-waits", "SLOW_WAITS", 0},
    {"--square-pass", "SQUARE_PASS", 0},
    {"--staging-keep-mb", "STAGING_KEEP_MB", 0},
    {"--stale-readback", "STALE_READBACK", 0},
    {"--stall-ms", "STALL_MS", 0},
    {"--stream-memory", "STREAM_MEMORY", 0},
    {"--strict-handles", "STRICT_HANDLES", 0},
    {"--target-sizes", "TARGET_SIZES", 0},
    {"--tearing", "TEARING", 0},
    {"--test-refuse-dmem", "TEST_REFUSE_DMEM", 0},
    {"--texture-helper", "TEXTURE_HELPER", 0},
    {"--thread-front", "THREAD_FRONT", 0},
    {"--throw-trace", "THROW_TRACE", 0},
    {"--title-back-exits", "TITLE_BACK_EXITS", 0},
    {"--toggle-file", "TOGGLE_FILE", 1},
    {"--trace-dingdong", "TRACE_DINGDONG", 0},
    {"--uffd", "UFFD", 0},
    {"--ui-language", "UI_LANGUAGE", 0},
    {"--vblank-hz", "VBLANK_HZ", 0},
    {"--vblank-timer", "VBLANK_TIMER", 0},
    {"--verbose", "VERBOSE", 0},
    {"--verify-prepared", "VERIFY_PREPARED", 0},
    {"--vk-markers", "VK_MARKERS", 0},
    {"--vk-record-thread", "VK_RECORD_THREAD", 0},
    {"--vk-validation-sync", "VK_VALIDATION_SYNC", 0},
    {"--vk-validation", "VK_VALIDATION", 0},
    {"--vsync", "VSYNC", 0},
    {"--wave-stats", "WAVE_STATS", 0},
    {"--window-icon", "WINDOW_ICON", 1},
    {"--window", "WINDOW", 0},
    {"--work-retired", "WORK_RETIRED", 0},
};

typedef struct {
    const char *game, *dlc, *data, *save, *log, *prepared, *shortcut;
    const char *boot, *profile, *isa, *timeout;
    int prepare_only, reprepare, any_build, console, no_dlc;
    int forward_count, mount_count;
    char *forward[106], mount[24][PATH_CHARS + 128];
} Options;

static const char *env_option(const char *suffix) {
    char name[128];
    snprintf(name, sizeof name, "EG_%s", suffix);
    const char *value = getenv(name);
    if (value && *value) return value;
    snprintf(name, sizeof name, "RESOGUN_%s", suffix);
    value = getenv(name);
    return value && *value ? value : NULL;
}

static int env_set(const char *suffix, const char *value) {
    char name[128];
    snprintf(name, sizeof name, "RESOGUN_%s", suffix);
    return set_env(name, value);
}

static void usage(void) {
    printf(TITLE " %s\n"
           "  everybodys-golf [OPTIONS]\n"
           "  everybodys-golf --game DIR --prepare-only\n"
           "  everybodys-golf BOOT --app0 DIR --content-profile FILE [LOADER OPTIONS]\n\n"
           "  --game DIR           Merged, decrypted PS4 dump (CUSA04687 update 1.17)\n"
           "  --data-dir DIR       Portable state root (default: executable directory)\n"
           "  --dlc DIR            Extra directory of extracted add-on folders\n"
           "  --no-dlc             Ignore the extra add-on directory\n"
           "  --entitlements MODE  Built-in or installed-folder mode: builtin | original\n"
           "  --np-mode MODE      Synthetic local account or signed out: local | offline\n"
           "  --online-id NAME    Local account name (up to 16 bytes)\n"
           "  --discovery-port N  Local replay port (0 disables replay)\n"
           "  --local-auth N      Synthetic login fixture, 0 or 1\n"
           "  --ticket-grants N   Synthetic ticket fixture, 0 or 1\n"
           "  --tus MODE          In-process Title User Storage, local or off\n"
           "  --font-file FILE    Host font for guest text\n"
           "  --save DIR           Save location (default: DATA/save)\n"
           "  --config FILE        Settings file (default: SAVE/settings.ini)\n"
           "  --log FILE           Append launcher and runtime output here\n"
           "  --prepare-only      Prepare or validate the cached image and exit\n"
           "  --reprepare         Regenerate the cached image\n"
           "  --prepared DIR      Use an existing preparation directory explicitly\n"
           "  --any-build         Permit an unpinned EBOOT and SFO\n"
           "  --fullscreen | --windowed | --borderless\n"
           "  --resolution WxH    Window size, for example 1920x1080\n"
           "  --monitor N         Output monitor index\n"
           "  --skip-intro | --play-intro\n"
           "  --timeout SECONDS   Runtime watchdog (default: 0, disabled)\n"
           "  --console           Keep console output when started graphically\n"
           "  --no-crash-watch    Disable the crash watcher\n"
           "  --shortcut DIR      Write a launcher shortcut to the specified directory and exit\n"
           "  --help | -h | --version\n\n"
           "  Loader options: --cpu-only --strict-imports --check-imports --app0 DIR\n"
           "                  --user DIR --content-profile FILE --isa-rewrite FILE\n"
           "                  --mount GUEST=HOST --guest-arg ARG\n"
           "  Standalone diagnostics: --cpu-features --gamepad --vulkan-only\n\n"
           "Runtime settings (CLI overrides EG_*, legacy RESOGUN_* and settings.ini)\n", GOLF_LAUNCHER_VERSION);
    for (size_t i = 0; i < sizeof settings / sizeof *settings; ++i)
        printf("  %-28s %-5s EG_%s\n", settings[i].flag, settings[i].path ? "PATH" : "VALUE", settings[i].suffix);
}

static int option_error(const char *option, const char *why) {
    fprintf(stderr, "ERROR: %s: %s. See --help.\n", option, why);
    return LAUNCH_ERROR;
}

static int number(const char *value, unsigned long maximum) {
    if (!*value || strspn(value, "0123456789") != strlen(value)) return 0;
    errno = 0;
    return strtoul(value, NULL, 10) <= maximum && !errno;
}

static int forward(Options *o, char *argument) {
    if (o->forward_count >= (int)(sizeof o->forward / sizeof *o->forward) - 1) return 0;
    o->forward[o->forward_count++] = argument;
    return 1;
}

/* Writable path settings are converted to absolute paths before any runtime code reads them. */
static int import_env(char *out, const char *root, const char *fallback, const char *value) {
    return value && *value ? rooted(out, root, value) : join_path(out, root, fallback);
}

static int parse(Options *o, int argc, char **argv) {
    for (size_t i = 0; i < sizeof settings / sizeof *settings; ++i) {
        const char *suffix = settings[i].suffix, *value = env_option(suffix);
        if (value && !env_set(suffix, value)) return option_error(settings[i].flag, "invalid environment value");
    }
    int start = 1;
    if (argc > 1 && argv[1][0] != '-') { o->boot = argv[1]; ++start; }
    for (int i = start; i < argc; ++i) {
        char *flag = argv[i];
        if (!strcmp(flag, "--help") || !strcmp(flag, "-h")) { usage(); return LAUNCH_DONE; }
        if (!strcmp(flag, "--version")) { printf(TITLE " %s\n", GOLF_LAUNCHER_VERSION); return LAUNCH_DONE; }
        if (!strcmp(flag, "--console")) { o->console = 1; continue; }
        if (!strcmp(flag, "--prepare-only")) { o->prepare_only = 1; continue; }
        if (!strcmp(flag, "--reprepare")) { o->reprepare = 1; continue; }
        if (!strcmp(flag, "--any-build")) { o->any_build = 1; continue; }
        if (!strcmp(flag, "--no-dlc")) { o->dlc = NULL; o->no_dlc = 1; continue; }
        if (!strcmp(flag, "--entitlements")) {
            if (++i >= argc || (strcmp(argv[i], "builtin") && strcmp(argv[i], "original")))
                return option_error(flag, "expected builtin or original");
            if (!set_env("GOLF_ENTITLEMENTS", argv[i])) return option_error(flag, "cannot set entitlement mode");
            continue;
        }
        const char *golf_env = NULL;
        if (!strcmp(flag, "--np-mode")) golf_env = "GOLF_NP";
        else if (!strcmp(flag, "--online-id")) golf_env = "GOLF_ONLINE_ID";
        else if (!strcmp(flag, "--discovery-port")) golf_env = "GOLF_DISCOVERY_PORT";
        else if (!strcmp(flag, "--local-auth")) golf_env = "GOLF_LOCAL_AUTH";
        else if (!strcmp(flag, "--ticket-grants")) golf_env = "GOLF_TICKET_GRANTS";
        else if (!strcmp(flag, "--tus")) golf_env = "GOLF_TUS";
        else if (!strcmp(flag, "--font-file")) golf_env = "GOLF_FONT_FILE";
        if (golf_env) {
            if (++i >= argc || !argv[i][0] || !strncmp(argv[i], "--", 2)) return option_error(flag, "missing argument");
            const char *value = argv[i];
            char absolute[PATH_CHARS];
            if (!strcmp(golf_env, "GOLF_NP") && strcmp(value, "local") && strcmp(value, "offline"))
                return option_error(flag, "expected local or offline");
            if (!strcmp(golf_env, "GOLF_ONLINE_ID") && strlen(value) > 16)
                return option_error(flag, "expected a name of at most 16 bytes");
            if (!strcmp(golf_env, "GOLF_DISCOVERY_PORT") && (strlen(value) > 5 || !number(value, 65535)))
                return option_error(flag, "expected a loopback port from 0 through 65535");
            if ((!strcmp(golf_env, "GOLF_LOCAL_AUTH") || !strcmp(golf_env, "GOLF_TICKET_GRANTS"))
                    && strcmp(value, "0") && strcmp(value, "1"))
                return option_error(flag, "expected 0 or 1");
            if (!strcmp(golf_env, "GOLF_TUS") && strcmp(value, "local") && strcmp(value, "off"))
                return option_error(flag, "expected local or off");
            if (!strcmp(golf_env, "GOLF_FONT_FILE")) {
                if (!from_cwd(absolute, value)) return option_error(flag, "invalid path");
                value = absolute;
            }
            if (!set_env(golf_env, value)) return option_error(flag, "invalid value");
            continue;
        }
        if (!strcmp(flag, "--no-crash-watch")) { if (!env_set("CRASH_WATCH", "0")) return LAUNCH_ERROR; continue; }
        if (!strcmp(flag, "--fullscreen") || !strcmp(flag, "--windowed") || !strcmp(flag, "--borderless")) {
            if (!env_set("DISPLAY_MODE", flag + 2)) return LAUNCH_ERROR;
            continue;
        }
        if (!strcmp(flag, "--play-intro") || (!strcmp(flag, "--skip-intro") &&
            (i + 1 == argc || (strcmp(argv[i + 1], "0") && strcmp(argv[i + 1], "1"))))) {
            if (!env_set("SKIP_INTRO", !strcmp(flag, "--play-intro") ? "0" : "1")) return LAUNCH_ERROR;
            continue;
        }
        if (!strcmp(flag, "--verbose") && (i + 1 == argc ||
            (strcmp(argv[i + 1], "0") && strcmp(argv[i + 1], "1")))) {
            if (!env_set("VERBOSE", "1")) return LAUNCH_ERROR;
            continue;
        }
        if (!strcmp(flag, "--cpu-only") || !strcmp(flag, "--strict-imports") || !strcmp(flag, "--check-imports")) {
            if (!forward(o, flag)) return option_error(flag, "too many loader options");
            continue;
        }
        if (!strcmp(flag, "--mount") || !strcmp(flag, "--guest-arg")) {
            if (++i >= argc) return option_error(flag, "missing argument");
            char *value = argv[i];
            if (!strcmp(flag, "--mount")) {
                const char *eq = strchr(value, '=');
                char host[PATH_CHARS];
                if (o->mount_count == 24 || !eq || eq - value < 1 || eq - value >= 128 || !from_cwd(host, eq + 1))
                    return option_error(flag, "expected GUEST=HOST (at most 24 mounts)");
                char *spec = o->mount[o->mount_count++];
                snprintf(spec, PATH_CHARS + 128, "%.*s=%s", (int)(eq - value), value, host);
                value = spec;
            }
            if (!forward(o, flag) || !forward(o, value)) return option_error(flag, "too many loader options");
            continue;
        }
        const char **target = NULL;
        if (!strcmp(flag, "--game") || !strcmp(flag, "--game-dir") || !strcmp(flag, "--app0")) target = &o->game;
        else if (!strcmp(flag, "--save") || !strcmp(flag, "--save-dir") ||
                 !strcmp(flag, "--user-dir") || !strcmp(flag, "--user")) target = &o->save;
        else if (!strcmp(flag, "--dlc") || !strcmp(flag, "--dlc-dir")) { target = &o->dlc; o->no_dlc = 0; }
        else if (!strcmp(flag, "--data-dir")) target = &o->data;
        else if (!strcmp(flag, "--prepared")) target = &o->prepared;
        else if (!strcmp(flag, "--shortcut")) target = &o->shortcut;
        else if (!strcmp(flag, "--content-profile")) target = &o->profile;
        else if (!strcmp(flag, "--isa-rewrite")) target = &o->isa;
        else if (!strcmp(flag, "--timeout")) target = &o->timeout;
        else if (!strcmp(flag, "--log")) target = &o->log;
        if (target) {
            if (++i >= argc || !argv[i][0] || !strncmp(argv[i], "--", 2)) return option_error(flag, "missing argument");
            *target = argv[i];
            if (target == &o->timeout && !number(*target, UINT_MAX)) return option_error(flag, "expected whole seconds");
            continue;
        }
        size_t n = 0;
        while (n < sizeof settings / sizeof *settings && strcmp(flag, settings[n].flag)) ++n;
        if (n == sizeof settings / sizeof *settings) return option_error(flag, "unknown option");
        if (++i >= argc || !argv[i][0] || !strncmp(argv[i], "--", 2)) return option_error(flag, "missing argument");
        const char *value = argv[i];
        char absolute[PATH_CHARS];
        if (settings[n].path) {
            if (!from_cwd(absolute, value)) return option_error(flag, "invalid path");
            value = absolute;
        }
        if (!env_set(settings[n].suffix, value)) return option_error(flag, "invalid value");
    }
    return LAUNCH_RUN;
}

typedef struct { char game[PATH_CHARS], dlc[PATH_CHARS], image[64], key[GOLF_SOURCE_KEY]; } Saved;

static void read_paths(const char *ini, Saved *saved) {
    FILE *f = fopen(ini, "rb");
    if (!f) return;
    char line[PATH_CHARS + 16];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *start = !strncmp(line, "\xef\xbb\xbf", 3) ? line + 3 : line;
        if (!strncmp(start, "game=", 5)) copy_path(saved->game, start + 5);
        else if (!strncmp(start, "dlc=", 4)) copy_path(saved->dlc, start + 4);
        else if (!strncmp(start, "image=image-", 12) && strlen(start + 6) < sizeof saved->image &&
                 start[12] && strspn(start + 12, "0123456789-") == strlen(start + 12))
            strcpy(saved->image, start + 6);
        else if (!strncmp(start, "key=", 4) && strlen(start + 4) == GOLF_SOURCE_KEY - 1)
            strcpy(saved->key, start + 4);
    }
    fclose(f);
}

static const char *relative_below(const char *root, const char *path) {
    size_t n = strlen(root);
    while (n && root[n - 1] == '/') --n;
    if (strncmp(path, root, n)) return NULL;
    const char *relative = path + n;
    if (*relative && *relative != '/') return NULL;
    while (*relative == '/') ++relative;
    return *relative ? relative : ".";
}
static int has_parent_step(const char *path) {
    const char *part = path;
    while (part) {
        if (part[0] == '.' && part[1] == '.' && (!part[2] || part[2] == '/')) return 1;
        part = strchr(part, '/');
        if (part) ++part;
    }
    return 0;
}
static int final_directory(char out[PATH_CHARS], const char *path) {
#ifdef _WIN32
    HANDLE dir = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dir == INVALID_HANDLE_VALUE) return 0;
    DWORD length = GetFinalPathNameByHandleA(dir, out, PATH_CHARS, FILE_NAME_NORMALIZED);
    CloseHandle(dir);
    return length && length < PATH_CHARS && copy_path(out, out);
#else
    char *absolute = realpath(path, NULL);
    if (!absolute) return 0;
    int ok = copy_path(out, absolute);
    free(absolute);
    return ok;
#endif
}
static const char *relative_to_root(const char *path, char resolved[PATH_CHARS]) {
    if (!*path) return path;
    const char *relative = relative_below(data_dir, path);
    if (relative && !has_parent_step(relative)) return relative;

    /* Resolve aliases and decide whether parent steps leave the portable root. */
    char root[PATH_CHARS];
    if (!final_directory(resolved, path)) return path;
    if (final_directory(root, data_dir)) {
        relative = relative_below(root, resolved);
        if (relative) return relative;
    }
    if (!has_parent_step(path)) return path;
#ifdef _WIN32
    return from_cwd(resolved, path) ? resolved : path;
#else
    return resolved;
#endif
}
static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}
static int write_paths(const char *ini, const char *image_name, const char *key) {
    char tmp[PATH_CHARS], resolved_game[PATH_CHARS], resolved_dlc[PATH_CHARS];
    int n = snprintf(tmp, sizeof tmp, "%s.%lu.tmp", ini, process_id());
    if (n < 0 || n >= PATH_CHARS) return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fprintf(f, "[paths]\ngame=%s\ndlc=%s\nimage=%s\nkey=%s\n",
                     relative_to_root(game, resolved_game), relative_to_root(dlc, resolved_dlc),
                     image_name, key) >= 0;
    if (fclose(f)) ok = 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(tmp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(tmp, ini) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

static int content_complete(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char data[28];
    int complete = fread(data, 1, sizeof data, file) == sizeof data &&
                   !memcmp(data, "RGCONT01", 8) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return complete;
}

static int image_paths(const char *dir, int need_isa) {
    return join_path(boot, dir, "boot-linked.bin") && prepare_boot_complete(boot) &&
           join_path(profile, dir, "content.bin") && content_complete(profile) &&
           join_path(isa, dir, "isa-rewrite.bin") &&
           ((!need_isa && !exists(isa)) || prepare_isa_complete(isa));
}
static void discard_image(const char *dir) {
    const char *files[] = {"boot-linked.bin", "content.bin", "isa-rewrite.bin"};
    char path[PATH_CHARS];
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i)
        if (join_path(path, dir, files[i])) remove(path);
    remove_dir(dir);
}

static int prepare_launch(const Options *o, const Saved *saved, const char *ini) {
    if (o->prepared) {
        char path[PATH_CHARS];
        if (!from_cwd(path, o->prepared) || !image_paths(path, 0)) return complain("Invalid --prepared directory");
        printf("Using prepared image: %s\n", path);
        return LAUNCH_RUN;
    }
    char key[GOLF_SOURCE_KEY], folder[PATH_CHARS], image[PATH_CHARS], name[64];
    if (prepare_source_key(game, key, o->any_build)) return complain("Unable to read the decrypted game dump");
    if (!join_path(folder, data_dir, "game") || !make_dirs(folder)) return complain("Cannot create the image cache");
    if (!o->reprepare && !strcmp(saved->key, key) && saved->image[0] &&
        join_path(image, folder, saved->image) && image_paths(image, 1)) {
        int legacy_paths = (has_parent_step(saved->game) || has_parent_step(saved->dlc)) &&
                           !env_option("GAME_DIR") && !env_option("DLC_DIR");
        if ((o->game || o->dlc || o->no_dlc || legacy_paths) && !write_paths(ini, saved->image, key))
            return complain("Cannot save paths.ini");
        printf("Using prepared image: %s\n", image);
        return LAUNCH_RUN;
    }

    /* Keep each successful generation immutable. Another launcher may already be reading it. */
    int created = 0;
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(name, sizeof name, "image-%lu-%u", process_id(), i);
        if (!join_path(image, folder, name)) return complain("Image cache path is too long");
        if (new_directory(image)) { created = 1; break; }
        if (!exists(image)) break;
    }
    if (!created) return complain("Cannot create a preparation directory");
    if (prepare_image(game, image, key, o->any_build) || !image_paths(image, 1)) {
        discard_image(image);
        return complain("Could not prepare the game image");
    }
    if (!write_paths(ini, name, key)) {
        discard_image(image);
        return complain("Cannot save paths.ini");
    }
    printf("Prepared image: %s\n", image);
    return LAUNCH_RUN;
}

static int setup_paths(const Options *o, int have_console) {
    char config[PATH_CHARS], defaults[PATH_CHARS], gpu[PATH_CHARS], shader[PATH_CHARS];
    const char *fallback = getenv("RESOGUN_CONFIG_DEFAULT");
    if (!fallback || !*fallback) {
        if (!join_path(defaults, data_dir, "settings.ini")) return LAUNCH_ERROR;
        if (exists(defaults)) fallback = defaults;
    }
    const char *save_override = env_option("SAVE_DIR");
    if (!save_override || !*save_override) save_override = env_option("USER_DIR");
    const char *value = o->save ? o->save : save_override;
    if (!(o->save ? from_cwd(save, value) : import_env(save, data_dir, "save", value)) || !make_dirs(save) ||
        !import_env(config, save, "settings.ini", getenv("RESOGUN_CONFIG")) ||
        !import_env(defaults, exe_dir, "settings.example.ini", fallback) ||
        !import_env(gpu, data_dir, "gpu", getenv("RESOGUN_GPU_USER_DIR")) || !make_dirs(gpu) ||
        !import_env(shader, data_dir, "shaders", getenv("RESOGUN_SHADER_DIR")) || !make_dirs(shader))
        return complain("Cannot configure portable save, config or cache paths");
    if (!env_set("CONFIG", config) || !env_set("CONFIG_DEFAULT", defaults) ||
        !env_set("GPU_USER_DIR", gpu) || !env_set("SHADER_DIR", shader) ||
        !env_set("GPU_ADOPT_SHARED", "0") || !env_set("USER_DIR", save))
        return complain("Cannot set the portable runtime environment");

    for (size_t i = 0; i < sizeof settings / sizeof *settings; ++i) {
        if (!settings[i].path) continue;
        const char *setting = settings[i].suffix;
        char name[128], path[PATH_CHARS];
        snprintf(name, sizeof name, "RESOGUN_%s", setting);
        const char *path_value = getenv(name);
        if (path_value && *path_value &&
            (!rooted(path, data_dir, path_value) || !env_set(setting, path)))
            return complain("Invalid environment path");
    }
#ifndef _WIN32
    /* Keep library fallbacks and driver caches inside the portable root. */
    char host_cache[PATH_CHARS], host_config[PATH_CHARS], host_data[PATH_CHARS];
    if (!join_path(host_cache, data_dir, "cache") || !set_env("XDG_CACHE_HOME", host_cache) ||
        !join_path(host_config, data_dir, "config") || !set_env("XDG_CONFIG_HOME", host_config) ||
        !join_path(host_data, data_dir, "gpu") || !set_env("XDG_DATA_HOME", host_data) ||
        !set_env("MESA_SHADER_CACHE_DIR", shader) || !set_env("__GL_SHADER_DISK_CACHE_PATH", shader) ||
        !set_env("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1")) return complain("Cannot set driver cache paths");
#endif

    if (!o->log && unattended && !have_console) {
        char log_dir[PATH_CHARS];
        if (!join_path(log_dir, data_dir, "log") ||
            !join_path(log_path, log_dir, "everybodys-golf.log")) return complain("Log path is too long");
        if (!make_dirs(log_dir)) return complain("Cannot create the log directory");
        const char *watch = getenv("RESOGUN_CRASH_WATCH");
        unsigned long code = 0;
        if (unattended && !have_console && !o->boot && !o->prepare_only && !getenv("EG_LAUNCH_WATCH_CHILD") &&
            (!watch || strcmp(watch, "0")) && watch_game(&code)) {
            if (!quiet_exit(code)) report_crash(log_dir, code);
            exit((int)code);
        }
        FILE *log = fopen(log_path, "w");
        if (!log || fclose(log)) return complain("Cannot open the runtime log");
        if (!freopen(log_path, "a", stdout) || !freopen(log_path, "a", stderr)) return complain("Cannot redirect the runtime log");
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    return LAUNCH_RUN;
}

static int explicit_log(const char *value) {
    char dir[PATH_CHARS];
    if (!from_cwd(log_path, value) || !copy_path(dir, log_path)) return complain("Invalid log path");
    char *end = strrchr(dir, '/');
    if (!end) return complain("Invalid log directory");
    if (end == dir || (end == dir + 2 && dir[1] == ':')) ++end;
    *end = 0;
    if (!make_dirs(dir)) return complain("Cannot create the log directory");
    FILE *test = fopen(log_path, "a");
    if (!test || fclose(test)) return complain("Cannot open the runtime log");
    if (!freopen(log_path, "a", stdout) || !freopen(log_path, "a", stderr))
        return complain("Cannot redirect the runtime log");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return LAUNCH_RUN;
}

static int shortcut(const char *destination);
static int has_console(int requested) {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    unattended = (!out || out == INVALID_HANDLE_VALUE) && !GetConsoleWindow();
    if (unattended && (requested ? AllocConsole() : AttachConsole(ATTACH_PARENT_PROCESS))) {
        if (freopen("CONOUT$", "w", stdout) && freopen("CONOUT$", "w", stderr)) {
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            unattended = 0;
        }
    }
#else
    unattended = !isatty(STDOUT_FILENO) && !isatty(STDERR_FILENO);
#endif
    return requested;
}

int launcher_start(int *argc, char ***argv) {
    static Options o;
    int console = 0;
    for (int i = 1; i < *argc; ++i) console |= !strcmp((*argv)[i], "--console");
    console = has_console(console);
    show_dialog = unattended && *argc == 1;
    int status = parse(&o, *argc, *argv);
    if (status != LAUNCH_RUN) return status;
    if ((o.boot && (o.reprepare || o.prepare_only || o.prepared)) || (o.prepared && o.reprepare))
        return option_error("image", "choose a prepared image or native preparation");

    if (!find_exe_dir()) return complain("Cannot locate the executable directory");
    const char *data = o.data ? o.data : env_option("DATA_DIR");
    if (!(data ? from_cwd(data_dir, data) : copy_path(data_dir, exe_dir)))
        return option_error("--data-dir", "invalid path");
#ifdef _WIN32
    /* Libraries that honor AppData can use a process-local portable location. */
    char local[PATH_CHARS], roaming[PATH_CHARS];
    if (!join_path(local, data_dir, "host/local") || !join_path(roaming, data_dir, "host/roaming") ||
        !set_env("LOCALAPPDATA", local) || !set_env("APPDATA", roaming))
        return complain("Cannot configure portable Windows cache paths");
#endif
    if (o.log && explicit_log(o.log) != LAUNCH_RUN) return LAUNCH_ERROR;
    if (o.shortcut) return shortcut(o.shortcut);

    char ini[PATH_CHARS];
    if (!join_path(ini, data_dir, "paths.ini")) return complain("Portable path is too long");
    Saved saved = {0};
    if (!o.boot) read_paths(ini, &saved);
    const char *dump = env_option("GAME_DIR");
    if (o.game) {
        if (!from_cwd(game, o.game)) return option_error("--game", "invalid path");
    } else if (dump || saved.game[0]) {
        if (!rooted(game, data_dir, dump ? dump : saved.game)) return complain("Invalid game directory");
    } else if (!o.boot) {
        char eboot[PATH_CHARS];
        if (!join_path(game, data_dir, "dump") || !join_path(eboot, game, "eboot.bin")) return LAUNCH_ERROR;
        if (!exists(eboot) && !(*argc == 1 && unattended && pick_folder(game)))
            return complain("Select a merged, decrypted game dump with --game DIR");
    }

    if (game[0] && !is_directory(game)) {
        char *leaf = strrchr(game, '/');
        if (!leaf || strcmp(leaf + 1, "eboot.bin") || !exists(game)) return complain("Game directory is missing");
        if (leaf == game || (leaf == game + 2 && game[1] == ':')) ++leaf;
        *leaf = 0;
    }
    const char *dlc_env = env_option("DLC_DIR");
    if (!o.no_dlc && (o.dlc || dlc_env || saved.dlc[0])) {
        if (!(o.dlc ? from_cwd(dlc, o.dlc) : rooted(dlc, data_dir, dlc_env ? dlc_env : saved.dlc)) ||
            !is_directory(dlc)) return complain("DLC directory is missing or invalid");
    }

    if (o.boot) {
        if (!from_cwd(absolute_boot, o.boot)) return option_error("boot", "invalid path");
        if (o.profile && !from_cwd(explicit_profile, o.profile)) return option_error("--content-profile", "invalid path");
        if (o.isa && !from_cwd(explicit_isa, o.isa)) return option_error("--isa-rewrite", "invalid path");
    } else {
        int prepared = prepare_launch(&o, &saved, ini);
        if (prepared != LAUNCH_RUN) return prepared;
        if (o.prepare_only) return LAUNCH_DONE;
        if (o.profile && !from_cwd(explicit_profile, o.profile)) return option_error("--content-profile", "invalid path");
        if (o.isa && !from_cwd(explicit_isa, o.isa)) return option_error("--isa-rewrite", "invalid path");
    }

    status = setup_paths(&o, console);
    if (status != LAUNCH_RUN) return status;
    int count = 0;
    runtime_args[count++] = (*argv)[0];
    runtime_args[count++] = o.boot ? absolute_boot : boot;
    const char *content = explicit_profile[0] ? explicit_profile : profile;
    const char *table = explicit_isa[0] ? explicit_isa : exists(isa) ? isa : "";
    const char *names[] = {"--content-profile", "--app0", "--user", "--dlc", "--timeout", "--isa-rewrite"};
    const char *values[] = {content, game, save, dlc, o.timeout ? o.timeout : (o.boot ? "10" : "0"), table};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        if (!values[i][0]) continue;
        runtime_args[count++] = (char *)names[i];
        runtime_args[count++] = (char *)values[i];
    }
    for (int i = 0; i < o.forward_count; ++i) runtime_args[count++] = o.forward[i];
    runtime_args[count] = NULL;
    *argv = runtime_args;
    *argc = count;
    return LAUNCH_RUN;
}

#ifdef _WIN32
static int shortcut(const char *destination) {
    char folder[PATH_CHARS], filename[PATH_CHARS];
    wchar_t target[PATH_CHARS], path[PATH_CHARS], root[PATH_CHARS], args[PATH_CHARS + 32];
    DWORD n = GetModuleFileNameW(NULL, target, PATH_CHARS);
    if (!n || n >= PATH_CHARS || !from_cwd(folder, destination) || !make_dirs(folder) ||
        !join_path(filename, folder, TITLE ".lnk") ||
        !MultiByteToWideChar(CP_UTF8, 0, filename, -1, path, PATH_CHARS) ||
        !MultiByteToWideChar(CP_UTF8, 0, data_dir, -1, root, PATH_CHARS))
        return complain("Cannot set shortcut paths");
    swprintf(args, PATH_CHARS + 32, L"--data-dir \"%ls\"", root);
    HRESULT started = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(started)) return complain("Cannot initialize Windows shortcuts");
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    HRESULT result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(result)) result = link->lpVtbl->SetPath(link, target);
    if (SUCCEEDED(result)) result = link->lpVtbl->SetArguments(link, args);
    if (SUCCEEDED(result)) result = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(result)) result = file->lpVtbl->Save(file, path, TRUE);
    if (file) file->lpVtbl->Release(file);
    if (link) link->lpVtbl->Release(link);
    CoUninitialize();
    if (FAILED(result)) return complain("Cannot write the shortcut");
    printf("Shortcut: %s\n", filename);
    return LAUNCH_DONE;
}
#else
static void desktop_arg(FILE *f, const char *arg) {
    fputc('"', f);
    for (; *arg; ++arg) {
        if (*arg == '%') fputc('%', f);
        if (*arg == '\\') fputs("\\\\\\", f);
        if (*arg == '"' || *arg == '$' || *arg == 0x60) fputs("\\\\", f);
        fputc(*arg, f);
    }
    fputc('"', f);
}
static int shortcut(const char *destination) {
    char folder[PATH_CHARS], file[PATH_CHARS], target[PATH_CHARS];
    const char *image = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    int appimage = image && *image && appdir && *appdir;
    if (appimage) {
        if (!is_absolute(image) || !exists(image) || is_directory(image) || !copy_path(target, image))
            return complain("Cannot locate the AppImage file");
    } else {
        ssize_t n = readlink("/proc/self/exe", target, sizeof target - 1);
        if (n <= 0 || n == (ssize_t)sizeof target - 1) return complain("Cannot locate the executable");
        target[n] = 0;
        if (strpbrk(target, "\r\n")) return complain("Invalid executable path");
    }
    if (!from_cwd(folder, destination) || !make_dirs(folder) ||
        !join_path(file, folder, "everybodys-golf.desktop")) return complain("Cannot set shortcut paths");
    FILE *f = fopen(file, "wb");
    if (!f) return complain("Cannot create the shortcut");
    fputs("[Desktop Entry]\nType=Application\nName=" TITLE "\nCategories=Game;\nExec=", f);
    desktop_arg(f, target);
    fputs(appimage ? " " : " --data-dir ", f);
    desktop_arg(f, data_dir);
    fputs("\nTerminal=false\n", f);
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok || chmod(file, 0755)) return complain("Cannot write the shortcut");
    printf("Shortcut: %s\n", file);
    return LAUNCH_DONE;
}
#endif
