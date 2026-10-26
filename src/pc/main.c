/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "paths.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/card.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>
#if defined(MELEE_XBOX_UWP)
#define SDL_MAIN_NOIMPL
#endif
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#define MELEE_EXPORT __attribute__((visibility("default")))
#else
#define MELEE_EXPORT
#endif

#include "pc/pc.h"
#include "pc/android_hooks.h"
#include "pc/launcher.h"

#if defined(MELEE_XBOX_UWP)
#include "../../platforms/xbox/storage_api.h"
#endif

int melee_main(void);

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

typedef struct {
    int n;
} MeleeUnwindState;

static _Unwind_Reason_Code melee_unwind_frame(struct _Unwind_Context* ctx, void* arg) {
    MeleeUnwindState* st = (MeleeUnwindState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc == 0 || st->n >= 32) {
        return _URC_END_OF_STACK;
    }
    Dl_info info;
    memset(&info, 0, sizeof info);
    if (dladdr((void*)pc, &info) != 0 && info.dli_fname != NULL) {
        const char* base = strrchr(info.dli_fname, '/');
        __android_log_print(ANDROID_LOG_FATAL, "melee", "  #%02d pc %012zx  %s (%s+%zu)", st->n,
            (size_t)(pc - (uintptr_t)info.dli_fbase), base ? base + 1 : info.dli_fname,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_saddr ? (size_t)(pc - (uintptr_t)info.dli_saddr) : (size_t)0);
    } else {
        __android_log_print(ANDROID_LOG_FATAL, "melee", "  #%02d pc %016zx  ?", st->n, (size_t)pc);
    }
    st->n++;
    return _URC_NO_REASON;
}

static struct sigaction s_prev_sa[NSIG];

static void melee_fatal_signal(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    __android_log_print(ANDROID_LOG_FATAL, "melee", "FATAL signal %d at %p (tid %d)", sig,
        si ? si->si_addr : NULL, (int)gettid());
    MeleeUnwindState st = {0};
    _Unwind_Backtrace(melee_unwind_frame, &st);
    if (sig >= 0 && sig < NSIG) {
        sigaction(sig, &s_prev_sa[sig], NULL);
    } else {
        signal(sig, SIG_DFL);
    }
    raise(sig);
}

static void melee_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = melee_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, &s_prev_sa[sigs[i]]);
    }
}
#else
static void melee_install_crash_handler(void) {}
#endif

static FILE* log_file(void) {
    static FILE* fp;
    static bool tried;
    if (!tried) {
        tried = true;
        const char* path = getenv("MELEE_LOG_FILE");
        static char portable_log[4096];
#if defined(MELEE_XBOX_UWP)
        if (path == NULL) {
            char* directory = SDL_GetPrefPath(NULL, "melee-pc");
#else
        if (path == NULL && pc_is_portable()) {
            char* directory = pc_get_pref_path();
#endif
            if (directory != NULL) {
                snprintf(portable_log, sizeof portable_log, "%smelee-pc.log", directory);
                SDL_free(directory);
                path = portable_log;
            }
        }
#if defined(_WIN32)
        if (path == NULL) {
            path = "melee-pc.log";
        }
#endif
        if (path != NULL && path[0] != '\0') {
            fp = fopen(path, "w");
        }
    }
    return fp;
}

static double log_now_ms(void) {
    static Uint64 t0;
    const Uint64 now = SDL_GetTicksNS();
    if (t0 == 0) {
        t0 = now;
    }
    return (double)(now - t0) / 1e6;
}

#if defined(__APPLE__)
#include <os/log.h>
#endif

void pc_log_line(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const double t = log_now_ms();
#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[Melee] %{public}s", msg);
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Melee", "%s", msg);
#endif
    fprintf(stderr, "[%9.3f] %s\n", t, msg);
    fflush(stderr);
    FILE* lf = log_file();
    if (lf != NULL) {
        fprintf(lf, "[%9.3f] %s\n", t, msg);
        fflush(lf);
    }
}

static void log_callback(
    AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, level >= LOG_ERROR ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
        "[Aurora:%{public}s] %{public}.*s", module, (int)len, message);
#endif
#if defined(__ANDROID__)
    int prio = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG:
        prio = ANDROID_LOG_DEBUG;
        break;
    case LOG_INFO:
        prio = ANDROID_LOG_INFO;
        break;
    case LOG_WARNING:
        prio = ANDROID_LOG_WARN;
        break;
    case LOG_ERROR:
        prio = ANDROID_LOG_ERROR;
        break;
    case LOG_FATAL:
        prio = ANDROID_LOG_FATAL;
        break;
    }
    __android_log_print(prio, "Aurora", "[%s] %.*s", module, (int)len, message);
