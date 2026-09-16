#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook: keep same-set PBs by (player, mcode, style, difficulty)
 * instead of the 5 stage-local ghost slots that pfree reuses.
 *
 * After a song, the compacted ghost is copied out of the stage slot. On the
 * next play of that chart, pfree's stage>0 shortcut often leaves GhostActor
 * with an empty vector (ghost/target 0) even when the lookup id is not a
 * local-slot id. If we have a same-chart cache and vanilla did not produce a
 * ghost, copy it into the actor. Network loads in flight (state 0, id > 0)
 * are left alone.
 */

#ifdef _WIN64
#define INI_NAME L"pfree_ghost_64bit.ini"
#define LOG_NAME L"pfree_ghost_64bit.log"
static const uintptr_t kDefaultGhostInit = 0x38120;
static const uintptr_t kDefaultCommit = 0x3CF00;
static const uintptr_t kDefaultCopy = 0x38680;
static const uintptr_t kDefaultPlayers = 0x2EF000;
static const uintptr_t kDefaultGame = 0x2ED6D0;
static const unsigned char kGhostPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B };
static const unsigned char kCommitPrologue[] = { 0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC };
#define SLOT_STRIDE 488
#define SLOT_BASE 1080
#define SLOT_GHOST 112
#define GPA_PLAYER 0x84
#define GPA_STYLE 0x88
#define GPA_SCORE 0x1B4
#define GPA_NOTES 0xB0
#define GA_PLAYER 0x84
#define GA_PLAY 0x88
#define GA_ST_BASE 0x58
#define GA_ST_IDX 0x82
#define GA_GHOST_ID 0x90
#define GA_VEC 0x98
#define GA_READY 0xC0
#define PLAYER_MUSIC 84
#define PLAYER_DIFF 92
#define NOTE_STRIDE 64
#define NOTE_FLAG 16
#define NOTE_JUDGE 12
#define NOTE_TRIM 48
typedef __int64 (__fastcall *GhostInitFn)(__int64 self);
typedef __int64 (__fastcall *CommitFn)(__int64 self);
typedef __int64 (__fastcall *GhostCopyFn)(void *dest, void *src);
#else
#define INI_NAME L"pfree_ghost_32bit.ini"
#define LOG_NAME L"pfree_ghost_32bit.log"
static const uintptr_t kDefaultGhostInit = 0x2F520;
static const uintptr_t kDefaultCommit = 0x33530;
static const uintptr_t kDefaultCopy = 0x2F820;
static const uintptr_t kDefaultPlayers = 0x25A9B0;
static const uintptr_t kDefaultGame = 0x25433C;
/* Stop before the security-cookie `mov eax, [imm32]` (relocated load). */
static const unsigned char kGhostPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };
static const unsigned char kCommitPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x83, 0xEC, 0x74 };
#define SLOT_STRIDE 440
#define SLOT_BASE 912
#define SLOT_GHOST 108
#define GPA_PLAYER 0x6C
#define GPA_STYLE 0x70
#define GPA_SCORE 0x144
#define GPA_NOTES 0x88
#define GA_PLAYER 0x6C
#define GA_PLAY 0x70
#define GA_ST_BASE 0x40
#define GA_ST_IDX 0x6A
#define GA_GHOST_ID 0x74
#define GA_VEC 0x78
#define GA_READY 0x8C
#define PLAYER_MUSIC 84
#define PLAYER_DIFF 92
#define NOTE_STRIDE 56
#define NOTE_FLAG 12
#define NOTE_JUDGE 8
#define NOTE_TRIM 44
typedef int (__fastcall *GhostInitFn)(void *self, void *edx);
typedef int (__fastcall *CommitFn)(void *self, void *edx);
#endif

#define MAX_CACHE 256

struct Vec {
    char *begin;
    char *end;
    char *cap;
};

