#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook (WORLD/A3 64-bit + A3 32-bit / Whisky+Wine):
 *  Drive gamemdx's high-precision song clock (system+0x1268) from QPC
 *  (optional WASAPI play cursor) while a chart is in play, so judgment and
 *  scroll stay locked through BPM changes and stops.
 *
 *  Stock path: clock update writes AVS high-precision tick into system+0x1268.
 *  GamePlayActor song time =
 *      (uint32)[sys+0x1268] - [actor SOUND_OFFSET] - [actor begin]
 *      + Work::GetTickCount().
 *  Clock units match SOUND_OFFSET (milliseconds).
 *
 *  Does not touch BPL pre-start music-sync waits. Override only after the
 *  DancePlay / MatchingDancePlay sequence is in-chart.
 *
 *  SOUND OFFSET in GAME OPTIONS remains the constant latency knob.
 *  x64: WORLD vs A3 auto-detected by probing known RVAs (prologues match).
 *  x86: A3 2024040200 only.
 */

#ifdef _WIN64
#define INI_NAME L"sync_lock_64bit.ini"
#define LOG_NAME L"sync_lock_64bit.log"

typedef __int64 (__fastcall *ClockUpdateFn)(void);
typedef __int64 (__fastcall *OnUpdateFn)(__int64 seq);

static const unsigned char kClockPrologue[] = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
};
static const unsigned char kDpPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const unsigned char kMdpPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};

/* WORLD 2025080500 */
static const uintptr_t kDefaultClockUpdate = 0x22D40;
static const uintptr_t kDefaultDp = 0x549D0;
static const uintptr_t kDefaultMdp = 0x5E780;
static const uintptr_t kDefaultScene = 0x488A30;
static const uintptr_t kDefaultSystem = 0x6B5A80;
/* A3 2024040200 x64 */
static const uintptr_t kA3ClockUpdate = 0x1B670;
static const uintptr_t kA3Dp = 0x39650;
static const uintptr_t kA3Mdp = 0x40B60;
static const uintptr_t kA3Scene = 0x2E8550;
static const uintptr_t kA3System = 0x2EEE30;
#define SEQ_IDX 146
#define SEQ_BASE 104
#else
#define INI_NAME L"sync_lock_32bit.ini"
#define LOG_NAME L"sync_lock_32bit.log"

typedef __int64 (__cdecl *ClockUpdateFn)(void);
typedef int (__thiscall *OnUpdateFn)(void *seq);

/* Stop before relocated immediates (load away from preferred base). */
static const unsigned char kClockPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C, 0x53, 0x56, 0x57
};
static const unsigned char kDpPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF
};
static const unsigned char kMdpPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF
};

/* A3 2024040200 x86 */
static const uintptr_t kDefaultClockUpdate = 0x172E0;
static const uintptr_t kDefaultDp = 0x30670;
static const uintptr_t kDefaultMdp = 0x369F0;
static const uintptr_t kDefaultScene = 0x2549D0;
static const uintptr_t kDefaultSystem = 0x25A964;
#define SEQ_IDX 0x72
#define SEQ_BASE 0x48

struct SyncThis {
    int dp();
    int mdp();
};
#endif

static const uintptr_t kOffClock = 0x1268;

enum {
    SRC_AUTO = 0,
    SRC_QPC = 1,
    SRC_WASAPI = 2,
    SRC_XACT = 3
};

static HMODULE g_self;
static uintptr_t g_base;
static ClockUpdateFn g_orig_clock;
static OnUpdateFn g_orig_dp;
static OnUpdateFn g_orig_mdp;
static volatile LONG g_ready;

static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_verify_prologue = 1;
static int g_is_a3;
static int g_source = SRC_AUTO;
static int g_latency_ms;
static int g_tick_rate = 1000;
static int g_log_interval_ms = 2000;
static int g_period1 = 1;
static uintptr_t g_rva_clock = kDefaultClockUpdate;
static uintptr_t g_rva_dp = kDefaultDp;
static uintptr_t g_rva_mdp = kDefaultMdp;
static uintptr_t g_rva_scene = kDefaultScene;
static uintptr_t g_rva_system = kDefaultSystem;
static uintptr_t g_off_clock = kOffClock;

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_clock_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static LARGE_INTEGER g_qpc_freq;
static volatile LONG g_in_chart;
static volatile LONG g_armed;
static LONGLONG g_anchor_qpc;
static int64_t g_anchor_tick;
static int64_t g_last_tick;
static int g_active_source;
static DWORD g_last_log_tick;
static int64_t g_last_game_tick;
static int64_t g_last_ours_tick;