#else
    static const char* const names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    FILE* out = level >= LOG_ERROR ? stderr : stdout;
    const double t = log_now_ms();
    fprintf(out, "[%9.3f] [%s] %s: %.*s\n", t, names[level], module, (int)len, message);
    fflush(out);
    FILE* lf = log_file();
    if (lf != NULL) {
        fprintf(lf, "[%9.3f] [%s] %s: %.*s\n", t, names[level], module, (int)len, message);
        fflush(lf);
    }
#endif
    if (level == LOG_FATAL) {
        fflush(stdout);
        fflush(stderr);
        abort();
    }
}

#if defined(_WIN32)
#include <windows.h>

#if !defined(MELEE_XBOX_UWP)
static void describe_addr(void* addr, char* out, size_t out_size) {
    char path[MAX_PATH];
    HMODULE mod = NULL;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) &&
        GetModuleFileNameA(mod, path, sizeof(path)) != 0)
    {
        const char* base = strrchr(path, '\\');
        base = base != NULL ? base + 1 : path;
        snprintf(out, out_size, "%p %s+0x%llX", addr, base,
            (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        snprintf(out, out_size, "%p <unknown>", addr);
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(0, 32, frames, NULL);
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;

    FILE* streams[] = {stderr, log_file()};
    for (size_t i = 0; i < sizeof(streams) / sizeof(*streams); i++) {
        FILE* s = streams[i];
        if (s == NULL) {
            continue;
        }
        char where[MAX_PATH + 64];
        describe_addr((void*)rec->ExceptionAddress, where, sizeof(where));
        fprintf(
            s, "[FATAL] crash: exception 0x%08X at %s\n", (unsigned int)rec->ExceptionCode, where);
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            const char* op = "access to";
            if (rec->ExceptionInformation[0] == 0) {
                op = "read from";
            } else if (rec->ExceptionInformation[0] == 1) {
                op = "write to";
            } else if (rec->ExceptionInformation[0] == 8) {
                op = "execute at";
            }
            fprintf(s, "[FATAL] crash: %s address 0x%llX\n", op,
                (unsigned long long)rec->ExceptionInformation[1]);
        }
        for (USHORT f = 0; f < count; f++) {
            describe_addr(frames[f], where, sizeof(where));
            fprintf(s, "[FATAL]   #%02u %s\n", (unsigned)f, where);
        }
        fflush(s);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    pc_log_line("[FATAL] crash: exception 0x%08X at %p",
                (unsigned int)rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        pc_log_line("[FATAL] crash: access kind %llu, address 0x%llX",
                    (unsigned long long)rec->ExceptionInformation[0],
                    (unsigned long long)rec->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
#endif

static void usage(const char* argv0) {
    fprintf(stderr,
        "usage: %s [--no-card] [--dvd] [disc image (iso/gcm/ciso/rvz/...)]\n"
        "       %s --version | --help\n"
        "No disc argument opens the launcher. MELEE_BACKEND, MELEE_VSYNC, MELEE_LOG_FILE,\n"
        "MELEE_DEBUG and the other MELEE_* knobs are documented in README.md.\n",
        argv0, argv0);
    exit(2);
}

#include "pc/input_poll.h"
#include "pc/net.h"
#include "pc/net_match.h"
#include "pc/net_lan.h"
#include <signal.h>

static void pc_shutdown_once(void) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
    pc_net_match_stop();
    pc_net_disconnect();
    pc_lan_stop();
    pc_input_poll_shutdown();
    AXQuit();
    aurora_dvd_close();
    pc_textures_shutdown();
    ARQReset();
    aurora_shutdown();
}

static void pc_on_signal_exit(int sig) {
    pc_exit_requested = true;
    signal(sig, SIG_DFL);
}

static const struct {
    const char* name;
    AuroraBackend backend;
} k_backends[] = {
    {"auto", BACKEND_AUTO},
    {"d3d11", BACKEND_D3D11},
    {"d3d12", BACKEND_D3D12},
    {"metal", BACKEND_METAL},
    {"vulkan", BACKEND_VULKAN},
    {"opengl", BACKEND_OPENGL},
    {"gles", BACKEND_OPENGLES},
    {"webgpu", BACKEND_WEBGPU},
    {"null", BACKEND_NULL},
};

static int ieq(const char* a, const char* b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b;
}

static const char* backend_name(AuroraBackend b) {
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        if (k_backends[i].backend == b) {
            return k_backends[i].name;
        }
    }
    return "?";
}

static AuroraBackend backend_from_env(void) {
    const char* want = getenv("MELEE_BACKEND");
    if (want == NULL || want[0] == '\0') {
#if defined(__APPLE__)
        return BACKEND_METAL;
#else
        return BACKEND_AUTO;
#endif
    }
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        if (ieq(want, k_backends[i].name)) {
            return k_backends[i].backend;
        }
    }
    fprintf(stderr, "MELEE_BACKEND: unknown backend '%s'; valid values are", want);
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(*k_backends); i++) {
        fprintf(stderr, "%s %s", i ? "," : "", k_backends[i].name);
    }
    fprintf(stderr, "\nMELEE_BACKEND: falling back to auto\n");
    return BACKEND_AUTO;
}

static void pc_env_file_bootstrap(void) {
    const char* paths[] = {
#if defined(__ANDROID__)
        "/sdcard/Android/data/dev.melee.game/files/melee-env.txt",
#endif
        "melee-env.txt",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE* f = fopen(paths[i], "r");
        if (f == NULL) {
#if defined(__ANDROID__)
            __android_log_print(
                ANDROID_LOG_INFO, "melee", "env: no %s (errno %d)", paths[i], errno);
#endif
            continue;
        }
        char line[512];
        while (fgets(line, sizeof line, f) != NULL) {
            char* s = line;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s == '#' || *s == '\n' || *s == '\0') {
                continue;
            }
            char* eq = strchr(s, '=');
            if (eq == NULL) {
                continue;
            }
            *eq = '\0';
            char* val = eq + 1;
            size_t n = strlen(val);
            while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ')) {
                val[--n] = '\0';
            }
            SDL_setenv_unsafe(s, val, 0);
        }
        fclose(f);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "melee", "env: applied %s", paths[i]);
