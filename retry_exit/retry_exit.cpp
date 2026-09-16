#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook (A3 64-bit and 32-bit), WORLD-style:
 *  If START is held during life update, zero life in that same update so the
 *  native fail/shutter runs immediately (no hold delay).
 *  After that fail (buttons currently held, not a press-edge window):
 *    START still held              -> retry DancePlay (TS 29 / 50)
 *    START + MENU LEFT             -> music select (latched once both held)
 *    START released                -> stock Result
 *  Miss-fail (event 4157 during a life-tick chart): keep playing until the
 *  actor leaves in-play (inner >= 5), then failed Result. After miss-fail,
 *  life-add no longer runs, so Start is polled from song_end for Quick Fail
 *  / retry / select. Hold START during play still Quick Fails immediately.
 *
 * 32-bit: createNext is stdcall (seq, ts) ret 8. song_end uses esi as this.
 * Object offsets differ (life +0x70 / fail +0x158 / play idx +0x72).
 */

#ifdef _WIN64
typedef __int64 (__fastcall *CreateNextFn)(__int64 seq, int ts);
typedef unsigned char (__fastcall *SongEndFn)(__int64 seq);
typedef __int64 (__fastcall *OnUpdateFn)(__int64 seq);
typedef void (__fastcall *ArkIo3Fn)(unsigned int player, char *held, char *trigger);
typedef void (__fastcall *SendEventFn)(__int64 obj, unsigned int ev, __int64 a3, unsigned int a4);
#define INI_NAME L"retry_exit_64bit.ini"
#define LOG_NAME L"retry_exit_64bit.log"
#define LIFE_OFF 0x90
#define LIFE_FLAG 0xD8
#define FAIL_FLAG 0x1C8
#define PLAY_IDX 0x92
#define PLAY_BASE 0x68
#define ACTOR_IDX 0x82
#define ACTOR_BASE 0x58
#else
typedef void *(__stdcall *CreateNextFn)(void *seq, int ts);
typedef int (__thiscall *OnUpdateFn)(void *seq);
typedef void (__stdcall *ArkIo3Fn)(unsigned int player, char *held, char *trigger);
typedef void (__thiscall *SendEventFn)(void *obj, unsigned int ev, int a3, unsigned int a4);
#define INI_NAME L"retry_exit_32bit.ini"
#define LOG_NAME L"retry_exit_32bit.log"
#define LIFE_OFF 0x70
#define LIFE_FLAG 0xB4
#define FAIL_FLAG 0x158
#define PLAY_IDX 0x72
#define PLAY_BASE 0x48
#define ACTOR_IDX 0x6A
#define ACTOR_BASE 0x40
struct RetryThis {
    int dp();
    int mdp();
    void send(unsigned int ev, int a3, unsigned int a4);
};
#endif

#ifdef _WIN64
static const unsigned char kCreatePrologue[] = {
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0x6C, 0x24
};
static const uintptr_t kDefaultCreateNext = 0x26D30;
static const uintptr_t kDefaultSongEnd = 0x3B3E0;
static const uintptr_t kDefaultGetStart = 0x2EE478;
static const uintptr_t kDefaultGetMenuLeft = 0x2EE458;
static const uintptr_t kDefaultIoState = 0x2EEE30;
static const uintptr_t kDefaultDpUpdate = 0x39650;
static const uintptr_t kDefaultMdpUpdate = 0x40B60;
static const uintptr_t kDefaultLifeAdd = 0x549F1;
static const uintptr_t kDefaultSendEvent = 0x18D5D0;
static const unsigned char kLifeAddInsn[] = { 0x01, 0xBB, 0x90, 0x00, 0x00, 0x00 };
#else
static const unsigned char kCreatePrologue[] = {
    /* Stop before `push offset SEH_*` — that immediate is relocated when
     * 32-bit gamemdx loads away from 0x10000000 (A3 used 0x6C080000). */
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF
};
static const uintptr_t kDefaultCreateNext = 0x21A20;
static const uintptr_t kDefaultSongEnd = 0x320A0;
static const uintptr_t kDefaultGetStart = 0x25A47C;
static const uintptr_t kDefaultGetMenuLeft = 0x25A46C;
static const uintptr_t kDefaultIoState = 0x25A964;
static const uintptr_t kDefaultDpUpdate = 0x30670;
static const uintptr_t kDefaultMdpUpdate = 0x369F0;
static const uintptr_t kDefaultLifeAdd = 0x445ED;
static const uintptr_t kDefaultSendEvent = 0x1390D0;
static const unsigned char kLifeAddInsn[] = { 0x01, 0x7E, 0x70, 0x8B, 0x46, 0x70 };
#endif
static const int kDefaultTsAfterSong = 30;
static const int kDefaultTsRetry = 29;
static const int kDefaultTsRetryMatching = 50;
static const int kDefaultTsResult = 31;
static const int kDefaultTsResultMatching = 51;
static const int kDefaultTsAfterUnload = 32;
static const int kDefaultTsSelect = 27;
static const int kDefaultDpPlay = 7;
static const int kDefaultMdpPlay = 11;