/* Optional WASAPI (best-effort; often unavailable under Wine). */
typedef HRESULT (STDMETHODCALLTYPE *AudioClock_GetPositionFn)(
    void *thisptr, UINT64 *pos, UINT64 *qpctime);
typedef HRESULT (STDMETHODCALLTYPE *AudioClock_GetFrequencyFn)(
    void *thisptr, UINT64 *freq);
static void *g_audio_clock;
static AudioClock_GetPositionFn g_ac_getpos;
static AudioClock_GetFrequencyFn g_ac_getfreq;
static UINT64 g_ac_freq;
static UINT64 g_ac_anchor_pos;
static int g_ac_ok;

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
    return (int)GetPrivateProfileIntW(L"sync_lock", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"sync_lock", key, L"", buf, 64, ini);
    if (!buf[0])
        return def;
    wchar_t *end = NULL;
    unsigned long v = wcstoul(buf, &end, 0);
    if (end == buf)
        return def;
    return (uintptr_t)v;
}

static int ini_source(void)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[32];
    if (!g_dir[0])
        return SRC_AUTO;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"sync_lock", L"source", L"auto", buf, 32, ini);
    if (!_wcsicmp(buf, L"qpc"))
        return SRC_QPC;
    if (!_wcsicmp(buf, L"wasapi"))
        return SRC_WASAPI;
    if (!_wcsicmp(buf, L"xact"))
        return SRC_XACT;
    return SRC_AUTO;
}