#endif
        pc_log_line("env: applied %s", paths[i]);
        return;
    }
}

MELEE_EXPORT int main(int argc, char* argv[]) {
    melee_install_crash_handler();
    pc_env_file_bootstrap();
#if defined(MELEE_XBOX_UWP)
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
#endif
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_handler);
#endif

    OSInit();

    const char* disc = NULL;
    bool card = true;
    for (int i = 1; i < argc; i++) {
        const char* d = NULL;
        if (strcmp(argv[i], "--dvd") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --dvd needs a disc path\n", argv[0]);
                usage(argv[0]);
            }
            d = argv[++i];
        } else if (strcmp(argv[i], "--no-card") == 0) {
            card = false;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("melee-pc %s\n", pc_app_version());
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
        } else {
            d = argv[i];
        }
        if (d != NULL) {
            if (disc != NULL) {
                fprintf(stderr, "%s: more than one disc given (%s, %s)\n", argv[0], disc, d);
                usage(argv[0]);
            }
            const bool uri = strncmp(d, "content://", 10) == 0;
            FILE* f = uri ? NULL : fopen(d, "rb");
            if (d[0] == '\0' || (!uri && f == NULL)) {
                fprintf(stderr, "%s: cannot open disc %s\n", argv[0], d);
#if defined(__ANDROID__)
                continue;
#else
                exit(2);
#endif
            }
            if (f != NULL) {
                fclose(f);
            }
            disc = d;
        }
    }

    static char resources_path[4096];
    const char* base_path = SDL_GetBasePath();