struct Entry {
    int used;
    int player;
    unsigned int music;
    unsigned int style;
    unsigned int diff;
    int score;
    char *data;
    size_t size;
};

static HMODULE g_self;
static uintptr_t g_base;
static GhostInitFn g_orig_ghost;
static CommitFn g_orig_commit;
#ifdef _WIN64
static GhostCopyFn g_copy;
#else
static void *g_copy;
#endif

static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_verify_prologue = 1;
static uintptr_t g_rva_ghost = kDefaultGhostInit;
static uintptr_t g_rva_commit = kDefaultCommit;
static uintptr_t g_rva_copy = kDefaultCopy;
static uintptr_t g_rva_players = kDefaultPlayers;
static uintptr_t g_rva_game = kDefaultGame;

static CRITICAL_SECTION g_cs;
static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;
static struct Entry g_cache[MAX_CACHE];

static void log_msg(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;
    if (!g_log_enabled || !g_log)
        return;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir[0] = 0;
    if (n == 0 || n >= MAX_PATH)
        return;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = 0;
    wcsncpy_s(g_dir, path, _TRUNCATE);
}

static void open_log(void)
{
    wchar_t log_path[MAX_PATH];
    if (!g_log_enabled || !g_dir[0])
        return;
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
    return (int)GetPrivateProfileIntW(L"pfree_ghost", key, def, ini);
}

static uintptr_t ini_rva(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    wchar_t *end = NULL;
    unsigned long v;
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"pfree_ghost", key, L"", buf, 64, ini);
    if (!buf[0])
        return def;
    v = wcstoul(buf, &end, 0);
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
    if (player < 0 || player > 1 || !g_base || !g_rva_players)
        return NULL;
    slot = (void **)(g_base + g_rva_players);
    handle = slot[player];
    if (!handle)
        return NULL;
    return *(void **)handle;
}

static int game_stage(void)
{
    void *gs;
    if (!g_base || !g_rva_game)
        return 0;
    gs = *(void **)(g_base + g_rva_game);
    if (!gs)
        return 0;
    return *(int *)((char *)gs + 8);
}

static unsigned int game_style(void)
{
    void *gs;
    if (!g_base || !g_rva_game)
        return 0;
    gs = *(void **)(g_base + g_rva_game);
    if (!gs)
        return 0;
    return **(unsigned int **)gs;
}

static char *slot_ptr(void *player, int stage, unsigned offset)
{
    if (!player || stage < 0 || stage > 4)
        return NULL;
    return (char *)player + SLOT_BASE + SLOT_STRIDE * stage + offset;
}

/* Match sub_1800165B0 / sub_10012C10: single style clamps beginner/basic to 1. */
static unsigned int clamp_diff(unsigned int style, unsigned int diff)
{
    if (style == 1 && (int)diff <= 1)
        return 1;
    return diff;
}

static struct Entry *find_entry(int player, unsigned int music, unsigned int style,
                                unsigned int diff, int alloc_slot)
{
    int i;
    int free_i = -1;
    for (i = 0; i < MAX_CACHE; i++) {
        if (!g_cache[i].used) {
            if (free_i < 0)
                free_i = i;
            continue;
        }
        if (g_cache[i].player == player && g_cache[i].music == music &&
            g_cache[i].style == style && g_cache[i].diff == diff)
            return &g_cache[i];
    }
    if (alloc_slot && free_i >= 0)
        return &g_cache[free_i];
    return NULL;
}

