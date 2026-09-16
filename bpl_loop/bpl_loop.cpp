#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook (WORLD / A3 64-bit, pattern-resolved):
 *  After the 2nd matching song, Result requests TotalResult (credit end).
 *  Rewrite that to the stock matching transition (TS 46). Reset stage so the
 *  picker opens a fresh dual pick. Hold cab START during play to skip.
 *
 *  RVAs are found at runtime from shared byte/string patterns — no A3/WORLD
 *  offset tables. INI: log, loop_battle, skip_song, skip_hold_ms.
 */

typedef void(__fastcall *FinishFn)(void *seq, unsigned int nextId);
typedef __int64(__fastcall *OnUpdateFn)(void *seq);
typedef void(__fastcall *AdvanceStateFn)(void *state_obj, int next_case);
typedef void(__fastcall *ArkIo3Fn)(unsigned int player, char *held, char *trigger);

#define INI_NAME L"bpl_loop.ini"
#define LOG_NAME L"bpl_loop.log"

static HMODULE g_self;
static uintptr_t g_base;
static size_t g_image_size;
static uintptr_t g_text_rva;
static size_t g_text_size;
static uintptr_t g_rdata_rva;
static size_t g_rdata_size;
static FinishFn g_orig_finish;
static OnUpdateFn g_orig_mdp;
static OnUpdateFn g_orig_dp;
static volatile LONG g_ready;
static int g_cs_ready;
static int g_mh_inited;
static int g_hooks_enabled;

static int g_log_enabled = 1;
static int g_loop_battle = 1;
static int g_skip_song = 1;
static int g_skip_hold_ms = 400;
static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static uintptr_t g_rva_finish;
static uintptr_t g_rva_set_table;
static uintptr_t g_rva_game_ptr;
static uintptr_t g_rva_result_ret_lo;
static uintptr_t g_rva_result_ret_hi;
static uintptr_t g_rva_mdp_update;
static uintptr_t g_rva_dp_update;
static uintptr_t g_rva_advance_state;
static uintptr_t g_rva_get_start;
static uintptr_t g_rva_io_state;
static uintptr_t g_off_stage = 0x0C;
static int g_mdp_play = 11;
static int g_mdp_after_play = 12;
static int g_dp_play = 7;
static int g_dp_after_play = 8;
static unsigned int g_ts_total_result = 56;
static unsigned int g_ts_matching_transition = 46;
static unsigned int g_ts_result_continue; /* learned from Result leave (e.g. A3 53) */


/* Shared on WORLD + A3. */
static const unsigned char kFinishSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
    0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x59, 0x08, 0x48,
    0x8B, 0xF1, 0x8B, 0xFA, 0xF6, 0x43, 0x20, 0x20, 0x75, 0x3E,
    0x48, 0x8B, 0x03, 0x44, 0x8B, 0xC7, 0xBA, 0x01, 0x02, 0x00,
    0x00
};
static const unsigned char kAdvanceSig[] = {
    0x0F, 0xB7, 0x41, 0x2A, 0x45, 0x33, 0xC9, 0x4C, 0x8B, 0xC1,
    0x89, 0x14, 0xC1, 0x0F, 0xB7, 0x41, 0x2A, 0x44, 0x89, 0x4C,
    0xC1, 0x04, 0x0F, 0xB7, 0x51, 0x2A, 0x41, 0x0F, 0xB7, 0x40,
    0x28
};
static const unsigned char kDpPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57
};

static const uintptr_t kSetStride = 464;
static const uintptr_t kSetNameOff = 12;
static const uintptr_t kSetFlagOff = 0x136;
static const uintptr_t kSetBeginOff = 160;
static const uintptr_t kSetEndOff = 168;

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

static void ini_path(wchar_t *ini, size_t n)
{
    _snwprintf_s(ini, n, _TRUNCATE, L"%s\\" INI_NAME, g_dir);
}

static void open_log(void)
{
    if (!g_log_enabled || !g_dir[0])
        return;
    wchar_t log_path[MAX_PATH];
    _snwprintf_s(log_path, _TRUNCATE, L"%s\\" LOG_NAME, g_dir);
    _wfopen_s(&g_log, log_path, L"a");
}

static int ini_int(const wchar_t *key, int def)
{
    wchar_t ini[MAX_PATH];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    return (int)GetPrivateProfileIntW(L"bpl_loop", key, def, ini);
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
    return v > 0x10000 && v < 0x00007FFFFFFEFFFFULL;
}

