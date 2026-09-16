#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook. Does not touch EVENT MODE.
 *  x64: WORLD 2025080500 and A3 2024040200 (prologue auto-detect).
 *  x86: A3 2024040200 (modules32a3).
 *
 * In-chart D3D9 overlay (translucent black panel, grayscale-AA text).
 *
 * Line 1: EX: -X  Max: Xms  Abs(mu): X.Xms  mu: +/-X.Xms
 *         Missing EX so far, worst |ms|, mean |ms|, signed mean ms.
 *         mu is hit_tick - note_tick. Positive = late / Slow.
 *         The official EVENT MODE hack negates this; we do not.
 * Line 2: sigma:X.X  w32:abs/sigma  F:n/S:n  last:+/-Xms  try +/-N
 *         Overall sd, last-32 window abs-mean/sd, fast/slow counts,
 *         most recent hit ms, suggested offset (negated mean).
 * Line 3: lamp  M:n P:n GR:n GD:n Mi:n OK:n NG:n
 *         MFC/PFC/GFC/FC or * if a miss/NG. Judge totals.
 *         OK/NG are freeze and shock (judge 6 / 7). Miss is 5.
 * Line 4: EX n/n (x.x%)  proj n  judged/total left:n
 *         EX vs EX-possible-so-far, PFC projection, notes remaining.
 * Line 5: Mst:n  cb:n/n  nps:n  life:n  die:nGR
 *         Marvelous streak, combo/max, notes in the last second,
 *         life; die:nGR only on LIFE4 when death is that close.
 * Line 6: L/D/U/R:+ms/sd   (doubles: L1..R1 and L2..R2)
 *         Per-column mean and sd. Omitted until a timed tap exists.
 * Line 7: |==eElL<>| sparkline of the last 32 timed hits
 *         = in window, e/l early-late, E/L bigger, </> off-window.
 * Line 8: vs 2P EX:+/-n  lamp/lamp
 *         2P gap only when both sides are playing.
 *
 * Shown only on play scenes while the play sequence is ticking in-chart.
 * WORLD: 4 (DancePlay) and 12 (MatchingDancePlay / BPL).
 * A3:    4 (DancePlay) and 11 (MatchingDancePlay).
 * Any other scene (select, result, test menu) hides and resets immediately.
 *
 * 1P: bottom-right, lines grow left/up.  2P: bottom-left, grow right/up.
 * The block is never shifted to the other side to make room.
 *
 * Hooks (RVAs from imagebase; 64-bit picks WORLD vs A3 by judge prologue):
 *   GamePlayActor judge dispatcher
 *   DancePlaySequence::onUpdate
 *   MatchingDancePlaySequence::onUpdate (BPL)
 *   IDirect3DDevice9::EndScene      (shared d3d9 vtable)
 */

#ifdef _WIN64
#define INI_NAME L"stats_overlay_64bit.ini"
#define LOG_NAME L"stats_overlay_64bit.log"
#define PLAYER_OFF 0x84
#define NOTE_JUDGE 12
#define SEQ_IDX 146
#define SEQ_BASE 104
#define SLOT_HIT 2
#else
#define INI_NAME L"stats_overlay_32bit.ini"
#define LOG_NAME L"stats_overlay_32bit.log"
#define PLAYER_OFF 0x6C
#define NOTE_JUDGE 8
#define SEQ_IDX 0x72
#define SEQ_BASE 0x48
#define SLOT_HIT 1
#endif
#define ROLL_N 32
#define COLS 8
#define TEX_W 560
#define TEX_H 256
#define TEXT_MAX 1024
#define PLAY_MAX 8
#define D3D9_ENDSCENE 42
#define SEQ_STALE_MS 80
#define TEX_PAD 8
#define BOX_PAD 8

enum {
    J_MARV = 0,
    J_PERF = 1,
    J_GREAT = 2,
    J_GOOD = 3,
    J_MISS = 5,
    J_OK = 6,   /* freeze / shock OK */
    J_NG = 7    /* freeze / shock NG */
};

#ifdef _WIN64
typedef __int64 (__fastcall *JudgeFn)(__int64 actor, unsigned int *slot,
                                      unsigned int ev, __int64 payload);
typedef __int64 (__fastcall *OnUpdateFn)(__int64 seq);
#else
/* thiscall: ecx=this. Extra args match stdcall stack; dummy edx is ignored. */
typedef int (__fastcall *JudgeFn)(void *slot, void *edx, void *actor,
                                  unsigned int ev, void *payload);
typedef int (__fastcall *OnUpdateFn)(void *seq, void *edx);
#endif
typedef HRESULT (WINAPI *EndSceneFn)(IDirect3DDevice9 *dev);

struct OverlayVert {
    float x, y, z, rhw;
    float u, v;
};

#ifdef _WIN64
static const unsigned char kJudgePrologueWorld[] = {
    0x40, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8B, 0xEC
};
static const unsigned char kJudgePrologueA3[] = {
    0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70
};
static const unsigned char kDpPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const unsigned char kMdpPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
};
static const uintptr_t kDefaultJudge = 0x5C780;
static const uintptr_t kDefaultDp = 0x549D0;
static const uintptr_t kDefaultMdp = 0x5E780;
static const uintptr_t kDefaultScene = 0x488A30;
static const uintptr_t kA3Judge = 0x3EFD0;
static const uintptr_t kA3Dp = 0x39650;
static const uintptr_t kA3Mdp = 0x40B60;
static const uintptr_t kA3Scene = 0x2E8550;
#else
/* Stop before relocated immediates (security cookie / SEH). */
static const unsigned char kJudgePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30
};
static const unsigned char kDpPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF
};
static const unsigned char kMdpPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF
};
static const uintptr_t kDefaultJudge = 0x353A0;
static const uintptr_t kDefaultDp = 0x30670;
static const uintptr_t kDefaultMdp = 0x369F0;
static const uintptr_t kDefaultScene = 0x2549D0;
#endif

static HMODULE g_self;
static uintptr_t g_base;
static JudgeFn g_orig_judge;
static OnUpdateFn g_orig_dp;
static OnUpdateFn g_orig_mdp;
static EndSceneFn g_orig_endscene;
static volatile LONG g_ready;