static HMODULE g_self;
static uintptr_t g_base;
static CreateNextFn g_orig_create;
#ifdef _WIN64
static SongEndFn g_orig_song_end;
#else
static void *g_orig_song_end;
extern "C" void song_end_detour(void);
extern "C" unsigned char call_orig_song_end(void *fn, void *seq);
#endif
static OnUpdateFn g_orig_dp;
static OnUpdateFn g_orig_mdp;
static SendEventFn g_orig_send;
extern "C" uintptr_t g_life_add_cont = 0;
extern "C" void life_add_cave(void);
extern "C" void apply_quick_fail(uintptr_t gauge);
static volatile LONG g_ready;

static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_verify_prologue = 1;
static uintptr_t g_rva_create = kDefaultCreateNext;
static uintptr_t g_rva_song_end = kDefaultSongEnd;
static uintptr_t g_rva_get_start = kDefaultGetStart;
static uintptr_t g_rva_get_menu_left = kDefaultGetMenuLeft;
static uintptr_t g_rva_io_state = kDefaultIoState;
static uintptr_t g_rva_dp_update = kDefaultDpUpdate;
static uintptr_t g_rva_mdp_update = kDefaultMdpUpdate;
static uintptr_t g_rva_life_add = kDefaultLifeAdd;
static uintptr_t g_rva_send = kDefaultSendEvent;
static int g_ts_after_song = kDefaultTsAfterSong;
static int g_ts_retry = kDefaultTsRetry;
static int g_ts_retry_matching = kDefaultTsRetryMatching;
static int g_ts_result = kDefaultTsResult;
static int g_ts_result_matching = kDefaultTsResultMatching;
static int g_ts_after_unload = kDefaultTsAfterUnload;
static int g_ts_select = kDefaultTsSelect;
static int g_dp_play = kDefaultDpPlay;
static int g_mdp_play = kDefaultMdpPlay;

static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static int g_logged_fail;
static int g_song_ending;
static int g_want_select;
static int g_matching;
static int g_in_matching_play;
static int g_passed_unload;
static int g_block_qf;
static int g_saw_start;
static int g_path; /* 0 none, 1 retry, 2 select, 3 result */
static int g_in_play; /* set by life-add ticks / play-case; cleared on create_next */
static int g_logged_hold_result;
static int g_hold_stageover; /* miss-fail: hold song_end until inner >= 5 */
/* Retry skip-unload: set when post-song/select-chain TS -> DancePlay without
 * unloading. Unload then DancePlay crashes. Result after skip-unload must
 * inject TS 30 first (AFP). Not cleared by finish_ending. */
static int g_skipped_unload;
static int g_result_after_unload;

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
    return (int)GetPrivateProfileIntW(L"retry_exit", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"retry_exit", key, L"", buf, 64, ini);
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

static int ptr_ok(const void *p)
{
    uintptr_t v = (uintptr_t)p;
#ifdef _WIN64
    return v > 0x10000 && v < 0x00007FFFFFFEFFFFULL;
#else
    return v > 0x10000 && v < 0x7FFEFFFFUL;
#endif
}