static const char *source_name(int s)
{
    switch (s) {
    case SRC_QPC: return "qpc";
    case SRC_WASAPI: return "wasapi";
    case SRC_XACT: return "xact";
    default: return "auto";
    }
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

static int seq_case(uintptr_t seq)
{
    __try {
        unsigned short idx = *(unsigned short *)(seq + SEQ_IDX);
        return *(int *)(seq + SEQ_BASE + 8 * (int)idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static uintptr_t system_obj(void)
{
    __try {
        return *(uintptr_t *)(g_base + g_rva_system);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int64_t read_game_tick(void)
{
    uintptr_t sys = system_obj();
    if (!sys)
        return 0;
    __try {
        return (int64_t)*(uint64_t *)(sys + g_off_clock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void write_game_tick(int64_t tick)
{
    uintptr_t sys = system_obj();
    if (!sys)
        return;
    __try {
        *(uint64_t *)(sys + g_off_clock) = (uint64_t)tick;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int64_t qpc_elapsed_ms(void)
{
    LARGE_INTEGER now;
    LONGLONG delta;
    if (g_qpc_freq.QuadPart <= 0 || g_anchor_qpc <= 0)
        return 0;
    QueryPerformanceCounter(&now);
    delta = now.QuadPart - g_anchor_qpc;
    if (delta < 0)
        delta = 0;
    /* tick_rate units per second (1000 = ms). */
    return (int64_t)((delta * (LONGLONG)g_tick_rate) / g_qpc_freq.QuadPart);
}

static int wasapi_elapsed_ms(int64_t *out_ms)
{
    UINT64 pos = 0;
    UINT64 qpc = 0;
    HRESULT hr;
    if (!g_ac_ok || !g_audio_clock || !g_ac_getpos || g_ac_freq == 0)
        return 0;
    hr = g_ac_getpos(g_audio_clock, &pos, &qpc);
    if (FAILED(hr))
        return 0;
    if (pos < g_ac_anchor_pos)
        return 0;
    *out_ms = (int64_t)(((pos - g_ac_anchor_pos) * (UINT64)g_tick_rate) / g_ac_freq);
    return 1;
}

static void disarm_clock(const char *why)
{
    if (!g_armed)
        return;
    EnterCriticalSection(&g_clock_cs);
    InterlockedExchange(&g_armed, 0);
    g_anchor_qpc = 0;
    g_anchor_tick = 0;
    g_last_tick = 0;
    g_ac_ok = 0;
    LeaveCriticalSection(&g_clock_cs);
    log_msg("disarm (%s)", why ? why : "?");
}

static void arm_clock(void)
{
    LARGE_INTEGER now;
    int64_t tick;
    EnterCriticalSection(&g_clock_cs);
    QueryPerformanceCounter(&now);
    tick = read_game_tick();
    g_anchor_qpc = now.QuadPart;
    g_anchor_tick = tick;
    g_last_tick = tick;
    g_ac_ok = 0;
    g_active_source = SRC_QPC;

    if ((g_source == SRC_WASAPI || g_source == SRC_AUTO) &&
        g_audio_clock && g_ac_getpos && g_ac_getfreq) {
        UINT64 freq = 0;
        UINT64 pos = 0;
        UINT64 qpc = 0;
        if (SUCCEEDED(g_ac_getfreq(g_audio_clock, &freq)) && freq != 0 &&
            SUCCEEDED(g_ac_getpos(g_audio_clock, &pos, &qpc))) {
            g_ac_freq = freq;
            g_ac_anchor_pos = pos;
            g_ac_ok = 1;
            g_active_source = SRC_WASAPI;
        }
    }

    if (g_source == SRC_XACT)
        log_msg("source=xact requested; no stable cue pointer under Wine - using qpc");

    InterlockedExchange(&g_armed, 1);
    LeaveCriticalSection(&g_clock_cs);
    log_msg("arm tick=%lld scene=%d src=%s latency_ms=%d tick_rate=%d",
            (long long)tick, current_scene(), source_name(g_active_source),
            g_latency_ms, g_tick_rate);
}

static void apply_locked_tick(void)
{
    int64_t elapsed;
    int64_t desired;
    int64_t game;
    DWORD now;
    int used_wasapi = 0;

    if (!g_armed || !g_enabled)
        return;

    EnterCriticalSection(&g_clock_cs);
    if (!g_armed) {
        LeaveCriticalSection(&g_clock_cs);
        return;
    }

    elapsed = qpc_elapsed_ms();
    if (g_ac_ok) {
        int64_t audio_ms = 0;
        if (wasapi_elapsed_ms(&audio_ms)) {
            elapsed = audio_ms;
            used_wasapi = 1;
            g_active_source = SRC_WASAPI;
        } else {
            g_active_source = SRC_QPC;
        }
    }

    desired = g_anchor_tick + elapsed - (int64_t)g_latency_ms;
    if (desired < g_last_tick)
        desired = g_last_tick;
    /* Stock AVS tick left by orig clock update - compare before overwrite. */
    game = read_game_tick();
    g_last_tick = desired;
    write_game_tick(desired);
    LeaveCriticalSection(&g_clock_cs);

    now = GetTickCount();
    if (g_log_interval_ms > 0 &&
        (g_last_log_tick == 0 || now - g_last_log_tick >= (DWORD)g_log_interval_ms)) {
        g_last_log_tick = now;
        log_msg("lock game=%lld ours=%lld delta=%lld elapsed=%lld src=%s%s",
                (long long)game, (long long)desired,
                (long long)(desired - game), (long long)elapsed,
                source_name(g_active_source),
                used_wasapi ? "" : "");
        g_last_game_tick = game;
        g_last_ours_tick = desired;
    }
}

static void apply_seq_state(int st, int play_case, int reset_hi)
{
    LONG was;
    if (st < 0)
        return;
    if (st <= reset_hi) {
        was = InterlockedExchange(&g_in_chart, 0);
        if (was)
            disarm_clock("seq reset");
        return;
    }
    /* Only lock after begin-tick (DP case 7 / MDP case 11), not ready/holds. */
    if (st == play_case) {
        was = InterlockedCompareExchange(&g_in_chart, 1, 0);
        if (was == 0 || !g_armed)
            arm_clock();
        return;
    }
    if (st > play_case) {
        was = InterlockedExchange(&g_in_chart, 0);
        if (was)
            disarm_clock("seq leave play");
    }
}

#ifdef _WIN64
static __int64 __fastcall detour_clock(void)
{
    __int64 ret = 0;
    if (g_orig_clock)
        ret = g_orig_clock();
    if (g_ready && g_in_chart)
        apply_locked_tick();
    return ret;
}

static __int64 __fastcall detour_dp(__int64 seq)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 7, 1);
    return g_orig_dp(seq);
}

static __int64 __fastcall detour_mdp(__int64 seq)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 11, 2);
    return g_orig_mdp(seq);
}
#else
static __int64 __cdecl detour_clock(void)
{
    __int64 ret = 0;
    if (g_orig_clock)
        ret = g_orig_clock();
    if (g_ready && g_in_chart)
        apply_locked_tick();
    return ret;
}

int SyncThis::dp()
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)this), 7, 1);
    if (!g_orig_dp)
        return 0;
    return g_orig_dp((void *)this);
}

int SyncThis::mdp()
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)this), 11, 2);
    if (!g_orig_mdp)
        return 0;
    return g_orig_mdp((void *)this);
}

static LPVOID sync_mfn(const void *m, size_t n)
{
    LPVOID p = NULL;
    if (n >= sizeof(p))
        memcpy(&p, m, sizeof(p));
    return p;
}

