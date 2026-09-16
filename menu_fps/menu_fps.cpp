#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook:
 *  Fullscreen FPS Target makes the whole app present faster, so frame-stepped
 *  menus run hot. Cap the application tick to menu_fps on cap_scenes
 *  (SelectMusic by default). Use cap_scenes=-1 to cap every valid scene.
 *  Otherwise play / Result / attract / boot keep the high present rate.
 *
 *  x64: menu_fps_64bit.dll + menu_fps_64bit.ini
 *  x86: menu_fps_32bit.dll + menu_fps_32bit.ini
 */

#ifdef _WIN64
typedef __int64 (*TickFn)(void);
#define INI_NAME L"menu_fps_64bit.ini"
#define LOG_NAME L"menu_fps_64bit.log"
static const unsigned char kTickPrologue[] = { 0x48, 0x83, 0xEC, 0x28 };
static const int kDefaultCap[] = { 11 }; /* WORLD SelectMusic */
static const uintptr_t kDefaultTick = 0x1F8830;
static const uintptr_t kDefaultScene = 0x488A30;
#else
typedef int (__cdecl *TickFn)(void);
#define INI_NAME L"menu_fps_32bit.ini"
#define LOG_NAME L"menu_fps_32bit.log"
static const unsigned char kTickPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
static const int kDefaultCap[] = { 10 }; /* A3 SelectMusic */
static const uintptr_t kDefaultTick = 0x109100;
static const uintptr_t kDefaultScene = 0x2549D0;
#endif

static HMODULE g_self;
static uintptr_t g_base;
static TickFn g_orig_tick;
static volatile LONG g_ready;

static int g_log_enabled = 0;
static int g_enabled = 1;
static int g_menu_fps = 60;
static int g_verify_tick_prologue = 1;
static uintptr_t g_rva_tick = kDefaultTick;
static uintptr_t g_rva_scene = kDefaultScene;
static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static LARGE_INTEGER g_qpc_freq;
static LARGE_INTEGER g_next_tick;
static int g_have_next;
static int g_last_scene = -1;
static int g_last_capped = -1;

#define CAP_MAX 16
static int g_cap_scenes[CAP_MAX];
static int g_cap_count;
static int g_cap_all;