#if defined(__ANDROID__)
    resources_path[0] = '\0';
#else
    snprintf(
        resources_path, sizeof resources_path, "%sresources/", base_path != NULL ? base_path : "");
#endif

#if defined(MELEE_XBOX_UWP)
    char* user_path = SDL_GetPrefPath(NULL, "melee-pc");
#else
    char* user_path = pc_get_pref_path();
#endif
    if (user_path == NULL) {
        fprintf(stderr, "Could not create game data directory: %s\n", SDL_GetError());
        return 1;
    }
#if defined(MELEE_XBOX_UWP)
    pc_log_line("Startup: LocalState ready");
    const char* cache_path = "D:\\developmentfiles\\Melee\\cache";
    if (SDL_CreateDirectory(cache_path)) {
        const char* databases[] = {"pipeline_cache.db", "dawn_cache.db"};
        for (size_t i = 0; i < sizeof databases / sizeof databases[0]; ++i) {
            char source[4096], destination[4096];
            snprintf(source, sizeof source, "%s%s", user_path, databases[i]);
            snprintf(destination, sizeof destination, "%s\\%s", cache_path, databases[i]);
            SDL_PathInfo info;
            if (SDL_GetPathInfo(source, &info) && info.type == SDL_PATHTYPE_FILE &&
                !SDL_GetPathInfo(destination, &info) && !SDL_CopyFile(source, destination))
                SDL_Log("Could not migrate %s: %s", databases[i], SDL_GetError());
        }
    } else {
        SDL_Log("Could not create internal cache folder: %s", SDL_GetError());
    }