static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_multiline = 1;
static int g_show_columns = 1;
static int g_show_sigma = 1;
static int g_show_fast_slow = 1;
static int g_show_window = 1;
static int g_verify_prologue = 1;
static int g_font_size = 14;
static int g_margin_px = 8;
static int g_box_alpha = 176;
static int g_play_scenes[PLAY_MAX];
static int g_play_count;
static uintptr_t g_rva_judge = kDefaultJudge;
static uintptr_t g_rva_dp = kDefaultDp;
static uintptr_t g_rva_mdp = kDefaultMdp;
static uintptr_t g_rva_scene = kDefaultScene;
#ifdef _WIN64
static const unsigned char *g_judge_prologue = kJudgePrologueWorld;
static size_t g_judge_prologue_n = sizeof(kJudgePrologueWorld);
static int g_is_a3;
static int g_off_tap = 404;
static int g_off_frz = 408;
static int g_off_shock = 412;
static int g_off_combo = 476;
static int g_off_maxcombo = 480;
static int g_off_life = 660;
static int g_life_walk;
#else
static const unsigned char *g_judge_prologue = kJudgePrologue;
static size_t g_judge_prologue_n = sizeof(kJudgePrologue);
static int g_off_tap = 260;
static int g_off_frz = 264;
static int g_off_shock = 268;
static int g_off_combo = 332;
static int g_off_maxcombo = 336;
static int g_off_life;
static int g_life_walk = 1;
#endif

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_stats_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static volatile LONG g_in_chart;
static volatile LONG g_seq_tick;
static int g_last_scene = -1;
static int g_logged_reset;

static IDirect3DDevice9 *g_overlay_dev;
static IDirect3DTexture9 *g_tex[2];
static char g_last_text[2][TEXT_MAX];

struct PlayerStats {
    int n_m;
    int n_p;
    int n_gr;
    int n_gd;
    int n_ok;
    int n_miss;
    int n_ng;
    int n_fast;
    int n_slow;
    int n_timed;
    int n_ex_steps;
    int ex;
    int max_abs_ms;
    int last_ms;
    int has_last;
    int m_streak;
    int m_streak_best;
    int n_tap;
    int n_jump;
    int n_frz;
    int n_shock;
    int tap_n;
    int jump_n;
    double tap_sum;
    double jump_sum;
    int total_tap;
    int total_frz;
    int total_shock;
    int combo;
    int max_combo;
    int life;
    double sum_ms;
    double sum_abs_ms;
    double sum_sq_ms;
    int roll[ROLL_N];
    int roll_s[ROLL_N];
    int roll_n;
    int roll_i;
    int col_n[COLS];
    double col_sum[COLS];
    double col_sq[COLS];
    int used_hi;
    DWORD hit_tick[ROLL_N];
};

static struct PlayerStats g_p[2];

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
    return (int)GetPrivateProfileIntW(L"stats_overlay", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"stats_overlay", key, L"", buf, 64, ini);
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

static int scene_is_play(int scene)
{
    int i;
    for (i = 0; i < g_play_count; i++) {
        if (g_play_scenes[i] == scene)
            return 1;
    }
    return 0;
}

static void parse_play_scenes(void)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[128];
#ifdef _WIN64
    const wchar_t *def = L"4,12";
#else
    const wchar_t *def = L"4,11";