static LPVOID sync_dp_addr(void)
{
    int (SyncThis::*m)() = &SyncThis::dp;
    return sync_mfn(&m, sizeof(m));
}

static LPVOID sync_mdp_addr(void)
{
    int (SyncThis::*m)() = &SyncThis::mdp;
    return sync_mfn(&m, sizeof(m));
}
#endif

static int hooks_match(uintptr_t clock, uintptr_t dp, uintptr_t mdp)
{
    return bytes_match((void *)(g_base + clock), kClockPrologue, sizeof(kClockPrologue)) &&
           bytes_match((void *)(g_base + dp), kDpPrologue, sizeof(kDpPrologue)) &&
           bytes_match((void *)(g_base + mdp), kMdpPrologue, sizeof(kMdpPrologue));
}

#ifdef _WIN64
static void apply_world_64_layout(int switch_rvas)
{
    g_is_a3 = 0;
    if (switch_rvas) {
        g_rva_clock = kDefaultClockUpdate;
        g_rva_dp = kDefaultDp;
        g_rva_mdp = kDefaultMdp;
        g_rva_scene = kDefaultScene;
        g_rva_system = kDefaultSystem;
    }
}

static void apply_a3_64_layout(int switch_rvas)
{
    g_is_a3 = 1;
    if (switch_rvas) {
        g_rva_clock = kA3ClockUpdate;
        g_rva_dp = kA3Dp;
        g_rva_mdp = kA3Mdp;
        g_rva_scene = kA3Scene;
        g_rva_system = kA3System;
    }
}

static int detect_64_build(void)
{
    /* Prefer INI RVAs when their prologues already match. */
    if (hooks_match(g_rva_clock, g_rva_dp, g_rva_mdp)) {
        if (g_rva_clock == kA3ClockUpdate || g_rva_dp == kA3Dp)
            apply_a3_64_layout(0);
        else if (g_rva_clock == kDefaultClockUpdate || g_rva_dp == kDefaultDp)
            apply_world_64_layout(0);
        else {
            /* Custom INI that still matches - keep RVAs; guess build by system. */
            if (g_rva_system == kA3System)
                g_is_a3 = 1;
            else
                g_is_a3 = 0;
        }
        return 1;
    }
    if (hooks_match(kA3ClockUpdate, kA3Dp, kA3Mdp)) {
        apply_a3_64_layout(1);
        return 1;
    }
    if (hooks_match(kDefaultClockUpdate, kDefaultDp, kDefaultMdp)) {
        apply_world_64_layout(1);
        return 1;
    }
    return 0;
}
#else
static void apply_a3_32_layout(int switch_rvas)
{
    g_is_a3 = 1;
    if (switch_rvas) {
        g_rva_clock = kDefaultClockUpdate;
        g_rva_dp = kDefaultDp;
        g_rva_mdp = kDefaultMdp;
        g_rva_scene = kDefaultScene;
        g_rva_system = kDefaultSystem;
    }
}

static int detect_32_build(void)
{
    if (hooks_match(g_rva_clock, g_rva_dp, g_rva_mdp)) {
        apply_a3_32_layout(0);
        return 1;
    }
    if (hooks_match(kDefaultClockUpdate, kDefaultDp, kDefaultMdp)) {
        apply_a3_32_layout(1);
        return 1;
    }
    return 0;
}
#endif

static int wait_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int stable = 0;
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
#ifdef _WIN64
            int ok = detect_64_build();
#else
            int ok = detect_32_build();
#endif
            if (!ok && !g_verify_prologue)
                ok = 1; /* keep INI RVAs; user disabled prologue checks */
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

