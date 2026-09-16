#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "MinHook.h"

/*
 * Spice -k hook (WORLD 64-bit / CrossOver+Wine).
 * Queue CS around enqueue 0x172FC0 and consume 0x173760 (held during the
 * walk). Texture ring writers JMP through enqueue. 0x1676C0 enqueues after
 * releasing the texture table lock. VB/IB writers stay native.
 * Failed CreateTexture (dummy slot+154) is retried as DEFAULT+DYNAMIC;
 * ~40k-wide float skinning maps become 4xH. 0x177BB0 caps blit rows to
 * the texture height and restores the generation lock on AV.
 * Walk stub: JAE ends, skip NULL/bad type, huge/size<12 leaves the walk,
 * per-packet SEH. Do not hook 0x168350. F10 "Skip null XACT commands" off.
 */

#ifdef _WIN64
#define INI_NAME L"pump_guard_64bit.ini"
#define LOG_NAME L"pump_guard_64bit.log"
static const uintptr_t kDefaultDispatch = 0x1730D0;
static const uintptr_t kDefaultOuter = 0x173760;
static const uintptr_t kDefaultEnqueue = 0x172FC0;
static const uintptr_t kDefaultFlush = 0x173860;
static const uintptr_t kDefaultWalkOther = 0x173BC0;
static const uintptr_t kDefaultCall = 0x173267;
static const uintptr_t kDefaultJae1 = 0x173367;
static const uintptr_t kDefaultJae2 = 0x173375;
static const uintptr_t kDefaultCont = 0x17326F;
static const uintptr_t kDefaultExit = 0x173380;
static const uintptr_t kDefaultAdvance = 0x173360;
static const uintptr_t kRvaLockGlobal = 0x2EDF28;
static const uintptr_t kRvaLockPump = 0x2EDF40;
static const uintptr_t kRvaFlip = 0x2EDF44;
static const uintptr_t kRvaSlots = 0x2EDF50;
static const uintptr_t kRvaByteFlag = 0x2ED73A;
static const uintptr_t kRvaTexTableLock = 0x2EDA88;
static const uintptr_t kRvaTexTable = 0x2EDA80;
static const uintptr_t kRvaDummyTex = 0x2EF150;
static const uintptr_t kRvaSkinBlit = 0x177BB0;
static const uintptr_t kRvaCreateTex = 0x168090;
static const unsigned char kDispatchPrologue[] = {
    0x4C, 0x8B, 0xDC, 0x53, 0x41, 0x54, 0x41, 0x55
};
static const unsigned char kOuterPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10
};
static const unsigned char kEnqueuePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10
};
static const unsigned char kFlushPrologue[] = {
    0x40, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x40
};
static const unsigned char kWalkOtherPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20
};
static const unsigned char kCallInsn[] = {
    0x42, 0xFF, 0x94, 0xD4, 0xB8, 0x00, 0x00, 0x00
};
typedef __int64 (__fastcall *DispatchFn)(__int64 a1, __int64 a2);
typedef __int64 (__fastcall *OuterFn)(__int64 a1);
typedef __int64 (__fastcall *EnqueueFn)(unsigned char a1, unsigned char a2, unsigned a3,
                                        unsigned short a4, void *src, size_t size);
typedef __int64 (__fastcall *NoArgFn)(void);
typedef __int64 (__fastcall *CmdHandler)(__int64 a1, unsigned int a2, unsigned int a3,
                                         const void *a4, unsigned int a5, unsigned int a6);
typedef char (__fastcall *SkinBlitFn)(__int64 a1, __int64 a2);
typedef int (__fastcall *CreateTexFn)(__int64 device, unsigned id, BYTE *payload);
#else
#error pump_guard is WORLD 64-bit only
#endif

static HMODULE g_self;
static uintptr_t g_base;
static OuterFn g_orig_outer;
static EnqueueFn g_orig_enqueue;
static NoArgFn g_orig_flush;
static NoArgFn g_orig_walk_other;
static SkinBlitFn g_orig_skin_blit;
static CreateTexFn g_orig_create_tex;
static BYTE *g_stub;
static volatile LONG g_ready;
static volatile LONG g_seh_dispatch_n;
static volatile LONG g_seh_reset_n;
static volatile LONG g_seh_packet_n;
static volatile LONG g_size0_n;
static volatile LONG g_last_bad_size;
static volatile LONG g_dxt_skip_n;
static volatile LONG g_skin_seh_n;
static volatile CmdHandler g_packet_fn;
static DWORD g_last_ex_code;
static void *g_last_ex_addr;

static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_verify_prologue = 1;
static int g_seh_dispatch = 1;
static int g_seh_packet = 1;
static int g_patch_walk = 1;
static int g_hook_outer = 1;
static int g_yield_sleep = 1;
static int g_hook_enqueue = 1;
static int g_use_queue_cs = 1;
static uintptr_t g_rva_dispatch = kDefaultDispatch;
static uintptr_t g_rva_outer = kDefaultOuter;
static uintptr_t g_rva_enqueue = kDefaultEnqueue;
static uintptr_t g_rva_flush = kDefaultFlush;
static uintptr_t g_rva_walk_other = kDefaultWalkOther;
static uintptr_t g_rva_call = kDefaultCall;
static uintptr_t g_rva_jae1 = kDefaultJae1;
static uintptr_t g_rva_jae2 = kDefaultJae2;
static uintptr_t g_rva_cont = kDefaultCont;
static uintptr_t g_rva_exit = kDefaultExit;
static uintptr_t g_rva_advance = kDefaultAdvance;

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_queue_cs;
static volatile LONG g_queue_cs_ready;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static void queue_enter(void)
{
    if (g_use_queue_cs && g_queue_cs_ready)
        EnterCriticalSection(&g_queue_cs);
}

static void queue_leave(void)
{
    if (g_use_queue_cs && g_queue_cs_ready)
        LeaveCriticalSection(&g_queue_cs);
}

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
    return (int)GetPrivateProfileIntW(L"pump_guard", key, def, ini);
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
    GetPrivateProfileStringW(L"pump_guard", key, L"", buf, 64, ini);
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