#endif
    g_play_count = 0;
    if (!g_dir[0]) {
        g_play_scenes[0] = 4;
#ifdef _WIN64
        g_play_scenes[1] = 12;
#else
        g_play_scenes[1] = 11;
#endif
        g_play_count = 2;
        return;
    }
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"stats_overlay", L"play_scenes", def, buf, 128, ini);
    {
        wchar_t *p = buf;
        while (*p && g_play_count < PLAY_MAX) {
            wchar_t *end = NULL;
            unsigned long v;
            while (*p == L' ' || *p == L'\t' || *p == L',')
                p++;
            if (!*p)
                break;
            v = wcstoul(p, &end, 0);
            if (end == p)
                break;
            g_play_scenes[g_play_count++] = (int)v;
            p = end;
        }
    }
    if (g_play_count == 0) {
        g_play_scenes[0] = 4;
#ifdef _WIN64
        g_play_scenes[1] = 12;
#else
        g_play_scenes[1] = 11;
#endif
        g_play_count = 2;
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

static void stats_reset(void)
{
    EnterCriticalSection(&g_stats_cs);
    memset(g_p, 0, sizeof(g_p));
    LeaveCriticalSection(&g_stats_cs);
    g_last_text[0][0] = 0;
    g_last_text[1][0] = 0;
}

static void seq_heartbeat(void)
{
    InterlockedExchange(&g_seq_tick, (LONG)GetTickCount());
}

static int seq_stale(void)
{
    return (GetTickCount() - (DWORD)g_seq_tick) > SEQ_STALE_MS;
}

/* Leave play immediately: hide overlay and wipe in-progress chart stats. */
static void overlay_abort_chart(void)
{
    if (InterlockedExchange(&g_in_chart, 0))
        stats_reset();
    else {
        g_last_text[0][0] = 0;
        g_last_text[1][0] = 0;
    }
}

static int ticks_to_ms(int ticks)
{
    if (ticks >= 0)
        return (int)((ticks * 1000 + 510) / 1020);
    return -(int)((-ticks * 1000 + 510) / 1020);
}

static int ex_for_judge(int j)
{
    if (j == J_MARV)
        return 3;
    if (j == J_PERF)
        return 2;
    if (j == J_GREAT)
        return 1;
    return 0;
}

static int round_ms(double v)
{
    return (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

static int popcnt32(unsigned int x)
{
    int n = 0;
    while (x) {
        n += (int)(x & 1u);
        x >>= 1;
    }
    return n;
}

static void read_actor(struct PlayerStats *p, uintptr_t actor)
{
    if (!actor)
        return;
    __try {
        p->total_tap = *(int *)(actor + g_off_tap);
        p->total_frz = *(int *)(actor + g_off_frz);
        p->total_shock = *(int *)(actor + g_off_shock);
        p->combo = *(int *)(actor + g_off_combo);
        p->max_combo = *(int *)(actor + g_off_maxcombo);
        if (g_off_life)
            p->life = *(int *)(actor + g_off_life);
        else if (g_life_walk) {
            /* A3: life lives on LifeGaugeActor (child), not GamePlayActor. */
#ifdef _WIN64
            uintptr_t c = *(uintptr_t *)(actor + 24);
            int n = 0;
            while (c && n++ < 64) {
                if (memcmp((const char *)c + 44, "LifeGauge", 9) == 0) {
                    p->life = *(int *)(c + 144);
                    break;
                }
                c = *(uintptr_t *)(c + 16);
            }
#else
            uintptr_t c = *(uintptr_t *)(actor + 12);
            int n = 0;
            while (c && n++ < 64) {
                if (memcmp((const char *)c + 28, "LifeGauge", 9) == 0 ||
                    memcmp((const char *)c + 44, "LifeGauge", 9) == 0) {
                    p->life = *(int *)(c + 0x70);
                    break;
                }
                c = *(uintptr_t *)(c + 8);
            }
#endif
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void record_judge(int player, int judge, int err_ticks, unsigned int panels,
                         uintptr_t actor, int note_type, int n_panels)
{
    struct PlayerStats *p;
    int ms;
    int abs_ms;
    int c;
    int freeze;
    int shock;
    int jump;
    DWORD now;

    if (player < 0 || player > 1)
        return;
    if (judge < 0 || judge > J_NG)
        return;

    p = &g_p[player];
    ms = ticks_to_ms(err_ticks);
    abs_ms = ms < 0 ? -ms : ms;
    freeze = (note_type == 2);
    shock = (!freeze && (judge == J_OK || judge == J_NG));
    jump = (!freeze && !shock && n_panels >= 2);
    now = GetTickCount();

    switch (judge) {
    case J_MARV: p->n_m++; p->m_streak++; break;
    case J_PERF: p->n_p++; p->m_streak = 0; break;
    case J_GREAT: p->n_gr++; p->m_streak = 0; break;
    case J_GOOD: p->n_gd++; p->m_streak = 0; break;
    case J_OK: p->n_ok++; break;
    case J_MISS:
    case 4:
        p->n_miss++;
        p->m_streak = 0;
        break;
    case J_NG: p->n_ng++; p->m_streak = 0; break;
    default: break;
    }
    if (p->m_streak > p->m_streak_best)
        p->m_streak_best = p->m_streak;

    if (freeze)
        p->n_frz++;
    else if (shock)
        p->n_shock++;
    else if (jump)
        p->n_jump++;
    else if (judge <= J_GOOD || judge == 4 || judge == J_MISS)
        p->n_tap++;

    if (judge <= J_GOOD || judge == 4 || judge == J_MISS) {
        p->n_ex_steps++;
        p->ex += ex_for_judge(judge);
    }

    if (judge <= J_GOOD) {
        p->n_timed++;
        p->sum_ms += (double)ms;
        p->sum_abs_ms += (double)abs_ms;
        p->sum_sq_ms += (double)ms * (double)ms;
        if (abs_ms > p->max_abs_ms)
            p->max_abs_ms = abs_ms;
        p->last_ms = ms;
        p->has_last = 1;
        if (ms < 0)
            p->n_fast++;
        else if (ms > 0)
            p->n_slow++;
        p->roll[p->roll_i] = abs_ms;
        p->roll_s[p->roll_i] = ms;
        p->hit_tick[p->roll_i] = now;
        p->roll_i = (p->roll_i + 1) % ROLL_N;
        if (p->roll_n < ROLL_N)
            p->roll_n++;
        for (c = 0; c < COLS; c++) {
            if (panels & (1u << c)) {
                p->col_n[c]++;
                p->col_sum[c] += (double)ms;
                p->col_sq[c] += (double)ms * (double)ms;
                if (c >= 4)
                    p->used_hi = 1;
            }
        }
        if (!freeze && !shock) {
            if (jump) {
                p->jump_n++;
                p->jump_sum += (double)ms;
            } else {
                p->tap_n++;
                p->tap_sum += (double)ms;
            }
        }
    }
    read_actor(p, actor);
}

static int missing_ex(const struct PlayerStats *p)
{
    return 3 * p->n_ex_steps - p->ex;
}

static double window_sigma(const struct PlayerStats *p)
{
    int i;
    double mean;
    double var;
    double s = 0.0;
    if (p->roll_n <= 1)
        return 0.0;
    for (i = 0; i < p->roll_n; i++)
        s += (double)p->roll_s[i];
    mean = s / (double)p->roll_n;
    var = 0.0;
    for (i = 0; i < p->roll_n; i++) {
        double d = (double)p->roll_s[i] - mean;
        var += d * d;
    }
    var /= (double)p->roll_n;
    return sqrt(var);
}

static int nps_now(const struct PlayerStats *p)
{
    int i;
    int n = 0;
    DWORD now = GetTickCount();
    if (p->roll_n <= 0)
        return 0;
    for (i = 0; i < p->roll_n; i++) {
        if (now - p->hit_tick[i] <= 1000)
            n++;
    }
    return n;
}

static const char *lamp_name(const struct PlayerStats *p)
{
    int miss = p->n_miss + p->n_ng;
    int steps = p->n_m + p->n_p + p->n_gr + p->n_gd + miss;
    if (steps <= 0)
        return "";
    if (miss)
        return "*";
    if (p->n_gd == 0 && p->n_gr == 0 && p->n_p == 0)
        return "MFC";
    if (p->n_gd == 0 && p->n_gr == 0)
        return "PFC";
    if (p->n_gd == 0)
        return "GFC";
    return "FC";
}

static int append(char *dst, int used, int cap, const char *fmt, ...);

static void sparkline(char *dst, int cap, const struct PlayerStats *p)
{
    int i;
    int n;
    int used = 0;
    if (p->roll_n <= 0) {
        dst[0] = 0;
        return;
    }
    used = append(dst, used, cap, "|");
    n = p->roll_n;
    for (i = 0; i < n; i++) {
        int idx = (p->roll_i - n + i + ROLL_N) % ROLL_N;
        int ms = p->roll_s[idx];
        int a = ms < 0 ? -ms : ms;
        char ch;
        if (a <= 4)
            ch = '=';
        else if (a <= 16)
            ch = (ms < 0) ? 'e' : 'l';
        else if (a <= 33)
            ch = (ms < 0) ? 'E' : 'L';
        else
            ch = (ms < 0) ? '<' : '>';
        used = append(dst, used, cap, "%c", ch);
    }
    append(dst, used, cap, "|");
}

static int player_active(const struct PlayerStats *p)
{
    return (p->n_m + p->n_p + p->n_gr + p->n_gd + p->n_ok + p->n_miss + p->n_ng) > 0;
}

static double mean_ms(const struct PlayerStats *p)
{
    if (p->n_timed <= 0)
        return 0.0;
    return p->sum_ms / (double)p->n_timed;
}

static double mean_abs_ms(const struct PlayerStats *p)
{
    if (p->n_timed <= 0)
        return 0.0;
    return p->sum_abs_ms / (double)p->n_timed;
}

static double sigma_ms(const struct PlayerStats *p)
{
    double n;
    double var;
    if (p->n_timed <= 1)
        return 0.0;
    n = (double)p->n_timed;
    var = p->sum_sq_ms / n - (p->sum_ms / n) * (p->sum_ms / n);
    if (var < 0.0)
        var = 0.0;
    return sqrt(var);
}

static double window_abs(const struct PlayerStats *p)
{
    int i;
    int s = 0;
    if (p->roll_n <= 0)
        return 0.0;
    for (i = 0; i < p->roll_n; i++)
        s += p->roll[i];
    return (double)s / (double)p->roll_n;
}

static int append(char *dst, int used, int cap, const char *fmt, ...)
{
    int n;
    va_list ap;
    if (used < 0 || used >= cap)
        return used;
    va_start(ap, fmt);
    n = _vsnprintf_s(dst + used, (size_t)(cap - used), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0)
        return cap - 1;
    return used + n;
}

static int fmt_signed(char *dst, int used, int cap, const char *key, double v)
{
    int iv = (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
    return append(dst, used, cap, "%s%+d", key, iv);
}

static int fmt_columns(char *dst, int used, int cap, const struct PlayerStats *p)
{
    static const char *lo[] = { "L", "D", "U", "R" };
    int i;
    int first = 1;
    int n = p->used_hi ? 8 : 4;
    for (i = 0; i < n; i++) {
        int iv;
        double sd;
        const char *name;
        if (p->col_n[i] <= 0)
            continue;
        iv = round_ms(p->col_sum[i] / (double)p->col_n[i]);
        sd = 0.0;
        if (p->col_n[i] > 1) {
            double mean = p->col_sum[i] / (double)p->col_n[i];
            double var = p->col_sq[i] / (double)p->col_n[i] - mean * mean;
            if (var < 0.0)
                var = 0.0;
            sd = sqrt(var);
        }
        if (i < 4)
            name = lo[i];
        else
            name = lo[i - 4];
        if (!first)
            used = append(dst, used, cap, " ");
        first = 0;
        if (p->used_hi && i >= 4)
            used = append(dst, used, cap, "%s2:%+d/%.1f", name, iv, sd);
        else if (p->used_hi)
            used = append(dst, used, cap, "%s1:%+d/%.1f", name, iv, sd);
        else
            used = append(dst, used, cap, "%s:%+d/%.1f", name, iv, sd);
    }
    return used;
}

static void fmt_player(char *dst, int cap, const struct PlayerStats *p, int pid, int tag,
                       const struct PlayerStats *other)
{
    const char *lamp = lamp_name(p);
    const char *sep = g_multiline ? "\n" : "  ";
    int used = 0;
    int miss = p->n_miss + p->n_ng;
    int judged = p->n_m + p->n_p + p->n_gr + p->n_gd + miss;
    int total = p->total_tap + p->total_frz + p->total_shock;
    int remain;
    int possible;
    int proj;
    int sug;
    char spark[ROLL_N + 8];

    dst[0] = 0;
    if (tag)
        used = append(dst, used, cap, "%dP ", pid + 1);

    /* Official WORLD EVENT MODE calibration line, 0.1ms on Abs(?) / ?. */
    used = append(dst, used, cap, "EX: -%d  Max: %dms  Abs(\xCE\xBC): %.1fms  \xCE\xBC: %+.1fms",
                  missing_ex(p), p->max_abs_ms, mean_abs_ms(p), mean_ms(p));

    used = append(dst, used, cap, "%s", sep);
    if (g_show_sigma)
        used = append(dst, used, cap, "\xCF\x83:%.1f  ", sigma_ms(p));
    if (g_show_window) {
        if (p->roll_n > 0)
            used = append(dst, used, cap, "w32:%.1f/%.1f  ", window_abs(p), window_sigma(p));
        else
            used = append(dst, used, cap, "w32:--/--  ");
    }
    if (g_show_fast_slow)
        used = append(dst, used, cap, "F:%d/S:%d  ", p->n_fast, p->n_slow);
    if (p->has_last)
        used = append(dst, used, cap, "last:%+dms", p->last_ms);
    else
        used = append(dst, used, cap, "last:--");
    sug = -round_ms(mean_ms(p));
    used = append(dst, used, cap, "  try %+d", sug);

    used = append(dst, used, cap, "%s", sep);
    if (lamp[0] && lamp[0] != '*')
        used = append(dst, used, cap, "%s  ", lamp);
    else if (lamp[0] == '*')
        used = append(dst, used, cap, "*  ");
    used = append(dst, used, cap, "M:%d P:%d GR:%d GD:%d Mi:%d OK:%d NG:%d",
                  p->n_m, p->n_p, p->n_gr, p->n_gd, p->n_miss, p->n_ok, p->n_ng);

    possible = 3 * p->n_ex_steps;
    remain = total > judged ? total - judged : 0;
    if (p->total_tap > p->n_ex_steps)
        proj = p->ex + 3 * (p->total_tap - p->n_ex_steps);
    else
        proj = p->ex;
    used = append(dst, used, cap, "%sEX %d/%d", sep, p->ex, possible > 0 ? possible : 0);
    if (possible > 0)
        used = append(dst, used, cap, " (%.1f%%)", 100.0 * (double)p->ex / (double)possible);
    used = append(dst, used, cap, "  proj %d  %d/%d left:%d", proj, judged, total, remain);

    used = append(dst, used, cap, "%sMst:%d  cb:%d/%d  nps:%d",
                  sep, p->m_streak, p->combo, p->max_combo, nps_now(p));
    if (p->life > 0)
        used = append(dst, used, cap, "  life:%d", p->life);
    if (p->life > 0 && p->life <= 4)
        used = append(dst, used, cap, "  die:%dGR", p->life);

    if (g_show_columns && p->n_timed > 0) {
        used = append(dst, used, cap, "%s", sep);
        used = fmt_columns(dst, used, cap, p);
    }

    spark[0] = 0;
    sparkline(spark, (int)sizeof(spark), p);
    if (spark[0])
        used = append(dst, used, cap, "%s%s", sep, spark);

    if (other && player_active(other)) {
        used = append(dst, used, cap, "%svs %dP EX:%+d  %s/%s",
                      sep, pid == 0 ? 2 : 1, p->ex - other->ex, lamp, lamp_name(other));
    }
    (void)used;
}

static void overlay_release_tex(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_tex[i]) {
            g_tex[i]->Release();
            g_tex[i] = NULL;
        }
        g_last_text[i][0] = 0;
    }
    g_overlay_dev = NULL;
}

static int overlay_ensure_tex(IDirect3DDevice9 *dev, int slot)
{
    HRESULT hr;
    if (g_overlay_dev != dev) {
        overlay_release_tex();
        g_overlay_dev = dev;
    }
    if (g_tex[slot])
        return 1;
    hr = dev->CreateTexture(TEX_W, TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                            &g_tex[slot], NULL);
    if (FAILED(hr) || !g_tex[slot]) {
        g_tex[slot] = NULL;
        return 0;
    }
    return 1;
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static unsigned int pixel_lum(unsigned int p)
{
    unsigned int b = p & 0xFFu;
    unsigned int g = (p >> 8) & 0xFFu;
    unsigned int r = (p >> 16) & 0xFFu;
    unsigned int lum = r;
    if (g > lum)
        lum = g;
    if (b > lum)
        lum = b;
    return lum;
}

static void overlay_bake(IDirect3DTexture9 *tex, const char *text, int align_right)
{
    BITMAPINFO bmi;
    void *bits = NULL;
    HDC hdc;
    HDC mem;
    HBITMAP dib;
    HBITMAP oldbm;
    HFONT font;
    HFONT oldf;
    D3DLOCKED_RECT lr;
    RECT rc, dst, box;
    int x, y;
    int fs = g_font_size;
    wchar_t wide[TEXT_MAX];
    int nw;
    UINT dt;
    int tw, th;
    int box_a;

    if (fs < 10)
        fs = 10;
    if (fs > 48)
        fs = 48;
    box_a = clamp_i(g_box_alpha, 0, 255);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = TEX_W;
    bmi.bmiHeader.biHeight = -TEX_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(NULL);
    mem = CreateCompatibleDC(hdc);
    dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!mem || !dib || !bits) {
        if (dib)
            DeleteObject(dib);
        if (mem)
            DeleteDC(mem);
        if (hdc)
            ReleaseDC(NULL, hdc);
        return;
    }
    oldbm = (HBITMAP)SelectObject(mem, dib);
    /* ANTIALIASED_QUALITY = grayscale AA. ClearType on a DIB looks like colored junk. */
    font = CreateFontW(-fs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Arial");
    oldf = (HFONT)SelectObject(mem, font);
    memset(bits, 0, TEX_W * TEX_H * 4);
    SetBkMode(mem, TRANSPARENT);
    SetBkColor(mem, RGB(0, 0, 0));
    SetTextAlign(mem, TA_LEFT | TA_TOP);
    nw = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, TEXT_MAX);
    if (nw <= 0)
        wide[0] = 0;

    dt = DT_NOPREFIX | DT_NOCLIP | (align_right ? DT_RIGHT : DT_LEFT);
    rc.left = TEX_PAD;
    rc.top = 0;
    rc.right = TEX_W - TEX_PAD;
    rc.bottom = TEX_H;
    DrawTextW(mem, wide, -1, &rc, dt | DT_CALCRECT);
    tw = rc.right - rc.left;
    th = rc.bottom - rc.top;
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    if (th > TEX_H - 2 * TEX_PAD)
        th = TEX_H - 2 * TEX_PAD;
    if (tw > TEX_W - 2 * TEX_PAD)
        tw = TEX_W - 2 * TEX_PAD;

    if (align_right) {
        dst.right = TEX_W - TEX_PAD;
        dst.left = dst.right - tw;
    } else {
        dst.left = TEX_PAD;
        dst.right = dst.left + tw;
    }
    dst.bottom = TEX_H - TEX_PAD;
    dst.top = dst.bottom - th;

    box.left = clamp_i(dst.left - BOX_PAD, 0, TEX_W);
    box.top = clamp_i(dst.top - BOX_PAD, 0, TEX_H);
    box.right = clamp_i(dst.right + BOX_PAD, 0, TEX_W);
    box.bottom = clamp_i(dst.bottom + BOX_PAD, 0, TEX_H);

    SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, wide, -1, &dst, dt);

    if (SUCCEEDED(tex->LockRect(0, &lr, NULL, 0))) {
        unsigned char *src = (unsigned char *)bits;
        unsigned char *dstp = (unsigned char *)lr.pBits;
        unsigned int box_pix = ((unsigned int)box_a << 24);
        for (y = 0; y < TEX_H; y++) {
            unsigned int *srow = (unsigned int *)(src + y * TEX_W * 4);
            unsigned int *drow = (unsigned int *)(dstp + y * lr.Pitch);
            int in_box_y = (y >= box.top && y < box.bottom);
            for (x = 0; x < TEX_W; x++) {
                unsigned int lum = pixel_lum(srow[x]);
                if (lum) {
                    drow[x] = (lum << 24) | 0x00FFFFFFu;
                } else if (in_box_y && x >= box.left && x < box.right) {
                    drow[x] = box_pix;
                } else {
                    drow[x] = 0;
                }
            }
        }
        tex->UnlockRect(0);
    }

    SelectObject(mem, oldf);
    SelectObject(mem, oldbm);
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, hdc);
}

static void overlay_draw_quad(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, float x, float y)
{
    struct OverlayVert v[4];
    float x2 = x + (float)TEX_W;
    float y2 = y + (float)TEX_H;

    v[0].x = x - 0.5f;
    v[0].y = y - 0.5f;
    v[0].z = 0.0f;
    v[0].rhw = 1.0f;
    v[0].u = 0.0f;
    v[0].v = 0.0f;
    v[1].x = x2 - 0.5f;
    v[1].y = y - 0.5f;
    v[1].z = 0.0f;
    v[1].rhw = 1.0f;
    v[1].u = 1.0f;
    v[1].v = 0.0f;
    v[2].x = x - 0.5f;
    v[2].y = y2 - 0.5f;
    v[2].z = 0.0f;
    v[2].rhw = 1.0f;
    v[2].u = 0.0f;
    v[2].v = 1.0f;
    v[3].x = x2 - 0.5f;
    v[3].y = y2 - 0.5f;
    v[3].z = 0.0f;
    v[3].rhw = 1.0f;
    v[3].u = 1.0f;
    v[3].v = 1.0f;

    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct OverlayVert));
}

static int overlay_on_backbuffer(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *bb = NULL;
    int ok = 0;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
        return 0;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        ok = (rt == bb);
    if (rt)
        rt->Release();
    if (bb)
        bb->Release();
    return ok;
}

static void overlay_draw(IDirect3DDevice9 *dev)
{
    struct PlayerStats snap[2];
    char text[2][TEXT_MAX];
    int active[2];
    int tag;
    int i;
    D3DVIEWPORT9 vp;
    IDirect3DStateBlock9 *sb = NULL;

    {
        int scene = current_scene();
        if (scene != g_last_scene) {
            log_msg("scene %d -> %d (%s)", g_last_scene, scene,
                    scene_is_play(scene) ? "play" : "hide");
            g_last_scene = scene;
        }
        if (!scene_is_play(scene)) {
            overlay_abort_chart();
            return;
        }
    }
    /* Play sequence not ticking (test menu / pause): hide this frame.
     * Do not wipe stats here; a hitch would erase an in-progress chart. */
    if (seq_stale())
        return;
    if (!g_in_chart)
        return;
    if (!overlay_on_backbuffer(dev))
        return;

    EnterCriticalSection(&g_stats_cs);
    snap[0] = g_p[0];
    snap[1] = g_p[1];
    LeaveCriticalSection(&g_stats_cs);

    active[0] = player_active(&snap[0]);
    active[1] = player_active(&snap[1]);
    if (!active[0] && !active[1])
        return;
    tag = active[0] && active[1];

    text[0][0] = 0;
    text[1][0] = 0;
    if (active[0])
        fmt_player(text[0], TEXT_MAX, &snap[0], 0, tag,
                   tag ? &snap[1] : NULL);
    if (active[1])
        fmt_player(text[1], TEXT_MAX, &snap[1], 1, tag,
                   tag ? &snap[0] : NULL);

    if (FAILED(dev->GetViewport(&vp)))
        return;

    for (i = 0; i < 2; i++) {
        if (!active[i])
            continue;
        if (!overlay_ensure_tex(dev, i))
            return;
        if (strcmp(g_last_text[i], text[i]) != 0) {
            overlay_bake(g_tex[i], text[i], i == 0);
            strncpy_s(g_last_text[i], text[i], _TRUNCATE);
        }
    }

    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb)
        return;
    sb->Capture();

    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    {
        float m = (float)g_margin_px;
        float y = (float)vp.Y + (float)vp.Height - (float)TEX_H - m;
        if (active[0] && g_tex[0])
            overlay_draw_quad(dev, g_tex[0],
                              (float)vp.X + (float)vp.Width - (float)TEX_W - m, y);
        if (active[1] && g_tex[1])
            overlay_draw_quad(dev, g_tex[1],
                              (float)vp.X + m, y);
    }

    sb->Apply();
    sb->Release();
}

