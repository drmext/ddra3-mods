#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "MinHook.h"

/*
 * Spice -k hook (A3 64-bit and 32-bit):
 *  Always-on World-style Real Speed (M-Mod):
 *    hispeed = clamp(m_bpm / chart_bpm, 0.25x, 8.00x)
 *  Chart BPM prefers duration-weighted Core (from SSQ note walk),
 *  then music::Info bpmmax, then bpmmin.
 *  A3 stores SPEED as a 0-based 0.25x index on Option
 *  (64: +0x0C, 32: +0x08; index 0 = 0.25x ... 31 = 8.00x).
 *  GamePlayActor bakes scroll floats (from/to); note scroll reads those.
 *  SPEED menu x-mod writes are ignored while enabled=1.
 *  In-song Menu Left/Right nudges live m_bpm by m_bpm_step (default 10),
 *  updates scroll + HUD, then restores each side's INI m_bpm when play ends.
 *  Optional WORLD-style CONSTANT arrow visibility (INI constant=1): notes still
 *  scroll via M-Mod, but draw is gated to a fixed display time (ms). Menu L/R
 *  defaults to nudging m_bpm; Menu Left+Right chord toggles to constant_ms
 *  (ONLINE appends " C####", with "*" while editing display time).
 *  During play, the green ONLINE label shows liveBPM (effective) via a
 *  retargeted string pointer (no D3D). One side: "150 BPM (600)". Both sides
 *  same live: "150 BPM (600 / 660)". Different live: "150 BPM (600) / 300 BPM
 *  (660)". effective = live * m_bpm / chart_ref per side.
 *  Status HUD refresh restores stock ONLINE once GamePlayActors are gone.
 */

#ifdef _WIN64
#define INI_NAME L"mmod_speed_64bit.ini"
#define LOG_NAME L"mmod_speed_64bit.log"
#define STOCK_ONLINE " ONLINE"
#define STOCK_ONLINE_LEN 8
typedef uintptr_t ActorPtr;
typedef ActorPtr (__fastcall *OptionCopyFn)(ActorPtr dst, ActorPtr src);
typedef void *(__fastcall *MusicLookupFn)(unsigned int mcode);
typedef void (__fastcall *SetHispeedFn)(ActorPtr opt, int index);
typedef void **(__fastcall *SsqAnalyzeFn)(ActorPtr ctx);
typedef ActorPtr (__fastcall *GameplaySetupFn)(ActorPtr gp);
typedef ActorPtr (__fastcall *GameplayUpdateFn)(ActorPtr gp);
typedef void (__fastcall *OptionIconRefreshFn)(ActorPtr icon);
typedef void (__fastcall *OptionIconLiveFn)(ActorPtr icon, unsigned int ev,
                                            void *payload);
typedef void (__fastcall *StatusHudFn)(void);
typedef void (__fastcall *ArkIo3Fn)(unsigned int player, char *held, char *trigger);
typedef char (__fastcall *NoteParseFn)(
    ActorPtr reader, char **notes, ActorPtr extra, ActorPtr counts,
    ActorPtr density, unsigned int style_pass, int difficulty, ActorPtr option);
static const unsigned char kCopyPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20
};
static const unsigned char kAnalyzePrologue[] = { 0x48, 0x8B, 0xC4 };
static const unsigned char kParsePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08
};
static const unsigned char kGameplaySetupPrologue[] = {
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54
};
static const unsigned char kGameplayUpdatePrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54
};
static const unsigned char kStatusHudPrologue[] = {
    0x4C, 0x8B, 0xDC, 0x4D, 0x89, 0x63, 0x20
};
static const unsigned char kOnlineLeaPrologue[] = { 0x48, 0x8D, 0x15 };
static const unsigned char kOptionIconRefreshPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10
};
static const uintptr_t kDefaultOptionCopy = 0x125DC0;
static const uintptr_t kDefaultMusicLookup = 0x106100;
static const uintptr_t kDefaultSsqAnalyze = 0x29F70;
static const uintptr_t kDefaultNoteParse = 0x11C220;
static const uintptr_t kDefaultGameplaySetup = 0x3B490;
static const uintptr_t kDefaultGameplayUpdate = 0x3C2F0;
static const uintptr_t kDefaultOptionIconRefresh = 0x2B630;
static const uintptr_t kDefaultOptionIconLive = 0x2D500;
static const uintptr_t kDefaultOptionIconVftable = 0x2674B8;
static const uintptr_t kDefaultCsaVftable = 0x268AA8;
static const uintptr_t kDefaultStatusHud = 0x8A00;
static const uintptr_t kDefaultOnlineLea = 0x8F9F;
static const uintptr_t kDefaultUiTextOnline = 0x2EEDF8;
static const uintptr_t kDefaultGetMenuLeft = 0x2EE458;
static const uintptr_t kDefaultGetMenuRight = 0x2EE460;
static const uintptr_t kDefaultGetStart = 0x2EE478;
static const uintptr_t kDefaultIoState = 0x2EEE30;
static const uintptr_t kOffGpSpeedFrom = 0x268;
static const uintptr_t kOffGpSpeedTo = 0x26C;
static const uintptr_t kOffGpSpeedIdx = 0x274;
static const uintptr_t kOffGpPlayer = 0x84;
static const uintptr_t kOffGpLiveBpm = 0x16C;
/* Note-params child: CONSTANT flag +0xD0, display ms +0xD4 (gate 0x1DD00). */
static const uintptr_t kOffGpNoteParams = 0x128;
static const uintptr_t kOffNoteConstFlag = 0xD0;
static const uintptr_t kOffNoteConstMs = 0xD4;
/* Option fields copied into note params during GamePlay update case 0. */
static const uintptr_t kOffOptionConstFlag = 0x64;
static const uintptr_t kOffOptionConstMs = 0x68;
static const uintptr_t kOffCsaOption = 0x90;
static const uintptr_t kOffOptionIconOption = 0x60;
static const uintptr_t kOffOptionHispeed = 0x0C;
static const uintptr_t kOffActorChild = 0x18;
static const uintptr_t kOffActorSibling = 0x10;
static const uintptr_t kDefaultAnalyzeParseRet = 0x2A1AF;
static const uintptr_t kDefaultGameplayParseRet = 0x3AA4A;
static const uintptr_t kDefaultRadarParseRet = 0xE643C;
static const uintptr_t kDefaultPlayers = 0x2EF000;
static const uintptr_t kDefaultGame = 0x2ED6D0;
static const uintptr_t kDefaultOptionVftable = 0x280538;
static const uintptr_t kDefaultGameplayVftable = 0x268F08;
static const uintptr_t kDefaultOffBpmMax = 0x90;
static const uintptr_t kDefaultOffBpmMin = 0x92;
static const uintptr_t kDefaultOffPlayerOption = 0xD0;
static const uintptr_t kOffGameMcode = 0x10;
static const uintptr_t kOffUiTextMid = 0x18;
#else
#define INI_NAME L"mmod_speed_32bit.ini"
#define LOG_NAME L"mmod_speed_32bit.log"
#define STOCK_ONLINE "ONLINE"
#define STOCK_ONLINE_LEN 7
typedef uintptr_t ActorPtr;
/* option_copy is EDI=dst / ESI=src usercall - trampoline via asm only. */
typedef void *OptionCopyFn;
typedef void *(*MusicLookupFn)(unsigned int mcode);
/* thiscall fnptrs are OK; free-function detours use fastcall(self,edx) or naked. */
typedef void (__thiscall *SetHispeedFn)(ActorPtr opt, int index);
typedef int (__fastcall *SsqAnalyzeFn)(ActorPtr ctx, void *edx);
typedef int (__fastcall *GameplaySetupFn)(ActorPtr gp, void *edx);
typedef ActorPtr (__fastcall *GameplayUpdateFn)(ActorPtr gp, void *edx);
typedef void (__thiscall *OptionIconRefreshFn)(ActorPtr icon);
typedef int (__thiscall *OptionIconLiveFn)(ActorPtr icon, unsigned int ev,
                                           void *payload);
typedef int (*StatusHudFn)(void);
typedef void (__stdcall *ArkIo3Fn)(unsigned int player, char *held, char *trigger);
typedef char (__stdcall *NoteParseFn)(
    void *reader, char **notes, void *extra, void *counts, void *density,
    unsigned int style_pass, int difficulty, void *option);
/* Stop before relocated immediates (cookie / push offset SEH). */
static const unsigned char kCopyPrologue[] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x06
};
static const unsigned char kAnalyzePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
};
static const unsigned char kParsePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C
};
static const unsigned char kGameplaySetupPrologue[] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF
};
static const unsigned char kGameplayUpdatePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
};
static const unsigned char kStatusHudPrologue[] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC
};
static const unsigned char kOnlineLeaPrologue[] = { 0x68 }; /* push imm32 */
static const unsigned char kOptionIconRefreshPrologue[] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC
};
static const uintptr_t kDefaultOptionCopy = 0xE0E20;
static const uintptr_t kDefaultMusicLookup = 0xC67B0;
static const uintptr_t kDefaultSsqAnalyze = 0x243A0;
static const uintptr_t kDefaultNoteParse = 0xD9270;
static const uintptr_t kDefaultGameplaySetup = 0x32110;
static const uintptr_t kDefaultGameplayUpdate = 0x32B50;
static const uintptr_t kDefaultOptionIconRefresh = 0x25820;
static const uintptr_t kDefaultOptionIconLive = 0x263F0;
static const uintptr_t kDefaultOptionIconVftable = 0x208390;
static const uintptr_t kDefaultCsaVftable = 0x2095F0;
static const uintptr_t kDefaultStatusHud = 0x7460;
static const uintptr_t kDefaultOnlineLea = 0x79C3;
static const uintptr_t kDefaultUiTextOnline = 0x25A944;
static const uintptr_t kDefaultGetMenuLeft = 0x25A46C;
static const uintptr_t kDefaultGetMenuRight = 0x25A470;
static const uintptr_t kDefaultGetStart = 0x25A47C;
static const uintptr_t kDefaultIoState = 0x25A964;
static const uintptr_t kOffGpSpeedFrom = 0x1D8;
static const uintptr_t kOffGpSpeedTo = 0x1DC;
static const uintptr_t kOffGpSpeedIdx = 0x1E4;
static const uintptr_t kOffGpPlayer = 0x6C;
/* Live song BPM float +0xFC (helper 0xD9410, same role as 64-bit +0x16C). */
static const uintptr_t kOffGpLiveBpm = 0xFC;
/* Note-params child: CONSTANT flag +0xA0, display ms +0xA4 (gate 0x19830). */
static const uintptr_t kOffGpNoteParams = 0xC4;
static const uintptr_t kOffNoteConstFlag = 0xA0;
static const uintptr_t kOffNoteConstMs = 0xA4;
/* Option fields copied into note params during GamePlay update case 0. */
static const uintptr_t kOffOptionConstFlag = 0x60;
static const uintptr_t kOffOptionConstMs = 0x64;
static const uintptr_t kOffCsaOption = 0x74;
static const uintptr_t kOffOptionIconOption = 0x44;
static const uintptr_t kOffOptionHispeed = 0x08;
static const uintptr_t kOffActorChild = 0x0C;
static const uintptr_t kOffActorSibling = 0x08;
static const uintptr_t kDefaultAnalyzeParseRet = 0x2465A;
static const uintptr_t kDefaultGameplayParseRet = 0x318DA;
static const uintptr_t kDefaultRadarParseRet = 0xAC001;
static const uintptr_t kDefaultPlayers = 0x25A9B0;
static const uintptr_t kDefaultGame = 0x25433C;
static const uintptr_t kDefaultOptionVftable = 0x21DD6C;
static const uintptr_t kDefaultGameplayVftable = 0x20992C;
static const uintptr_t kDefaultOffBpmMax = 0x64;
static const uintptr_t kDefaultOffBpmMin = 0x66;
static const uintptr_t kDefaultOffPlayerOption = 0xA0;
static const uintptr_t kOffGameMcode = 0x10;
/* Status HUD ONLINE wrapper: mid ptr at +0x10 (see status_hud dword_1025A944+16). */
static const uintptr_t kOffUiTextMid = 0x10;
extern "C" void *option_copy_detour(void);
extern "C" void *__cdecl call_orig_option_copy(void *fn, void *dst, void *src);
extern "C" void *__cdecl detour_option_copy_c(void *dst, void *src);
#endif

/* Event 4165: hispeed index payload; OptionIcon rebuilds the SPEED visual. */
static const unsigned int kEvHispeed = 0x1045;
/* Packed IO: bit0 Start, bit1 Menu Left, bit2 Menu Right. */
static const int kIoBitStart = 0;
static const int kIoBitMenuLeft = 1;
static const int kIoBitMenuRight = 2;
static const DWORD kNudgeRepeatFirstMs = 300;
static const DWORD kNudgeRepeatMs = 50;
static const uintptr_t kDefaultOffPlayerMusic = 0x54;
static const uintptr_t kDefaultOffPlayerDiff = 0x5C;
static const uintptr_t kDefaultOffPlayerStyle = 0x58;
static const int kMaxNotesWalk = 20000;
static const double kBpmClampLo = 1.0;
static const double kBpmClampHi = 2000.0;
static const int kDefaultNoteStride = 0x4C;
static const int kDefaultOffNoteType = 0;
static const int kDefaultOffNoteTime = 4;
static const int kDefaultOffNoteMs = 8;