static int pe_sections(void)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    WORD i;
    g_text_rva = g_text_size = g_rdata_rva = g_rdata_size = 0;
    g_image_size = 0;
    __try {
        dos = (IMAGE_DOS_HEADER *)g_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        nt = (IMAGE_NT_HEADERS64 *)(g_base + (uintptr_t)dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;
        g_image_size = nt->OptionalHeader.SizeOfImage;
        sec = IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                g_text_rva = sec[i].VirtualAddress;
                g_text_size = sec[i].Misc.VirtualSize;
            } else if (memcmp(sec[i].Name, ".rdata", 6) == 0) {
                g_rdata_rva = sec[i].VirtualAddress;
                g_rdata_size = sec[i].Misc.VirtualSize;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return g_text_size != 0 && g_image_size != 0;
}

static uintptr_t find_bytes(uintptr_t rva, size_t span, const unsigned char *pat, size_t n,
                            int want_unique)
{
    uintptr_t end;
    uintptr_t p;
    uintptr_t hit = 0;
    int count = 0;
    if (!span || n == 0 || span < n)
        return 0;
    end = rva + span - n;
    for (p = rva; p <= end; p++) {
        if (bytes_match((void *)(g_base + p), pat, n)) {
            count++;
            if (!hit)
                hit = p;
            if (!want_unique)
                return hit;
            if (count > 1)
                return 0;
        }
    }
    return hit;
}

static uintptr_t find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    uintptr_t hit;
    if (g_rdata_size) {
        hit = find_bytes(g_rdata_rva, g_rdata_size, (const unsigned char *)s, n, 0);
        if (hit)
            return hit;
    }
    return find_bytes(0, g_image_size, (const unsigned char *)s, n, 0);
}