static HRESULT WINAPI detour_endscene(IDirect3DDevice9 *dev)
{
    if (g_ready && dev) {
        __try {
            overlay_draw(dev);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            overlay_release_tex();
        }
    }
    return g_orig_endscene(dev);
}

static LRESULT CALLBACK dummy_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    return DefWindowProcW(hwnd, msg, w, l);
}

static BOOL CALLBACK find_game_hwnd(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetParent(hwnd) == NULL) {
        *(HWND *)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HRESULT try_create_dev(IDirect3D9 *d3d, HWND hwnd, IDirect3DDevice9 **dev)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = E_FAIL;
    DWORD flags[2];
    int i;
    flags[0] = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    flags[1] = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;
    for (i = 0; i < 2; i++) {
        *dev = NULL;
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags[i], &pp, dev);
        if (SUCCEEDED(hr) && *dev)
            return hr;
    }
    return hr;
}

static int hook_endscene(void)
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    WNDCLASSW wc;
    HWND hwnd;
    HWND game = NULL;
    void **vt;
    HRESULT hr;
    MH_STATUS st;
    static int registered;

    if (g_orig_endscene)
        return 1;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dummy_wndproc;
    wc.hInstance = g_self;
    wc.lpszClassName = L"so_d3d9";
    if (!registered) {
        RegisterClassW(&wc);
        registered = 1;
    }
    hwnd = CreateWindowExW(0, L"so_d3d9", L"", WS_OVERLAPPED, 0, 0, 32, 32,
                           NULL, NULL, g_self, NULL);
    if (!hwnd)
        return 0;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(hwnd);
        return 0;
    }
    hr = try_create_dev(d3d, hwnd, &dev);
    if (FAILED(hr) || !dev) {
        EnumWindows(find_game_hwnd, (LPARAM)&game);
        if (game && game != hwnd)
            hr = try_create_dev(d3d, game, &dev);
    }
    if (FAILED(hr) || !dev) {
        static int logged;
        if (!logged) {
            log_msg("dummy D3D9 CreateDevice failed hr=%08X (will retry)", hr);
            logged = 1;
        }
        d3d->Release();
        DestroyWindow(hwnd);
        return 0;
    }
    vt = *(void ***)dev;
    st = MH_CreateHook(vt[D3D9_ENDSCENE], (LPVOID)detour_endscene, (LPVOID *)&g_orig_endscene);
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        log_msg("MH_CreateHook EndScene: %s", MH_StatusToString(st));
        g_orig_endscene = NULL;
        return 0;
    }
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        log_msg("MH_EnableHook EndScene: %s", MH_StatusToString(st));
        return 0;
    }
    log_msg("hooked IDirect3DDevice9::EndScene");
    return 1;
}