static void log_msg(const char *fmt, ...)
{
    if (!g_log_enabled || !g_log)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static void init_paths(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir[0] = 0;
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = 0;
    wcsncpy_s(g_dir, path, _TRUNCATE);
}

static void open_log(void)
{
    if (!g_log_enabled || !g_dir[0])
        return;
    wchar_t log_path[MAX_PATH];
    _snwprintf_s(log_path, _TRUNCATE, L"%s\\" LOG_NAME, g_dir);
    _wfopen_s(&g_log, log_path, L"a");
}

static void ini_path(wchar_t *ini, size_t n)
{
    _snwprintf_s(ini, n, _TRUNCATE, L"%s\\" INI_NAME, g_dir);
}

static int ini_int(const wchar_t *key, int def)
{
    wchar_t ini[MAX_PATH];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    return (int)GetPrivateProfileIntW(L"menu_fps", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"menu_fps", key, L"", buf, 64, ini);
    if (!buf[0])
        return def;
    wchar_t *end = NULL;
    unsigned long v = wcstoul(buf, &end, 0);
    if (end == buf)
        return def;
    return (uintptr_t)v;
}

static int bytes_match(const void *addr, const unsigned char *expect, size_t n)
{
    __try {
        return memcmp(addr, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int current_scene(void)
{
    __try {
        return *(int *)(g_base + g_rva_scene);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void parse_cap_scenes(void)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[128];
#ifdef _WIN64
    const wchar_t *def = L"11";
#else
    const wchar_t *def = L"10";
#endif
    g_cap_count = 0;
    g_cap_all = 0;
    if (g_dir[0]) {
        ini_path(ini, MAX_PATH);
        GetPrivateProfileStringW(L"menu_fps", L"cap_scenes", def, buf, 128, ini);
        wchar_t *p = buf;
        while (*p && g_cap_count < CAP_MAX) {
            long v;
            wchar_t *end = NULL;
            while (*p == L' ' || *p == L'\t' || *p == L',')
                p++;
            if (!*p)
                break;
            v = wcstol(p, &end, 0);
            if (end == p)
                break;
            if (v == -1) {
                g_cap_all = 1;
                g_cap_count = 0;
                break;
            }
            g_cap_scenes[g_cap_count++] = (int)v;
            p = end;
        }
    }
    if (!g_cap_all && g_cap_count == 0) {
        g_cap_count = (int)(sizeof(kDefaultCap) / sizeof(kDefaultCap[0]));
        memcpy(g_cap_scenes, kDefaultCap, sizeof(kDefaultCap));
    }
}

static int scene_capped(int scene)
{
    int i;
    if (g_cap_all)
        return scene >= 0;
    for (i = 0; i < g_cap_count; i++) {
        if (g_cap_scenes[i] == scene)
            return 1;
    }
    return 0;
}

static void cap_menu_tick(void)
{
    if (!g_enabled || g_menu_fps <= 0 || g_qpc_freq.QuadPart <= 0)
        return;

    int scene = current_scene();
    int cap = scene_capped(scene);
    if (scene != g_last_scene) {
        log_msg("scene %d -> %d (%s menu_fps=%d)", g_last_scene, scene,
                cap ? (g_cap_all ? "all" : "cap") : "uncap", g_menu_fps);
        g_last_scene = scene;
        g_have_next = 0;
    }

    if (!cap) {
        g_have_next = 0;
        if (g_last_capped != 0) {
            g_last_capped = 0;
            log_msg("uncapped");
        }
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_have_next) {
        g_next_tick.QuadPart = now.QuadPart + g_qpc_freq.QuadPart / g_menu_fps;
        g_have_next = 1;
        g_last_capped = 1;
        return;
    }

    LONGLONG period = g_qpc_freq.QuadPart / g_menu_fps;
    if (now.QuadPart >= g_next_tick.QuadPart) {
        /* Hitch or first frames after a long stall: don't try to catch up. */
        g_next_tick.QuadPart = now.QuadPart + period;
        return;
    }

    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= g_next_tick.QuadPart)
            break;
        LONGLONG remain = g_next_tick.QuadPart - now.QuadPart;
        if (remain > g_qpc_freq.QuadPart / 1000)
            Sleep(1);
        else
            YieldProcessor();
    }
    g_next_tick.QuadPart += period;
    g_last_capped = 1;
}

#ifdef _WIN64
static __int64 detour_tick(void)
#else
static int __cdecl detour_tick(void)
#endif
{
    if (g_ready)
        cap_menu_tick();
    if (!g_orig_tick)
        return 0;
    return g_orig_tick();
}

static int tick_readable(void)
{
    unsigned char b;
    __try {
        b = *(unsigned char *)(g_base + g_rva_tick);
        (void)b;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int wait_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int stable = 0;
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            int ok;
            if (g_verify_tick_prologue)
                ok = bytes_match((void *)(g_base + g_rva_tick), kTickPrologue, sizeof(kTickPrologue));
            else
                ok = tick_readable();
            if (ok) {
                if (++stable >= 3)
                    return 1;
            } else {
                stable = 0;
            }
        }
        Sleep(50);
    }
    return 0;
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;
    int mh_inited = 0;
    InitializeCriticalSection(&g_log_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 0);
    g_enabled = ini_int(L"enabled", 1);
    g_menu_fps = ini_int(L"menu_fps", 60);
    g_verify_tick_prologue = ini_int(L"verify_tick_prologue", 1);
    g_rva_tick = ini_rva(L"rva_tick", kDefaultTick);
    g_rva_scene = ini_rva(L"rva_scene", kDefaultScene);
    parse_cap_scenes();
    if (g_menu_fps < 1)
        g_menu_fps = 1;
    if (g_menu_fps > 360)
        g_menu_fps = 360;
    open_log();
    QueryPerformanceFrequency(&g_qpc_freq);
    {
        char list[128];
        int n = 0;
        int i;
        list[0] = 0;
        if (g_cap_all) {
            _snprintf_s(list, sizeof(list), _TRUNCATE, "-1 (all)");
        } else {
            for (i = 0; i < g_cap_count; i++) {
                int w = _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, "%s%d",
                                    i ? "," : "", g_cap_scenes[i]);
                if (w > 0)
                    n += w;
            }
        }
        log_msg("menu_fps starting (%s enabled=%d menu_fps=%d rva_tick=0x%X rva_scene=0x%X verify_prologue=%d cap_scenes=%s)",
#ifdef _WIN64
                "x64",
#else
                "x86",
#endif
                g_enabled, g_menu_fps, (unsigned)g_rva_tick, (unsigned)g_rva_scene,
                g_verify_tick_prologue, list[0] ? list : "(none)");
    }

    if (!wait_unpacked()) {
        log_msg("timeout waiting for gamemdx.dll at rva_tick=0x%X (try verify_tick_prologue=0 after an update)",
                (unsigned)g_rva_tick);
        return 1;
    }
    Sleep(200);
    log_msg("gamemdx unpacked at %p", (void *)g_base);

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }
    mh_inited = 1;
#define FAIL_HOOK(msg) do { \
        log_msg("FAIL_HOOK: %s", (msg)); \
        InterlockedExchange(&g_ready, 0); \
        if (mh_inited) MH_Uninitialize(); \
        mh_inited = 0; \
        return 1; \
    } while (0)

    st = MH_CreateHook((LPVOID)(g_base + g_rva_tick), (LPVOID)detour_tick, (LPVOID *)&g_orig_tick);
    if (st != MH_OK) {
        log_msg("MH_CreateHook tick: %s", MH_StatusToString(st));
        FAIL_HOOK("tick hook failed");
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        FAIL_HOOK("MH_EnableHook failed");
    }
#undef FAIL_HOOK

    InterlockedExchange(&g_ready, 1);
    log_msg("hook enabled on Application tick RVA 0x%X", (unsigned)g_rva_tick);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