static HMODULE g_self;
static uintptr_t g_base;
static OptionCopyFn g_orig_copy;
static MusicLookupFn g_music_lookup;
static SsqAnalyzeFn g_orig_analyze;
static NoteParseFn g_orig_parse;
static GameplaySetupFn g_orig_gameplay_setup;
static GameplayUpdateFn g_orig_gameplay_update;
static OptionIconRefreshFn g_option_icon_refresh;
static OptionIconLiveFn g_option_icon_live;
static StatusHudFn g_orig_status_hud;
static SetHispeedFn g_orig_set_hispeed;
/* Active GamePlayActor per side (scroll floats live here, not on Option). */
static ActorPtr g_gameplay[2];
/* ControlSpeedActor per side - early Menu L/R + Option embed. */
static ActorPtr g_csa[2];
/* OptionIconActor per side - owns speed_x%03d graphic. */
static ActorPtr g_option_icon[2];
static volatile LONG g_ready;
static volatile LONG g_shutdown;
static volatile LONG g_note_parse_logs;

static int g_log_enabled = 1;
static int g_enabled = 1;
/* 1 = replace ONLINE label with live BPM / mmod readout. */
static int g_online_hud = 1;
/* Per-side INI M-Mod targets (m_bpm_1p / m_bpm_2p). */
static int g_m_bpm_ini[2] = {600, 600};
static int g_m_bpm_live[2] = {600, 600};
static int g_m_bpm_step = 10;
/* WORLD-style CONSTANT display-time gate (orthogonal to M-Mod scroll). */
static int g_constant = 1;
static int g_constant_ms_ini[2] = {600, 600};
static int g_constant_ms_live[2] = {600, 600};
static int g_constant_ms_step = 10;
/* Per-side chart BPM (core/max) for ratio + ONLINE effective. */
static int g_chart_bpm_hud[2];
/* Per-side: seen live tempo; held parity-rounded live / effective. */
static int g_seen_live_bpm[2];
static int g_display_live_bpm[2];
static int g_display_effective[2];
/* Buffer the ONLINE string pointer is retargeted to. */
static char g_online_text[80] = STOCK_ONLINE;
/* Saved ONLINE patch bytes (lea disp or push imm) for restore. */
static unsigned char g_online_lea_saved[4];
static int g_online_lea_patched;
static int g_verify_prologue = 1;
static uintptr_t g_rva_option_copy = kDefaultOptionCopy;
static uintptr_t g_rva_music_lookup = kDefaultMusicLookup;
static uintptr_t g_rva_ssq_analyze = kDefaultSsqAnalyze;
static uintptr_t g_rva_note_parse = kDefaultNoteParse;
static uintptr_t g_rva_gameplay_setup = kDefaultGameplaySetup;
static uintptr_t g_rva_gameplay_update = kDefaultGameplayUpdate;
static uintptr_t g_rva_option_icon_refresh = kDefaultOptionIconRefresh;
static uintptr_t g_rva_option_icon_live = kDefaultOptionIconLive;
static uintptr_t g_rva_option_icon_vftable = kDefaultOptionIconVftable;
static uintptr_t g_rva_csa_vftable = kDefaultCsaVftable;
static uintptr_t g_rva_status_hud = kDefaultStatusHud;
static uintptr_t g_rva_online_lea = kDefaultOnlineLea;
static uintptr_t g_rva_ui_text_online = kDefaultUiTextOnline;
static uintptr_t g_rva_get_menu_left = kDefaultGetMenuLeft;
static uintptr_t g_rva_get_menu_right = kDefaultGetMenuRight;
static uintptr_t g_rva_get_start = kDefaultGetStart;
static uintptr_t g_rva_io_state = kDefaultIoState;
static uintptr_t g_rva_analyze_parse_ret = kDefaultAnalyzeParseRet;
static uintptr_t g_rva_gameplay_parse_ret = kDefaultGameplayParseRet;
static uintptr_t g_rva_radar_parse_ret = kDefaultRadarParseRet;
static uintptr_t g_rva_players = kDefaultPlayers;
static uintptr_t g_rva_game = kDefaultGame;
static uintptr_t g_rva_option_vftable = kDefaultOptionVftable;
static uintptr_t g_rva_gameplay_vftable = kDefaultGameplayVftable;
/* Hold-repeat state for in-song Menu Left/Right nudges (per player). */
static int g_nudge_left_down[2];
static int g_nudge_right_down[2];
static DWORD g_nudge_next_ms[2];
/* 1 = Menu L/R edits constant_ms; 0 = edits m_bpm (default). Chord L+R toggles. */
static int g_nudge_edit_constant[2];
static int g_nudge_chord_down[2];
static uintptr_t g_off_bpmmax = kDefaultOffBpmMax;
static uintptr_t g_off_bpmmin = kDefaultOffBpmMin;
static uintptr_t g_off_player_option = kDefaultOffPlayerOption;
static uintptr_t g_off_player_music = kDefaultOffPlayerMusic;
static uintptr_t g_off_player_diff = kDefaultOffPlayerDiff;
static uintptr_t g_off_player_style = kDefaultOffPlayerStyle;
static int g_note_stride = kDefaultNoteStride;
static int g_off_note_type = kDefaultOffNoteType;
static int g_off_note_time = kDefaultOffNoteTime;
static int g_off_note_ms = kDefaultOffNoteMs;

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_cache_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static void *g_vt_set_slot;
static void *g_saved_vt_set;
static void *g_saved_vt_get;
static int g_vt_patched;

/* ---- Core BPM cache (mcode, World-style diff idx) ---- */

struct CoreBpmEntry {
    unsigned int mcode;
    int diff_idx;
    int core;
    int max_bpm;
    int min_bpm;
};

#define CORE_CACHE_MAX 4096
static CoreBpmEntry g_cache[CORE_CACHE_MAX];
static int g_cache_count;
/* Last gameplay-parse Core - recovers Mac (mcode,diff) key mismatches. */
static int g_play_core;
static int g_play_max;
static int g_play_min;

struct DurBucket {
    int bpm;
    int duration;
};

#define DUR_BUCKET_MAX 512