static void apply_seq_state(int st, int in_lo, int in_hi, int reset_hi);

static void capture_judge(void *slot, void *actor, void *payload)
{
    int player = 0;
    int judge = -1;
    int err_ticks = 0;
    unsigned int panels = 0;
    int note_type = 0;
    int n_panels = 0;
    unsigned int *sdw;

    if (!slot || !actor)
        return;
    sdw = (unsigned int *)slot;
    __try {
        player = *(int *)((char *)actor + PLAYER_OFF);
        judge = (int)sdw[NOTE_JUDGE / 4];
#ifdef _WIN64
        if (payload)
            err_ticks = *(int *)((char *)payload + 4);
        else if (*(uintptr_t *)slot)
            err_ticks = (int)sdw[SLOT_HIT] - *(int *)(*(uintptr_t *)slot + 8);
#else
        if (*(uintptr_t *)slot)
            err_ticks = (int)sdw[SLOT_HIT] - *(int *)(*(uintptr_t *)slot + 8);
#endif
        if (*(uintptr_t *)slot)
            note_type = *(unsigned char *)*(uintptr_t *)slot;
        if (payload)
            panels = *(unsigned int *)((char *)payload + 8);
        n_panels = popcnt32(panels);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        judge = -1;
    }
    if (judge >= 0) {
        EnterCriticalSection(&g_stats_cs);
        record_judge(player, judge, err_ticks, panels, (uintptr_t)actor,
                     note_type, n_panels);
        LeaveCriticalSection(&g_stats_cs);
    }
}