static int player_held3(ArkIo3Fn fn, unsigned int player, int include_trigger)
{
    char held = 0;
    char trigger = 0;
    if (!fn)
        return 0;
    __try {
        fn(player, &held, &trigger);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (held)
        return 1;
    return include_trigger && trigger;
}

static ArkIo3Fn start_fn(void)
{
    ArkIo3Fn fn;
    if (!g_base || !g_rva_get_start)
        return NULL;
    __try {
        fn = *(ArkIo3Fn *)(g_base + g_rva_get_start);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (!ptr_ok((void *)(uintptr_t)fn))
        return NULL;
    return fn;
}

static ArkIo3Fn menu_left_fn(void)
{
    ArkIo3Fn fn;
    if (!g_base || !g_rva_get_menu_left)
        return NULL;
    __try {
        fn = *(ArkIo3Fn *)(g_base + g_rva_get_menu_left);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (!ptr_ok((void *)(uintptr_t)fn))
        return NULL;
    return fn;
}

static int packed_io_bit(unsigned int player, int bit, int include_trigger)
{
    unsigned char *base;
    unsigned int *slot;
    unsigned int mask;
    if (player >= 2 || !g_base || !g_rva_io_state)
        return 0;
    __try {
        base = *(unsigned char **)(g_base + g_rva_io_state);
        if (!ptr_ok(base))
            return 0;
        slot = (unsigned int *)(base + 1176u * player);
        mask = 1u << bit;
        /* dword2 = held. dword1 = trigger (one frame). dword0 is a sticky
         * press latch and stays set after release, so it cannot be used. */
        if (slot[2] & mask)
            return 1;
        if (include_trigger && (slot[1] & mask))
            return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static int start_down(int include_trigger)
{
    ArkIo3Fn fn = start_fn();
    unsigned int player;
    for (player = 0; player < 2; player++) {
        if (packed_io_bit(player, 0, include_trigger))
            return 1;
        if (fn && player_held3(fn, player, include_trigger))
            return 1;
    }
    return 0;
}

static int menu_left_down(int include_trigger)
{
    ArkIo3Fn fn = menu_left_fn();
    unsigned int player;
    /* Packed bit 1 = arkMDXGetLeft (cab Menu Left). Bit 5 is pad Left. */
    for (player = 0; player < 2; player++) {
        if (packed_io_bit(player, 1, include_trigger))
            return 1;
        if (fn && player_held3(fn, player, include_trigger))
            return 1;
    }
    return 0;
}

/* Held only — path commit. A one-frame Menu Left tap must not flip select. */
static int start_held(void)
{
    return start_down(0);
}

static int menu_left_held(void)
{
    return menu_left_down(0);
}

/* Trigger allowed — Quick Fail can start from a Start tap. */
static int start_for_qf(void)
{
    return start_down(1);
}

static int start_held_now(void)
{
    int held = start_for_qf();
    if (g_block_qf) {
        if (!held)
            g_block_qf = 0;
        else
            return 0;
    }
    return held;
}

/* Update g_path / g_want_select from buttons currently held.
 * Once Start+MenuLeft were held together this fail, select stays latched. */
static void sample_hold_through(void)
{
    int start;
    int left;
    if (!g_logged_fail)
        return;
    start = start_held();
    left = menu_left_held();
    if (start && left) {
        if (!g_want_select)
            log_msg("select latched (START+MENU LEFT held)");
        g_want_select = 1;
        g_saw_start = 1;
        g_path = 2;
    } else if (start) {
        g_saw_start = 1;
        if (g_path != 2)
            g_path = g_want_select ? 2 : 1;
    } else if (g_want_select) {
        /* Select stays latched even if Start is released before TS commit. */
        g_path = 2;
    } else if (g_saw_start && !g_passed_unload) {
        /* Result only before unload begins. After unload for retry is in
         * flight, path stays retry through DancePlay create. */
        g_path = 3;
    }
}

static void consume_start_hold(void)
{
    g_block_qf = 1;
}

static int ts_is_result(int ts)
{
    return ts == g_ts_result || ts == g_ts_result_matching;
}

static int ts_is_after_unload(int ts)
{
    return g_ts_after_unload != 0 && ts == g_ts_after_unload;
}

static int ts_is_danceplay(int ts)
{
    return ts == g_ts_retry || ts == g_ts_retry_matching;
}

static int play_case_is(uintptr_t seq, int play_case)
{
    unsigned short idx;
    int cur;
    __try {
        idx = *(unsigned short *)(seq + PLAY_IDX);
        cur = *(int *)(seq + PLAY_BASE + 8ull * idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return cur == play_case;
}

/* Called from life_add_cave.asm after add [gauge+LIFE_OFF], edi and before
 * the native life<=0 check — same place as WORLD 0x181270BC8.
 * No Win32 calls here (keeps the cave light); path is refined on play update
 * / create_next from buttons currently held. */
extern "C" void apply_quick_fail(uintptr_t gauge)
{
    if (!g_ready || g_song_ending)
        return;
    /* Life gauge ticks only during chart play — mark in-play for 4157 arm. */
    g_in_play = 1;
    if (!start_held_now())
        return;
    __try {
        *(int *)(gauge + LIFE_OFF) = 0;
        *(unsigned char *)(gauge + LIFE_FLAG) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (g_logged_fail)
        return;
    g_logged_fail = 1;
    g_saw_start = 1;
    g_hold_stageover = 0; /* QF must not arm miss-fail hold */
    sample_hold_through();
    if (!g_path)
        g_path = 1;
    log_msg("quick fail (START held, select=%d)", g_want_select);
}

static int actor_state(uintptr_t seq)
{
    unsigned short idx;
    int cur;
    __try {
        idx = *(unsigned short *)(seq + ACTOR_IDX);
        cur = *(int *)(seq + ACTOR_BASE + 8ull * idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return cur;
}

/*
 * 4157 during a life-tick chart arms miss-fail hold. Ignore 4157 while
 * ending a Quick Fail or when no chart life ticks were seen. 4160 is a
 * harmless extra release if it ever fires. Returns 0 to pass through.
 */
static int begin_send_event(unsigned int ev)
{
    if (!g_ready)
        return 0;
    if (ev == 4157 && g_in_play && !g_logged_fail && !g_song_ending) {
        if (!g_logged_hold_result) {
            g_logged_hold_result = 1;
            log_msg("normal fail (life 0, Result after song end)");
        }
        g_hold_stageover = 1;
        return 0;
    }
    if (ev == 4160 && g_hold_stageover) {
        g_hold_stageover = 0;
        log_msg("chart end 4160 (release stageover hold)");
        return 0;
    }
    return 0;
}

#ifdef _WIN64
static void __fastcall detour_send(__int64 obj, unsigned int ev, __int64 a3, unsigned int a4)
{
    int token = begin_send_event(ev);
    if (token < 0)
        return;
    if (g_orig_send)
        g_orig_send(obj, ev, a3, a4);
}
#else
void RetryThis::send(unsigned int ev, int a3, unsigned int a4)
{
    int token = begin_send_event(ev);
    if (token < 0)
        return;
    if (g_orig_send)
        g_orig_send((void *)this, ev, a3, a4);
}

static LPVOID retry_mfn(const void *m, size_t n)
{
    LPVOID p = NULL;
    if (n >= sizeof(p))
        memcpy(&p, m, sizeof(p));
    return p;
}

static LPVOID retry_send_addr(void)
{
    void (RetryThis::*m)(unsigned int, int, unsigned int) = &RetryThis::send;
    return retry_mfn(&m, sizeof(m));
}

static LPVOID retry_dp_addr(void)
{
    int (RetryThis::*m)() = &RetryThis::dp;
    return retry_mfn(&m, sizeof(m));
}

static LPVOID retry_mdp_addr(void)
{
    int (RetryThis::*m)() = &RetryThis::mdp;
    return retry_mfn(&m, sizeof(m));
}
#endif

static unsigned char song_end_after(uintptr_t seq, unsigned char ended)
{
    int inner;
    /* DancePlay case 7 waits for EVERY child to report ended. Zeroing life
     * does not always make that true on the first song, so the chart keeps
     * running with an empty bar. Force ended after this song's Quick Fail.
     * Case 8/9 still run the shutter.
     * After miss-fail, life-add no longer runs so Start QF is polled here.
     * Keep ended=0 while inner is still in-play (<5); release only when
     * inner >= 5. A failed actor_state read keeps the hold (do not release
     * on inner == -1). */
    if (g_ready && g_hold_stageover && !g_logged_fail) {
        if (start_held_now()) {
            g_logged_fail = 1;
            g_saw_start = 1;
            g_hold_stageover = 0;
            sample_hold_through();
            if (!g_path)
                g_path = 1;
            log_msg("quick fail from miss-fail hold (START held, select=%d)",
                    g_want_select);
        }
    }
    if (g_ready && g_logged_fail) {
        /* Only write FAIL_FLAG on a readable play SM — song_end is shared. */
        inner = actor_state(seq);
        if (inner >= 0) {
            __try {
                *(unsigned char *)(seq + FAIL_FLAG) = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ended = 1;
        if (!g_song_ending) {
            g_song_ending = 1;
            g_passed_unload = 0;
            g_matching = g_in_matching_play;
            sample_hold_through();
            log_msg("song ending after quick fail (%s, select=%d)",
                    g_matching ? "matching" : "solo", g_want_select);
        }
        return ended;
    }
    if (g_ready && g_hold_stageover && !g_logged_fail) {
        inner = actor_state(seq);
        if (!ended)
            return ended;
        if (inner >= 0 && inner < 5) {
            ended = 0;
        } else if (inner >= 5) {
            g_hold_stageover = 0;
            log_msg("song end after miss-fail (inner=%d)", inner);
        } else {
            /* SEH / unreadable SM: keep freezing, do not clear the hold. */
            ended = 0;
        }
        return ended;
    }
    return ended;
}

#ifdef _WIN64
static unsigned char __fastcall detour_song_end(__int64 seq)
{
    unsigned char ended;
    if (!g_orig_song_end)
        return 0;
    ended = g_orig_song_end(seq);
    return song_end_after((uintptr_t)seq, ended);
}
#else
extern "C" unsigned char detour_song_end_c(void *seq)
{
    unsigned char ended;
    if (!g_orig_song_end)
        return 0;
    ended = call_orig_song_end(g_orig_song_end, seq);
    return song_end_after((uintptr_t)seq, ended);
}
#endif

static void note_play_update(uintptr_t seq, int play_case, int matching)
{
    int in_play = 0;
    if (g_ready && play_case_is(seq, play_case)) {
        g_in_matching_play = matching;
        in_play = 1;
    }
    if (g_ready) {
        if (in_play) {
            if (!g_in_play) {
                g_logged_hold_result = 0;
                g_hold_stageover = 0;
            }
            g_in_play = 1;
        }
        if (in_play || g_logged_fail || g_song_ending || g_hold_stageover)
            sample_hold_through();
    }
}

#ifdef _WIN64
static __int64 __fastcall detour_dp(__int64 seq)
{
    note_play_update((uintptr_t)seq, g_dp_play, 0);
    if (!g_orig_dp)
        return 0;
    return g_orig_dp(seq);
}

static __int64 __fastcall detour_mdp(__int64 seq)
{
    note_play_update((uintptr_t)seq, g_mdp_play, 1);
    if (!g_orig_mdp)
        return 0;
    return g_orig_mdp(seq);
}
#else
int RetryThis::dp()
{
    note_play_update((uintptr_t)this, g_dp_play, 0);
    if (!g_orig_dp)
        return 0;
    return g_orig_dp((void *)this);
}

int RetryThis::mdp()
{
    note_play_update((uintptr_t)this, g_mdp_play, 1);
    if (!g_orig_mdp)
        return 0;
    return g_orig_mdp((void *)this);
}
#endif

static void finish_ending(void)
{
    g_song_ending = 0;
    g_want_select = 0;
    g_logged_fail = 0;
    g_passed_unload = 0;
    g_saw_start = 0;
    g_path = 0;
    g_in_play = 0;
    g_logged_hold_result = 0;
    g_hold_stageover = 0;
}

#ifdef _WIN64
static __int64 __fastcall detour_create_next(__int64 seq, int ts)
#else
static void *__stdcall detour_create_next(void *seq, int ts)
#endif
{
    if (!g_orig_create)
#ifdef _WIN64
        return 0;
#else
        return NULL;
#endif
    if (g_ready && !(g_song_ending && g_logged_fail) && !g_hold_stageover)
        g_in_play = 0;
    if (g_ready && g_song_ending && g_logged_fail) {
        int orig_ts = ts;

        if (!g_passed_unload) {
            sample_hold_through();
            /* Upgrade retry -> select if Menu Left added during shutter. */
            if (g_path == 1 && g_want_select) {
                g_path = 2;
                log_msg("upgrade retry -> select (START+MENU LEFT) ts=%d", orig_ts);
            }
            if (!g_path)
                g_path = 3;
            log_msg("path=%d ts=%d (1 retry, 2 select, 3 result)", g_path, orig_ts);
        } else if (g_path == 1 && (g_want_select || (start_held() && menu_left_held()))) {
            g_want_select = 1;
            g_path = 2;
            log_msg("upgrade retry -> select (START+MENU LEFT) ts=%d", orig_ts);
        }

        if (g_path == 1) {
            int retry_ts = g_matching ? g_ts_retry_matching : g_ts_retry;
            /* DancePlay retry must skip unload (unload then DancePlay crashes).
             * After N skip-unloads the next TS may be 30/31/32 then SelectMusic
             * 26/27/28; all become DancePlay while packages stay loaded. */
            if (ts != retry_ts) {
                ts = retry_ts;
                log_msg("rewrite TS %d -> %d (retry %s, START held through)",
                        orig_ts, ts, g_matching ? "matching" : "solo");
                g_skipped_unload = 1;
                consume_start_hold();
                finish_ending();
            }
        } else if (g_path == 2) {
            consume_start_hold();
            if (ts == g_ts_after_song) {
                g_passed_unload = 1;
                g_skipped_unload = 0;
                log_msg("exit: pass TS %d (unload, START+LEFTMENU held)", ts);
            } else if (ts_is_result(ts)) {
                if (!g_passed_unload) {
                    ts = g_ts_after_song;
                    g_passed_unload = 1;
                    g_skipped_unload = 0;
                    log_msg("exit: TS %d -> %d (unload before select)", orig_ts, ts);
                } else {
                    ts = g_ts_after_unload;
                    g_skipped_unload = 0;
                    log_msg("exit: TS %d -> %d (skip Result to next-stage)",
                            orig_ts, ts);
                    finish_ending();
                }
            } else if (ts_is_after_unload(ts)) {
                g_skipped_unload = 0;
                log_msg("exit: pass TS %d (stock next to select)", orig_ts);
                finish_ending();
            } else if (ts_is_danceplay(ts)) {
                /* Still requesting play — unload then SelectMusic. */
                if (!g_passed_unload) {
                    ts = g_ts_after_song;
                    g_passed_unload = 1;
                    g_skipped_unload = 0;
                    log_msg("exit: TS %d -> %d (unload before select, was DancePlay)",
                            orig_ts, ts);
                } else {
                    ts = g_ts_select;
                    g_skipped_unload = 0;
                    log_msg("exit: TS %d -> %d (SelectMusic after unload)",
                            orig_ts, ts);
                    finish_ending();
                }
            } else {
                /* SelectMusic / adjacent (26/27/28): already leaving play.
                 * Pass through to select; do not force unload. */
                g_skipped_unload = 0;
                log_msg("exit: pass TS %d (select-bound)", orig_ts);
                finish_ending();
            }
        } else {
            /* Stock Result. Quick-fail often asks for TS 31 first; creating
             * Result without TS 30 crashes AFP. Inject 30, then the follow-up
             * TS 32 becomes Result once. */
            if (ts_is_result(ts) && !g_passed_unload) {
                ts = g_ts_after_song;
                g_passed_unload = 1;
                g_skipped_unload = 0;
                log_msg("result: TS %d -> %d (unload before Result)", orig_ts, ts);
            } else if (ts == g_ts_after_song) {
                g_passed_unload = 1;
                g_skipped_unload = 0;
                log_msg("result: pass TS %d (unload)", ts);
            } else if (ts_is_after_unload(ts) && g_passed_unload) {
                ts = g_matching ? g_ts_result_matching : g_ts_result;
                g_skipped_unload = 0;
                log_msg("result: TS %d -> %d (Result after unload)", orig_ts, ts);
                finish_ending();
            } else if (ts_is_result(ts) && g_passed_unload) {
                g_skipped_unload = 0;
                log_msg("result: pass TS %d", ts);
                finish_ending();
            } else {
                log_msg("result: pass TS %d (stock)", orig_ts);
                finish_ending();
            }
        }
    } else if (g_ready && g_skipped_unload) {
        int orig_ts = ts;
        if (ts_is_result(ts)) {
            ts = g_ts_after_song;
            g_skipped_unload = 0;
            g_result_after_unload = 1;
            log_msg("post-retry Result TS %d -> %d (unload first)", orig_ts, ts);
        } else if (ts == g_ts_after_song) {
            g_skipped_unload = 0;
            log_msg("post-retry native unload TS %d", orig_ts);
        }
    } else if (g_ready && g_result_after_unload) {
        int orig_ts = ts;
        if (ts_is_after_unload(ts)) {
            ts = g_matching ? g_ts_result_matching : g_ts_result;
            g_result_after_unload = 0;
            log_msg("post-retry TS %d -> %d (Result after unload)", orig_ts, ts);
        } else if (ts_is_result(ts)) {
            g_result_after_unload = 0;
            log_msg("post-retry pass Result TS %d", orig_ts);
        }
    }
    return g_orig_create(seq, ts);
}

static int wait_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int match_stable = 0;
    int mismatch_stable = 0;
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            if (!g_verify_prologue) {
                unsigned char b;
                __try {
                    b = *(unsigned char *)(g_base + g_rva_create);
                    (void)b;
                    if (++match_stable >= 3)
                        return 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    match_stable = 0;
                }
            } else if (bytes_match((void *)(g_base + g_rva_create), kCreatePrologue,
                                   sizeof(kCreatePrologue))) {
                mismatch_stable = 0;
                if (++match_stable >= 3)
                    return 1;
            } else {
                match_stable = 0;
                {
                    unsigned char b;
                    int readable = 0;
                    __try {
                        b = *(unsigned char *)(g_base + g_rva_create);
                        (void)b;
                        readable = 1;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        readable = 0;
                    }
                    if (readable) {
                        if (++mismatch_stable >= 10) {
                            unsigned char got[16];
                            size_t n = sizeof(kCreatePrologue);
                            if (n > sizeof(got))
                                n = sizeof(got);
                            memset(got, 0, sizeof(got));
                            __try {
                                memcpy(got, (void *)(g_base + g_rva_create), n);
                            } __except (EXCEPTION_EXECUTE_HANDLER) {
                            }
                            log_msg("prologue mismatch at rva_create_next=0x%X (wrong gamemdx build, not hooking) got %02X %02X %02X %02X %02X %02X %02X %02X",
                                    (unsigned)g_rva_create,
                                    got[0], got[1], got[2], got[3],
                                    got[4], got[5], got[6], got[7]);
                            return 0;
                        }
                    } else {
                        mismatch_stable = 0;
                    }
                }
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
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_rva_create = ini_rva(L"rva_create_next", kDefaultCreateNext);
    g_rva_song_end = ini_rva(L"rva_song_end", kDefaultSongEnd);
    g_rva_get_start = ini_rva(L"rva_get_start", kDefaultGetStart);
    g_rva_get_menu_left = ini_rva(L"rva_get_menu_left", kDefaultGetMenuLeft);
    g_rva_io_state = ini_rva(L"rva_io_state", kDefaultIoState);
    g_rva_dp_update = ini_rva(L"rva_dp_update", kDefaultDpUpdate);
    g_rva_mdp_update = ini_rva(L"rva_mdp_update", kDefaultMdpUpdate);
    g_rva_life_add = ini_rva(L"rva_life_add", kDefaultLifeAdd);
    g_rva_send = ini_rva(L"rva_send_event", kDefaultSendEvent);
    g_ts_after_song = ini_int(L"ts_after_song", kDefaultTsAfterSong);
    g_ts_retry = ini_int(L"ts_retry", kDefaultTsRetry);
    g_ts_retry_matching = ini_int(L"ts_retry_matching", kDefaultTsRetryMatching);
    g_ts_result = ini_int(L"ts_result", kDefaultTsResult);
    g_ts_result_matching = ini_int(L"ts_result_matching", kDefaultTsResultMatching);
    g_ts_after_unload = ini_int(L"ts_after_unload", kDefaultTsAfterUnload);
    g_ts_select = ini_int(L"ts_select", kDefaultTsSelect);
    g_dp_play = ini_int(L"dp_play_case", kDefaultDpPlay);
    g_mdp_play = ini_int(L"mdp_play_case", kDefaultMdpPlay);
    open_log();
    log_msg("retry_exit starting (%s enabled=%d "
            "rva_create=0x%X rva_song_end=0x%X rva_get_start=0x%X rva_get_menu_left=0x%X "
            "rva_dp=0x%X rva_mdp=0x%X rva_life_add=0x%X rva_send=0x%X "
            "ts_after=%d retry=%d/%d result=%d/%d after_unload=%d verify=%d)",
#ifdef _WIN64
            "x64",
#else
            "x86",
#endif
            g_enabled,
            (unsigned)g_rva_create, (unsigned)g_rva_song_end,
            (unsigned)g_rva_get_start, (unsigned)g_rva_get_menu_left,
            (unsigned)g_rva_dp_update, (unsigned)g_rva_mdp_update,
            (unsigned)g_rva_life_add, (unsigned)g_rva_send,
            g_ts_after_song, g_ts_retry, g_ts_retry_matching,
            g_ts_result, g_ts_result_matching, g_ts_after_unload, g_verify_prologue);

    if (!wait_unpacked()) {
        if (g_verify_prologue)
            log_msg("not hooking (timeout or prologue mismatch)");
        else
            log_msg("timeout waiting for gamemdx.dll at rva_create_next=0x%X",
                    (unsigned)g_rva_create);
        return 1;
    }
    Sleep(200);
    log_msg("gamemdx at %p", (void *)g_base);

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
        log_msg("%s: %s", (msg), MH_StatusToString(st)); \
        if (mh_inited) MH_Uninitialize(); \
        return 1; \
    } while (0)

    st = MH_CreateHook((LPVOID)(g_base + g_rva_create), (LPVOID)detour_create_next,
                       (LPVOID *)&g_orig_create);
    if (st != MH_OK)
        FAIL_HOOK("MH_CreateHook createNextSequence");

#ifdef _WIN64
    st = MH_CreateHook((LPVOID)(g_base + g_rva_song_end), (LPVOID)detour_song_end,
                       (LPVOID *)&g_orig_song_end);
#else
    st = MH_CreateHook((LPVOID)(g_base + g_rva_song_end), (LPVOID)song_end_detour,
                       (LPVOID *)&g_orig_song_end);
#endif
    if (st != MH_OK)
        FAIL_HOOK("MH_CreateHook song-end");

    if (g_rva_dp_update) {
#ifdef _WIN64
        st = MH_CreateHook((LPVOID)(g_base + g_rva_dp_update), (LPVOID)detour_dp,
                           (LPVOID *)&g_orig_dp);
#else
        st = MH_CreateHook((LPVOID)(g_base + g_rva_dp_update), retry_dp_addr(),
                           (LPVOID *)&g_orig_dp);
#endif
        if (st != MH_OK)
            FAIL_HOOK("MH_CreateHook DancePlay onUpdate");
    }

    if (g_rva_mdp_update) {
#ifdef _WIN64
        st = MH_CreateHook((LPVOID)(g_base + g_rva_mdp_update), (LPVOID)detour_mdp,
                           (LPVOID *)&g_orig_mdp);
#else
        st = MH_CreateHook((LPVOID)(g_base + g_rva_mdp_update), retry_mdp_addr(),
                           (LPVOID *)&g_orig_mdp);
#endif
        if (st != MH_OK)
            FAIL_HOOK("MH_CreateHook MatchingDancePlay onUpdate");
    }

    if (g_rva_life_add) {
        unsigned char *add_at = (unsigned char *)(g_base + g_rva_life_add);
        if (!bytes_match(add_at, kLifeAddInsn, sizeof(kLifeAddInsn))) {
            log_msg("life-add insn mismatch at rva=0x%X (not hooking Quick Fail)",
                    (unsigned)g_rva_life_add);
        } else {
            g_life_add_cont = g_base + g_rva_life_add + sizeof(kLifeAddInsn);
            st = MH_CreateHook((LPVOID)add_at, (LPVOID)life_add_cave, NULL);
            if (st != MH_OK)
                FAIL_HOOK("MH_CreateHook life-add");
            log_msg("life-add hook at %p (cont %p)", (void *)add_at, (void *)g_life_add_cont);
        }
    }

    if (g_rva_send) {
#ifdef _WIN64
        st = MH_CreateHook((LPVOID)(g_base + g_rva_send), (LPVOID)detour_send,
                           (LPVOID *)&g_orig_send);
#else
        st = MH_CreateHook((LPVOID)(g_base + g_rva_send), retry_send_addr(),
                           (LPVOID *)&g_orig_send);
#endif
        if (st != MH_OK)
            FAIL_HOOK("MH_CreateHook send-event");
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK)
        FAIL_HOOK("MH_EnableHook");

#undef FAIL_HOOK

    InterlockedExchange(&g_ready, 1);
    log_msg("hooks enabled");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        {
            HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
            if (t)
                CloseHandle(t);
        }
    }
    return TRUE;
}