static void log_msg_flush(int do_flush, const char *fmt, ...)
{
    va_list ap;

    if (!g_log_enabled || !g_log)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    __try {
        fprintf(g_log, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
        va_start(ap, fmt);
        __try {
            vfprintf(g_log, fmt, ap);
        } __finally {
            va_end(ap);
        }
        fputc('\n', g_log);
        if (do_flush)
            fflush(g_log);
    } __finally {
        LeaveCriticalSection(&g_log_cs);
    }
}

#define log_msg(...) log_msg_flush(0, __VA_ARGS__)
#define log_msg_f(...) log_msg_flush(1, __VA_ARGS__)

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
    return (int)GetPrivateProfileIntW(L"mmod_speed", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"mmod_speed", key, L"", buf, 64, ini);
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

static void *player_obj(int player)
{
    void **slot;
    void *handle;
    void *po = NULL;
    if (player < 0 || player > 1 || !g_base || !g_rva_players)
        return NULL;
    __try {
        slot = (void **)(g_base + g_rva_players);
        handle = slot[player];
        if (!handle)
            return NULL;
        po = *(void **)handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return po;
}

/* True if gp still looks like a live GamePlayActor (not freed/reused). */
static int gameplay_alive(ActorPtr gp)
{
    void *vt;
    void *expect;
    if (!gp || !g_base || !g_rva_gameplay_vftable)
        return 0;
    expect = (void *)(g_base + g_rva_gameplay_vftable);
    __try {
        vt = *(void **)gp;
        return vt == expect;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int clamp_i(int v, int lo, int hi); /* defined below */
static int mcode_plausible(unsigned int mcode);
static void refresh_online_text_buffer(void);
static void online_hud_reset(void);

static void gameplay_slot_set(int player, ActorPtr gp)
{
    if (player < 0 || player > 1)
        return;
    g_gameplay[player] = gp;
}

static void gameplay_slot_clear(int player)
{
    if (player >= 0 && player < 2) {
        g_gameplay[player] = 0;
        g_csa[player] = 0;
        g_option_icon[player] = 0;
        g_chart_bpm_hud[player] = 0;
        g_seen_live_bpm[player] = 0;
        g_display_live_bpm[player] = 0;
        g_display_effective[player] = 0;
    }
    /* Last actor gone: restore stock ONLINE immediately (result / select). */
    if (!g_gameplay[0] && !g_gameplay[1]) {
        online_hud_reset();
        refresh_online_text_buffer();
    }
}

static int actor_has_vftable(ActorPtr obj, uintptr_t vft_rva)
{
    void *vt;
    void *expect;
    if (!obj || !g_base || !vft_rva)
        return 0;
    expect = (void *)(g_base + vft_rva);
    __try {
        vt = *(void **)obj;
        return vt == expect;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int csa_alive(ActorPtr csa)
{
    return actor_has_vftable(csa, g_rva_csa_vftable);
}

static int option_icon_alive(ActorPtr icon)
{
    return actor_has_vftable(icon, g_rva_option_icon_vftable);
}

/*
 * Walk actor tree: first child / next sibling (add_child layout).
 * depth 1 = direct child of the starting node.
 */
static ActorPtr find_descendant_by_vftable_rec(ActorPtr node, void *expect,
                                              int depth, int max_depth,
                                              int *guard)
{
    ActorPtr child;
    ActorPtr hit;

    if (!node || depth > max_depth || !guard || *guard > 256)
        return 0;
    (*guard)++;

    __try {
        if (*(void **)node == expect)
            return node;
        if (depth >= max_depth)
            return 0;
        child = *(ActorPtr *)(node + kOffActorChild);
        while (child) {
            hit = find_descendant_by_vftable_rec(child, expect, depth + 1,
                                                max_depth, guard);
            if (hit)
                return hit;
            child = *(ActorPtr *)(child + kOffActorSibling);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static ActorPtr find_descendant_by_vftable(ActorPtr root, uintptr_t vft_rva,
                                          int max_depth)
{
    void *expect;
    ActorPtr child;
    ActorPtr hit;
    int guard = 0;

    if (!root || !g_base || !vft_rva || max_depth < 1)
        return 0;
    expect = (void *)(g_base + vft_rva);
    __try {
        child = *(ActorPtr *)(root + kOffActorChild);
        while (child) {
            hit = find_descendant_by_vftable_rec(child, expect, 1, max_depth,
                                                &guard);
            if (hit)
                return hit;
            child = *(ActorPtr *)(child + kOffActorSibling);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static ActorPtr find_csa_child(ActorPtr gp)
{
    return find_descendant_by_vftable(gp, g_rva_csa_vftable, 1);
}

static ActorPtr find_option_icon(ActorPtr gp)
{
    /* GPA → DanceOptionIcon → OptionIcon (depth 2). */
    return find_descendant_by_vftable(gp, g_rva_option_icon_vftable, 3);
}

static void cache_csa_for_player(int player, ActorPtr gp)
{
    if (player < 0 || player > 1 || !gp)
        return;
    /* Always replace: never retain a stale CSA after a miss. */
    g_csa[player] = find_csa_child(gp);
}

static void cache_option_icon_for_player(int player, ActorPtr gp)
{
    if (player < 0 || player > 1 || !gp)
        return;
    /* Always replace: never retain a stale OptionIcon after a miss. */
    g_option_icon[player] = find_option_icon(gp);
}

/* Forward: exact scroll re-apply after graphic refresh (defined later). */
static void reapply_exact_scroll(int player);
static void apply_constant_gate(int player);

/*
 * Force the on-screen speed_x%03d graphic via OptionIconActor (not CSA).
 * Write nearest index into OptionIcon+0x60+0x0C, then full rebuild 0x2B630
 * (GetHispeed only). Fall back to live 4165 if rebuild is unavailable.
 */
static void refresh_speed_visual(int player, int index)
{
    ActorPtr gp;
    ActorPtr icon;
    struct {
        int index;
        float xmod;
    } payload;

    if (player < 0 || player > 1)
        return;
    index = clamp_i(index, 0, 31);
    payload.index = index;
    payload.xmod = (float)(index + 1) * 0.25f;

    gp = g_gameplay[player];
    if (!gp || !gameplay_alive(gp))
        return;

    cache_option_icon_for_player(player, gp);

    icon = g_option_icon[player];
    if (!icon || !option_icon_alive(icon)) {
        g_option_icon[player] = 0;
        icon = find_option_icon(gp);
        g_option_icon[player] = icon;
    }
    if (!icon)
        return;

    /* Embed must hold nearest HUD index before GetHispeed-based rebuild. */
    __try {
        *(int *)(icon + (int)kOffOptionIconOption + (int)kOffOptionHispeed) =
            index;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_option_icon[player] = 0;
        return;
    }

    if (g_option_icon_refresh) {
        __try {
            g_option_icon_refresh(icon);
            goto visual_done;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_option_icon[player] = 0;
        }
    }
    if (g_option_icon_live) {
        __try {
            g_option_icon_live(icon, kEvHispeed, &payload);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_option_icon[player] = 0;
        }
    }
visual_done:
    /* Graphic paths use snapped xmod; keep scroll on exact M-Mod floats. */
    reapply_exact_scroll(player);
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

static int player_held3(ArkIo3Fn fn, unsigned int player)
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
    return held ? 1 : 0;
}

static ArkIo3Fn ark_io_fn(uintptr_t rva)
{
    ArkIo3Fn fn;
    if (!g_base || !rva)
        return NULL;
    __try {
        fn = *(ArkIo3Fn *)(g_base + rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (!ptr_ok((void *)(uintptr_t)fn))
        return NULL;
    return fn;
}

static int packed_io_bit(unsigned int player, int bit)
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
        /* dword2 = held. dword1 = trigger (one frame). */
        if (slot[2] & mask)
            return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static int io_held(unsigned int player, uintptr_t rva, int bit)
{
    if (packed_io_bit(player, bit))
        return 1;
    return player_held3(ark_io_fn(rva), player);
}

static int menu_left_held(unsigned int player)
{
    return io_held(player, g_rva_get_menu_left, kIoBitMenuLeft);
}

static int menu_right_held(unsigned int player)
{
    return io_held(player, g_rva_get_menu_right, kIoBitMenuRight);
}

static int start_held(unsigned int player)
{
    return io_held(player, g_rva_get_start, kIoBitStart);
}

/* Either side - matches retry_exit, so cab START always suppresses nudge. */
static int any_start_held(void)
{
    return start_held(0) || start_held(1);
}

/* Forward decls - defined later. */
static int player_for_option(ActorPtr opt);
static void apply_mmod_to_option(ActorPtr opt, const char *why);
static void maybe_reset_live_m_bpm(const char *why);

static int m_bpm_for_player(int player)
{
    if (player < 0 || player > 1)
        return g_m_bpm_ini[0];
    return g_m_bpm_live[player];
}

static int any_gameplay_alive(void)
{
    int p;
    int alive = 0;
    for (p = 0; p < 2; p++) {
        if (!g_gameplay[p])
            continue;
        if (gameplay_alive(g_gameplay[p]))
            alive = 1;
        else
            gameplay_slot_clear(p);
    }
    return alive;
}

static void online_hud_reset(void)
{
    int p;
    for (p = 0; p < 2; p++) {
        g_seen_live_bpm[p] = 0;
        g_display_live_bpm[p] = 0;
        g_display_effective[p] = 0;
        g_chart_bpm_hud[p] = 0;
    }
    g_play_core = 0;
    g_play_max = 0;
    g_play_min = 0;
}

static void remember_chart_bpm(int player, int chart_bpm)
{
    if (player < 0 || player > 1)
        return;
    if (chart_bpm > 0)
        g_chart_bpm_hud[player] = chart_bpm;
}

/*
 * Round to nearest integer with the same parity as the chart core (odd core ->
 * odd BPM, even core -> even BPM). Absorbs typical +/-1 float flicker while
 * keeping charts like 155 accurate.
 */
static int round_parity_bpm(float f, int want_odd)
{
    int n;
    if (f < 0.5f)
        return 0;
    n = (int)(f + 0.5f);
    if (want_odd) {
        if ((n & 1) == 0) {
            if ((float)n + 0.5f <= f)
                n++;
            else
                n--;
            if (n < 1)
                n = 1;
        }
    } else {
        if (n & 1) {
            if ((float)n + 0.5f <= f)
                n++;
            else
                n--;
            if (n < 0)
                n = 0;
        }
    }
    return n;
}

static int round_parity_i(int v, int want_odd)
{
    if (v <= 0)
        return want_odd ? 1 : 0;
    return round_parity_bpm((float)v, want_odd);
}

/*
 * Hold displayed BPM unless |delta| > 2. Same-parity neighbors are 2 apart, so
 * a single step of noise stays held; real tempo changes (>= 3) still update.
 */
static int stabilize_parity(int *held, int cand)
{
    int d;
    if (cand <= 0) {
        *held = 0;
        return 0;
    }
    if (*held <= 0) {
        *held = cand;
        return cand;
    }
    d = cand - *held;
    if (d < 0)
        d = -d;
    if (d <= 2)
        return *held;
    *held = cand;
    return cand;
}

struct OnlineSide {
    int alive;
    int chart_ref;
    int m_bpm;
    int live_bpm;
    int effective;
};

static void online_side_sample(int player, struct OnlineSide *out)
{
    float live_f;
    int want_odd;
    ActorPtr gp;

    memset(out, 0, sizeof(*out));
    if (player < 0 || player > 1)
        return;
    gp = g_gameplay[player];
    if (!gp || !gameplay_alive(gp))
        return;
    out->chart_ref = g_chart_bpm_hud[player];
    out->m_bpm = g_m_bpm_live[player];
    if (out->chart_ref <= 0)
        return;
    out->alive = 1;

    live_f = 0.0f;
    if (kOffGpLiveBpm) {
        __try {
            live_f = *(float *)(gp + kOffGpLiveBpm);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            live_f = 0.0f;
        }
    }

    want_odd = out->chart_ref & 1;

    /* STOP after we have seen a real tempo. */
    if (g_seen_live_bpm[player] && live_f < 0.5f) {
        g_display_live_bpm[player] = 0;
        g_display_effective[player] = 0;
        out->live_bpm = 0;
        out->effective = 0;
        return;
    }

    if (live_f >= 0.5f) {
        g_seen_live_bpm[player] = 1;
        if (live_f > (float)kBpmClampHi)
            live_f = (float)kBpmClampHi;
        out->live_bpm = stabilize_parity(&g_display_live_bpm[player],
                                         round_parity_bpm(live_f, want_odd));
        out->effective = (int)(((long long)out->live_bpm * out->m_bpm +
                                out->chart_ref / 2) /
                               out->chart_ref);
        if (out->effective < 0)
            out->effective = 0;
        out->effective = stabilize_parity(
            &g_display_effective[player],
            round_parity_i(out->effective, out->m_bpm & 1));
        return;
    }

    /* Intro: show chart core + current mmod target. */
    out->live_bpm = out->chart_ref;
    out->effective = out->m_bpm;
}

/*
 * ONLINE label while playing (single shared stock slot):
 *   one side:  "150 BPM (600)"
 *   both same live: "150 BPM (600 / 660)"
 *   both different live: "150 BPM (600) / 300 BPM (660)"
 * With constant=1, append " C####" (or " C####/C####" if sides differ).
 * Redraw PASELI-white after stock green ONLINE paint.
 */
static void refresh_online_text_buffer(void)
{
    struct OnlineSide side[2];
    int n = 0;
    int a = -1;
    char base[56];

    if (!g_online_hud) {
        memcpy(g_online_text, STOCK_ONLINE, STOCK_ONLINE_LEN);
        return;
    }
    if (!g_enabled) {
        memcpy(g_online_text, STOCK_ONLINE, STOCK_ONLINE_LEN);
        return;
    }

    online_side_sample(0, &side[0]);
    online_side_sample(1, &side[1]);
    if (side[0].alive) {
        a = 0;
        n++;
    }
    if (side[1].alive) {
        if (a < 0)
            a = 1;
        n++;
    }

    if (n <= 0) {
        memcpy(g_online_text, STOCK_ONLINE, STOCK_ONLINE_LEN);
        return;
    }

    if (n == 1) {
        _snprintf_s(base, sizeof(base), _TRUNCATE, "%3d BPM (%d)",
                    side[a].live_bpm, side[a].effective);
    } else if (side[0].live_bpm == side[1].live_bpm) {
        /* Both alive - always 1P then 2P. */
        _snprintf_s(base, sizeof(base), _TRUNCATE, "%3d BPM (%d / %d)",
                    side[0].live_bpm, side[0].effective, side[1].effective);
    } else {
        _snprintf_s(base, sizeof(base), _TRUNCATE,
                    "%3d BPM (%d) / %3d BPM (%d)", side[0].live_bpm,
                    side[0].effective, side[1].live_bpm, side[1].effective);
    }

    if (!g_constant) {
        _snprintf_s(g_online_text, sizeof(g_online_text), _TRUNCATE, "%s", base);
        return;
    }

    if (n == 1) {
        _snprintf_s(g_online_text, sizeof(g_online_text), _TRUNCATE, "%s C%d%s",
                    base, g_constant_ms_live[a],
                    g_nudge_edit_constant[a] ? "*" : "");
    } else if (g_constant_ms_live[0] == g_constant_ms_live[1]) {
        _snprintf_s(g_online_text, sizeof(g_online_text), _TRUNCATE, "%s C%d%s",
                    base, g_constant_ms_live[0],
                    (g_nudge_edit_constant[0] || g_nudge_edit_constant[1])
                        ? "*"
                        : "");
    } else {
        _snprintf_s(g_online_text, sizeof(g_online_text), _TRUNCATE,
                    "%s C%d%s/C%d%s", base, g_constant_ms_live[0],
                    g_nudge_edit_constant[0] ? "*" : "", g_constant_ms_live[1],
                    g_nudge_edit_constant[1] ? "*" : "");
    }
}

static int online_text_is_mmod(void)
{
    /* Stock is STOCK_ONLINE (" ONLINE" / "ONLINE"); mmod is "NNN BPM (...)". */
    return g_online_text[0] && strcmp(g_online_text, STOCK_ONLINE) != 0;
}

/* Stock UI text wrapper used by bottom ONLINE / PASELI labels. */
static void *ui_text_wrapper(uintptr_t slot_rva)
{
    void *wrapper = NULL;
    if (!g_base || !slot_rva)
        return NULL;
    __try {
        wrapper = *(void **)(g_base + slot_rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wrapper = NULL;
    }
    return wrapper;
}

static void ui_text_set_color(uintptr_t slot_rva, float r, float g, float b,
                              float a)
{
    void *wrapper;
    void **vt;
    float rgba[4];

    wrapper = ui_text_wrapper(slot_rva);
    if (!wrapper)
        return;
    rgba[0] = r;
    rgba[1] = g;
    rgba[2] = b;
    rgba[3] = a;
    __try {
        vt = *(void ***)wrapper;
        if (!vt || !vt[3])
            return;
#ifdef _WIN64
        ((void (__fastcall *)(void *, float *))vt[3])(wrapper, rgba);
#else
        ((void (__thiscall *)(void *, float *))vt[3])(wrapper, rgba);
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ui_text_draw(uintptr_t slot_rva, const char *text)
{
    void *wrapper;
    void *mid;
    void *obj;
    void **vt;

    if (!text)
        return;
    wrapper = ui_text_wrapper(slot_rva);
    if (!wrapper)
        return;
    __try {
        mid = *(void **)((char *)wrapper + kOffUiTextMid);
        if (!mid)
            return;
        obj = *(void **)mid;
        if (!obj)
            return;
        vt = *(void ***)obj;
        if (!vt || !vt[2])
            return;
#ifdef _WIN64
        ((void (__fastcall *)(void *, const char *, unsigned))vt[2])(obj, text,
                                                                     1);
#else
        ((void (__thiscall *)(void *, const char *, unsigned))vt[2])(obj, text,
                                                                     1);
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int patch_online_lea(void)
{
    unsigned char *site;
    DWORD old_prot;
    DWORD tmp;
#ifdef _WIN64
    intptr_t rel;
#endif

    if (!g_base || !g_rva_online_lea)
        return 0;
    if (g_online_lea_patched)
        return 1;

    __try {
        site = (unsigned char *)(g_base + g_rva_online_lea);
        if (!bytes_match(site, kOnlineLeaPrologue, sizeof(kOnlineLeaPrologue))) {
            log_msg_f("ONLINE patch prologue mismatch at %X",
                      (unsigned)g_rva_online_lea);
            return 0;
        }
#ifdef _WIN64
        rel = (intptr_t)g_online_text - (intptr_t)(site + 7);
        if (rel != (intptr_t)(int)rel) {
            log_msg_f("ONLINE lea reloc out of range");
            return 0;
        }
        if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old_prot)) {
            log_msg_f("ONLINE lea VirtualProtect failed");
            return 0;
        }
        g_online_lea_saved[0] = site[3];
        g_online_lea_saved[1] = site[4];
        g_online_lea_saved[2] = site[5];
        g_online_lea_saved[3] = site[6];
        site[3] = (unsigned char)(rel & 0xFF);
        site[4] = (unsigned char)((rel >> 8) & 0xFF);
        site[5] = (unsigned char)((rel >> 16) & 0xFF);
        site[6] = (unsigned char)((rel >> 24) & 0xFF);
        VirtualProtect(site, 7, old_prot, &tmp);
        FlushInstructionCache(GetCurrentProcess(), site, 7);
#else
        /* push imm32 - rewrite absolute address to our buffer. */
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old_prot)) {
            log_msg_f("ONLINE push VirtualProtect failed");
            return 0;
        }
        memcpy(g_online_lea_saved, site + 1, 4);
        {
            uintptr_t abs = (uintptr_t)g_online_text;
            site[1] = (unsigned char)(abs & 0xFF);
            site[2] = (unsigned char)((abs >> 8) & 0xFF);
            site[3] = (unsigned char)((abs >> 16) & 0xFF);
            site[4] = (unsigned char)((abs >> 24) & 0xFF);
        }
        VirtualProtect(site, 5, old_prot, &tmp);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
#endif
        g_online_lea_patched = 1;
        log_msg_f("ONLINE patched %X -> g_online_text",
                  (unsigned)g_rva_online_lea);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_online_lea_patched = 0;
        log_msg_f("ONLINE patch fault at %X", (unsigned)g_rva_online_lea);
        return 0;
    }
}

static void restore_online_lea(void)
{
    unsigned char *site;
    DWORD old_prot;
    DWORD tmp;
#ifdef _WIN64
    size_t nbytes = 7;
#else
    size_t nbytes = 5;
#endif

    if (!g_online_lea_patched || !g_base || !g_rva_online_lea)
        return;

    __try {
        site = (unsigned char *)(g_base + g_rva_online_lea);
        if (!VirtualProtect(site, (DWORD)nbytes, PAGE_EXECUTE_READWRITE,
                            &old_prot)) {
            log_msg_f("ONLINE restore VirtualProtect failed");
            g_online_lea_patched = 0;
            return;
        }
#ifdef _WIN64
        site[3] = g_online_lea_saved[0];
        site[4] = g_online_lea_saved[1];
        site[5] = g_online_lea_saved[2];
        site[6] = g_online_lea_saved[3];
#else
        memcpy(site + 1, g_online_lea_saved, 4);
#endif
        VirtualProtect(site, (DWORD)nbytes, old_prot, &tmp);
        FlushInstructionCache(GetCurrentProcess(), site, nbytes);
        g_online_lea_patched = 0;
        memcpy(g_online_text, STOCK_ONLINE, STOCK_ONLINE_LEN);
        log_msg_f("ONLINE restored at %X", (unsigned)g_rva_online_lea);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_online_lea_patched = 0;
    }
}

/*
 * Before stock draws ONLINE: drop dead GamePlayActor slots and rewrite the
 * buffer so result / music select show stock " ONLINE" immediately.
 */
#ifdef _WIN64
static void detour_status_hud(void)
#else
static int detour_status_hud(void)
#endif
{
    int p;
#ifndef _WIN64
    int result = 0;
#endif

    for (p = 0; p < 2; p++) {
        if (g_gameplay[p] && !gameplay_alive(g_gameplay[p]))
            gameplay_slot_clear(p);
    }
    if (!g_gameplay[0] && !g_gameplay[1]) {
        online_hud_reset();
    }
    refresh_online_text_buffer();
    if (g_orig_status_hud)
#ifdef _WIN64
        g_orig_status_hud();
#else
        result = g_orig_status_hud();
#endif
    /*
     * Stock paints ONLINE with the green modulate. PASELI uses white. Redraw
     * our mmod string in white so "BPM" matches PASELI (slot is one color).
     */
    if (g_online_hud && online_text_is_mmod() && g_rva_ui_text_online) {
        ui_text_set_color(g_rva_ui_text_online, 1.0f, 1.0f, 1.0f, 1.0f);
        ui_text_draw(g_rva_ui_text_online, g_online_text);
    }
#ifndef _WIN64
    return result;
#endif
}

static void nudge_reset_buttons(int player)
{
    if (player < 0 || player > 1)
        return;
    g_nudge_left_down[player] = 0;
    g_nudge_right_down[player] = 0;
    g_nudge_next_ms[player] = 0;
    g_nudge_chord_down[player] = 0;
}

static unsigned int mcode_from_game(void)
{
    ActorPtr game;
    if (!g_base || !g_rva_game)
        return 0;
    __try {
        game = *(ActorPtr *)(g_base + g_rva_game);
        if (!game)
            return 0;
        return *(unsigned int *)(game + kOffGameMcode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static unsigned int mcode_from_player(int player)
{
    void *po = player_obj(player);
    if (!po)
        return 0;
    __try {
        return *(unsigned int *)((char *)po + g_off_player_music);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static unsigned int mcode_from_player0(void)
{
    return mcode_from_player(0);
}

static int player_for_option(ActorPtr opt)
{
    int p;

    if (!opt)
        return -1;

    /* Canonical embedded Option on the player object. */
    for (p = 0; p < 2; p++) {
        void *po = player_obj(p);
        if (!po)
            continue;
        __try {
            if ((ActorPtr)((char *)po + g_off_player_option) == opt)
                return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    /* ControlSpeedActor flattened Option copy. */
    for (p = 0; p < 2; p++) {
        ActorPtr csa = g_csa[p];
        if (!csa || !csa_alive(csa)) {
            if (g_gameplay[p] && gameplay_alive(g_gameplay[p])) {
                csa = find_csa_child(g_gameplay[p]);
                if (csa)
                    g_csa[p] = csa;
            }
        }
        if (!csa)
            continue;
        __try {
            if (csa + (int)kOffCsaOption == opt)
                return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    /* OptionIconActor Option embed (SPEED graphic GetHispeed). */
    for (p = 0; p < 2; p++) {
        ActorPtr icon = g_option_icon[p];
        if (!icon || !option_icon_alive(icon)) {
            if (g_gameplay[p] && gameplay_alive(g_gameplay[p])) {
                icon = find_option_icon(g_gameplay[p]);
                if (icon)
                    g_option_icon[p] = icon;
            }
        }
        if (!icon)
            continue;
        __try {
            if (icon + (int)kOffOptionIconOption == opt)
                return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    return -1;
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* WORLD CONSTANT display time: 100–3000 ms, snapped to 10 ms. */
static int snap_constant_ms(int ms)
{
    ms = clamp_i(ms, 100, 3000);
    ms = ((ms + 5) / 10) * 10;
    return clamp_i(ms, 100, 3000);
}

static int round_bpm(double bpm)
{
    return (int)floorf((float)bpm + 0.5f);
}

/* A3 index: 0 = 0.25x ... 31 = 8.00x. World hundredths snap to 0.25. */
static int hundredths_to_index(int hundredths)
{
    hundredths = clamp_i(hundredths, 25, 800);
    hundredths = (hundredths / 25) * 25;
    return hundredths / 25 - 1;
}

static int index_to_hundredths(int index)
{
    index = clamp_i(index, 0, 31);
    return (index + 1) * 25;
}

static int read_u16(const void *base, uintptr_t off, unsigned int *out)
{
    __try {
        *out = *(unsigned short *)((const char *)base + off);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void cache_put(unsigned int mcode, int diff_idx, int core, int max_bpm,
                      int min_bpm)
{
    int i;
    static int logged_full;

    EnterCriticalSection(&g_cache_cs);
    for (i = 0; i < g_cache_count; i++) {
        if (g_cache[i].mcode == mcode && g_cache[i].diff_idx == diff_idx) {
            g_cache[i].core = core;
            g_cache[i].max_bpm = max_bpm;
            g_cache[i].min_bpm = min_bpm;
            LeaveCriticalSection(&g_cache_cs);
            return;
        }
    }
    if (g_cache_count < CORE_CACHE_MAX) {
        g_cache[g_cache_count].mcode = mcode;
        g_cache[g_cache_count].diff_idx = diff_idx;
        g_cache[g_cache_count].core = core;
        g_cache[g_cache_count].max_bpm = max_bpm;
        g_cache[g_cache_count].min_bpm = min_bpm;
        g_cache_count++;
    } else if (!logged_full) {
        logged_full = 1;
        LeaveCriticalSection(&g_cache_cs);
        log_msg_f("core cache full (%d) - further new entries dropped",
                  CORE_CACHE_MAX);
        return;
    }
    LeaveCriticalSection(&g_cache_cs);
}

static int cache_get(unsigned int mcode, int diff_idx, int *core, int *max_bpm, int *min_bpm)
{
    int i;
    int found = 0;
    EnterCriticalSection(&g_cache_cs);
    for (i = 0; i < g_cache_count; i++) {
        if (g_cache[i].mcode == mcode && g_cache[i].diff_idx == diff_idx) {
            *core = g_cache[i].core;
            *max_bpm = g_cache[i].max_bpm;
            *min_bpm = g_cache[i].min_bpm;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_cache_cs);
    return found;
}

/* Recover Core when (mcode,diff) missed - Mac often caches under a wrong key. */
static int cache_get_by_max(unsigned int bpm_max, unsigned int bpm_min,
                            int *core, int *max_bpm, int *min_bpm)
{
    int i;
    int found = 0;

    if (bpm_max == 0)
        return 0;
    EnterCriticalSection(&g_cache_cs);
    for (i = 0; i < g_cache_count; i++) {
        if (g_cache[i].core <= 0)
            continue;
        if ((unsigned)g_cache[i].max_bpm != bpm_max)
            continue;
        if (bpm_min > 0 && g_cache[i].min_bpm > 0 &&
            (unsigned)g_cache[i].min_bpm != bpm_min)
            continue;
        *core = g_cache[i].core;
        *max_bpm = g_cache[i].max_bpm;
        *min_bpm = g_cache[i].min_bpm;
        found = 1;
        break;
    }
    LeaveCriticalSection(&g_cache_cs);
    return found;
}

static void dur_add(DurBucket *buckets, int *n, int bpm, int duration)
{
    int i;
    if (duration <= 0)
        return;
    /* Fold +/-1 derive noise into an existing bucket. */
    for (i = 0; i < *n; i++) {
        int d = buckets[i].bpm - bpm;
        if (d < 0)
            d = -d;
        if (d <= 1) {
            buckets[i].duration += duration;
            return;
        }
    }
    if (*n < DUR_BUCKET_MAX) {
        buckets[*n].bpm = bpm;
        buckets[*n].duration = duration;
        (*n)++;
    }
}

/* World-style chart index: SP = difficulty, DP = difficulty + 5. */
static int chart_diff_idx(int difficulty, int is_dp)
{
    if (is_dp)
        return difficulty + 5;
    return difficulty;
}

/* Also store Core under each active player's music/diff (fixes Mac key miss). */
static void cache_alias_to_players(int core, int max_bpm, int min_bpm)
{
    int p;

    for (p = 0; p < 2; p++) {
        void *po = player_obj(p);
        unsigned int mcode = 0;
        unsigned int diff = 0;
        unsigned int style = 0;
        int diff_idx;

        if (!po)
            continue;
        __try {
            mcode = *(unsigned int *)((char *)po + g_off_player_music);
            diff = *(unsigned int *)((char *)po + g_off_player_diff);
            if (g_off_player_style)
                style = *(unsigned int *)((char *)po + g_off_player_style);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!mcode_plausible(mcode))
            continue;
        diff_idx = chart_diff_idx((int)diff, style != 0);
        cache_put(mcode, diff_idx, core, max_bpm, min_bpm);
        if (diff_idx != (int)diff)
            cache_put(mcode, (int)diff, core, max_bpm, min_bpm);
    }
}

/*
 * World sub_1801B7460 duration-weighted Core, adapted to A3 76-byte notes.
 * type 0 = panel note; type 0x80 = BPM event.
 * BPM double is not at +16 on A3 (panels); recompute like sub_18011CAD0:
 *   bpm = (tick_delta * 0.0009765625 * 60) / (ms_delta / 1000)
 * after note_parse has normalized +8 to millisecond-scale.
 */
static int compute_core_bpm(const char *begin, const char *end, int *out_core,
                            int *out_max, int *out_min)
{
    DurBucket buckets[DUR_BUCKET_MAX];
    int n_buckets = 0;
    int notes = 0;
    int last_time = 0;
    double last_bpm = 0.0;
    int has_bpm_event = 0;
    int prev_tick = 0;
    int prev_ms = 0;
    int has_prev_ev = 0;
    int max_note_time = 0;
    double bpm_min = 1.0e300;
    double bpm_max = 0.0;
    int i;
    int best_dur = -1;
    int core = 0;
    const char *p;
    int stride = g_note_stride;

    *out_core = 0;
    *out_max = 0;
    *out_min = 0;

    if (!begin || !end || end <= begin || stride <= 0)
        return 0;

    __try {
        for (p = begin; p < end; p += stride) {
            unsigned char type;
            int tick;
            int ms;
            int note_i = (int)((p - begin) / stride);

            if (note_i >= kMaxNotesWalk)
                break;

            type = *(const unsigned char *)(p + g_off_note_type);
            tick = *(const int *)(p + g_off_note_time);
            ms = *(const int *)(p + g_off_note_ms);

            if (type == 0) {
                notes++;
                if (tick > max_note_time)
                    max_note_time = tick;
            }

            if (type == 0x80) {
                double bpm = 0.0;
                if (has_prev_ev) {
                    int td = tick - prev_tick;
                    int md = ms - prev_ms;
                    if (md != 0)
                        bpm = ((double)td * 0.0009765625 * 60.0) /
                              ((double)md / 1000.0);
                }

                if (bpm < kBpmClampLo || bpm > kBpmClampHi)
                    bpm = 0.0;

                if (fabs(bpm) >= 0.01) {
                    int rbpm = round_bpm(bpm);
                    if (notes > 0) {
                        if (bpm < bpm_min)
                            bpm_min = bpm;
                        if (bpm > bpm_max)
                            bpm_max = bpm;
                        dur_add(buckets, &n_buckets, rbpm, tick - last_time);
                    }
                }

                last_time = tick;
                last_bpm = bpm;
                notes = 0;
                has_bpm_event = 1;
                prev_tick = tick;
                prev_ms = ms;
                has_prev_ev = 1;
            }
        }

        /* World epilogue: attribute trailing notes to last BPM. */
        if (has_bpm_event && max_note_time > last_time &&
            last_bpm >= kBpmClampLo && last_bpm <= kBpmClampHi) {
            if (last_bpm < bpm_min)
                bpm_min = last_bpm;
            if (last_bpm > bpm_max)
                bpm_max = last_bpm;
            dur_add(buckets, &n_buckets, round_bpm(last_bpm),
                    max_note_time - last_time);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    for (i = 0; i < n_buckets; i++) {
        if (buckets[i].duration > best_dur) {
            best_dur = buckets[i].duration;
            core = buckets[i].bpm;
        }
    }

    if (core <= 0)
        return 0;

    *out_core = core;
    if (bpm_max > 0.0)
        *out_max = round_bpm(bpm_max);
    if (bpm_min < 1.0e300)
        *out_min = round_bpm(bpm_min);
    return 1;
}

/*
 * Real music IDs are small (e.g. 37288). Radar call site 0xE643C used to
 * misread analyzer stack slots and cache under values like 56666496.
 */
static int mcode_plausible(unsigned int mcode)
{
    return mcode > 0 && mcode < 1000000u;
}

/*
 * Analyzer call site only (ret == g_rva_analyze_parse_ret):
 *   stack: v73 at ret_slot+0x58 (3*slot), v74 at +0x60 (song list)
 *   mcode = *(uint32 *)(v74 + 4*v73 + 8)
 * Do not use RtlCaptureContext ? /O2 and memset already clobber rbx/r15.
 */
static int ptr_user_canonical(uintptr_t p)
{
    /* Reject null, low, and non-user pointers. */
    if (p < 0x10000)
        return 0;
#ifdef _WIN64
    if (p > 0x00007FFFFFFFFFFFULL)
        return 0;
#else
    if (p > 0x7FFEFFFFUL)
        return 0;
#endif
    return 1;
}

static int analyzer_mcode_from_stack(void *ret_slot, unsigned int *out_mcode)
{
#ifdef _WIN64
    uintptr_t v74 = 0;
    uintptr_t v73 = 0;
    unsigned int mc;

    *out_mcode = 0;
    if (!ret_slot)
        return 0;
    __try {
        v74 = *(uintptr_t *)((char *)ret_slot + 0x60);
        v73 = *(uintptr_t *)((char *)ret_slot + 0x58);
        if (!ptr_user_canonical(v74))
            return 0;
        if ((intptr_t)v73 < 0 || v73 > 3 * 64 || (v73 % 3) != 0)
            return 0;
        mc = *(unsigned int *)(v74 + 4 * v73 + 8);
        if (!mcode_plausible(mc))
            return 0;
        *out_mcode = mc;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#else
    /* 32-bit analyze frame layout differs; fall back to game/player mcode. */
    (void)ret_slot;
    *out_mcode = 0;
    return 0;
#endif
}

/*
 * Only cache when mcode belongs to the chart being parsed:
 *   - SSQ analyzer (ret 0x2A1AF): song-list stack formula
 *   - GamePlayActor ctor (ret 0x3AA4A): game/player selected song
 * Music-select radar must never attribute under game/player.
 */
static unsigned int resolve_note_parse_mcode(uintptr_t ret, void *ret_slot)
{
    unsigned int mcode = 0;
    uintptr_t ret_rva = g_base ? (ret - g_base) : 0;

    if (ret_rva == g_rva_radar_parse_ret)
        return 0;

    if (ret_rva == g_rva_analyze_parse_ret)
        analyzer_mcode_from_stack(ret_slot, &mcode);

    if (ret_rva == g_rva_gameplay_parse_ret ||
        (ret_rva == g_rva_analyze_parse_ret && !mcode_plausible(mcode))) {
        if (!mcode_plausible(mcode))
            mcode = mcode_from_game();
        if (!mcode_plausible(mcode))
            mcode = mcode_from_player(0);
        if (!mcode_plausible(mcode))
            mcode = mcode_from_player(1);
    }

    if (!mcode_plausible(mcode))
        return 0;
    return mcode;
}

static void cache_from_notes(char **notes, unsigned int mcode,
                             unsigned int style_pass, int difficulty,
                             int from_gameplay_parse)
{
    char *begin;
    char *end;
    int core = 0, max_bpm = 0, min_bpm = 0;
    int diff_idx;

    if (!notes || !mcode_plausible(mcode))
        return;
    __try {
        begin = notes[0];
        end = notes[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!begin || !end || end <= begin)
        return;

    diff_idx = chart_diff_idx(difficulty, style_pass != 0);

    if (!compute_core_bpm(begin, end, &core, &max_bpm, &min_bpm)) {
        log_msg("analyze mcode=%u diff_idx=%d: no Core (notes=%d)",
                mcode, diff_idx, (int)((end - begin) / g_note_stride));
        return;
    }

    cache_put(mcode, diff_idx, core, max_bpm, min_bpm);
    /* Alias under live player music IDs - parse mcode can be wrong on Wine. */
    cache_alias_to_players(core, max_bpm, min_bpm);
    if (from_gameplay_parse && core > 0) {
        g_play_core = core;
        g_play_max = max_bpm;
        g_play_min = min_bpm;
    }
    log_msg_f("analyze mcode=%u diff_idx=%d core=%d max=%d min=%d notes=%d%s",
              mcode, diff_idx, core, max_bpm, min_bpm,
              (int)((end - begin) / g_note_stride),
              from_gameplay_parse ? " (gameplay)" : "");
}

/*
 * Chart BPM: Core from SSQ cache, else Max, else Min, else 0 (1.00x).
 */
/*
 * Chart BPM for a known side. Does not fall back to P1 when player is invalid.
 */
static int chart_bpm_for_player(int player, int *out_bpm, const char **out_src,
                                unsigned int *out_max, unsigned int *out_min)
{
    void *po;
    unsigned int mcode = 0;
    unsigned int diff = 0;
    unsigned int style = 0;
    void *music = NULL;
    unsigned int bpm_max = 0;
    unsigned int bpm_min = 0;
    int core = 0, cmax = 0, cmin = 0;
    int diff_idx;
    const char *how = NULL;

    *out_bpm = 0;
    *out_src = "none";
    *out_max = 0;
    *out_min = 0;

    if (player < 0 || player > 1)
        return 0;
    po = player_obj(player);
    if (!po)
        return 0;

    __try {
        mcode = *(unsigned int *)((char *)po + g_off_player_music);
        diff = *(unsigned int *)((char *)po + g_off_player_diff);
        if (g_off_player_style)
            style = *(unsigned int *)((char *)po + g_off_player_style);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (!g_music_lookup || mcode == 0)
        return 0;

    diff_idx = chart_diff_idx((int)diff, style != 0);

    if (cache_get(mcode, diff_idx, &core, &cmax, &cmin) && core > 0)
        how = "core";
    else if (diff_idx != (int)diff &&
             cache_get(mcode, (int)diff, &core, &cmax, &cmin) && core > 0)
        how = "core";
    else if (cache_get(mcode, diff_idx == (int)diff ? diff + 5 : diff_idx,
                       &core, &cmax, &cmin) &&
             core > 0)
        how = "core";

    if (how) {
        *out_bpm = core;
        *out_src = how;
        *out_max = (unsigned)(cmax > 0 ? cmax : core);
        *out_min = (unsigned)(cmin > 0 ? cmin : core);
        return 1;
    }

    __try {
        music = g_music_lookup(mcode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!music)
        return 0;

    if (!read_u16(music, g_off_bpmmax, &bpm_max))
        return 0;
    read_u16(music, g_off_bpmmin, &bpm_min);
    *out_max = bpm_max;
    *out_min = bpm_min;

    /* Mac: Core was cached under a different mcode - match by bpmmax/min. */
    if (cache_get_by_max(bpm_max, bpm_min, &core, &cmax, &cmin) && core > 0) {
        cache_put(mcode, diff_idx, core, cmax, cmin);
        *out_bpm = core;
        *out_src = "core";
        *out_max = (unsigned)(cmax > 0 ? cmax : core);
        *out_min = (unsigned)(cmin > 0 ? cmin : core);
        return 1;
    }
    if (g_play_core > 0 && g_play_max > 0 &&
        (unsigned)g_play_max == bpm_max &&
        (bpm_min == 0 || g_play_min <= 0 ||
         (unsigned)g_play_min == bpm_min)) {
        cache_put(mcode, diff_idx, g_play_core, g_play_max, g_play_min);
        *out_bpm = g_play_core;
        *out_src = "core";
        *out_max = (unsigned)g_play_max;
        *out_min = (unsigned)(g_play_min > 0 ? g_play_min : g_play_core);
        return 1;
    }

    if (bpm_max > 0) {
        *out_bpm = (int)bpm_max;
        *out_src = "max";
        return 1;
    }
    if (bpm_min > 0) {
        *out_bpm = (int)bpm_min;
        *out_src = "min";
        return 1;
    }
    return 0;
}

/*
 * Exact Real Speed ratio for GamePlay floats (no 0.25 snap).
 * HUD index is nearest 0.25 step only (Option+0x0C assets).
 * player must be 0 or 1 - no P1 fallback for unknown sides.
 */
static float compute_mmod_speed_for_player(int player, int *out_index,
                                          int *out_bpm, const char **out_src)
{
    float speed;
    int chart_bpm = 0;
    unsigned int bpm_max = 0;
    unsigned int bpm_min = 0;
    const char *src = "none";
    double ratio;
    int m_bpm;

    *out_index = hundredths_to_index(100);
    *out_bpm = 0;
    *out_src = "fallback";

    if (player < 0 || player > 1)
        return 1.0f;

    if (!chart_bpm_for_player(player, &chart_bpm, &src, &bpm_max, &bpm_min) ||
        chart_bpm <= 0) {
        *out_src = "fallback";
        return 1.0f;
    }

    m_bpm = m_bpm_for_player(player);
    *out_bpm = chart_bpm;
    *out_src = src;
    ratio = (double)m_bpm / (double)chart_bpm;
    if (ratio < 0.25)
        ratio = 0.25;
    if (ratio > 8.0)
        ratio = 8.0;
    speed = (float)ratio;

    /* Nearest 0.25x for HUD only (Option+0x0C). */
    {
        int steps = (int)(speed * 4.0f + 0.5f);
        steps = clamp_i(steps, 1, 32);
        *out_index = steps - 1;
    }
    return speed;
}

/* Note scroll uses GamePlayActor floats, not Option hispeed field. */
static void apply_mmod_to_gameplay(ActorPtr gp, float speed, int index,
                                  const char *why)
{
    int prev_idx;
    float prev_from;
    int player;

    if (!gp)
        return;
    if (!gameplay_alive(gp)) {
        /* Stale/freed actor ? drop any matching cache slots. */
        for (player = 0; player < 2; player++) {
            if (g_gameplay[player] == gp)
                gameplay_slot_clear(player);
        }
        maybe_reset_live_m_bpm("GameplayStale");
        return;
    }
    if (speed < 0.25f)
        speed = 0.25f;
    if (speed > 8.0f)
        speed = 8.0f;
    if (index < 0)
        index = 0;
    if (index > 31)
        index = 31;
    __try {
        player = *(int *)(gp + kOffGpPlayer);
        if (player >= 0 && player < 2)
            gameplay_slot_set(player, gp);
        prev_idx = *(int *)(gp + kOffGpSpeedIdx);
        prev_from = *(float *)(gp + kOffGpSpeedFrom);
        *(float *)(gp + kOffGpSpeedFrom) = speed;
        *(float *)(gp + kOffGpSpeedTo) = speed;
        *(int *)(gp + kOffGpSpeedIdx) = index;
        if (prev_idx != index || prev_from < speed - 0.001f ||
            prev_from > speed + 0.001f) {
            log_msg("%s gameplay=%p float %.3f->%.3f hud_idx %d->%d",
                    why, (void *)gp, prev_from, speed, prev_idx, index);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (player = 0; player < 2; player++) {
            if (g_gameplay[player] == gp)
                gameplay_slot_clear(player);
        }
        maybe_reset_live_m_bpm("GameplayExcept");
    }
}

static void apply_mmod_gameplays_for_player(int player, float speed, int index,
                                            const char *why)
{
    ActorPtr gp;

    if (player < 0 || player > 1)
        return;
    gp = g_gameplay[player];
    if (gp)
        apply_mmod_to_gameplay(gp, speed, index, why);
}

/* Overwrite any stock snapped lerp with exact M-Mod floats. */
static void reapply_exact_scroll(int player)
{
    int index = 0;
    int chart_bpm = 0;
    const char *src = "none";
    float speed;

    if (player < 0 || player > 1 || !g_enabled || !g_ready)
        return;
    if (!g_gameplay[player] || !gameplay_alive(g_gameplay[player]))
        return;
    speed = compute_mmod_speed_for_player(player, &index, &chart_bpm, &src);
    apply_mmod_to_gameplay(g_gameplay[player], speed, index, "ReapplyExact");
}

/*
 * Force WORLD-style CONSTANT gate on the note-params object under GamePlay.
 * Does not touch M-Mod scroll floats. Stock GamePlay update may refresh these
 * from Option; we re-assert after that path each frame.
 */
static void apply_constant_gate(int player)
{
    ActorPtr gp;
    ActorPtr note;
    void *po;
    char *opt;
    int ms;

    if (player < 0 || player > 1 || !g_enabled || !g_ready || !g_constant)
        return;
    gp = g_gameplay[player];
    if (!gp || !gameplay_alive(gp))
        return;

    ms = snap_constant_ms(g_constant_ms_live[player]);
    g_constant_ms_live[player] = ms;

    po = player_obj(player);
    opt = NULL;
    if (po) {
        __try {
            opt = (char *)po + g_off_player_option;
            *(int *)(opt + (int)kOffOptionConstFlag) = 1;
            *(int *)(opt + (int)kOffOptionConstMs) = ms;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            opt = NULL;
        }
    }

    note = 0;
    __try {
        note = *(ActorPtr *)(gp + kOffGpNoteParams);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        note = 0;
    }
    if (!note || !ptr_ok((void *)note))
        return;

    __try {
        volatile int probe = *(int *)(note + kOffNoteConstFlag);
        (void)probe;
        *(int *)(note + kOffNoteConstFlag) = 1;
        *(int *)(note + kOffNoteConstMs) = ms;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gameplay_slot_clear(player);
    }
    (void)opt;
}

static void maybe_reset_live_m_bpm(const char *why)
{
    int p;
    int changed = 0;
    static volatile LONG resetting;

    if (any_gameplay_alive())
        return;

    /*
     * No live GamePlayActors: always restore stock ONLINE here. Previously we
     * only did this when m_bpm needed a reset, so an untouched song left the
     * BPM label stuck on result / music select.
     */
    online_hud_reset();
    refresh_online_text_buffer();

    /* Always drop chord/edit state when leaving play. */
    for (p = 0; p < 2; p++) {
        if (g_nudge_edit_constant[p]) {
            log_msg("%s reset P%d nudge -> m_bpm", why, p + 1);
            g_nudge_edit_constant[p] = 0;
        }
        nudge_reset_buttons(p);
    }

    /* Restore per-side INI m_bpm / constant_ms if a chart left live dirty. */
    if (g_m_bpm_live[0] == g_m_bpm_ini[0] && g_m_bpm_live[1] == g_m_bpm_ini[1] &&
        g_constant_ms_live[0] == g_constant_ms_ini[0] &&
        g_constant_ms_live[1] == g_constant_ms_ini[1])
        return;
    if (InterlockedCompareExchange(&resetting, 1, 0) != 0)
        return;

    __try {
        for (p = 0; p < 2; p++) {
            if (g_m_bpm_live[p] != g_m_bpm_ini[p]) {
                log_msg("%s reset P%d m_bpm %d -> ini %d", why, p + 1,
                        g_m_bpm_live[p], g_m_bpm_ini[p]);
                g_m_bpm_live[p] = g_m_bpm_ini[p];
                changed = 1;
            }
            if (g_constant_ms_live[p] != g_constant_ms_ini[p]) {
                log_msg("%s reset P%d constant_ms %d -> ini %d", why, p + 1,
                        g_constant_ms_live[p], g_constant_ms_ini[p]);
                g_constant_ms_live[p] = g_constant_ms_ini[p];
            }
        }
        if (changed) {
            for (p = 0; p < 2; p++) {
                void *po = player_obj(p);
                if (!po)
                    continue;
                __try {
                    apply_mmod_to_option(
                        (ActorPtr)((char *)po + g_off_player_option), why);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    } __finally {
        InterlockedExchange(&resetting, 0);
    }
}

static void nudge_constant_ms(int player, int delta, const char *why)
{
    int before;
    int after;

    if (player < 0 || player > 1 || !delta || !g_constant)
        return;
    before = g_constant_ms_live[player];
    after = snap_constant_ms(before + delta);
    if (after == before)
        return;
    g_constant_ms_live[player] = after;
    log_msg("%s P%d constant_ms %d -> %d (step %d)", why, player + 1, before,
            after, delta);
    apply_constant_gate(player);
    refresh_online_text_buffer();
}

static void nudge_m_bpm(int player, int delta, const char *why)
{
    int before;
    int after;
    void *po;
    ActorPtr opt;

    if (player < 0 || player > 1 || !delta)
        return;
    before = g_m_bpm_live[player];
    after = clamp_i(before + delta, 10, 2000);
    if (after == before)
        return;
    po = player_obj(player);
    if (!po)
        return;
    __try {
        opt = (ActorPtr)((char *)po + g_off_player_option);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_m_bpm_live[player] = after;
    log_msg("%s P%d m_bpm %d -> %d (step %d)", why, player + 1, before, after,
            delta);
    apply_mmod_to_option(opt, why);
    {
        int index = 0;
        int chart_bpm = 0;
        const char *src = "none";
        compute_mmod_speed_for_player(player, &index, &chart_bpm, &src);
        remember_chart_bpm(player, chart_bpm);
        refresh_online_text_buffer();
        refresh_speed_visual(player, index);
    }
}

static void poll_mmod_nudge(int player)
{
    int left;
    int right;
    int start;
    DWORD now;
    int delta = 0;
    int step;
    int edit_const;

    if (player < 0 || player > 1 || !g_enabled || !g_ready)
        return;

    start = any_start_held();
    left = menu_left_held((unsigned)player);
    right = menu_right_held((unsigned)player);
    now = GetTickCount();
    edit_const = g_constant && g_nudge_edit_constant[player];
    step = edit_const ? g_constant_ms_step : g_m_bpm_step;

    /* Ignore while START held so retry_exit Start+MenuLeft select is safe. */
    if (start) {
        nudge_reset_buttons(player);
        return;
    }

    /* Menu Left+Right chord: toggle nudge target (m_bpm <-> constant_ms). */
    if (left && right) {
        if (!g_nudge_chord_down[player]) {
            if (g_constant) {
                g_nudge_edit_constant[player] = !g_nudge_edit_constant[player];
                log_msg("MenuChord P%d nudge -> %s", player + 1,
                        g_nudge_edit_constant[player] ? "constant_ms" : "m_bpm");
                refresh_online_text_buffer();
            }
            g_nudge_chord_down[player] = 1;
        }
        /* Hold both as "already down" so releasing one side cannot edge-nudge. */
        g_nudge_left_down[player] = 1;
        g_nudge_right_down[player] = 1;
        return;
    }
    g_nudge_chord_down[player] = 0;

    if (left && !right) {
        if (!g_nudge_left_down[player]) {
            delta = -step;
            g_nudge_next_ms[player] = now + kNudgeRepeatFirstMs;
        } else if ((int)(now - g_nudge_next_ms[player]) >= 0) {
            delta = -step;
            g_nudge_next_ms[player] = now + kNudgeRepeatMs;
        }
        g_nudge_left_down[player] = 1;
        g_nudge_right_down[player] = 0;
    } else if (right && !left) {
        if (!g_nudge_right_down[player]) {
            delta = step;
            g_nudge_next_ms[player] = now + kNudgeRepeatFirstMs;
        } else if ((int)(now - g_nudge_next_ms[player]) >= 0) {
            delta = step;
            g_nudge_next_ms[player] = now + kNudgeRepeatMs;
        }
        g_nudge_right_down[player] = 1;
        g_nudge_left_down[player] = 0;
    } else {
        nudge_reset_buttons(player);
    }

    if (!delta)
        return;
    if (edit_const)
        nudge_constant_ms(player, delta, "MenuNudge");
    else
        nudge_m_bpm(player, delta, "MenuNudge");
}

/*
 * force_player >= 0: use that side's m_bpm/chart even if opt is a temp copy
 * (OptionCopy dst). force_player < 0: resolve from opt; skip if unknown.
 */
static void apply_mmod_to_option_ex(ActorPtr opt, int force_player,
                                   const char *why)
{
    int index;
    float speed;
    int chart_bpm = 0;
    const char *src = "none";
    int prev;
    int hud_h;
    int eq;
    int m_bpm;
    int player;

    if (!g_enabled || !opt)
        return;

    player = force_player >= 0 ? force_player : player_for_option(opt);
    if (player < 0 || player > 1) {
        log_msg("%s skip unresolved option=%p", why, (void *)opt);
        return;
    }

    speed = compute_mmod_speed_for_player(player, &index, &chart_bpm, &src);
    m_bpm = m_bpm_for_player(player);
    hud_h = index_to_hundredths(index);
    eq = chart_bpm > 0 ? (int)((double)chart_bpm * (double)speed + 0.5) : m_bpm;
    remember_chart_bpm(player, chart_bpm);
    refresh_online_text_buffer();
    __try {
        prev = *(int *)(opt + (int)kOffOptionHispeed);
        *(int *)(opt + (int)kOffOptionHispeed) = index;
        if (prev != index) {
            log_msg("%s P%d m_bpm=%d chart=%d (%s) -> float %.3f (eq %d) "
                    "hud %d.%02dx idx %d->%d",
                    why, player + 1, m_bpm, chart_bpm, src, speed, eq,
                    hud_h / 100, hud_h % 100, prev, index);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    apply_mmod_gameplays_for_player(player, speed, index, why);
}

static void apply_mmod_to_option(ActorPtr opt, const char *why)
{
    apply_mmod_to_option_ex(opt, -1, why);
}

#ifdef _WIN64
static void __fastcall detour_set_hispeed(ActorPtr opt, int index)
{
    (void)index;
    if (!g_ready || !g_enabled) {
        if (g_orig_set_hispeed)
            g_orig_set_hispeed(opt, index);
        else if (opt) {
            __try {
                *(int *)(opt + (int)kOffOptionHispeed) = index;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return;
    }
    maybe_reset_live_m_bpm("SetHispeed");
    apply_mmod_to_option(opt, "SetHispeed");
}

static unsigned int __fastcall detour_get_hispeed(ActorPtr opt)
{
    if (!opt)
        return 3;
    __try {
        return *(unsigned int *)(opt + (int)kOffOptionHispeed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}
#else
/* Stock SetHispeed returns the index in EAX - callers may use it. */
static int set_hispeed_impl(ActorPtr opt, int index)
{
    int written = index;
    if (!g_ready || !g_enabled) {
        if (g_orig_set_hispeed)
            g_orig_set_hispeed(opt, index);
        else if (opt) {
            __try {
                *(int *)(opt + (int)kOffOptionHispeed) = index;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return written;
    }
    maybe_reset_live_m_bpm("SetHispeed");
    apply_mmod_to_option(opt, "SetHispeed");
    if (opt) {
        __try {
            written = *(int *)(opt + (int)kOffOptionHispeed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            written = index;
        }
    }
    return written;
}

static unsigned int get_hispeed_impl(ActorPtr opt)
{
    if (!opt)
        return 3;
    __try {
        return *(unsigned int *)(opt + (int)kOffOptionHispeed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}

/* Vtable entries must accept thiscall (ecx=this, stack args). */
static __declspec(naked) void detour_set_hispeed(void)
{
    __asm {
        push dword ptr [esp + 4]
        push ecx
        call set_hispeed_impl
        add esp, 8
        ret 4
    }
}

static __declspec(naked) unsigned int detour_get_hispeed(void)
{
    __asm {
        push ecx
        call get_hispeed_impl
        add esp, 4
        ret
    }
}
#endif

#ifdef _WIN64
static ActorPtr __fastcall detour_option_copy(ActorPtr dst, ActorPtr src)
{
    ActorPtr result = dst;

    if (!g_orig_copy)
        return dst;
    result = g_orig_copy(dst, src);
#else
extern "C" void *__cdecl detour_option_copy_c(void *dst, void *src)
{
    void *result = dst;

    if (!g_orig_copy)
        return dst;
    result = call_orig_option_copy(g_orig_copy, dst, src);
#endif
    if (g_ready && g_enabled) {
        int src_player;
        int dst_player;

        maybe_reset_live_m_bpm("OptionCopy");
        src_player = src ? player_for_option((ActorPtr)src) : -1;
        if (src)
            apply_mmod_to_option_ex((ActorPtr)src, src_player, "OptionCopySrc");
        dst_player = dst ? player_for_option((ActorPtr)dst) : -1;
        if (dst_player < 0 && src_player >= 0)
            dst_player = src_player;
        if (dst)
            apply_mmod_to_option_ex((ActorPtr)dst, dst_player, "OptionCopy");
    }
    return result;
}

/* After stock bakes scroll floats from GetHispeed, force exact M-Mod. */
#ifdef _WIN64
static ActorPtr __fastcall detour_gameplay_setup(ActorPtr gp)
{
    ActorPtr result;
#else
static int __fastcall detour_gameplay_setup(ActorPtr gp, void *edx)
{
    int result;
    (void)edx;
#endif
    char *opt;
    int index;
    float speed;
    int chart_bpm = 0;
    const char *src = "none";
    int player;
    void *po;

    if (!g_orig_gameplay_setup)
        return 0;
#ifdef _WIN64
    result = g_orig_gameplay_setup(gp);
#else
    result = g_orig_gameplay_setup(gp, edx);
#endif
    if (!g_ready || !g_enabled || !gp)
        return result;

    player = -1;
    __try {
        player = *(int *)(gp + kOffGpPlayer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return result;
    }

    if (player >= 0 && player < 2)
        gameplay_slot_set(player, gp);

    if (!gameplay_alive(gp)) {
        if (player >= 0 && player < 2)
            gameplay_slot_clear(player);
        return result;
    }

    opt = NULL;
    po = player_obj(player >= 0 && player < 2 ? player : 0);
    if (po) {
        __try {
            opt = (char *)po + g_off_player_option;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            opt = NULL;
        }
    }
    if (opt && player >= 0 && player < 2) {
        speed = compute_mmod_speed_for_player(player, &index, &chart_bpm, &src);
        remember_chart_bpm(player, chart_bpm);
        refresh_online_text_buffer();
        __try {
            *(int *)(opt + (int)kOffOptionHispeed) = index;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    } else {
        speed = 1.0f;
        index = hundredths_to_index(100);
    }
    {
        int m_bpm = m_bpm_for_player(player >= 0 && player < 2 ? player : 0);
        int hud_h = index_to_hundredths(index);
        int eq = chart_bpm > 0
                     ? (int)((double)chart_bpm * (double)speed + 0.5)
                     : m_bpm;
        log_msg_f("GameplaySetup P%d m_bpm=%d chart=%d (%s) -> float %.3f "
                  "(eq %d) hud %d.%02dx idx %d",
                  (player >= 0 && player < 2) ? player + 1 : 0, m_bpm, chart_bpm,
                  src, speed, eq, hud_h / 100, hud_h % 100, index);
    }
    apply_mmod_to_gameplay(gp, speed, index, "GameplaySetup");
    if (player >= 0 && player < 2) {
        apply_constant_gate(player);
        cache_csa_for_player(player, gp);
        cache_option_icon_for_player(player, gp);
        refresh_speed_visual(player, index);
    }
    return result;
}

#ifdef _WIN64
static ActorPtr __fastcall detour_gameplay_update(ActorPtr gp)
{
    int player = -1;
    ActorPtr result = 0;
    int alive = 0;
#else
static ActorPtr __fastcall detour_gameplay_update(ActorPtr gp, void *edx)
{
    int player = -1;
    ActorPtr result = 0;
    int alive = 0;
    (void)edx;
#endif

    if (g_ready && g_enabled && gp) {
        if (!gameplay_alive(gp)) {
            for (player = 0; player < 2; player++) {
                if (g_gameplay[player] == gp)
                    gameplay_slot_clear(player);
            }
            maybe_reset_live_m_bpm("GameplayUpdateDead");
            player = -1;
        } else {
            __try {
                player = *(int *)(gp + kOffGpPlayer);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                player = -1;
            }
            if (player >= 0 && player < 2) {
                alive = 1;
                gameplay_slot_set(player, gp);
                cache_csa_for_player(player, gp);
                cache_option_icon_for_player(player, gp);
                poll_mmod_nudge(player);
            }
        }
    }

    if (g_orig_gameplay_update)
#ifdef _WIN64
        result = g_orig_gameplay_update(gp);
#else
        result = g_orig_gameplay_update(gp, edx);
#endif

    if (g_ready && g_enabled && alive && player >= 0 && player < 2) {
        reapply_exact_scroll(player);
        apply_constant_gate(player);
    }

    return result;
}

#ifdef _WIN64
static void **__fastcall detour_ssq_analyze(ActorPtr ctx)
{
    void **result;

    if (!g_orig_analyze)
        return NULL;
    log_msg("ssq_analyze enter ctx=%p", (void *)ctx);
    result = g_orig_analyze(ctx);
    log_msg("ssq_analyze leave cache_entries=%d", g_cache_count);
    return result;
}

static char __fastcall detour_note_parse(
    ActorPtr reader, char **notes, ActorPtr extra, ActorPtr counts,
    ActorPtr density, unsigned int style_pass, int difficulty, ActorPtr option)
#else
static int __fastcall detour_ssq_analyze(ActorPtr ctx, void *edx)
{
    int result;

    (void)edx;
    if (!g_orig_analyze)
        return 0;
    log_msg("ssq_analyze enter ctx=%p", (void *)ctx);
    result = g_orig_analyze(ctx, edx);
    log_msg("ssq_analyze leave cache_entries=%d", g_cache_count);
    return result;
}

static char __stdcall detour_note_parse(void *reader, char **notes, void *extra,
                                      void *counts, void *density,
                                      unsigned int style_pass, int difficulty,
                                      void *option)
#endif
{
    void *ret_slot;
    uintptr_t ret;
    unsigned int mcode = 0;
    char ok;
    LONG nlog;
    uintptr_t ret_rva;

    ret_slot = _AddressOfReturnAddress();
    ret = (uintptr_t)_ReturnAddress();
    ret_rva = g_base ? (ret - g_base) : 0;

    nlog = InterlockedIncrement(&g_note_parse_logs);
    /* Radar is intentionally uncached ? skip its spam. */
    if (nlog <= 40 && ret_rva != g_rva_radar_parse_ret)
        log_msg("note_parse #%ld ret=0x%X style=%u diff=%d",
                (long)nlog, (unsigned)ret_rva, style_pass, difficulty);

    if (ret_rva != g_rva_radar_parse_ret)
        mcode = resolve_note_parse_mcode(ret, ret_slot);

    if (!g_orig_parse)
        return 0;
    ok = g_orig_parse(reader, notes, extra, counts, density, style_pass,
                      difficulty, option);
    if (ok && mcode)
        cache_from_notes(notes, mcode, style_pass, difficulty,
                         ret_rva == g_rva_gameplay_parse_ret ? 1 : 0);
    else if (ok && !mcode && nlog <= 40 &&
             (ret_rva == g_rva_analyze_parse_ret ||
              ret_rva == g_rva_gameplay_parse_ret))
        log_msg("note_parse #%ld ok but mcode=0 (no cache) ret=0x%X",
                (long)nlog, (unsigned)ret_rva);
    return ok;
}

static int patch_option_vtable(void)
{
    void **vt;
    DWORD old_prot;
    SIZE_T sz = sizeof(void *) * 2;

    if (!g_base || !g_rva_option_vftable)
        return 0;
    vt = (void **)(g_base + g_rva_option_vftable);
    g_vt_set_slot = &vt[3]; /* +0x18 ? covers Set+Get pair for VP */

    __try {
        g_orig_set_hispeed = (SetHispeedFn)vt[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (!VirtualProtect(g_vt_set_slot, sz, PAGE_READWRITE, &old_prot))
        return 0;
    g_saved_vt_set = vt[3];
    g_saved_vt_get = vt[4];
    vt[3] = (void *)detour_set_hispeed;
    vt[4] = (void *)detour_get_hispeed;
    VirtualProtect(g_vt_set_slot, sz, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), g_vt_set_slot, sz);
    g_vt_patched = 1;
    log_msg("vtable patched SetHispeed=%p GetHispeed slot=%p",
            (void *)g_orig_set_hispeed, g_saved_vt_get);
    return 1;
}

static void unpatch_option_vtable(void)
{
    DWORD old_prot;
    SIZE_T sz = sizeof(void *) * 2;
    void **vt;
    if (!g_vt_patched || !g_base)
        return;
    __try {
        vt = (void **)(g_base + g_rva_option_vftable);
        if (!VirtualProtect(g_vt_set_slot, sz, PAGE_READWRITE, &old_prot)) {
            g_vt_patched = 0;
            return;
        }
        vt[3] = g_saved_vt_set;
        vt[4] = g_saved_vt_get;
        VirtualProtect(g_vt_set_slot, sz, old_prot, &old_prot);
        g_vt_patched = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_vt_patched = 0;
    }
}

static void disable_installed_hooks(void)
{
    /*
     * Best-effort unwind after a failed init phase. Spice keeps the DLL mapped;
     * we only disable hooks so stock code runs. Do not MH_Uninitialize here.
     */
    if (!g_base)
        return;
    MH_DisableHook(MH_ALL_HOOKS);
    restore_online_lea();
    log_msg_f("disabled installed hooks (init rollback)");
}

/* Install analyze+parse as soon as those prologues are valid ? do not wait
 * for option_copy. Boot SSQ analysis often finishes before full init. */
static int install_core_hooks_early(void)
{
    MH_STATUS st;
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();

    if (MH_Initialize() != MH_OK) {
        log_msg("MH_Initialize failed (early)");
        return 0;
    }

    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            int ok = 1;
            if (g_verify_prologue)
                ok = bytes_match((void *)(g_base + g_rva_ssq_analyze),
                                 kAnalyzePrologue, sizeof(kAnalyzePrologue)) &&
                     bytes_match((void *)(g_base + g_rva_note_parse),
                                 kParsePrologue, sizeof(kParsePrologue));
            if (ok) {
                st = MH_CreateHook((LPVOID)(g_base + g_rva_ssq_analyze),
                                   (LPVOID)detour_ssq_analyze,
                                   (LPVOID *)&g_orig_analyze);
                if (st != MH_OK) {
                    log_msg("early MH_CreateHook ssq_analyze failed: %d",
                            (int)st);
                    return 0;
                }
                st = MH_CreateHook((LPVOID)(g_base + g_rva_note_parse),
                                   (LPVOID)detour_note_parse,
                                   (LPVOID *)&g_orig_parse);
                if (st != MH_OK) {
                    log_msg("early MH_CreateHook note_parse failed: %d",
                            (int)st);
                    MH_RemoveHook((LPVOID)(g_base + g_rva_ssq_analyze));
                    return 0;
                }
                st = MH_EnableHook((LPVOID)(g_base + g_rva_ssq_analyze));
                if (st != MH_OK) {
                    log_msg("early MH_EnableHook analyze failed: %d", (int)st);
                    MH_RemoveHook((LPVOID)(g_base + g_rva_ssq_analyze));
                    MH_RemoveHook((LPVOID)(g_base + g_rva_note_parse));
                    return 0;
                }
                st = MH_EnableHook((LPVOID)(g_base + g_rva_note_parse));
                if (st != MH_OK) {
                    log_msg("early MH_EnableHook parse failed: %d", (int)st);
                    MH_DisableHook((LPVOID)(g_base + g_rva_ssq_analyze));
                    MH_RemoveHook((LPVOID)(g_base + g_rva_ssq_analyze));
                    MH_RemoveHook((LPVOID)(g_base + g_rva_note_parse));
                    return 0;
                }
                log_msg("early core hooks ON base=%p analyze=%X parse=%X "
                        "(elapsed %ums)",
                        (void *)g_base, (unsigned)g_rva_ssq_analyze,
                        (unsigned)g_rva_note_parse,
                        (unsigned)(GetTickCount() - start));
                return 1;
            }
        }
        Sleep(5);
    }
    log_msg("timeout waiting for analyze/parse prologues");
    return 0;
}

static int wait_option_copy_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int stable = 0;
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            int ok;
            if (g_verify_prologue)
                ok = bytes_match((void *)(g_base + g_rva_option_copy),
                                 kCopyPrologue, sizeof(kCopyPrologue));
            else {
                __try {
                    (void)*(unsigned char *)(g_base + g_rva_option_copy);
                    ok = 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ok = 0;
                }
            }
            if (ok) {
                if (++stable >= 2)
                    return 1;
            } else {
                stable = 0;
            }
        }
        Sleep(25);
    }
    return 0;
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;

    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    open_log();

    g_enabled = ini_int(L"enabled", 1);
    g_online_hud = ini_int(L"online_hud", 1);
    g_m_bpm_ini[0] = clamp_i(ini_int(L"m_bpm_1p", 600), 10, 2000);
    g_m_bpm_ini[1] = clamp_i(ini_int(L"m_bpm_2p", 600), 10, 2000);
    g_m_bpm_live[0] = g_m_bpm_ini[0];
    g_m_bpm_live[1] = g_m_bpm_ini[1];
    g_m_bpm_step = clamp_i(ini_int(L"m_bpm_step", 10), 1, 100);
    g_constant = ini_int(L"constant", 1) ? 1 : 0;
    g_constant_ms_ini[0] = snap_constant_ms(ini_int(L"constant_ms_1p", 600));
    g_constant_ms_ini[1] = snap_constant_ms(ini_int(L"constant_ms_2p", 600));
    g_constant_ms_live[0] = g_constant_ms_ini[0];
    g_constant_ms_live[1] = g_constant_ms_ini[1];
    g_constant_ms_step = clamp_i(ini_int(L"constant_ms_step", 10), 10, 100);
    g_constant_ms_step = (g_constant_ms_step / 10) * 10;
    if (g_constant_ms_step < 10)
        g_constant_ms_step = 10;
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_rva_option_copy = ini_rva(L"rva_option_copy", kDefaultOptionCopy);
    g_rva_music_lookup = ini_rva(L"rva_music_lookup", kDefaultMusicLookup);
    g_rva_ssq_analyze = ini_rva(L"rva_ssq_analyze", kDefaultSsqAnalyze);
    g_rva_note_parse = ini_rva(L"rva_note_parse", kDefaultNoteParse);
    g_rva_gameplay_setup = ini_rva(L"rva_gameplay_setup", kDefaultGameplaySetup);
    g_rva_gameplay_update = ini_rva(L"rva_gameplay_update", kDefaultGameplayUpdate);
    /* Legacy key rva_csa_refresh still accepted as OptionIcon full rebuild. */
    g_rva_option_icon_refresh =
        ini_rva(L"rva_option_icon_refresh",
                ini_rva(L"rva_csa_refresh", kDefaultOptionIconRefresh));
    g_rva_option_icon_live =
        ini_rva(L"rva_option_icon_live", kDefaultOptionIconLive);
    g_rva_option_icon_vftable =
        ini_rva(L"rva_option_icon_vftable", kDefaultOptionIconVftable);
    g_rva_csa_vftable = ini_rva(L"rva_csa_vftable", kDefaultCsaVftable);
    g_rva_status_hud = ini_rva(L"rva_status_hud", kDefaultStatusHud);
    g_rva_online_lea = ini_rva(L"rva_online_lea", kDefaultOnlineLea);
    g_rva_ui_text_online = ini_rva(L"rva_ui_text_online", kDefaultUiTextOnline);
    g_rva_get_menu_left = ini_rva(L"rva_get_menu_left", kDefaultGetMenuLeft);
    g_rva_get_menu_right = ini_rva(L"rva_get_menu_right", kDefaultGetMenuRight);
    g_rva_get_start = ini_rva(L"rva_get_start", kDefaultGetStart);
    g_rva_io_state = ini_rva(L"rva_io_state", kDefaultIoState);
    g_rva_analyze_parse_ret = ini_rva(L"rva_analyze_parse_ret", kDefaultAnalyzeParseRet);
    g_rva_gameplay_parse_ret = ini_rva(L"rva_gameplay_parse_ret",
                                       kDefaultGameplayParseRet);
    g_rva_radar_parse_ret = ini_rva(L"rva_radar_parse_ret", kDefaultRadarParseRet);
    g_rva_players = ini_rva(L"rva_players", kDefaultPlayers);
    g_rva_game = ini_rva(L"rva_game", kDefaultGame);
    g_rva_option_vftable = ini_rva(L"rva_option_vftable", kDefaultOptionVftable);
    g_rva_gameplay_vftable = ini_rva(L"rva_gameplay_vftable",
                                     kDefaultGameplayVftable);
    g_off_bpmmax = ini_rva(L"off_bpmmax", kDefaultOffBpmMax);
    g_off_bpmmin = ini_rva(L"off_bpmmin", kDefaultOffBpmMin);
    g_off_player_option = ini_rva(L"off_player_option", kDefaultOffPlayerOption);
    g_off_player_music = ini_rva(L"off_player_music", kDefaultOffPlayerMusic);
    g_off_player_diff = ini_rva(L"off_player_diff", kDefaultOffPlayerDiff);
    g_off_player_style = ini_rva(L"off_player_style", kDefaultOffPlayerStyle);
    g_note_stride = ini_int(L"note_stride", kDefaultNoteStride);
    if (g_note_stride <= 0)
        g_note_stride = kDefaultNoteStride;
    g_off_note_type = ini_int(L"off_note_type", kDefaultOffNoteType);
    g_off_note_time = ini_int(L"off_note_time", kDefaultOffNoteTime);
    g_off_note_ms = ini_int(L"off_note_ms", kDefaultOffNoteMs);

    log_msg_f("init enabled=%d online_hud=%d m_bpm_1p=%d m_bpm_2p=%d step=%d "
              "constant=%d cms1=%d cms2=%d cms_step=%d "
              "copy=%X analyze=%X parse=%X music=%X players=%X vt=%X gpvt=%X "
              "gpupd=%X icon=%X left=%X right=%X note=%d verify=%d",
              g_enabled, g_online_hud, g_m_bpm_ini[0], g_m_bpm_ini[1],
              g_m_bpm_step, g_constant, g_constant_ms_ini[0],
              g_constant_ms_ini[1], g_constant_ms_step,
              (unsigned)g_rva_option_copy,
              (unsigned)g_rva_ssq_analyze, (unsigned)g_rva_note_parse,
              (unsigned)g_rva_music_lookup, (unsigned)g_rva_players,
              (unsigned)g_rva_option_vftable, (unsigned)g_rva_gameplay_vftable,
              (unsigned)g_rva_gameplay_update,
              (unsigned)g_rva_option_icon_refresh,
              (unsigned)g_rva_get_menu_left, (unsigned)g_rva_get_menu_right,
              g_note_stride, g_verify_prologue);

    /* Phase 1: catch boot SSQ analysis before it finishes. */
    if (!install_core_hooks_early()) {
        log_msg("early core hooks failed");
        return 0;
    }
    if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0) {
        disable_installed_hooks();
        return 0;
    }

    /* Phase 2: option_copy + vtable once that code is unpacked. */
    if (!wait_option_copy_unpacked()) {
        log_msg("timeout waiting for option_copy prologue");
        disable_installed_hooks();
        return 0;
    }
    if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0) {
        disable_installed_hooks();
        return 0;
    }
    log_msg("gamemdx base=%p (option_copy ready), cache_entries=%d",
            (void *)g_base, g_cache_count);

    g_music_lookup = (MusicLookupFn)(g_base + g_rva_music_lookup);
    if (g_rva_option_icon_refresh &&
        (!g_verify_prologue ||
         bytes_match((void *)(g_base + g_rva_option_icon_refresh),
                     kOptionIconRefreshPrologue,
                     sizeof(kOptionIconRefreshPrologue)))) {
        g_option_icon_refresh =
            (OptionIconRefreshFn)(g_base + g_rva_option_icon_refresh);
        log_msg("option_icon_refresh=%X ready (speed_x full rebuild)",
                (unsigned)g_rva_option_icon_refresh);
    } else {
        g_option_icon_refresh = NULL;
        log_msg_f("option_icon_refresh prologue mismatch/skip at %X",
                  (unsigned)g_rva_option_icon_refresh);
    }
    if (g_rva_option_icon_live)
        g_option_icon_live =
            (OptionIconLiveFn)(g_base + g_rva_option_icon_live);
    else
        g_option_icon_live = NULL;
    log_msg("option_icon_live=%X vftable=%X",
            (unsigned)g_rva_option_icon_live,
            (unsigned)g_rva_option_icon_vftable);

    st = MH_CreateHook((LPVOID)(g_base + g_rva_option_copy),
#ifdef _WIN64
                       (LPVOID)detour_option_copy,
#else
                       (LPVOID)option_copy_detour,
#endif
                       (LPVOID *)&g_orig_copy);
    if (st != MH_OK) {
        log_msg("MH_CreateHook option_copy failed: %d", (int)st);
        disable_installed_hooks();
        return 0;
    }
    st = MH_EnableHook((LPVOID)(g_base + g_rva_option_copy));
    if (st != MH_OK) {
        log_msg("MH_EnableHook option_copy failed: %d", (int)st);
        disable_installed_hooks();
        return 0;
    }
    log_msg("hooks enabled option_copy (analyze+parse already early)");

    g_rva_gameplay_setup = ini_rva(L"rva_gameplay_setup", kDefaultGameplaySetup);
    if (g_verify_prologue &&
        !bytes_match((void *)(g_base + g_rva_gameplay_setup),
                     kGameplaySetupPrologue, sizeof(kGameplaySetupPrologue))) {
        log_msg_f("gameplay_setup prologue mismatch at %X ? skip hook "
                  "(scroll follows Option index only)",
                  (unsigned)g_rva_gameplay_setup);
    } else {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_gameplay_setup),
                           (LPVOID)detour_gameplay_setup,
                           (LPVOID *)&g_orig_gameplay_setup);
        if (st != MH_OK) {
            log_msg_f("MH_CreateHook gameplay_setup failed: %d", (int)st);
        } else {
            st = MH_EnableHook((LPVOID)(g_base + g_rva_gameplay_setup));
            if (st != MH_OK)
                log_msg_f("MH_EnableHook gameplay_setup failed: %d", (int)st);
            else
                log_msg_f("hooks enabled gameplay_setup=%X (scroll float bake)",
                          (unsigned)g_rva_gameplay_setup);
        }
    }

    g_rva_gameplay_update = ini_rva(L"rva_gameplay_update", kDefaultGameplayUpdate);
    if (g_verify_prologue &&
        !bytes_match((void *)(g_base + g_rva_gameplay_update),
                     kGameplayUpdatePrologue, sizeof(kGameplayUpdatePrologue))) {
        log_msg_f("gameplay_update prologue mismatch at %X - skip nudge hook",
                  (unsigned)g_rva_gameplay_update);
    } else {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_gameplay_update),
                           (LPVOID)detour_gameplay_update,
                           (LPVOID *)&g_orig_gameplay_update);
        if (st != MH_OK) {
            log_msg_f("MH_CreateHook gameplay_update failed: %d", (int)st);
        } else {
            st = MH_EnableHook((LPVOID)(g_base + g_rva_gameplay_update));
            if (st != MH_OK)
                log_msg_f("MH_EnableHook gameplay_update failed: %d", (int)st);
            else
                log_msg_f("hooks enabled gameplay_update=%X (in-song m_bpm nudge)",
                          (unsigned)g_rva_gameplay_update);
        }
    }

    g_rva_online_lea = ini_rva(L"rva_online_lea", kDefaultOnlineLea);
    if (g_online_hud) {
        if (!patch_online_lea())
            log_msg_f("ONLINE lea patch failed - BPM label will stay stock ONLINE");
    } else {
        log_msg_f("online_hud=0 - stock ONLINE label left alone");
    }

    g_rva_status_hud = ini_rva(L"rva_status_hud", kDefaultStatusHud);
    if (!g_online_hud) {
        log_msg_f("online_hud=0 - status_hud ONLINE refresh skipped");
    } else if (g_verify_prologue &&
               !bytes_match((void *)(g_base + g_rva_status_hud),
                            kStatusHudPrologue, sizeof(kStatusHudPrologue))) {
        log_msg_f("status_hud prologue mismatch at %X - restoring ONLINE lea",
                  (unsigned)g_rva_status_hud);
        restore_online_lea();
    } else {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_status_hud),
                           (LPVOID)detour_status_hud,
                           (LPVOID *)&g_orig_status_hud);
        if (st != MH_OK) {
            log_msg_f("MH_CreateHook status_hud failed: %d - restoring ONLINE lea",
                      (int)st);
            restore_online_lea();
        } else {
            st = MH_EnableHook((LPVOID)(g_base + g_rva_status_hud));
            if (st != MH_OK) {
                log_msg_f("MH_EnableHook status_hud failed: %d - restoring ONLINE lea",
                          (int)st);
                MH_RemoveHook((LPVOID)(g_base + g_rva_status_hud));
                restore_online_lea();
            } else {
                log_msg_f("hooks enabled status_hud=%X (ONLINE buffer refresh)",
                          (unsigned)g_rva_status_hud);
            }
        }
    }

    if (!patch_option_vtable()) {
        log_msg_f("vtable patch failed (OptionCopy hook still active)");
    }

    if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0) {
        log_msg_f("shutdown during init - disabling hooks, not ready");
        disable_installed_hooks();
        unpatch_option_vtable();
        return 0;
    }

    InterlockedExchange(&g_ready, 1);
    log_msg_f("ready (M-Mod 1p=%d 2p=%d step %d, constant=%d cms=%d/%d step %d, "
              "Core from SSQ, cache=%d, clamp 0.25x-8.00x)",
              g_m_bpm_ini[0], g_m_bpm_ini[1], g_m_bpm_step, g_constant,
              g_constant_ms_ini[0], g_constant_ms_ini[1], g_constant_ms_step,
              g_cache_count);

    /* Sanity: M600 / 150 = 4.00x index 15; M600 / 200 = 3.00x index 11 */
    {
        /* Exact float vs nearest HUD (e.g. Core 320 -> 1.875 / hud 2.00x). */
        float f320 = (float)(600.0 / 320.0);
        float f400 = (float)(600.0 / 400.0);
        int s320 = clamp_i((int)(f320 * 4.0f + 0.5f), 1, 32);
        int s400 = clamp_i((int)(f400 * 4.0f + 0.5f), 1, 32);
        log_msg("math check M600/320 -> float %.3f (eq %d) hud %d.%02dx idx %d; "
                "M600/400 -> float %.3f (eq %d) hud %d.%02dx idx %d",
                f320, 600, (s320 * 25) / 100, (s320 * 25) % 100, s320 - 1,
                f400, 600, (s400 * 25) / 100, (s400 * 25) % 100, s400 - 1);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE th;
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_cache_cs);
        /* Close handle after create ? thread keeps running; avoid leak. */
        th = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH) {
        /*
         * Do NOT MH_Uninitialize / fclose / DeleteCS here: game threads may
         * still be inside detours or log_msg under the loader lock. Spice
         * keeps -k DLLs for process lifetime; restore Option vtable + ONLINE
         * lea only. g_shutdown blocks a late init_thread from setting ready.
         */
        InterlockedExchange(&g_shutdown, 1);
        InterlockedExchange(&g_ready, 0);
        restore_online_lea();
        unpatch_option_vtable();
    }
    return TRUE;
}