static void store_bytes(int player, unsigned int music, unsigned int style,
                        unsigned int diff, int score, const char *data, size_t n)
{
    struct Entry *e;
    char *copy;
    if (!data || n == 0)
        return;
    EnterCriticalSection(&g_cs);
    e = find_entry(player, music, style, diff, 1);
    if (!e) {
        LeaveCriticalSection(&g_cs);
        log_msg("cache full, dropping mcode=%u style=%u diff=%u", music, style, diff);
        return;
    }
    if (e->used && score < e->score) {
        LeaveCriticalSection(&g_cs);
        log_msg("skip p%d mcode=%u style=%u diff=%u score=%d < cached %d",
                player, music, style, diff, score, e->score);
        return;
    }
    copy = (char *)malloc(n);
    if (!copy) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    memcpy(copy, data, n);
    if (e->data)
        free(e->data);
    e->used = 1;
    e->player = player;
    e->music = music;
    e->style = style;
    e->diff = diff;
    e->score = score;
    e->data = copy;
    e->size = n;
    LeaveCriticalSection(&g_cs);
    log_msg("cache p%d mcode=%u style=%u diff=%u score=%d bytes=%u",
            player, music, style, diff, score, (unsigned)n);
}

/* Compact one play note to the 1-byte ghost stream. */
static unsigned char pack_note(const unsigned char *n)
{
    const int *info;
    if (n[NOTE_FLAG])
        return (unsigned char)*(const int *)(n + NOTE_JUDGE);
    info = *(const int *const *)n;
    if (!info)
        return 5;
    if ((info[3] == 1 && info[4] == 1 && info[5] == 1 && info[6] == 1) ||
        (info[7] == 1 && info[8] == 1 && info[9] == 1 && info[10] == 1) ||
        *(const unsigned char *)info == 2)
        return 7;
    return 5;
}

static char *pack_notes(char *begin, char *end, size_t *out_n)
{
    char *trimmed;
    size_t n;
    size_t k;
    char *buf;
    *out_n = 0;
    if (!begin || !end || end < begin)
        return NULL;
    if (((size_t)(end - begin) % NOTE_STRIDE) != 0)
        return NULL;
    trimmed = end;
    if (end != begin) {
        char *cur = end;
        do {
            if (cur[-NOTE_TRIM] != 0) {
                trimmed = cur;
                break;
            }
            cur -= NOTE_STRIDE;
            trimmed = begin;
        } while (cur != begin);
    }
    n = (size_t)(trimmed - begin) / NOTE_STRIDE;
    if (n == 0 || n > 100000)
        return NULL;
    buf = (char *)malloc(n);
    if (!buf)
        return NULL;
    for (k = 0; k < n; k++)
        buf[k] = (char)pack_note((const unsigned char *)begin + k * NOTE_STRIDE);
    *out_n = n;
    return buf;
}

#ifndef _WIN64
static __declspec(noinline) void copy_vec_32(void *dest, void *src)
{
    void *fn = g_copy;
    __asm {
        mov eax, src
        mov ecx, dest
        call fn
    }
}
#endif

static size_t vec_size(const struct Vec *v)
{
    if (!v || !v->begin || v->end < v->begin)
        return 0;
    return (size_t)(v->end - v->begin);
}

static int ghost_state(void *actor)
{
    unsigned idx;
    if (!actor)
        return -1;
    idx = *(unsigned short *)((char *)actor + GA_ST_IDX);
    return *(int *)((char *)actor + GA_ST_BASE + 8 * idx);
}

static void mark_ghost_ready(void *actor)
{
    unsigned idx;
    char *play;
    if (!actor)
        return;
    idx = *(unsigned short *)((char *)actor + GA_ST_IDX);
    *(int *)((char *)actor + GA_ST_BASE + 8 * idx) = 2;
    play = *(char **)((char *)actor + GA_PLAY);
    if (play)
        play[GA_READY] = 1;
}

static void apply_vec(void *actor, const struct Entry *e)
{
    struct Vec src;
    if (!actor || !e || !e->data || !e->size || !g_copy)
        return;
    src.begin = e->data;
    src.end = e->data + e->size;
    src.cap = e->data + e->size;
#ifdef _WIN64
    g_copy((char *)actor + GA_VEC, &src);
#else
    copy_vec_32((char *)actor + GA_VEC, &src);
#endif
    mark_ghost_ready(actor);
}