#ifdef _WIN64
static __int64 __fastcall detour_judge(__int64 actor, unsigned int *slot,
                                       unsigned int ev, __int64 payload)
{
    (void)ev;
    if (g_ready)
        capture_judge(slot, (void *)actor, (void *)payload);
    return g_orig_judge(actor, slot, ev, payload);
}

static __int64 __fastcall detour_dp(__int64 seq)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 5, 7, 1);
    return g_orig_dp(seq);
}

static __int64 __fastcall detour_mdp(__int64 seq)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 9, 11, 2);
    return g_orig_mdp(seq);
}
#else
static int __fastcall detour_judge(void *slot, void *edx, void *actor,
                                   unsigned int ev, void *payload)
{
    (void)ev;
    if (g_ready)
        capture_judge(slot, actor, payload);
    return g_orig_judge(slot, edx, actor, ev, payload);
}

static int __fastcall detour_dp(void *seq, void *edx)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 5, 7, 1);
    return g_orig_dp(seq, edx);
}

static int __fastcall detour_mdp(void *seq, void *edx)
{
    if (g_ready)
        apply_seq_state(seq_case((uintptr_t)seq), 9, 11, 2);
    return g_orig_mdp(seq, edx);
}
#endif

static void apply_seq_state(int st, int in_lo, int in_hi, int reset_hi)
{
    if (st < 0)
        return;
    seq_heartbeat();
    if (st <= reset_hi) {
        InterlockedExchange(&g_in_chart, 0);
        stats_reset();
        if (!g_logged_reset) {
            g_logged_reset = 1;
            log_msg("chart reset (seq case %d)", st);
        }
        return;
    }
    g_logged_reset = 0;
    if (st >= in_lo && st <= in_hi)
        InterlockedExchange(&g_in_chart, 1);
    else if (st > in_hi && g_in_chart)
        InterlockedExchange(&g_in_chart, 0);
}