#define PATCH_MAX 16
#define STUB_MAX 16
struct PatchSlot {
    void *addr;
    unsigned char saved[16];
    size_t len;
    int applied;
};
static PatchSlot g_patches[PATCH_MAX];
static int g_patch_count;
static void *g_owned_stubs[STUB_MAX];
static int g_stub_count;

static void track_stub(void *p)
{
    int i;
    if (!p)
        return;
    for (i = 0; i < g_stub_count; i++) {
        if (g_owned_stubs[i] == p)
            return;
    }
    if (g_stub_count < STUB_MAX)
        g_owned_stubs[g_stub_count++] = p;
}

static void free_owned_stubs(void)
{
    int i;
    for (i = 0; i < g_stub_count; i++) {
        if (g_owned_stubs[i])
            VirtualFree(g_owned_stubs[i], 0, MEM_RELEASE);
        g_owned_stubs[i] = NULL;
    }
    g_stub_count = 0;
    g_stub = NULL;
}

static int write_bytes(void *dst, const void *src, size_t n)
{
    DWORD old = 0;
    SIZE_T wrote = 0;
    int ok = 0;
    __try {
        if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
            return 0;
        if (WriteProcessMemory(GetCurrentProcess(), dst, src, n, &wrote) && wrote == n) {
            FlushInstructionCache(GetCurrentProcess(), dst, n);
            ok = 1;
        }
        VirtualProtect(dst, n, old, &old);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return ok;
}

/* Save stock bytes then patch; tracked for restore_patches(). */
static int apply_patch(void *addr, const void *src, size_t n)
{
    PatchSlot *slot;
    if (!addr || !src || n == 0 || n > sizeof(g_patches[0].saved))
        return 0;
    if (g_patch_count >= PATCH_MAX)
        return 0;
    slot = &g_patches[g_patch_count];
    __try {
        memcpy(slot->saved, addr, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!write_bytes(addr, src, n))
        return 0;
    slot->addr = addr;
    slot->len = n;
    slot->applied = 1;
    g_patch_count++;
    return 1;
}

static void restore_patches(void)
{
    int i;
    for (i = g_patch_count - 1; i >= 0; i--) {
        if (g_patches[i].applied)
            write_bytes(g_patches[i].addr, g_patches[i].saved, g_patches[i].len);
        g_patches[i].applied = 0;
    }
    g_patch_count = 0;
    free_owned_stubs();
}

static void emit_rel32(BYTE *p, BYTE *from, uintptr_t to)
{
    INT64 delta = (INT64)to - (INT64)(from + 5);
    INT32 rel = (INT32)delta;
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
}

static int rel32_ok(BYTE *from, uintptr_t to)
{
    INT64 delta = (INT64)to - (INT64)(from + 5);
    return delta == (INT32)delta;
}

static void emit_mov_r11_imm64(BYTE *p, UINT64 v)
{
    p[0] = 0x49;
    p[1] = 0xBB;
    memcpy(p + 2, &v, 8);
}

/* E9 is +/-2GB; a default VirtualAlloc is often in another 64-bit region. */
static BYTE *alloc_near(void *near_addr, size_t size)
{
    SYSTEM_INFO si;
    uintptr_t gran, origin, off;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    origin = (uintptr_t)near_addr & ~(gran - 1);
    for (off = 0; off < 0x70000000ull; off += gran) {
        uintptr_t try_addr[2];
        int i;
        try_addr[0] = origin + off;
        try_addr[1] = (origin >= off) ? origin - off : 0;
        for (i = 0; i < 2; i++) {
            BYTE *m;
            if (!try_addr[i] || (i == 1 && off == 0))
                continue;
            m = (BYTE *)VirtualAlloc((void *)try_addr[i], size,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (m)
                return m;
        }
    }
    return NULL;
}

static LONG WINAPI packet_filter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord) {
        g_last_ex_code = ep->ExceptionRecord->ExceptionCode;
        g_last_ex_addr = ep->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void __fastcall on_bad_size(void *pkt)
{
    unsigned char b[16];
    unsigned sz = 0, id = 0, typ = 0;
    LONG n;

    memset(b, 0, sizeof(b));
    __try {
        memcpy(b, pkt, 16);
        typ = b[0];
        id = *(unsigned *)(b + 4);
        sz = *(unsigned *)(b + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sz = 0xFFFFFFFFu;
    }
    InterlockedExchange(&g_last_bad_size, (LONG)sz);
    n = InterlockedIncrement(&g_size0_n);
    if (n <= 20 || (n & 255) == 0)
        log_msg("walk bad size=%u type=%u id=%u hdr=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X count=%ld",
                sz, typ, id,
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], n);
}

static BYTE *tex_slot(unsigned id)
{
    BYTE *table;
    BYTE *slot;

    if (!g_base || id == 0)
        return NULL;
    __try {
        table = *(BYTE **)(g_base + kRvaTexTable);
        if (!table)
            return NULL;
        slot = table + 160ull * (id >> 17);
        if (*(unsigned *)slot == id)
            return slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return NULL;
}

static int dxt_lock_skip_reason(unsigned id, const void *payload, unsigned payload_size)
{
    BYTE *slot;
    void *com;
    void *vt;
    int i;

    if (!g_base || id == 0)
        return 0;
    slot = tex_slot(id);
    __try {
        if (!slot)
            return 0;
        if (slot[154])
            return 2;
        com = NULL;
        if (payload && payload_size >= 8)
            com = *(void **)payload;
        if (!com)
            com = *(void **)(slot + 40);
        if (!com)
            return 1;
        for (i = 0; i < 6; i++) {
            if (com == *(void **)(g_base + kRvaDummyTex + (uintptr_t)i * 8))
                return 2;
        }
        vt = *(void **)com;
        if (!vt)
            return 1;
        (void)*(void **)((BYTE *)vt + 0x98);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

/* Called from the walk stub with the six handler args already in place. */
static __int64 __fastcall seh_invoke_c(__int64 a1, unsigned int a2, unsigned int a3,
                                       const void *a4, unsigned int a5, unsigned int a6)
{
    CmdHandler h = (CmdHandler)g_packet_fn;
    __int64 r = 0;
    int skip;

    if (!h)
        return 0;
    /* Type-7 sub 3 is a 52-byte DXT Lock (payload 40). Dummy / bad COM AVs at 0x1685A8. */
    if (a3 == 3 && a5 == 40) {
        skip = dxt_lock_skip_reason(a2, a4, a5);
        if (skip) {
            LONG n = InterlockedIncrement(&g_dxt_skip_n);
            if (n <= 40 || (n & 255) == 0)
                log_msg("dxt lock skip texid=%u reason=%d payload0=%p count=%ld",
                        a2, skip, a4 ? *(void **)a4 : NULL, n);
            return 0;
        }
    }
    __try {
        r = h(a1, a2, a3, a4, a5, a6);
    } __except (packet_filter(GetExceptionInformation())) {
        LONG n = InterlockedIncrement(&g_seh_packet_n);
        uintptr_t rva = (g_base && g_last_ex_addr)
            ? (uintptr_t)g_last_ex_addr - g_base : 0;
        if (n <= 40 || (n & 255) == 0)
            log_msg("packet SEH code=%08X rva=0x%X texid=%u sub=%u psz=%u count=%ld (skipped)",
                    g_last_ex_code, (unsigned)rva, a2, a3, a5, n);
        r = 0;
    }
    return r;
}

static int patch_walk(void)
{
    BYTE *call = (BYTE *)(g_base + g_rva_call);
    BYTE *jae1 = (BYTE *)(g_base + g_rva_jae1);
    BYTE *jae2 = (BYTE *)(g_base + g_rva_jae2);
    BYTE stub[192];
    BYTE jmp[8];
    BYTE jae = 0x73;
    size_t n;
    size_t jb_sz, ja_sz, ja_ty, jz_null, at_adv, at_badsz;
    int use_seh = g_seh_packet;

    if (!bytes_match(call, kCallInsn, sizeof(kCallInsn))) {
        log_msg("call site mismatch at 0x%X (F10 hex patch still on? turn it off)",
                (unsigned)g_rva_call);
        return 0;
    }
    if (!bytes_match(jae1, (const unsigned char *)"\x74", 1) ||
        !bytes_match(jae2, (const unsigned char *)"\x74", 1)) {
        log_msg("end-check sites mismatch (expected JE)");
        return 0;
    }

    g_stub = alloc_near(call, 192);
    if (!g_stub) {
        log_msg("alloc_near stub failed");
        return 0;
    }
    if (!rel32_ok(call, (uintptr_t)g_stub) ||
        !rel32_ok(g_stub, g_base + g_rva_cont) ||
        !rel32_ok(g_stub, g_base + g_rva_exit) ||
        !rel32_ok(g_stub, g_base + g_rva_advance)) {
        log_msg("stub %p not within rel32 of call/cont/exit/advance", (void *)g_stub);
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = NULL;
        return 0;
    }

    memset(stub, 0x90, sizeof(stub));
    n = 0;
    /* cmp dword ptr [rdi+8], 0Ch  (size < 12: leave; do not rewrite) */
    stub[n++] = 0x83;
    stub[n++] = 0x7F;
    stub[n++] = 0x08;
    stub[n++] = 0x0C;
    jb_sz = n;
    stub[n++] = 0x72;
    stub[n++] = 0x00;
    /* cmp dword ptr [rdi+8], 01000000h  (huge: leave) */
    stub[n++] = 0x81;
    stub[n++] = 0x7F;
    stub[n++] = 0x08;
    stub[n++] = 0x00;
    stub[n++] = 0x00;
    stub[n++] = 0x00;
    stub[n++] = 0x01;
    ja_sz = n;
    stub[n++] = 0x77;
    stub[n++] = 0x00;
    /* r10 is type*2. Types 0-8 => r10 0-16. */
    stub[n++] = 0x41;
    stub[n++] = 0x83;
    stub[n++] = 0xFA;
    stub[n++] = 0x10;
    ja_ty = n;
    stub[n++] = 0x77;
    stub[n++] = 0x00;
    /* mov rax, [rsp+r10*8+0B8h] */
    stub[n++] = 0x42;
    stub[n++] = 0x8B;
    stub[n++] = 0x84;
    stub[n++] = 0xD4;
    stub[n++] = 0xB8;
    stub[n++] = 0x00;
    stub[n++] = 0x00;
    stub[n++] = 0x00;
    /* test rax, rax */
    stub[n++] = 0x48;
    stub[n++] = 0x85;
    stub[n++] = 0xC0;
    jz_null = n;
    stub[n++] = 0x74;
    stub[n++] = 0x00;
    if (use_seh) {
        /* mov r11, &g_packet_fn / mov [r11], rax / mov r11, seh_invoke_c / call r11 */
        emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)&g_packet_fn);
        n += 10;
        stub[n++] = 0x49;
        stub[n++] = 0x89;
        stub[n++] = 0x03;
        emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)seh_invoke_c);
        n += 10;
        stub[n++] = 0x41;
        stub[n++] = 0xFF;
        stub[n++] = 0xD3;
    } else {
        stub[n++] = 0xFF;
        stub[n++] = 0xD0;
    }
    emit_rel32(stub + n, g_stub + n, g_base + g_rva_cont);
    n += 5;
    at_adv = n;
    emit_rel32(stub + n, g_stub + n, g_base + g_rva_advance);
    n += 5;
    emit_rel32(stub + n, g_stub + n, g_base + g_rva_exit);
    n += 5;
    at_badsz = n;
    /* rdi = packet. Dump header then leave; do not rewrite size. */
    stub[n++] = 0x48;
    stub[n++] = 0x89;
    stub[n++] = 0xF9;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xEC;
    stub[n++] = 0x20;
    emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)on_bad_size);
    n += 10;
    stub[n++] = 0x41;
    stub[n++] = 0xFF;
    stub[n++] = 0xD3;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xC4;
    stub[n++] = 0x20;
    emit_rel32(stub + n, g_stub + n, g_base + g_rva_exit);
    n += 5;

    if ((at_badsz - (jb_sz + 2)) > 127 || (at_badsz - (ja_sz + 2)) > 127) {
        log_msg("walk stub rel8 overflow badsz=%u", (unsigned)at_badsz);
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = NULL;
        return 0;
    }
    stub[jb_sz + 1] = (BYTE)(at_badsz - (jb_sz + 2));
    stub[ja_sz + 1] = (BYTE)(at_badsz - (ja_sz + 2));
    /* Bad type / NULL handler: skip the call but keep walking (do not drop DXT). */
    stub[ja_ty + 1] = (BYTE)(at_adv - (ja_ty + 2));
    stub[jz_null + 1] = (BYTE)(at_adv - (jz_null + 2));
    memcpy(g_stub, stub, n);
    FlushInstructionCache(GetCurrentProcess(), g_stub, n);
    track_stub(g_stub);

    memset(jmp, 0x90, sizeof(jmp));
    emit_rel32(jmp, call, (uintptr_t)g_stub);
    if (!apply_patch(call, jmp, 8)) {
        log_msg("failed to patch call site");
        return 0;
    }
    if (!apply_patch(jae1, &jae, 1) || !apply_patch(jae2, &jae, 1)) {
        log_msg("failed to patch jae");
        return 0;
    }
    log_msg("walk stub at %p (%u bytes, seh_packet=%d bad_size_exit)",
            (void *)g_stub, (unsigned)n, use_seh);
    return 1;
}