static int is_lea_rax_rip(uintptr_t rva)
{
    unsigned char *p = (unsigned char *)(g_base + rva);
    __try {
        return p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static uintptr_t decode_rip_rel32(uintptr_t insn_rva, size_t insn_len)
{
    int32_t disp;
    __try {
        disp = *(int32_t *)(g_base + insn_rva + insn_len - 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return insn_rva + insn_len + (intptr_t)disp;
}

static uintptr_t find_lea_to(uintptr_t target_rva, uintptr_t scan_rva, size_t span)
{
    uintptr_t end = scan_rva + span;
    uintptr_t p;
    if (span < 7)
        return 0;
    end -= 7;
    for (p = scan_rva; p <= end; p++) {
        if (!is_lea_rax_rip(p))
            continue;
        if (decode_rip_rel32(p, 7) == target_rva)
            return p;
    }
    return 0;
}

static uintptr_t find_prev_lea_rax(uintptr_t from_rva, size_t back)
{
    uintptr_t start = from_rva > back ? from_rva - back : 0;
    uintptr_t p;
    if (from_rva < 7)
        return 0;
    for (p = from_rva - 7; p + 1 > start; p--) {
        if (is_lea_rax_rip(p))
            return p;
        if (p == start)
            break;
    }
    return 0;
}

static int is_lea_rip(uintptr_t rva)
{
    unsigned char *b = (unsigned char *)(g_base + rva);
    __try {
        if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D)
            return 0;
        /* ModRM: mod=00, rm=101 (RIP-relative) => low 3 bits 101, bits 6-7 = 00 */
        return (b[2] & 0xC7) == 0x05;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int collect_lea_xrefs(uintptr_t target_rva, uintptr_t *out, int max_out)
{
    uintptr_t p;
    uintptr_t end;
    int n = 0;
    if (!g_text_size || max_out <= 0)
        return 0;
    end = g_text_rva + g_text_size;
    if (g_text_size < 7)
        return 0;
    end -= 7;
    for (p = g_text_rva; p <= end && n < max_out; p++) {
        if (!is_lea_rip(p))
            continue;
        if (decode_rip_rel32(p, 7) == target_rva)
            out[n++] = p;
    }
    return n;
}

/* Walk back from a code site to a typical x64 function prologue. */
static uintptr_t func_start_near(uintptr_t code_rva)
{
    uintptr_t p;
    uintptr_t start = code_rva > 0x4000 ? code_rva - 0x4000 : g_text_rva;
    if (start < g_text_rva)
        start = g_text_rva;
    for (p = code_rva; p > start + 12; p--) {
        if (bytes_match((void *)(g_base + p), kDpPrologue, sizeof(kDpPrologue)))
            return p;
        /* sub rsp / mov [rsp+8],rbx style finish-like */
        if (bytes_match((void *)(g_base + p),
                        (const unsigned char *)"\x48\x89\x5C\x24\x08", 5))
            return p;
        if (bytes_match((void *)(g_base + p),
                        (const unsigned char *)"\x40\x53\x48\x83\xEC", 5))
            return p;
    }
    return code_rva;
}

static int find_calls_to(uintptr_t target_rva, uintptr_t *out, int max_out)
{
    uintptr_t p;
    uintptr_t end;
    int n = 0;
    if (!g_text_size || max_out <= 0)
        return 0;
    end = g_text_rva + g_text_size;
    if (g_text_size < 5)
        return 0;
    end -= 5;
    for (p = g_text_rva; p <= end && n < max_out; p++) {
        unsigned char *b = (unsigned char *)(g_base + p);
        int32_t rel;
        uintptr_t dest;
        __try {
            if (b[0] != 0xE8)
                continue;
            rel = *(int32_t *)(b + 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        dest = p + 5 + (intptr_t)rel;
        if (dest == target_rva)
            out[n++] = p;
    }
    return n;
}

static int match_result_leave(uintptr_t call_rva, unsigned int *out_ts)
{
    /* Look back ~40 bytes for: mov edx,0x21 / mov ecx,imm / test al,al / cmovnz edx,ecx */
    uintptr_t back = call_rva > 48 ? call_rva - 48 : g_text_rva;
    uintptr_t p;
    for (p = back; p + 15 < call_rva; p++) {
        unsigned char *b = (unsigned char *)(g_base + p);
        unsigned int ts;
        __try {
            if (b[0] != 0xBA || *(uint32_t *)(b + 1) != 0x21)
                continue;
            /* find mov ecx, imm32 after mov edx */
            {
                uintptr_t q;
                int found = 0;
                for (q = p + 5; q + 10 < call_rva; q++) {
                    unsigned char *c = (unsigned char *)(g_base + q);
                    if (c[0] == 0xB9) {
                        ts = *(uint32_t *)(c + 1);
                        /* test al,al ; cmovnz edx,ecx */
                        if (c[5] == 0x84 && c[6] == 0xC0 &&
                            c[7] == 0x0F && c[8] == 0x45 && c[9] == 0xD1) {
                            found = 1;
                            break;
                        }
                    }
                    /* allow a few filler instructions between */
                    if (c[0] == 0xE8)
                        break;
                }
                if (!found)
                    continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (ts < 30 || ts > 80)
            continue;
        if (out_ts)
            *out_ts = ts;
        return 1;
    }
    return 0;
}

static uintptr_t find_rip_load_before(uintptr_t call_rva, size_t back)
{
    uintptr_t start = call_rva > back ? call_rva - back : g_text_rva;
    uintptr_t p;
    for (p = call_rva; p > start + 7;) {
        p--;
        unsigned char *b = (unsigned char *)(g_base + p);
        __try {
            /* mov rax/rcx/rdx/rbx/rsi/rdi/r8.., [rip+disp] */
            if (b[0] == 0x48 && b[1] == 0x8B &&
                (b[2] == 0x05 || b[2] == 0x0D || b[2] == 0x15 || b[2] == 0x1D ||
                 b[2] == 0x35 || b[2] == 0x3D))
                return decode_rip_rel32(p, 7);
            if (b[0] == 0x4C && b[1] == 0x8B &&
                (b[2] == 0x05 || b[2] == 0x0D || b[2] == 0x15 || b[2] == 0x1D))
                return decode_rip_rel32(p, 7);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return 0;
}

static int resolve_finish(void)
{
    uintptr_t hit = find_bytes(g_text_rva, g_text_size, kFinishSig, sizeof(kFinishSig), 1);
    if (!hit)
        return 0;
    g_rva_finish = hit;
    return 1;
}

static int resolve_advance(void)
{
    uintptr_t hit = find_bytes(g_text_rva, g_text_size, kAdvanceSig, sizeof(kAdvanceSig), 1);
    if (!hit)
        return 0;
    g_rva_advance_state = hit;
    return 1;
}

static int resolve_mdp(void)
{
    uintptr_t str = find_string("sequence::dance::MatchingDancePlaySequence::onUpdate");
    uintptr_t xrefs[8];
    int nx;
    int i;
    if (!str)
        return 0;
    nx = collect_lea_xrefs(str, xrefs, 8);
    if (nx <= 0)
        return 0;
    for (i = 0; i < nx; i++) {
        uintptr_t fn = func_start_near(xrefs[i]);
        if (bytes_match((void *)(g_base + fn), kDpPrologue, sizeof(kDpPrologue))) {
            g_rva_mdp_update = fn;
            return 1;
        }
    }
    g_rva_mdp_update = func_start_near(xrefs[0]);
    return g_rva_mdp_update != 0;
}

static int resolve_dp(void)
{
    uintptr_t str = find_string("HIGH_PRECISION_BEGIN_TICK");
    uintptr_t xrefs[8];
    int nx;
    int i;
    if (!str)
        return 0;
    nx = collect_lea_xrefs(str, xrefs, 8);
    for (i = 0; i < nx; i++) {
        uintptr_t fn = func_start_near(xrefs[i]);
        if (!bytes_match((void *)(g_base + fn), kDpPrologue, sizeof(kDpPrologue)))
            continue;
        if (g_rva_mdp_update && fn == g_rva_mdp_update)
            continue;
        g_rva_dp_update = fn;
        return 1;
    }
    return 0;
}

static int resolve_result_leave(void)
{
    uintptr_t calls[64];
    int nc;
    int i;
    unsigned int ts = 0;
    uintptr_t best = 0;
    if (!g_rva_finish)
        return 0;
    nc = find_calls_to(g_rva_finish, calls, 64);
    for (i = 0; i < nc; i++) {
        unsigned int cand = 0;
        if (!match_result_leave(calls[i], &cand))
            continue;
        best = calls[i];
        ts = cand;
        break;
    }
    if (!best)
        return 0;
    g_rva_result_ret_lo = best;
    g_rva_result_ret_hi = best + 5;
    g_ts_total_result = ts;
    if (!g_rva_game_ptr) {
        uintptr_t rva = find_rip_load_before(best, 0x80);
        if (rva && rva < g_image_size)
            g_rva_game_ptr = rva;
    }
    return 1;
}

static int resolve_set_table_and_stage(void)
{
    /* mov rax,[rip+d] ; mov r?,[rax+0xA0] ; mov r?,[rax+0xA8] then add ?,0x1D0 nearby */
    uintptr_t p;
    uintptr_t end;
    if (!g_text_size)
        return 0;
    end = g_text_rva + g_text_size;
    if (g_text_size < 40)
        return 0;
    end -= 40;
    for (p = g_text_rva; p <= end; p++) {
        unsigned char *b = (unsigned char *)(g_base + p);
        uintptr_t holder;
        uintptr_t q;
        int saw_add = 0;
        int saw_stage = 0;
        uintptr_t stage_off = 0;
        uintptr_t game_abs = 0;
        __try {
            if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x05)
                continue;
            /* next should be mov r64, [r64+0xA0] */
            if (b[7] != 0x48 || b[8] != 0x8B)
                continue;
            if (b[10] != 0xA0 || b[11] != 0x00 || b[12] != 0x00 || b[13] != 0x00)
                continue;
            /* then mov with +0xA8 */
            if (b[14] != 0x48 || b[15] != 0x8B)
                continue;
            if (!(b[17] == 0xA8 && b[18] == 0x00 && b[19] == 0x00 && b[20] == 0x00))
                continue;
            holder = decode_rip_rel32(p, 7);
            if (!holder)
                continue;
            for (q = p; q < p + 0x60 && q + 8 < g_text_rva + g_text_size; q++) {
                unsigned char *c = (unsigned char *)(g_base + q);
                if (c[0] == 0x48 && c[1] == 0x81 &&
                    (c[2] & 0xF8) == 0xC0 &&
                    c[3] == 0xD0 && c[4] == 0x01 && c[5] == 0x00 && c[6] == 0x00)
                    saw_add = 1;
                if (c[0] == 0x81 && (c[1] & 0xF8) == 0xC0 &&
                    c[2] == 0xD0 && c[3] == 0x01 && c[4] == 0x00 && c[5] == 0x00)
                    saw_add = 1;
                /* mov r64,[rip]; mov r64,[r64]; cmp dword [r64+disp],0 */
                if (c[0] == 0x48 && c[1] == 0x8B &&
                    (c[2] == 0x05 || c[2] == 0x0D || c[2] == 0x15 || c[2] == 0x1D)) {
                    uintptr_t ga = decode_rip_rel32(q, 7);
                    unsigned char *d = c + 7;
                    if (d[0] == 0x48 && d[1] == 0x8B &&
                        (d[2] == 0x00 || d[2] == 0x01 || d[2] == 0x02 || d[2] == 0x03 ||
                         d[2] == 0x06 || d[2] == 0x07)) {
                        unsigned char *e = d + 3;
                        if (e[0] == 0x83 && (e[1] & 0xF8) == 0x78 && e[3] == 0x00) {
                            game_abs = ga;
                            stage_off = e[2];
                            saw_stage = 1;
                        }
                        if (e[0] == 0xFF && (e[1] & 0xF8) == 0x40) {
                            game_abs = ga;
                            stage_off = e[2];
                            saw_stage = 1;
                        }
                    }
                }
            }
            if (!saw_add)
                continue;
            if (holder && holder < g_image_size)
                g_rva_set_table = holder;
            if (saw_stage) {
                if (game_abs && game_abs < g_image_size)
                    g_rva_game_ptr = game_abs;
                if (stage_off == 0x08 || stage_off == 0x0C)
                    g_off_stage = stage_off;
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return 0;
}

static int resolve_get_start(void)
{
    uintptr_t str = find_string("arkMDXGetStart");
    uintptr_t lea_str;
    uintptr_t lea_slot;
    uintptr_t slot_abs;
    if (!str)
        return 0;
    lea_str = find_lea_to(str, g_text_rva, g_text_size);
    if (!lea_str)
        return 0;
    /* Registration builds (slot, name) pairs — slot LEA is immediately before name LEA. */
    lea_slot = find_prev_lea_rax(lea_str, 24);
    if (!lea_slot)
        return 0;
    slot_abs = g_base + decode_rip_rel32(lea_slot, 7);
    if (slot_abs <= g_base || slot_abs >= g_base + g_image_size)
        return 0;
    g_rva_get_start = slot_abs - g_base;
    return 1;
}

static int resolve_io_state(void)
{
    /* mov rax,[rip+d] ; imul r64, 0x498 */
    uintptr_t p;
    uintptr_t end;
    if (!g_text_size)
        return 0;
    end = g_text_rva + g_text_size;
    if (g_text_size < 16)
        return 0;
    end -= 16;
    for (p = g_text_rva; p <= end; p++) {
        unsigned char *b = (unsigned char *)(g_base + p);
        uintptr_t abs;
        __try {
            if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x05)
                continue;
            if (b[7] != 0x48 || b[8] != 0x69)
                continue;
            if (b[10] != 0x98 || b[11] != 0x04 || b[12] != 0x00 || b[13] != 0x00)
                continue;
            abs = g_base + decode_rip_rel32(p, 7);
            if (abs <= g_base || abs >= g_base + g_image_size)
                continue;
            g_rva_io_state = abs - g_base;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return 0;
}

static int resolve_stage_off(void)
{
    uintptr_t p;
    uintptr_t end;
    int n8 = 0, nC = 0;
    uintptr_t str;
    uintptr_t xrefs[8];
    int nx;
    int i;

    if (!g_rva_game_ptr || !g_text_size)
        return 0;

    /* Prefer createNext factory: game_ptr load then inc [reg+8/0xC]. */
    str = find_string("sequence::TransitionSequence::createNextSequence");
    if (str) {
        nx = collect_lea_xrefs(str, xrefs, 8);
        for (i = 0; i < nx; i++) {
            uintptr_t q;
            uintptr_t lim = xrefs[i] + 0x400;
            if (lim > g_text_rva + g_text_size)
                lim = g_text_rva + g_text_size;
            for (q = xrefs[i]; q + 16 < lim; q++) {
                unsigned char *b = (unsigned char *)(g_base + q);
                uintptr_t tgt;
                unsigned char *c;
                __try {
                    if (b[0] != 0x48 || b[1] != 0x8B ||
                        !(b[2] == 0x05 || b[2] == 0x0D || b[2] == 0x15 || b[2] == 0x1D))
                        continue;
                    tgt = decode_rip_rel32(q, 7);
                    if (tgt != g_rva_game_ptr)
                        continue;
                    for (c = b + 7; c < b + 28; c++) {
                        if (c[0] == 0x48 && c[1] == 0x8B &&
                            (c[2] == 0x00 || c[2] == 0x01 || c[2] == 0x02 ||
                             c[2] == 0x03 || c[2] == 0x06 || c[2] == 0x07)) {
                            unsigned char *d = c + 3;
                            if (d[0] == 0xFF && (d[1] & 0xF8) == 0x40 &&
                                (d[2] == 0x08 || d[2] == 0x0C)) {
                                g_off_stage = d[2];
                                return 1;
                            }
                        }
                        if (c[0] == 0xFF && (c[1] & 0xF8) == 0x40 &&
                            (c[2] == 0x08 || c[2] == 0x0C)) {
                            g_off_stage = c[2];
                            return 1;
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
            }
        }
    }

    /* Fallback: vote among game_ptr → deref → inc [reg+8/0xC] sites. */
    end = g_text_rva + g_text_size;
    if (g_text_size < 32)
        return 0;
    end -= 32;
    for (p = g_text_rva; p <= end; p++) {
        unsigned char *b = (unsigned char *)(g_base + p);
        uintptr_t tgt;
        unsigned char *c;
        __try {
            if (b[0] != 0x48 || b[1] != 0x8B ||
                !(b[2] == 0x05 || b[2] == 0x0D || b[2] == 0x15 || b[2] == 0x1D))
                continue;
            tgt = decode_rip_rel32(p, 7);
            if (tgt != g_rva_game_ptr)
                continue;
            for (c = b + 7; c < b + 28; c++) {
                if (c[0] == 0xFF && (c[1] & 0xF8) == 0x40 &&
                    (c[2] == 0x08 || c[2] == 0x0C)) {
                    if (c[2] == 0x08)
                        n8++;
                    else
                        nC++;
                    break;
                }
                if (c[0] == 0x48 && c[1] == 0x8B &&
                    (c[2] == 0x00 || c[2] == 0x01 || c[2] == 0x02 ||
                     c[2] == 0x03 || c[2] == 0x06 || c[2] == 0x07)) {
                    unsigned char *d = c + 3;
                    if (d[0] == 0xFF && (d[1] & 0xF8) == 0x40 &&
                        (d[2] == 0x08 || d[2] == 0x0C)) {
                        if (d[2] == 0x08)
                            n8++;
                        else
                            nC++;
                        break;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    if (n8 || nC) {
        g_off_stage = (n8 >= nC) ? 0x08 : 0x0C;
        return 1;
    }
    /* Last resort from TotalResult imm (54=A3/+8, 56=WORLD/+0xC). */
    if (g_ts_total_result == 54) {
        g_off_stage = 0x08;
        return 1;
    }
    if (g_ts_total_result == 56) {
        g_off_stage = 0x0C;
        return 1;
    }
    return 0;
}

static int resolve_symbols(void)
{
    int ok = 1;
    if (!pe_sections()) {
        log_msg("resolve: PE sections failed");
        return 0;
    }
    /* Always need finish for unpack wait / optional loop hook. */
    if (!g_rva_finish && !resolve_finish()) {
        log_msg("resolve: finish signature not found");
        ok = 0;
    }
    if (g_skip_song) {
        if (!g_rva_advance_state && !resolve_advance()) {
            log_msg("resolve: advance_state signature not found");
            ok = 0;
        }
        if (!g_rva_mdp_update && !resolve_mdp()) {
            log_msg("resolve: MatchingDancePlay onUpdate not found");
            ok = 0;
        }
        if (!g_rva_dp_update && !resolve_dp()) {
            log_msg("resolve: DancePlay onUpdate not found");
            ok = 0;
        }
        if (!g_rva_get_start && !resolve_get_start()) {
            log_msg("resolve: arkMDXGetStart slot not found");
            ok = 0;
        }
        if (!g_rva_io_state && !resolve_io_state()) {
            log_msg("resolve: io_state not found");
            /* packed IO optional if GetStart works */
        }
    }
    if (g_loop_battle) {
        if (!g_rva_result_ret_lo && !resolve_result_leave()) {
            log_msg("resolve: Result leave call site not found");
            ok = 0;
        }
        if (!g_rva_set_table && !resolve_set_table_and_stage()) {
            log_msg("resolve: matching-set table not found");
            /* soft fail — loop still works without set flag clear */
        }
        if (!resolve_stage_off()) {
            log_msg("resolve: stage off via createNext not found (using 0x%X)",
                    (unsigned)g_off_stage);
        }
        if (!g_rva_game_ptr) {
            log_msg("resolve: game_ptr not found");
            ok = 0;
        }
    }
    return ok;
}

static uint8_t *game_object(void)
{
    __try {
        uint8_t *holder = *(uint8_t **)(g_base + g_rva_game_ptr);
        if (!ptr_ok(holder))
            return NULL;
        uint8_t *game = *(uint8_t **)holder;
        return ptr_ok(game) ? game : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

static int current_stage(uint8_t *game)
{
    if (!ptr_ok(game))
        return -1;
    __try {
        return *(int *)(game + g_off_stage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int cstr_bounded(const char *s, size_t max_len)
{
    size_t i;
    if (!ptr_ok(s) || max_len == 0)
        return 0;
    __try {
        for (i = 0; i < max_len; i++) {
            if (s[i] == 0)
                return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static const char *matching_set_name(uint8_t *game)
{
    if (!ptr_ok(game))
        return NULL;
    __try {
        uint64_t cap = *(uint64_t *)(game + 0x78);
        if (cap >= 16) {
            if (cap > 0x10000)
                return NULL;
            const char *heap = *(const char **)(game + 0x60);
            if (!cstr_bounded(heap, 64))
                return NULL;
            return heap;
        }
        {
            const char *sso = (const char *)(game + 0x60);
            if (!cstr_bounded(sso, 16))
                return NULL;
            return sso;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

static uint8_t *lookup_matching_set(const char *name)
{
    const size_t kMaxEntries = 4096;
    const size_t kMaxName = 64;
    size_t name_len;
    size_t n;
    size_t i;

    if (!ptr_ok(name) || name[0] == 0 || !cstr_bounded(name, kMaxName))
        return NULL;
    name_len = strnlen(name, kMaxName);

    if (!g_rva_set_table)
        return NULL;

    __try {
        uint8_t *holder = *(uint8_t **)(g_base + g_rva_set_table);
        uint8_t *begin;
        uint8_t *end;
        uint8_t *p;
        uintptr_t span;
        if (!ptr_ok(holder))
            return NULL;
        begin = *(uint8_t **)(holder + kSetBeginOff);
        end = *(uint8_t **)(holder + kSetEndOff);
        if (!ptr_ok(begin) || !ptr_ok(end) || begin >= end)
            return NULL;
        span = (uintptr_t)(end - begin);
        if (span % kSetStride != 0)
            return NULL;
        n = span / kSetStride;
        if (n == 0 || n > kMaxEntries)
            return NULL;
        for (i = 0, p = begin; i < n; i++, p += kSetStride) {
            const char *entry;
            if (!ptr_ok(p))
                return NULL;
            entry = (const char *)(p + kSetNameOff);
            if (!cstr_bounded(entry, kMaxName))
                continue;
            if (strncmp(entry, name, kMaxName) == 0 && entry[name_len] == 0)
                return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return NULL;
}

static void reset_matching_set(uint8_t *game)
{
    if (!ptr_ok(game))
        return;
    __try {
        int stage = *(int *)(game + g_off_stage);
        if (stage < 0 || stage > 16)
            return;
        *(int *)(game + g_off_stage) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    {
        const char *name = matching_set_name(game);
        uint8_t *set;
        if (!ptr_ok(name))
            return;
        set = lookup_matching_set(name);
        if (!ptr_ok(set))
            return;
        __try {
            *(uint8_t *)(set + kSetFlagOff) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
    }
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
    return held || trigger;
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

static int packed_io_bit(unsigned int player, int bit)
{
    unsigned char *base;
    unsigned int *slot;
    unsigned int mask;
    if (!g_base || !g_rva_io_state)
        return 0;
    if (bit < 0 || bit > 31)
        return 0;
    __try {
        base = *(unsigned char **)(g_base + g_rva_io_state);
        if (!ptr_ok(base))
            return 0;
        slot = (unsigned int *)(base + 1176u * player);
        mask = 1u << bit;
        if ((slot[1] | slot[2]) & mask)
            return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static int start_held(void)
{
    ArkIo3Fn fn = start_fn();
    unsigned int player;
    for (player = 0; player < 2; player++) {
        if (packed_io_bit(player, 0))
            return 1;
        if (fn && player_held3(fn, player))
            return 1;
    }
    return 0;
}

static void maybe_skip_song(void *seq, int play_case, int after_play)
{
    static DWORD s_held_since;
    static int s_skipping;

    if (!g_skip_song || !g_ready || !seq || !g_rva_advance_state)
        return;

    __try {
        uint16_t idx = *(uint16_t *)((uint8_t *)seq + 0x68 + 0x2A);
        int cur;
        if (idx >= 32)
            return;
        cur = *(int *)((uint8_t *)seq + 0x68 + (size_t)idx * 8);
        if (cur != play_case) {
            s_held_since = 0;
            if (cur > play_case)
                s_skipping = 0;
            return;
        }
        if (!start_held()) {
            s_held_since = 0;
            s_skipping = 0;
            return;
        }
        {
            DWORD now = GetTickCount();
            if (!s_held_since)
                s_held_since = now;
            if (s_skipping)
                return;
            if (now - s_held_since < (DWORD)g_skip_hold_ms)
                return;
            log_msg("hold Start: skip song case %d -> %d", cur, after_play);
            {
                AdvanceStateFn adv = (AdvanceStateFn)(g_base + g_rva_advance_state);
                adv((uint8_t *)seq + 0x68, after_play);
            }
            s_skipping = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

static __int64 __fastcall detour_mdp(void *seq)
{
    maybe_skip_song(seq, g_mdp_play, g_mdp_after_play);
    if (!g_orig_mdp)
        return 0;
    return g_orig_mdp(seq);
}

static __int64 __fastcall detour_dp(void *seq)
{
    maybe_skip_song(seq, g_dp_play, g_dp_after_play);
    if (!g_orig_dp)
        return 0;
    return g_orig_dp(seq);
}

static void __fastcall detour_finish(void *seq, unsigned int nextId)
{
    void *ret = _ReturnAddress();
    uintptr_t rva = (uintptr_t)ret - g_base;
    int from_result = (rva >= g_rva_result_ret_lo && rva <= g_rva_result_ret_hi);
    int stage = -1;
    uint8_t *game = NULL;

    if (!g_orig_finish)
        return;

    game = game_object();
    stage = current_stage(game);

    if (!g_ready || !g_loop_battle || !from_result) {
        g_orig_finish(seq, nextId);
        return;
    }

    /* Learn non-TotalResult continue id from Result (A3 uses 53 after song 1). */
    if (nextId != 0 && nextId != g_ts_total_result && nextId != g_ts_matching_transition)
        g_ts_result_continue = nextId;

    if (nextId != g_ts_total_result) {
        g_orig_finish(seq, nextId);
        return;
    }

    /*
     * A3 matching can request TotalResult after song 1 on fail (winner may
     * still continue). Never use leave-site edx=0x21 as continue — that exits.
     * Learned continue from a prior Result is preferred (A3 observed 53).
     */
    if (stage < 1) {
        unsigned int cont = g_ts_result_continue;
        if (!cont) {
            /* A3 TotalResult=54 → continue was 53; WORLD rarely hits this path. */
            cont = (g_ts_total_result == 54) ? 53u : 0;
        }
        if (!cont) {
            g_orig_finish(seq, nextId);
            return;
        }
        log_msg("loop: Result nextId %u stage %d -> continue TS %u (force matching next)",
                nextId, stage, cont);
        g_orig_finish(seq, cont);
        return;
    }

    {
        reset_matching_set(game);
        log_msg("loop: Result nextId %u stage %d -> TS %u (stage 0)",
                nextId, stage, g_ts_matching_transition);
        g_orig_finish(seq, g_ts_matching_transition);
    }
}

static int wait_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int stable = 0;
    int resolved = 0;
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            if (!resolved) {
                if (!pe_sections() || !resolve_symbols()) {
                    stable = 0;
                    Sleep(50);
                    continue;
                }
                resolved = 1;
                log_msg("patterns resolved");
            }
            if (!g_rva_finish) {
                stable = 0;
            } else if (bytes_match((void *)(g_base + g_rva_finish), kFinishSig, 15)) {
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

static void close_log(void)
{
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

static void mh_teardown(void)
{
    if (g_hooks_enabled) {
        MH_DisableHook(MH_ALL_HOOKS);
        g_hooks_enabled = 0;
    }
    if (g_mh_inited) {
        MH_Uninitialize();
        g_mh_inited = 0;
    }
    g_orig_finish = NULL;
    g_orig_mdp = NULL;
    g_orig_dp = NULL;
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;
    int hooks = 0;

    InitializeCriticalSection(&g_log_cs);
    g_cs_ready = 1;
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_loop_battle = ini_int(L"loop_battle", 1);
    g_skip_song = ini_int(L"skip_song", 1);
    g_skip_hold_ms = ini_int(L"skip_hold_ms", 400);
    if (g_skip_hold_ms < 0)
        g_skip_hold_ms = 0;
    open_log();
    log_msg("bpl_loop starting (pattern resolve, loop_battle=%d skip_song=%d hold_ms=%d)",
            g_loop_battle, g_skip_song, g_skip_hold_ms);

    if (!wait_unpacked()) {
        log_msg("timeout waiting for gamemdx.dll / pattern resolve failed");
        return 1;
    }
    Sleep(200);
    log_msg("gamemdx at %p (ts_total=%u) rva_finish=0x%X rva_dp=0x%X rva_mdp=0x%X "
            "rva_adv=0x%X rva_get_start=0x%X rva_io=0x%X rva_game=0x%X "
            "rva_set=0x%X ret=0x%X-0x%X off_stage=0x%X",
            (void *)g_base, g_ts_total_result,
            (unsigned)g_rva_finish, (unsigned)g_rva_dp_update, (unsigned)g_rva_mdp_update,
            (unsigned)g_rva_advance_state, (unsigned)g_rva_get_start, (unsigned)g_rva_io_state,
            (unsigned)g_rva_game_ptr, (unsigned)g_rva_set_table,
            (unsigned)g_rva_result_ret_lo, (unsigned)g_rva_result_ret_hi,
            (unsigned)g_off_stage);

    if (!g_loop_battle && !g_skip_song) {
        log_msg("both loop_battle and skip_song disabled; no hooks installed");
        InterlockedExchange(&g_ready, 1);
        return 0;
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }
    g_mh_inited = 1;

    if (g_loop_battle) {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_finish), (LPVOID)detour_finish,
                           (LPVOID *)&g_orig_finish);
        if (st != MH_OK) {
            log_msg("MH_CreateHook finish: %s", MH_StatusToString(st));
            mh_teardown();
            return 1;
        }
        hooks++;
    }

    if (g_skip_song) {
        if (g_rva_mdp_update) {
            st = MH_CreateHook((LPVOID)(g_base + g_rva_mdp_update), (LPVOID)detour_mdp,
                               (LPVOID *)&g_orig_mdp);
            if (st != MH_OK)
                log_msg("MH_CreateHook MatchingDancePlay: %s", MH_StatusToString(st));
            else
                hooks++;
        }
        if (g_rva_dp_update) {
            st = MH_CreateHook((LPVOID)(g_base + g_rva_dp_update), (LPVOID)detour_dp,
                               (LPVOID *)&g_orig_dp);
            if (st != MH_OK)
                log_msg("MH_CreateHook DancePlay: %s", MH_StatusToString(st));
            else
                hooks++;
        }
    }

    if (hooks == 0) {
        log_msg("no hooks created; tearing down MinHook");
        mh_teardown();
        InterlockedExchange(&g_ready, 1);
        return 0;
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        mh_teardown();
        return 1;
    }
    g_hooks_enabled = 1;

    InterlockedExchange(&g_ready, 1);
    log_msg("hooks enabled (loop_battle=%d skip_song=%d; loop TS %u -> %u)",
            g_loop_battle, g_skip_song, g_ts_total_result, g_ts_matching_transition);
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
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_ready, 0);
        mh_teardown();
        close_log();
        if (g_cs_ready) {
            DeleteCriticalSection(&g_log_cs);
            g_cs_ready = 0;
        }
    }
    return TRUE;
}