static void store_from_commit(void *gpa)
{
    int player;
    int stage;
    void *pobj;
    unsigned int music;
    unsigned int style;
    unsigned int diff;
    unsigned int slot_diff = 0;
    int score;
    char *nb;
    char *ne;
    char *packed;
    size_t notes;
    size_t packed_n;
    size_t slot_n = 0;
    char *slot;
    if (!gpa)
        return;
    player = *(int *)((char *)gpa + GPA_PLAYER);
    style = *(unsigned int *)((char *)gpa + GPA_STYLE);
    score = *(int *)((char *)gpa + GPA_SCORE);
    stage = game_stage();
    pobj = player_obj(player);
    if (!pobj)
        return;
    /* Key with the same player fields GhostActor lookup uses, not the stage slot
     * (pfree often skips writing that slot; slot+4 was also the wrong diff). */
    music = *(unsigned int *)((char *)pobj + PLAYER_MUSIC);
    diff = clamp_diff(style, *(unsigned int *)((char *)pobj + PLAYER_DIFF));
    slot = slot_ptr(pobj, stage, 0);
    if (slot)
        slot_diff = *(unsigned int *)(slot + 4);
    nb = *(char **)((char *)gpa + GPA_NOTES);
    ne = *(char **)((char *)gpa + GPA_NOTES + sizeof(void *));
    notes = (nb && ne && ne >= nb) ? (size_t)(ne - nb) / NOTE_STRIDE : 0;
    if (slot)
        slot_n = vec_size((const struct Vec *)(slot + SLOT_GHOST));
    packed = NULL;
    packed_n = 0;
    __try {
        packed = pack_notes(nb, ne, &packed_n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        packed = NULL;
        packed_n = 0;
        log_msg("pack exception p%d notes=%u", player, (unsigned)notes);
    }
    log_msg("commit p%d mcode=%u style=%u diff=%u slot_diff=%u score=%d stage=%d notes=%u packed=%u slot_bytes=%u",
            player, music, style, diff, slot_diff, score, stage,
            (unsigned)notes, (unsigned)packed_n, (unsigned)slot_n);
    if (packed) {
        store_bytes(player, music, style, diff, score, packed, packed_n);
        free(packed);
    }
}

static void apply_to_ghost(void *actor)
{
    int ghost_id;
    int player;
    void *pobj;
    unsigned int music;
    unsigned int style;
    unsigned int diff;
    struct Entry *e;
    if (!actor)
        return;
    ghost_id = *(int *)((char *)actor + GA_GHOST_ID);
    player = *(int *)((char *)actor + GA_PLAYER);
    pobj = player_obj(player);
    if (!pobj)
        return;
    music = *(unsigned int *)((char *)pobj + PLAYER_MUSIC);
    style = game_style();
    diff = clamp_diff(style, *(unsigned int *)((char *)pobj + PLAYER_DIFF));
    {
        int st = ghost_state(actor);
        size_t have = vec_size((const struct Vec *)((char *)actor + GA_VEC));
        int empty = have == 0;
        /*
         * pfree (gs+0x78, stage>0) never uses local-slot IDs. Lookup often
         * stays >= 0 and the shortcut leaves an empty vector, which is the
         * "ghost is 0" bug. Inject our same-chart cache in that case.
         * Skip state 0 + positive id: vanilla network load is in flight.
         */
        EnterCriticalSection(&g_cs);
        e = find_entry(player, music, style, diff, 0);
        if (e && !(st == 0 && ghost_id > 0) && (ghost_id < 0 || empty)) {
            size_t n = e->size;
            apply_vec(actor, e);
            LeaveCriticalSection(&g_cs);
            log_msg("apply p%d mcode=%u style=%u diff=%u id=%d state=%d had=%u bytes=%u",
                    player, music, style, diff, ghost_id, st, (unsigned)have, (unsigned)n);
            return;
        }
        LeaveCriticalSection(&g_cs);
        log_msg("ghost p%d mcode=%u style=%u diff=%u id=%d state=%d vec=%u cache=%d",
                player, music, style, diff, ghost_id, st, (unsigned)have, e ? 1 : 0);
    }
}

#ifdef _WIN64
static __int64 __fastcall detour_commit(__int64 self)
{
    __int64 r = g_orig_commit(self);
    store_from_commit((void *)self);
    return r;
}

static __int64 __fastcall detour_ghost(__int64 self)
{
    __int64 r = g_orig_ghost(self);
    apply_to_ghost((void *)self);
    return r;
}
#else
static int __fastcall detour_commit(void *self, void *edx)
{
    int r = g_orig_commit(self, edx);
    store_from_commit(self);
    return r;
}

static int __fastcall detour_ghost(void *self, void *edx)
{
    int r = g_orig_ghost(self, edx);
    apply_to_ghost(self);
    return r;
}
#endif

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
                    b = *(unsigned char *)(g_base + g_rva_ghost);
                    (void)b;
                    if (++match_stable >= 3)
                        return 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    match_stable = 0;
                }
            } else if (bytes_match((void *)(g_base + g_rva_ghost), kGhostPrologue,
                                   sizeof(kGhostPrologue)) &&
                       bytes_match((void *)(g_base + g_rva_commit), kCommitPrologue,
                                   sizeof(kCommitPrologue))) {
                mismatch_stable = 0;
                if (++match_stable >= 3)
                    return 1;
            } else {
                match_stable = 0;
                {
                    unsigned char b;
                    int readable = 0;
                    __try {
                        b = *(unsigned char *)(g_base + g_rva_ghost);
                        (void)b;
                        readable = 1;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        readable = 0;
                    }
                    if (readable) {
                        if (++mismatch_stable >= 10) {
                            log_msg("prologue mismatch (wrong gamemdx build, not hooking)");
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
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_log_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_enabled = ini_int(L"enabled", 1);
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_rva_ghost = ini_rva(L"rva_ghost_init", kDefaultGhostInit);
    g_rva_commit = ini_rva(L"rva_commit", kDefaultCommit);
    g_rva_copy = ini_rva(L"rva_copy", kDefaultCopy);
    g_rva_players = ini_rva(L"rva_players", kDefaultPlayers);
    g_rva_game = ini_rva(L"rva_game", kDefaultGame);
    open_log();
    log_msg("pfree_ghost starting (%s enabled=%d ghost=0x%X commit=0x%X copy=0x%X players=0x%X game=0x%X verify=%d)",
#ifdef _WIN64
            "x64",
#else
            "x86",
#endif
            g_enabled, (unsigned)g_rva_ghost, (unsigned)g_rva_commit, (unsigned)g_rva_copy,
            (unsigned)g_rva_players, (unsigned)g_rva_game, g_verify_prologue);

    if (!wait_unpacked()) {
        log_msg("not hooking (timeout or prologue mismatch)");
        return 1;
    }
    Sleep(200);
    log_msg("gamemdx at %p", (void *)g_base);

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

#ifdef _WIN64
    g_copy = (GhostCopyFn)(g_base + g_rva_copy);
#else
    g_copy = (void *)(g_base + g_rva_copy);
#endif

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }

    st = MH_CreateHook((LPVOID)(g_base + g_rva_commit), (LPVOID)detour_commit,
                       (LPVOID *)&g_orig_commit);
    if (st != MH_OK) {
        log_msg("MH_CreateHook commit: %s", MH_StatusToString(st));
        return 1;
    }

    st = MH_CreateHook((LPVOID)(g_base + g_rva_ghost), (LPVOID)detour_ghost,
                       (LPVOID *)&g_orig_ghost);
    if (st != MH_OK) {
        log_msg("MH_CreateHook GhostActor: %s", MH_StatusToString(st));
        return 1;
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        return 1;
    }

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