static BOOL WINAPI sleep_yield(void)
{
    Sleep(0);
    return TRUE;
}

static int hook_iat_switchtothread(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int n = 0;

    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
        return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA64 *orig;
        IMAGE_THUNK_DATA64 *th;
        if (!imp->OriginalFirstThunk)
            continue;
        orig = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        th = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, th++) {
            IMAGE_IMPORT_BY_NAME *nm;
            DWORD old = 0;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue;
            nm = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            if (strcmp((char *)nm->Name, "SwitchToThread") != 0)
                continue;
            if (!VirtualProtect(&th->u1.Function, sizeof(ULONGLONG), PAGE_EXECUTE_READWRITE, &old))
                continue;
            th->u1.Function = (ULONGLONG)(uintptr_t)sleep_yield;
            VirtualProtect(&th->u1.Function, sizeof(ULONGLONG), old, &old);
            n++;
        }
    }
    return n;
}

static void spin_take(volatile LONG *lock)
{
    while (InterlockedExchangeAdd(lock, 1) != 0)
        sleep_yield();
}

/* Replaces 0x173760 so an AV in the inner walk cannot return with rbx=0.
 * CRITICAL_SECTION is taken before the game spinlocks so enqueue cannot
 * publish a torn packet while this thread walks. */
static __int64 __fastcall detour_outer(__int64 a1)
{
    volatile LONG *lock_g;
    volatile LONG *lock_p;
    volatile LONG *flip;
    __int64 *slots;
    DispatchFn inner;
    unsigned int idx;
    __int64 *v2;
    volatile LONG *buf_lock;
    __int64 r = 0;

    if (!g_ready) {
        if (g_orig_outer)
            return g_orig_outer(a1);
        return 0;
    }

    lock_g = (volatile LONG *)(g_base + kRvaLockGlobal);
    lock_p = (volatile LONG *)(g_base + kRvaLockPump);
    flip = (volatile LONG *)(g_base + kRvaFlip);
    slots = (__int64 *)(g_base + kRvaSlots);
    inner = (DispatchFn)(g_base + g_rva_dispatch);

    queue_enter();
    spin_take(lock_g);
    idx = (unsigned int)*flip;
    if (idx > 1)
        idx &= 1;
    v2 = slots + 5 * (uintptr_t)idx;
    *flip = 1 - (LONG)idx;
    buf_lock = (volatile LONG *)((BYTE *)v2 + 0x24);
    spin_take(buf_lock);
    InterlockedExchange(lock_g, 0);
    spin_take(lock_p);
    MemoryBarrier();

    if (g_seh_dispatch) {
        __try {
            r = inner(a1, (__int64)v2);
        } __except (packet_filter(GetExceptionInformation())) {
            LONG n = InterlockedIncrement(&g_seh_dispatch_n);
            if (n <= 20 || (n & 255) == 0)
                log_msg("dispatch SEH code=%08X addr=%p rva=0x%X count=%ld (queue reset; remaining cmds in this buffer dropped)",
                        g_last_ex_code, g_last_ex_addr,
                        g_base ? (unsigned)((uintptr_t)g_last_ex_addr - g_base) : 0, n);
            r = 0;
        }
    } else {
        r = inner(a1, (__int64)v2);
    }
    {
        static LONG last_s0, last_pk;
        LONG s0 = g_size0_n;
        LONG pk = g_seh_packet_n;
        LONG sum = s0 + pk;
        if (s0 != last_s0 || pk != last_pk) {
            if (sum <= 40 || (sum & 255) == 0) {
                last_s0 = s0;
                last_pk = pk;
                log_msg("walk bad_size_n=%ld last_size=%ld packet_seh=%ld dxt_skip=%ld dispatch_seh=%ld",
                        s0, (LONG)g_last_bad_size, pk, (LONG)g_dxt_skip_n, (LONG)g_seh_dispatch_n);
            }
        }
    }

    __try {
        __int64 head = *v2;
        v2[2] = head;
        v2[3] = head;
        *((DWORD *)v2 + 8) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LONG n = InterlockedIncrement(&g_seh_reset_n);
        if (n <= 20)
            log_msg("queue reset SEH code=%08X count=%ld", GetExceptionCode(), n);
    }
    *(BYTE *)(g_base + kRvaByteFlag) = 0;
    InterlockedExchange(lock_p, 0);
    InterlockedExchange(buf_lock, 0);
    queue_leave();
    return r;
}