#ifdef _WIN64
static int play_scenes_are(int a, int b)
{
    return g_play_count == 2 && g_play_scenes[0] == a && g_play_scenes[1] == b;
}

static void apply_a3_64_layout(int switch_rvas)
{
    g_is_a3 = 1;
    g_judge_prologue = kJudgePrologueA3;
    g_judge_prologue_n = sizeof(kJudgePrologueA3);
    g_off_tap = 372;
    g_off_frz = 376;
    g_off_shock = 380;
    g_off_combo = 444;
    g_off_maxcombo = 448;
    g_off_life = 0;
    g_life_walk = 1;
    if (switch_rvas) {
        g_rva_judge = kA3Judge;
        g_rva_dp = kA3Dp;
        g_rva_mdp = kA3Mdp;
        g_rva_scene = kA3Scene;
    }
    if (play_scenes_are(4, 12)) {
        g_play_scenes[0] = 4;
        g_play_scenes[1] = 11;
        g_play_count = 2;
    }
}

static void apply_world_64_layout(int switch_rvas)
{
    g_is_a3 = 0;
    g_judge_prologue = kJudgePrologueWorld;
    g_judge_prologue_n = sizeof(kJudgePrologueWorld);
    g_off_tap = 404;
    g_off_frz = 408;
    g_off_shock = 412;
    g_off_combo = 476;
    g_off_maxcombo = 480;
    g_off_life = 660;
    g_life_walk = 0;
    if (switch_rvas) {
        g_rva_judge = kDefaultJudge;
        g_rva_dp = kDefaultDp;
        g_rva_mdp = kDefaultMdp;
        g_rva_scene = kDefaultScene;
    }
}