#endif
    AuroraConfig config = {
        .appName = getenv("MELEE_WINDOW_TITLE") ? getenv("MELEE_WINDOW_TITLE") : "melee-pc",
        .userPath = user_path,
        .resourcesPath = resources_path[0] != '\0' ? resources_path : NULL,
#if defined(MELEE_XBOX_UWP)
        .cachePath = cache_path,
#else
        .cachePath = getenv("MELEE_CACHE_DIR") ? getenv("MELEE_CACHE_DIR") : user_path,
#endif
        .msaa = 1,
        .maxTextureAnisotropy = 16,
        .vsync = !(getenv("MELEE_VSYNC") && getenv("MELEE_VSYNC")[0] == '0'),
#if defined(__ANDROID__)
        .logLevel = LOG_DEBUG,
#else
        .logLevel = getenv("MELEE_DEBUG") ? LOG_DEBUG : LOG_INFO,
#endif
        .windowWidth = 1280,
        .windowHeight = 960,
        .logCallback = log_callback,
        .desiredBackend = backend_from_env(),
        .mem1Size = PC_MEM1_SIZE,
        .mem2Size = PC_ARAM_SIZE,
    };
    pc_gcadapter_init();
    pc_launcher_configure(&config);

    const AuroraInfo info = aurora_initialize(argc, argv, &config);

    static const char* const k_builtin_mappings[] = {
        "03000000790000004318000010010000,Mayflash GameCube "
        "Adapter,a:b1,b:b0,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b2,y:b3,platform:"
        "Linux,",
        "03000000790000004418000010010000,Mayflash GameCube "
        "Controller,a:b1,b:b0,dpdown:b14,dpleft:b15,dpright:b13,dpup:b12,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b2,y:b3,platform:"
        "Linux,",
        "03000000790000004618000010010000,GameCube "
        "Adapter,a:b1,b:b2,dpdown:b14,dpleft:b15,dpright:b13,dpup:b12,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b0,y:b3,platform:"
        "Linux,",
        "03000000790000004318000000000000,Mayflash GameCube "
        "Adapter,a:b1,b:b2,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,"
        "lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,"
        "start:b9,x:b0,y:b3,platform:Windows,",
        "03000000790000004418000000000000,Mayflash GameCube "
        "Controller,a:b1,b:b2,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,lefttrigger:a3,leftx:"
        "a0,lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b0,y:b3,"
        "platform:Windows,",
        "03000000790000004618000000000000,GameCube "
        "Adapter,a:b1,b:b2,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b0,y:b3,platform:"
        "Windows,",
        "03000000790000004318000000010000,Mayflash GameCube "
        "Adapter,a:b4,b:b0,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,lefttrigger:a12,leftx:a0,"
        "lefty:a4,rightshoulder:b28,righttrigger:a16,rightx:a20,righty:a8,start:b36,x:b8,y:b12,"
        "platform:Mac OS X,",
        "03000000790000004418000000010000,Mayflash GameCube "
        "Controller,a:b1,b:b2,dpdown:b14,dpleft:b15,dpright:b13,dpup:b12,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b0,y:b3,platform:"
        "Mac OS X,",
        "03000000790000004618000000010000,GameCube "
        "Adapter,a:b1,b:b2,dpdown:b14,dpleft:b15,dpright:b13,dpup:b12,lefttrigger:a3,leftx:a0,"
        "lefty:a1,rightshoulder:b7,righttrigger:a4,rightx:a5,righty:a2,start:b9,x:b0,y:b3,platform:"
        "Mac OS X,",
        "6d6179666c617368206c696d69746564,Mayflash GameCube "
        "Adapter,a:b22,b:b21,dpdown:b12,dpleft:b13,dpright:b14,dpup:b11,lefttrigger:b25,leftx:a0,"
        "lefty:a1,rightshoulder:b28,righttrigger:b26,rightx:a5,righty:a2,start:b30,x:b23,y:b24,"
        "platform:Android,",
    };
    for (size_t i = 0; i < sizeof(k_builtin_mappings) / sizeof(k_builtin_mappings[0]); i++) {
        SDL_AddGamepadMapping(k_builtin_mappings[i]);
    }

    int mappings_loaded = SDL_AddGamepadMappingsFromFile("resources/gamecontrollerdb.txt");
    if (mappings_loaded < 0) {
        char res_path[512];
        const char* base = SDL_GetBasePath();
        if (base != NULL) {
            snprintf(res_path, sizeof(res_path), "%sresources/gamecontrollerdb.txt", base);
            mappings_loaded = SDL_AddGamepadMappingsFromFile(res_path);
        }
    }
    if (config.userPath != NULL && config.userPath[0] != '\0') {
        char user_db[512];
        snprintf(user_db, sizeof(user_db), "%s/gamecontrollerdb.txt", config.userPath);
        int user_loaded = SDL_AddGamepadMappingsFromFile(user_db);
        if (user_loaded > 0) {
            mappings_loaded = (mappings_loaded > 0 ? mappings_loaded : 0) + user_loaded;
        }
    }
    if (mappings_loaded > 0) {
        pc_log_line("input: loaded %d gamepad mappings from gamecontrollerdb.txt", mappings_loaded);
    }
    char origin[64];
    if (config.desiredBackend == BACKEND_AUTO) {
        snprintf(origin, sizeof(origin), " (auto)");
    } else if (info.backend == config.desiredBackend) {
        snprintf(origin, sizeof(origin), " (pinned)");
    } else {
        snprintf(
            origin, sizeof(origin), " (fallback from %s)", backend_name(config.desiredBackend));
    }
    pc_log_line("graphics backend: %s%s, adapter: %s [%04x:%04x], driver: %s",
        backend_name(info.backend), origin, info.adapterName, info.adapterVendorId,
        info.adapterDeviceId, info.adapterDriver);
    atexit(pc_shutdown_once);
    signal(SIGINT, pc_on_signal_exit);
    signal(SIGTERM, pc_on_signal_exit);

    pc_android_set_launcher_active(true);
    const int launched = pc_launcher_run(disc, info.window);
    if (launched != 1)
        return launched == 0 ? 0 : 1;
    pc_android_set_launcher_active(false);

#if defined(MELEE_XBOX_UWP)
    CARDSetBasePath(melee_xbox_saves_directory(), -1);
#endif
    pc_menu_init(info.window);
    pc_platform_init();
    aurora_card_set_present(card);
    int rc = melee_main();
    pc_shutdown_once();
    return rc;
}