static __int64 detour_flush(void)
{
    __int64 r;
    if (!g_ready)
        return g_orig_flush ? g_orig_flush() : 0;
    queue_enter();
    r = g_orig_flush ? g_orig_flush() : 0;
    queue_leave();
    return r;
}

static __int64 detour_walk_other(void)
{
    __int64 r;
    if (!g_ready)
        return g_orig_walk_other ? g_orig_walk_other() : 0;
    queue_enter();
    r = g_orig_walk_other ? g_orig_walk_other() : 0;
    queue_leave();
    return r;
}

static char __fastcall detour_skin_blit(__int64 a1, __int64 a2)
{
    /* Spill before the try: orig clobbers rcx/rdx, and /EHs- will not
     * keep the incoming parameters alive for the except block. */
    volatile __int64 ctx = a1;
    volatile __int64 rec = a2;
    char r = 1;
    __int64 obj = 0;
    __int64 mesh = 0;
    unsigned token;
    unsigned saved_h = 0;
    int capped = 0;
    BYTE *slot;
    unsigned texid, tex_h;

    if (!g_ready || !g_orig_skin_blit)
        return g_orig_skin_blit ? g_orig_skin_blit(a1, a2) : 1;
    /* Mesh row count can exceed the texture height (33 vs 12) and walk
     * off the software buffer. Cap to the slot height for this call. */
    __try {
        if (rec && ctx) {
            obj = *(__int64 *)(rec + 64);
            if (obj) {
                mesh = *(__int64 *)(obj + 96);
                texid = *(unsigned *)(obj + 4ull * *(unsigned *)(ctx + 4) + 120);
                slot = tex_slot(texid);
                if (mesh && slot) {
                    saved_h = *(unsigned *)(mesh + 32);
                    tex_h = *(unsigned short *)(slot + 14);
                    if (tex_h > 0 && saved_h > tex_h) {
                        *(unsigned *)(mesh + 32) = tex_h;
                        capped = 1;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        capped = 0;
        mesh = 0;
    }
    __try {
        r = g_orig_skin_blit(ctx, rec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LONG n = InterlockedIncrement(&g_skin_seh_n);
        token = 0;
        obj = 0;
        __try {
            if (rec) {
                obj = *(__int64 *)(rec + 64);
                if (ctx)
                    token = *(unsigned *)ctx;
                if (obj)
                    InterlockedExchange((volatile LONG *)(obj + 180), (LONG)token);
                *(unsigned *)(rec + 72) = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (n <= 40 || (n & 255) == 0)
            log_msg("skin blit SEH code=%08X count=%ld obj=%p token=%u rec=%p",
                    GetExceptionCode(), n, (void *)obj, token, (void *)rec);
        r = 1;
    }
    if (capped && mesh) {
        __try {
            *(unsigned *)(mesh + 32) = saved_h;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

static int retry_gpu_tex(__int64 device, unsigned id, BYTE *slot)
{
    void **vt;
    HRESULT (WINAPI *create_tex)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD,
                                 D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
    HRESULT (WINAPI *get_caps)(IDirect3DDevice9 *, D3DCAPS9 *);
    IDirect3DTexture9 *tex = NULL;
    IUnknown *old;
    D3DCAPS9 caps;
    unsigned w, h, fmt, type, cw, maxw;
    HRESULT hr;

    if (!device || !slot)
        return 0;
    __try {
        type = *(unsigned *)(slot + 8);
        w = *(unsigned short *)(slot + 12);
        h = *(unsigned short *)(slot + 14);
        fmt = *(unsigned *)(slot + 32);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (type != 0 || w == 0 || h == 0 || fmt == 0)
        return 0;

    vt = *(void ***)device;
    if (!vt)
        return 0;
    get_caps = (HRESULT (WINAPI *)(IDirect3DDevice9 *, D3DCAPS9 *))vt[7];
    create_tex = (HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD,
                                     D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *))vt[23];
    if (!create_tex)
        return 0;

    maxw = 4096;
    memset(&caps, 0, sizeof(caps));
    if (get_caps && SUCCEEDED(get_caps((IDirect3DDevice9 *)device, &caps)) &&
        caps.MaxTextureWidth)
        maxw = caps.MaxTextureWidth;

    cw = w;
    hr = create_tex((IDirect3DDevice9 *)device, cw, h, 1, D3DUSAGE_DYNAMIC,
                    (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr) && w > 16) {
        /* D3D9 MaxTextureWidth is 4096–16384; these maps are ~40k wide.
         * The blit only writes 3 float4s (48 bytes) per row. */
        cw = 4;
        tex = NULL;
        hr = create_tex((IDirect3DDevice9 *)device, cw, h, 1, D3DUSAGE_DYNAMIC,
                        (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &tex, NULL);
    }
    if (FAILED(hr) || !tex) {
        log_msg("stage tex create fail id=%u %ux%u fmt=%08X hr=%08X maxw=%u",
                id, w, h, fmt, (unsigned)hr, maxw);
        return 0;
    }

    __try {
        old = *(IUnknown **)(slot + 40);
        *(void **)(slot + 40) = tex;
        slot[154] = 0;
        if (cw != w)
            *(unsigned short *)(slot + 12) = (unsigned short)cw;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tex->Release();
        return 0;
    }
    if (old)
        old->Release();
    log_msg("stage tex created id=%u %ux%u -> %ux%u fmt=%08X maxw=%u",
            id, w, h, cw, h, fmt, maxw);
    return 1;
}

static int __fastcall detour_create_tex(__int64 device, unsigned id, BYTE *payload)
{
    int r;
    BYTE *slot;
    if (!g_ready)
        return g_orig_create_tex ? g_orig_create_tex(device, id, payload) : 0;
    r = g_orig_create_tex ? g_orig_create_tex(device, id, payload) : 0;
    slot = tex_slot(id);
    if (slot) {
        __try {
            if (slot[154])
                retry_gpu_tex(device, id, slot);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

static __int64 __fastcall detour_enqueue(unsigned char a1, unsigned char a2, unsigned a3,
                                        unsigned short a4, void *src, size_t size)
{
    __int64 r;
    if (!g_ready || !g_orig_enqueue)
        return g_orig_enqueue ? g_orig_enqueue(a1, a2, a3, a4, src, size) : 0;
    queue_enter();
    r = g_orig_enqueue(a1, a2, a3, a4, src, size);
    MemoryBarrier();
    queue_leave();
    return r;
}

/* 0x1676C0 copies COM into a 52-byte packet and writes the ring itself.
 * Texture create (0x166CC0 / 0x166F30 / 0x1765D0) writes a 13-byte type-7
 * packet the same way. Redirect those writes through enqueue. */
static void __fastcall enqueue_dxt_lock_pkt(unsigned id, void *payload)
{
    if (!payload)
        return;
    detour_enqueue(7, 0, id, 3, payload, 40);
}

/* Unlock 2EDA88 before taking the queue CS so the pump can LockRect.
 * Arm from the skip site so the shared unlock (also used on early-out)
 * only enqueues when we actually skipped a ring write. */
static __declspec(thread) unsigned t_dxt_id;
static __declspec(thread) void *t_dxt_payload;

static void __fastcall dxt_arm_enqueue(unsigned id, void *payload)
{
    t_dxt_id = id;
    t_dxt_payload = payload;
}

static void dxt_fire_enqueue(void)
{
    void *p = t_dxt_payload;
    unsigned id = t_dxt_id;
    t_dxt_payload = NULL;
    InterlockedExchange((volatile LONG *)(g_base + kRvaTexTableLock), 0);
    if (p)
        enqueue_dxt_lock_pkt(id, p);
}

static int patch_dxt_enqueue_after_unlock(void)
{
    BYTE *skip_site = (BYTE *)(g_base + 0x16781E);
    BYTE *unlock_site = (BYTE *)(g_base + 0x16790B);
    BYTE jmp[6];
    BYTE stub[80];
    BYTE *skip_stub;
    BYTE *fire_stub;
    size_t n;
    static const unsigned char mov_eax_1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };

    if (!bytes_match((void *)(g_base + 0x16781C), (const unsigned char *)"\x8B\x2B", 2) ||
        !bytes_match(skip_site, mov_eax_1, 5)) {
        log_msg("dxt skip site mismatch");
        return 0;
    }
    if (!bytes_match(unlock_site, (const unsigned char *)"\x87\x35", 2)) {
        log_msg("dxt unlock site mismatch");
        return 0;
    }

    skip_stub = alloc_near(skip_site, 80);
    fire_stub = alloc_near(unlock_site, 80);
    if (!skip_stub || !fire_stub ||
        !rel32_ok(skip_site, (uintptr_t)skip_stub) ||
        !rel32_ok(skip_stub, g_base + 0x1678E7) ||
        !rel32_ok(unlock_site, (uintptr_t)fire_stub) ||
        !rel32_ok(fire_stub, g_base + 0x167911)) {
        log_msg("dxt stubs not within rel32");
        if (skip_stub)
            VirtualFree(skip_stub, 0, MEM_RELEASE);
        if (fire_stub)
            VirtualFree(fire_stub, 0, MEM_RELEASE);
        return 0;
    }
    track_stub(skip_stub);
    track_stub(fire_stub);

    n = 0;
    memset(stub, 0x90, sizeof(stub));
    stub[n++] = 0x8B;
    stub[n++] = 0xCD;
    stub[n++] = 0x48;
    stub[n++] = 0x8D;
    stub[n++] = 0x54;
    stub[n++] = 0x24;
    stub[n++] = 0x28;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xEC;
    stub[n++] = 0x20;
    emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)dxt_arm_enqueue);
    n += 10;
    stub[n++] = 0x41;
    stub[n++] = 0xFF;
    stub[n++] = 0xD3;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xC4;
    stub[n++] = 0x20;
    emit_rel32(stub + n, skip_stub + n, g_base + 0x1678E7);
    n += 5;
    memcpy(skip_stub, stub, n);
    FlushInstructionCache(GetCurrentProcess(), skip_stub, n);
    emit_rel32(jmp, skip_site, (uintptr_t)skip_stub);
    if (!apply_patch(skip_site, jmp, 5)) {
        log_msg("failed to patch dxt skip");
        return 0;
    }

    n = 0;
    memset(stub, 0x90, sizeof(stub));
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xEC;
    stub[n++] = 0x20;
    emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)dxt_fire_enqueue);
    n += 10;
    stub[n++] = 0x41;
    stub[n++] = 0xFF;
    stub[n++] = 0xD3;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xC4;
    stub[n++] = 0x20;
    emit_rel32(stub + n, fire_stub + n, g_base + 0x167911);
    n += 5;
    memcpy(fire_stub, stub, n);
    FlushInstructionCache(GetCurrentProcess(), fire_stub, n);
    memset(jmp, 0x90, sizeof(jmp));
    emit_rel32(jmp, unlock_site, (uintptr_t)fire_stub);
    if (!apply_patch(unlock_site, jmp, 6)) {
        log_msg("failed to patch dxt unlock");
        return 0;
    }
    log_msg("dxt_lock enqueue after table unlock");
    return 1;
}

static void __fastcall enqueue_tex_create_pkt(unsigned id, void *payload)
{
    if (!payload)
        return;
    detour_enqueue(7, 0, id, 0, payload, 1);
}

static int patch_ring_to_enqueue(uintptr_t site_rva, uintptr_t cont_rva,
                                 const unsigned char *expect, size_t expect_n,
                                 unsigned char id_modrm, int payload_disp,
                                 void *c_fn, const char *name)
{
    BYTE *site = (BYTE *)(g_base + site_rva);
    BYTE *cont = (BYTE *)(g_base + cont_rva);
    BYTE stub[96];
    BYTE jmp[5];
    BYTE *p;
    size_t n = 0;

    if (!bytes_match(site, expect, expect_n)) {
        log_msg("%s site mismatch, not patching 0x%X", name, (unsigned)site_rva);
        return 0;
    }
    p = alloc_near(site, 96);
    if (!p || !rel32_ok(site, (uintptr_t)p) || !rel32_ok(p, (uintptr_t)cont)) {
        log_msg("%s stub not within rel32", name);
        if (p)
            VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    memset(stub, 0x90, sizeof(stub));
    /* mov ecx, ebp/esi/edi */
    stub[n++] = 0x8B;
    stub[n++] = id_modrm;
    /* lea rdx, [rsp+disp]  (before shadow space; rdx stays valid across sub rsp) */
    if (payload_disp == (int)(char)payload_disp) {
        stub[n++] = 0x48;
        stub[n++] = 0x8D;
        stub[n++] = 0x54;
        stub[n++] = 0x24;
        stub[n++] = (BYTE)payload_disp;
    } else {
        stub[n++] = 0x48;
        stub[n++] = 0x8D;
        stub[n++] = 0x94;
        stub[n++] = 0x24;
        memcpy(stub + n, &payload_disp, 4);
        n += 4;
    }
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xEC;
    stub[n++] = 0x20;
    emit_mov_r11_imm64(stub + n, (UINT64)(uintptr_t)c_fn);
    n += 10;
    stub[n++] = 0x41;
    stub[n++] = 0xFF;
    stub[n++] = 0xD3;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xC4;
    stub[n++] = 0x20;
    emit_rel32(stub + n, p + n, (uintptr_t)cont);
    n += 5;
    memcpy(p, stub, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    emit_rel32(jmp, site, (uintptr_t)p);
    if (!apply_patch(site, jmp, 5)) {
        log_msg("failed to patch %s", name);
        VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    track_stub(p);
    log_msg("%s ring write -> enqueue (stub %p, %u bytes)", name, (void *)p, (unsigned)n);
    return 1;
}

static int patch_direct_ring_writers(void)
{
    static const unsigned char mov_eax_1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
    static const unsigned char mov_eax_r14[] = { 0x41, 0x8B, 0xC6, 0xF0, 0x0F };
    int n = 0;

    /* 0x1676C0: skip the in-lock ring write; enqueue after 2EDA88 is released. */
    if (!patch_dxt_enqueue_after_unlock())
        return 0;
    n++;

    /* 0x166CC0 2D create: mov ebp,[rsi]; payload [rsp+90h]. */
    if (bytes_match((void *)(g_base + 0x166E6C), (const unsigned char *)"\x8B\x2E", 2)) {
        if (!patch_ring_to_enqueue(0x166E6E, 0x166F13, mov_eax_1, sizeof(mov_eax_1),
                                   0xCD, 0x90, (void *)enqueue_tex_create_pkt, "tex_create"))
            return 0;
        n++;
    } else {
        log_msg("tex_create site mismatch");
    }

    /* 0x166F30 cube create: payload [rsp+88h]. */
    if (bytes_match((void *)(g_base + 0x1670AB), (const unsigned char *)"\x8B\x2E", 2)) {
        if (!patch_ring_to_enqueue(0x1670AD, 0x167153, mov_eax_1, sizeof(mov_eax_1),
                                   0xCD, 0x88, (void *)enqueue_tex_create_pkt, "cube_create"))
            return 0;
        n++;
    } else {
        log_msg("cube_create site mismatch");
    }

    /* 0x1765D0 RT create: id in esi, payload 0 at [rsp+50h]. */
    if (!patch_ring_to_enqueue(0x176798, 0x17682A, mov_eax_r14, sizeof(mov_eax_r14),
                               0xCE, 0x50, (void *)enqueue_tex_create_pkt, "rt_create"))
        return 0;
    n++;

    /* VB 0x16E810 / IB 0x170200 stay native (enqueue returns a count). */
    log_msg("direct ring writers patched=%d/4", n);
    return 1;
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
                    b = *(unsigned char *)(g_base + g_rva_dispatch);
                    (void)b;
                    if (++match_stable >= 3)
                        return 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    match_stable = 0;
                }
            } else if (bytes_match((void *)(g_base + g_rva_dispatch), kDispatchPrologue,
                                   sizeof(kDispatchPrologue)) &&
                       bytes_match((void *)(g_base + g_rva_outer), kOuterPrologue,
                                   sizeof(kOuterPrologue)) &&
                       bytes_match((void *)(g_base + g_rva_enqueue), kEnqueuePrologue,
                                   sizeof(kEnqueuePrologue)) &&
                       bytes_match((void *)(g_base + g_rva_flush), kFlushPrologue,
                                   sizeof(kFlushPrologue)) &&
                       bytes_match((void *)(g_base + g_rva_walk_other), kWalkOtherPrologue,
                                   sizeof(kWalkOtherPrologue))) {
                mismatch_stable = 0;
                if (++match_stable >= 3)
                    return 1;
            } else {
                match_stable = 0;
                {
                    unsigned char b;
                    int readable = 0;
                    __try {
                        b = *(unsigned char *)(g_base + g_rva_dispatch);
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
    int mh_inited = 0;
    InitializeCriticalSection(&g_log_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_enabled = ini_int(L"enabled", 1);
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_seh_dispatch = ini_int(L"seh_dispatch", 1);
    g_seh_packet = ini_int(L"seh_packet", 1);
    g_patch_walk = ini_int(L"patch_walk", 1);
    g_hook_outer = ini_int(L"hook_outer", 1);
    g_yield_sleep = ini_int(L"yield_sleep", 1);
    g_hook_enqueue = ini_int(L"hook_enqueue", 1);
    g_use_queue_cs = ini_int(L"queue_cs", 1);
    g_rva_dispatch = ini_rva(L"rva_dispatch", kDefaultDispatch);
    g_rva_outer = ini_rva(L"rva_outer", kDefaultOuter);
    g_rva_enqueue = ini_rva(L"rva_enqueue", kDefaultEnqueue);
    g_rva_flush = ini_rva(L"rva_flush", kDefaultFlush);
    g_rva_walk_other = ini_rva(L"rva_walk_other", kDefaultWalkOther);
    g_rva_call = ini_rva(L"rva_call", kDefaultCall);
    g_rva_jae1 = ini_rva(L"rva_jae1", kDefaultJae1);
    g_rva_jae2 = ini_rva(L"rva_jae2", kDefaultJae2);
    g_rva_cont = ini_rva(L"rva_cont", kDefaultCont);
    g_rva_exit = ini_rva(L"rva_exit", kDefaultExit);
    g_rva_advance = ini_rva(L"rva_advance", kDefaultAdvance);
    open_log();
    log_msg("pump_guard starting x64 enabled=%d queue_cs=%d enqueue=%d verify=%d",
            g_enabled, g_use_queue_cs, g_hook_enqueue, g_verify_prologue);

#define FAIL_HOOK(msg) do { \
        log_msg("FAIL_HOOK: %s", (msg)); \
        InterlockedExchange(&g_ready, 0); \
        InterlockedExchange(&g_queue_cs_ready, 0); \
        restore_patches(); \
        if (mh_inited) MH_Uninitialize(); \
        mh_inited = 0; \
        return 1; \
    } while (0)

    /* Wait for unpack before MH_Initialize (same as the other -k DLLs).
     * Do not MinHook d3d9!Direct3DCreate9 — rewriting that export while
     * Wine is inside the first Create9 can freeze the main thread. */
    log_msg("waiting for gamemdx");
    if (!wait_unpacked())
        FAIL_HOOK("timeout or prologue mismatch waiting for gamemdx");
    Sleep(200);
    log_msg("gamemdx at %p", (void *)g_base);

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    st = MH_Initialize();
    if (st != MH_OK)
        FAIL_HOOK("MH_Initialize failed");
    mh_inited = 1;

    if (g_patch_walk && !patch_walk())
        FAIL_HOOK("walk patch failed");

    if (g_yield_sleep) {
        int n = hook_iat_switchtothread((HMODULE)g_base);
        log_msg("SwitchToThread IAT hooks: %d", n);
    }

    if (g_hook_enqueue) {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_enqueue), (LPVOID)detour_enqueue,
                           (LPVOID *)&g_orig_enqueue);
        if (st != MH_OK) {
            log_msg("MH_CreateHook enqueue: %s", MH_StatusToString(st));
            g_hook_enqueue = 0;
        }
    }
    if (g_hook_outer) {
        st = MH_CreateHook((LPVOID)(g_base + g_rva_outer), (LPVOID)detour_outer,
                           (LPVOID *)&g_orig_outer);
        if (st != MH_OK) {
            log_msg("MH_CreateHook outer: %s", MH_StatusToString(st));
            FAIL_HOOK("outer hook failed");
        }
    }
    if (g_use_queue_cs && g_hook_enqueue && g_hook_outer) {
        if (g_verify_prologue &&
            !bytes_match((void *)(g_base + g_rva_flush), kFlushPrologue, sizeof(kFlushPrologue)))
            log_msg("flush prologue mismatch, hooking anyway");
        st = MH_CreateHook((LPVOID)(g_base + g_rva_flush), (LPVOID)detour_flush,
                           (LPVOID *)&g_orig_flush);
        if (st != MH_OK)
            log_msg("MH_CreateHook flush: %s", MH_StatusToString(st));
        if (g_verify_prologue &&
            !bytes_match((void *)(g_base + g_rva_walk_other), kWalkOtherPrologue, sizeof(kWalkOtherPrologue)))
            log_msg("walk_other prologue mismatch, hooking anyway");
        st = MH_CreateHook((LPVOID)(g_base + g_rva_walk_other), (LPVOID)detour_walk_other,
                           (LPVOID *)&g_orig_walk_other);
        if (st != MH_OK)
            log_msg("MH_CreateHook walk_other: %s", MH_StatusToString(st));
        InitializeCriticalSectionAndSpinCount(&g_queue_cs, 4000);
        InterlockedExchange(&g_queue_cs_ready, 1);
    } else if (g_use_queue_cs) {
        log_msg("queue_cs skipped (need both enqueue and outer hooks)");
        g_use_queue_cs = 0;
    }
    st = MH_CreateHook((LPVOID)(g_base + kRvaSkinBlit), (LPVOID)detour_skin_blit,
                       (LPVOID *)&g_orig_skin_blit);
    if (st != MH_OK)
        log_msg("MH_CreateHook skin_blit: %s", MH_StatusToString(st));
    st = MH_CreateHook((LPVOID)(g_base + kRvaCreateTex), (LPVOID)detour_create_tex,
                       (LPVOID *)&g_orig_create_tex);
    if (st != MH_OK)
        log_msg("MH_CreateHook create_tex: %s", MH_StatusToString(st));
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        FAIL_HOOK("MH_EnableHook failed");
    }
    if (g_use_queue_cs && !patch_direct_ring_writers())
        FAIL_HOOK("direct ring writers patch failed");
#undef FAIL_HOOK

    InterlockedExchange(&g_ready, 1);
    log_msg("hooks enabled seh_dispatch=%d seh_packet=%d yield=%d outer=%d enqueue=%d queue_cs=%d cs_hold_walk=1 flush=%d walk_other=%d skin_blit=%d create_tex=%d",
            g_seh_dispatch, g_seh_packet, g_yield_sleep, g_hook_outer, g_hook_enqueue,
            g_use_queue_cs, g_orig_flush != NULL, g_orig_walk_other != NULL,
            g_orig_skin_blit != NULL, g_orig_create_tex != NULL);
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