static int detect_64_build(void)
{
    int world_ini = bytes_match((void *)(g_base + g_rva_judge),
                                kJudgePrologueWorld, sizeof(kJudgePrologueWorld));
    int a3_ini = bytes_match((void *)(g_base + g_rva_judge),
                             kJudgePrologueA3, sizeof(kJudgePrologueA3));
    int world_known = bytes_match((void *)(g_base + kDefaultJudge),
                                  kJudgePrologueWorld, sizeof(kJudgePrologueWorld));
    int a3_known = bytes_match((void *)(g_base + kA3Judge),
                               kJudgePrologueA3, sizeof(kJudgePrologueA3));
    if (world_ini) {
        apply_world_64_layout(0);
        return 1;
    }
    if (a3_ini) {
        apply_a3_64_layout(0);
        return 1;
    }
    if (a3_known) {
        apply_a3_64_layout(1);
        return 1;
    }
    if (world_known) {
        apply_world_64_layout(1);
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
            int ok;
            if (!g_verify_prologue)
                ok = 1;
#ifdef _WIN64
            else
                ok = detect_64_build();
#else
            else
                ok = bytes_match((void *)(g_base + g_rva_judge),
                                 g_judge_prologue, g_judge_prologue_n);
#endif
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

static int hook_one(const char *name, uintptr_t rva, LPVOID detour, LPVOID *orig,
                    const unsigned char *prologue, size_t prologue_n, int required)
{
    MH_STATUS st;
    if (g_verify_prologue &&
        !bytes_match((void *)(g_base + rva), prologue, prologue_n)) {
        log_msg("%s prologue mismatch at RVA 0x%X%s",
                name, (unsigned)rva, required ? "" : " (skip)");
        return required ? 1 : 0;
    }
    st = MH_CreateHook((LPVOID)(g_base + rva), detour, orig);
    if (st != MH_OK) {
        log_msg("MH_CreateHook %s: %s", name, MH_StatusToString(st));
        return required ? 1 : 0;
    }
    log_msg("hooked %s RVA 0x%X", name, (unsigned)rva);
    return 0;
}

static DWORD WINAPI init_thread(LPVOID)
{
    DWORD endscene_start;
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_stats_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_enabled = ini_int(L"enabled", 1);
    g_multiline = ini_int(L"multiline", 1);
    g_show_columns = ini_int(L"show_columns", 1);
    g_show_sigma = ini_int(L"show_sigma", 1);
    g_show_fast_slow = ini_int(L"show_fast_slow", 1);
    g_show_window = ini_int(L"show_window", 1);
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_font_size = ini_int(L"font_size", 14);
    g_margin_px = ini_int(L"margin_px", 8);
    if (g_margin_px < 0)
        g_margin_px = 0;
    if (g_margin_px > 64)
        g_margin_px = 64;
    g_box_alpha = ini_int(L"box_alpha", 176);
    if (g_box_alpha < 0)
        g_box_alpha = 0;
    if (g_box_alpha > 255)
        g_box_alpha = 255;
    g_rva_judge = ini_rva(L"rva_judge", kDefaultJudge);
    g_rva_dp = ini_rva(L"rva_dp_update", kDefaultDp);
    g_rva_mdp = ini_rva(L"rva_mdp_update", kDefaultMdp);
    g_rva_scene = ini_rva(L"rva_scene", kDefaultScene);
    parse_play_scenes();
    open_log();
    {
        char list[64];
        int n = 0;
        int i;
        list[0] = 0;
        for (i = 0; i < g_play_count; i++) {
            int w = _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, "%s%d",
                                i ? "," : "", g_play_scenes[i]);
            if (w > 0)
                n += w;
        }
        log_msg("stats_overlay starting (enabled=%d font=%d margin=%d "
                "play_scenes=%s rva_judge=0x%X)",
                g_enabled, g_font_size, g_margin_px,
                list[0] ? list : "-", (unsigned)g_rva_judge);
    }

    if (!wait_unpacked()) {
        log_msg("timeout waiting for gamemdx.dll at rva_judge=0x%X",
                (unsigned)g_rva_judge);
        return 1;
    }
    Sleep(200);
#ifdef _WIN64
    log_msg("gamemdx unpacked at %p (%s rva_judge=0x%X rva_dp=0x%X rva_mdp=0x%X "
            "rva_scene=0x%X)",
            (void *)g_base, g_is_a3 ? "A3 2024040200 x64" : "WORLD 2025080500 x64",
            (unsigned)g_rva_judge, (unsigned)g_rva_dp, (unsigned)g_rva_mdp,
            (unsigned)g_rva_scene);
#else
    log_msg("gamemdx unpacked at %p (A3 2024040200 x86 rva_judge=0x%X)",
            (void *)g_base, (unsigned)g_rva_judge);
#endif

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        log_msg("MH_Initialize failed");
        return 1;
    }

    if (hook_one("judge", g_rva_judge, (LPVOID)detour_judge, (LPVOID *)&g_orig_judge,
                 g_judge_prologue, g_judge_prologue_n, 1))
        return 1;
    if (hook_one("dp_update", g_rva_dp, (LPVOID)detour_dp, (LPVOID *)&g_orig_dp,
                 kDpPrologue, sizeof(kDpPrologue), 1))
        return 1;
    hook_one("mdp_update", g_rva_mdp, (LPVOID)detour_mdp, (LPVOID *)&g_orig_mdp,
             kMdpPrologue, sizeof(kMdpPrologue), 0);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log_msg("MH_EnableHook failed");
        return 1;
    }

    InterlockedExchange(&g_ready, 1);

    endscene_start = GetTickCount();
    while (!hook_endscene()) {
        if (GetTickCount() - endscene_start > 60000) {
            log_msg("timeout hooking D3D9 EndScene (overlay will not draw)");
            break;
        }
        Sleep(250);
    }

    log_msg("hooks enabled");
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