static void try_find_wasapi_clock(void)
{
    /*
     * WASAPI IAudioClock is not reliably reachable from gamemdx under Wine.
     * Leave hooks unset unless a future Spice helper exports a clock pointer.
     * source=wasapi will log and fall back to QPC.
     */
    g_audio_clock = NULL;
    g_ac_getpos = NULL;
    g_ac_getfreq = NULL;
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;
    int mh_inited = 0;
#ifdef _WIN64
    const char *arch = "x64";
#else
    const char *arch = "x86";
#endif

    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_clock_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_enabled = ini_int(L"enabled", 1);
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_source = ini_source();
    g_latency_ms = ini_int(L"latency_ms", 0);
    g_tick_rate = ini_int(L"tick_rate", 1000);
    g_log_interval_ms = ini_int(L"log_interval_ms", 2000);
    g_period1 = ini_int(L"time_period_1", 1);
    g_rva_clock = ini_rva(L"rva_clock_update", kDefaultClockUpdate);
    g_rva_dp = ini_rva(L"rva_dp_update", kDefaultDp);
    g_rva_mdp = ini_rva(L"rva_mdp_update", kDefaultMdp);
    g_rva_scene = ini_rva(L"rva_scene", kDefaultScene);
    g_rva_system = ini_rva(L"rva_system", kDefaultSystem);
    g_off_clock = ini_rva(L"off_clock", kOffClock);
    if (g_tick_rate < 1)
        g_tick_rate = 1000;
    if (g_latency_ms < -500)
        g_latency_ms = -500;
    if (g_latency_ms > 500)
        g_latency_ms = 500;

    open_log();
    QueryPerformanceFrequency(&g_qpc_freq);
    if (g_period1)
        timeBeginPeriod(1);

    log_msg("sync_lock starting (%s enabled=%d source=%s latency_ms=%d "
            "tick_rate=%d rva_clock=0x%X rva_dp=0x%X rva_mdp=0x%X rva_system=0x%X "
            "off_clock=0x%X verify_prologue=%d)",
            arch, g_enabled, source_name(g_source), g_latency_ms, g_tick_rate,
            (unsigned)g_rva_clock, (unsigned)g_rva_dp, (unsigned)g_rva_mdp,
            (unsigned)g_rva_system, (unsigned)g_off_clock, g_verify_prologue);

    if (!wait_unpacked()) {
        log_msg("timeout waiting for gamemdx.dll (wrong build or verify_prologue mismatch)");
        return 1;
    }
    Sleep(200);
#ifdef _WIN64
    log_msg("gamemdx at %p (%s) rva_clock=0x%X rva_dp=0x%X rva_mdp=0x%X "
            "rva_scene=0x%X rva_system=0x%X",
            (void *)g_base,
            g_is_a3 ? "A3 2024040200 x64" : "WORLD 2025080500 x64",
            (unsigned)g_rva_clock, (unsigned)g_rva_dp, (unsigned)g_rva_mdp,
            (unsigned)g_rva_scene, (unsigned)g_rva_system);
#else
    log_msg("gamemdx at %p (A3 2024040200 x86) rva_clock=0x%X rva_dp=0x%X "
            "rva_mdp=0x%X rva_scene=0x%X rva_system=0x%X",
            (void *)g_base,
            (unsigned)g_rva_clock, (unsigned)g_rva_dp, (unsigned)g_rva_mdp,
            (unsigned)g_rva_scene, (unsigned)g_rva_system);
#endif

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    try_find_wasapi_clock();
    if (g_source == SRC_WASAPI && !g_audio_clock)
        log_msg("wasapi clock unavailable - will use qpc while in chart");
    if (g_source == SRC_XACT)
        log_msg("xact cue lock not wired on Wine; qpc will be used");

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

    st = MH_CreateHook((LPVOID)(g_base + g_rva_clock), (LPVOID)detour_clock,
                       (LPVOID *)&g_orig_clock);
    if (st != MH_OK) {
        log_msg("MH_CreateHook clock: %s", MH_StatusToString(st));
        FAIL_HOOK("clock hook failed");
    }

#ifdef _WIN64
    st = MH_CreateHook((LPVOID)(g_base + g_rva_dp), (LPVOID)detour_dp,
                       (LPVOID *)&g_orig_dp);
#else
    st = MH_CreateHook((LPVOID)(g_base + g_rva_dp), sync_dp_addr(),
                       (LPVOID *)&g_orig_dp);
#endif
    if (st != MH_OK) {
        log_msg("MH_CreateHook dp: %s", MH_StatusToString(st));
        FAIL_HOOK("dp hook failed");
    }

#ifdef _WIN64
    st = MH_CreateHook((LPVOID)(g_base + g_rva_mdp), (LPVOID)detour_mdp,
                       (LPVOID *)&g_orig_mdp);
#else
    st = MH_CreateHook((LPVOID)(g_base + g_rva_mdp), sync_mdp_addr(),
                       (LPVOID *)&g_orig_mdp);
#endif
    if (st != MH_OK) {
        log_msg("MH_CreateHook mdp: %s", MH_StatusToString(st));
        FAIL_HOOK("mdp hook failed");
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        FAIL_HOOK("MH_EnableHook failed");
    }
#undef FAIL_HOOK

    InterlockedExchange(&g_ready, 1);
    log_msg("hooks enabled (clock+DancePlay+MatchingDancePlay)");
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
