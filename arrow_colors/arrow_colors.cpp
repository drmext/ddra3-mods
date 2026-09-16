#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

/*
 * Spice -k hook (A3 64-bit and 32-bit). Ports WORLD gamemdx cave:
 *   Expanded Arrow Colors By Quantization  (0x181270D70 / 0xD18 / 0xD50 / freeze 0xE40)
 *   Solidify Arrow Color                   (0x181270E08 / 0x181270EB0)
 *
 * A3 slot expansion widens the stock arrow palette atlas from 16 rows to 21:
 *   Rows 0-15 : original A3 rows, preserved as-is
 *   Rows 16-20: WORLD-only extra quant rows (12th/24th/32nd/48th/64th)
 *
 * WORLD 0xE40 only recolors the approaching freeze head quad (remain < 0),
 * writing packed byte + UV directly to the vertex. With the widened atlas, the
 * packed byte now addresses the distinct WORLD rows directly instead of borrowing
 * stock A3 rows.
 *
 * The hit-flash explosion effect (sub_1800218C0) uses a separate direct-BGRA
 * vertex path with its own texture; it is NOT affected by the baked LUT.
 *
 * Solidify (WORLD dispatcher caves, independent of expand):
 *   1 FILL          types 2-4 always border-on (fill stays visible), fill still cycles
 *   2 FILL+BORDER   also lock fill phase to 0xFFFFFC72
 *   3 COMPLETE      also use border-on color for the fill
 *
 * 32-bit object offsets differ: NOTE type +0x98 (not +0xC8), tex +0x14 (not +0x20),
 * extra remain +4 (not +8). GetNoteColor vtable +0xC/+0x10/+0x14 = 64-bit +18h/+20h/+28h.
 */

#ifdef _WIN64
typedef unsigned int (__fastcall *FillFn)(void *self, unsigned int a2,
                                          unsigned int a3, unsigned int a4);
#define INI_NAME L"arrow_colors_64bit.ini"
#define LOG_NAME L"arrow_colors_64bit.log"
#define OBJ_CTYPE 0xC8
#define OBJ_TEX 0x20
#define EXTRA_REMAIN 8
#else
typedef unsigned int (__thiscall *FillFn)(void *self, unsigned int a2,
                                          unsigned int a3, unsigned int a4);
#define INI_NAME L"arrow_colors_32bit.ini"
#define LOG_NAME L"arrow_colors_32bit.log"
#define OBJ_CTYPE 0x98
#define OBJ_TEX 0x14
#define EXTRA_REMAIN 4
#endif

#ifdef _WIN64
static const unsigned char kClassifyInsn[] = {
    0x41, 0x81, 0xE3, 0xFF, 0x03, 0x00, 0x00
};
static const unsigned char kBakerInsn[] = {
    0x4C, 0x8B, 0x00, 0x4A, 0x8B, 0x0C, 0xC2
};
static const unsigned char kFreezeExtraInsn[] = {
    0x44, 0x8B, 0x4A, 0x08, 0x45, 0x85, 0xC9
};
static const unsigned char kFreezeUvInsn[] = {
    0x66, 0x41, 0x0F, 0x6E, 0xC4
};
static const unsigned char kFreezePatchInsn[] = {
    0xF3, 0x45, 0x0F, 0x5C, 0xC2
};
static const unsigned char kSolidifyBorderInsn[] = {
    0x3B, 0xC1, 0x49, 0x8B, 0x03, 0x49, 0x8B, 0xCB, 0x7D, 0x09
};
static const unsigned char kSolidifyFillInsn[] = {
    0x44, 0x03, 0xC3, 0x48, 0x83, 0xC4, 0x20
};
static const unsigned char kOtherBorderInsn[] = {
    0xB8, 0x80, 0xF8, 0x80, 0xFF, 0xC3
};
static const unsigned char kOtherFillInsn[] = {
    0x48, 0x83, 0xEC, 0x28
};

static const uintptr_t kDefaultClassify = 0x205A4;
static const uintptr_t kDefaultClassifyCont = 0x205FC;
static const uintptr_t kDefaultBaker = 0x1E7A4;
static const size_t kBakerContOff = 0x15;
static const uintptr_t kDefaultFreezeExtra = 0x20743;
static const uintptr_t kDefaultFreezeUv = 0x2086A;
static const uintptr_t kDefaultFreezePatch = 0x208F4;
static const uintptr_t kDefaultSolidifyBorder = 0x24841;
static const uintptr_t kDefaultPlus20 = 0x2484B;
static const uintptr_t kDefaultPlus18 = 0x24854;
static const uintptr_t kDefaultSolidifyFill = 0x24897;
static const uintptr_t kDefaultOtherBorder = 0x241E0;
static const uintptr_t kDefaultOtherFill = 0x241F0;
static const uintptr_t kDefaultCtorRows = 0x1E2E1;
static const uintptr_t kDefaultCtorR9 = 0x1E2E9;
static const uintptr_t kDefaultCtorR8 = 0x1E2ED;
static const uintptr_t kDefaultBakerRows = 0x1E7D4;
static const unsigned char kCtorRowsStock[] = { 0xBA, 0x10, 0x00, 0x00, 0x00 };
static const unsigned char kCtorRowsPatch[] = { 0xBA, 0x15, 0x00, 0x00, 0x00 };
static const unsigned char kCtorR9Patch[] = { 0x44, 0x8D, 0x0A, 0x90 };
static const unsigned char kCtorR8Patch[] = { 0x44, 0x8D, 0x42, 0xEC };
#else
static const unsigned char kClassifyInsn[] = {
    0x25, 0xFF, 0x03, 0x00, 0x00
};
static const unsigned char kBakerInsn[] = {
    0x8B, 0x00, 0x8B, 0x5D, 0xFC, 0x8B, 0x0C, 0x81
};
static const unsigned char kFreezeExtraInsn[] = {
    0x8B, 0x50, 0x04, 0x85, 0xD2
};
static const unsigned char kFreezeUvInsn[] = {
    0x8B, 0x44, 0x24, 0x24, 0x51, 0x8B, 0x4C, 0x24, 0x2C
};
static const unsigned char kFreezePatchInsn[] = {
    0xD9, 0x44, 0x10, 0x08, 0xD8, 0x64, 0x24, 0x38, 0xD9, 0x5C, 0x24, 0x30
};
static const unsigned char kSolidifyBorderInsn[] = {
    0x3B, 0xF2, 0x8B, 0x11, 0x50, 0x7D, 0x0A
};
static const unsigned char kSolidifyFillInsn[] = {
    0x57, 0x03, 0xC7, 0x50, 0x83, 0xC6, 0xFB, 0x56, 0xFF, 0xD2
};
static const unsigned char kOtherBorderInsn[] = {
    0xB8, 0x80, 0xF8, 0x80, 0xFF, 0xC2, 0x04, 0x00
};
static const unsigned char kOtherFillInsn[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
};

static const uintptr_t kDefaultClassify = 0x1BBF7;
static const uintptr_t kDefaultClassifyCont = 0x1BC46;
static const uintptr_t kDefaultBaker = 0x1A1CC;
static const size_t kBakerContOff = 0x16;
static const uintptr_t kDefaultFreezeExtra = 0x1BD6B;
static const uintptr_t kDefaultFreezeUv = 0x1BEA6;
static const uintptr_t kDefaultFreezePatch = 0x1BEEC;
static const uintptr_t kDefaultSolidifyBorder = 0x1F9BA;
static const uintptr_t kDefaultPlus20 = 0x1F9C1;
static const uintptr_t kDefaultPlus18 = 0x1F9CB;
static const uintptr_t kDefaultSolidifyFill = 0x1FA03;
static const uintptr_t kDefaultOtherBorder = 0x1F400;
static const uintptr_t kDefaultOtherFill = 0x1F410;
static const uintptr_t kDefaultCtorRows = 0x19DE4;
static const uintptr_t kDefaultBakerRows = 0x1A1FA;
#endif

static const unsigned int kDefaultBorder[8] = {
    0xFFF88080, 0xFF8080F8, 0xFFCC82F7, 0xFFF8F880,
    0xFFF880BE, 0xFFF8B981, 0xFFF8C4DE, 0xFF80F8F8
};
static const unsigned int kDefaultFill0[8] = {
    0xFFF80000, 0xFF0000F8, 0xFFCC00F8, 0xFFF8F800,
    0xFFF80080, 0xFFF88000, 0xFFF8A0C0, 0xFF00F8F8
};

static HMODULE g_self;
static uintptr_t g_base;
static FillFn g_orig_other_fill;
static volatile LONG g_ready;
static const unsigned int kAtlasRows = 21;
static const unsigned int kWorldRow12th = 16;
static const unsigned int kWorldRow24th = 17;
static const unsigned int kWorldRow32nd = 18;
static const unsigned int kWorldRow48th = 19;
static const unsigned int kWorldRow64th = 20;

static int g_log_enabled = 0;
static int g_enabled = 1;
static int g_verify_prologue = 1;
extern "C" int g_expand = 1;
extern "C" int g_solidify = 0;
static uintptr_t g_rva_classify = kDefaultClassify;
static uintptr_t g_rva_classify_cont = kDefaultClassifyCont;
static uintptr_t g_rva_baker = kDefaultBaker;
static uintptr_t g_rva_freeze_extra = kDefaultFreezeExtra;
static uintptr_t g_rva_freeze_uv = kDefaultFreezeUv;
static uintptr_t g_rva_freeze_patch = kDefaultFreezePatch;
static uintptr_t g_rva_solidify_border = kDefaultSolidifyBorder;
static uintptr_t g_rva_plus20 = kDefaultPlus20;
static uintptr_t g_rva_plus18 = kDefaultPlus18;
static uintptr_t g_rva_solidify_fill = kDefaultSolidifyFill;
static uintptr_t g_rva_other_border = kDefaultOtherBorder;
static uintptr_t g_rva_other_fill = kDefaultOtherFill;
static uintptr_t g_rva_ctor_rows = kDefaultCtorRows;
static uintptr_t g_rva_baker_rows = kDefaultBakerRows;
#ifdef _WIN64
static uintptr_t g_rva_ctor_r9 = kDefaultCtorR9;
static uintptr_t g_rva_ctor_r8 = kDefaultCtorR8;
static const unsigned char kTapPackInsn[] = { 0x66, 0x0F, 0x6E, 0xC2 };
static const unsigned char kBodyPackInsn[] = {
    0x66, 0x0F, 0x6E, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00
};
static const unsigned char kCommonPackInsn[] = {
    0xF3, 0x0F, 0x58, 0x1D, 0xEA, 0x61, 0x24, 0x00
};
static const uintptr_t kDefaultTapPack = 0x206C6;
static const uintptr_t kDefaultBodyPack = 0x1E907;
static const uintptr_t kDefaultCommonPack = 0x1EFA6;
static uintptr_t g_rva_tap_pack = kDefaultTapPack;
static uintptr_t g_rva_body_pack = kDefaultBodyPack;
static uintptr_t g_rva_common_pack = kDefaultCommonPack;
#else
/* sub_1001A300 / sub_1001A840: fmul qword ptr [imm32]. Opcode only —
 * the absolute 1/16 address relocates when gamemdx is not at 0x10000000. */
static const unsigned char kPackScaleInsn[] = { 0xDC, 0x0D };
static const size_t kPackScaleInsnN = 6;
static const uintptr_t kDefaultPackScale = 0x1A320;
static const uintptr_t kDefaultSpotPack = 0x1A94B;
static uintptr_t g_rva_pack_scale = kDefaultPackScale;
static uintptr_t g_rva_spot_pack = kDefaultSpotPack;
#endif

static unsigned int g_border[8];
static unsigned int g_fill0[8];

extern "C" uintptr_t g_classify_cont = 0;
extern "C" uintptr_t g_bake_cont = 0;
extern "C" uintptr_t g_freeze_uv_cont = 0;
extern "C" uintptr_t g_tap_pack_cont = 0;
extern "C" uintptr_t g_body_pack_cont = 0;
extern "C" uintptr_t g_common_pack_cont = 0;
extern "C" uintptr_t g_pack_scale_cont = 0;
extern "C" uintptr_t g_spot_pack_cont = 0;
extern "C" uintptr_t g_freeze_patch_cont = 0;
extern "C" uintptr_t g_plus20_epilogue = 0;
extern "C" uintptr_t g_plus18_epilogue = 0;
extern "C" void *g_freeze_vtx = 0;
extern "C" void *g_freeze_extra = 0;
extern "C" int g_freeze_remain = -1;
extern "C" int g_freeze_lane_progress = 0;
extern "C" int g_freeze_state = 0;
extern "C" uintptr_t g_freeze_extra_cont = 0;
extern "C" void classify_cave(void);
extern "C" void bake_cave(void);
extern "C" void freeze_extra_cave(void);
extern "C" void freeze_uv_cave(void);
#ifdef _WIN64
extern "C" void tap_pack_cave(void);
extern "C" void body_pack_cave(void);
extern "C" void common_pack_cave(void);
#else
extern "C" void pack_scale_cave(void);
extern "C" void spot_pack_cave(void);
#endif
extern "C" void freeze_patch_cave(void);
extern "C" void solidify_border_cave(void);
extern "C" void solidify_fill_cave(void);
extern "C" unsigned int bake_map_index(unsigned int row, unsigned int orig);
extern "C" unsigned int bake_fill_arg(unsigned int row, unsigned int obj);
#ifdef _WIN64
extern "C" FillFn g_bake_fill_fn = NULL;
extern "C" unsigned int bake_invoke_fill_reg(void *self, unsigned int a2, unsigned int a3,
                                             unsigned int a4);
#else
extern "C" FillFn g_bake_fill_fn = NULL;
extern "C" unsigned int __cdecl bake_invoke_fill_reg(void *self, unsigned int a2,
                                                     unsigned int a3, unsigned int a4);
extern "C" uintptr_t g_solidify_fill_cont = 0;
extern "C" void other_border_detour(void);
extern "C" void other_fill_detour(void);
extern "C" unsigned int __cdecl call_orig_other_fill(FillFn fn, void *self,
                                                     unsigned int a2, unsigned int a3,
                                                     unsigned int a4);
extern "C" unsigned int __cdecl call_bake_fill3(void *fn, void *self, unsigned int a2,
                                                unsigned int a3, unsigned int a4);
#endif

extern "C" int g_bake_quant = -1;
extern "C" int g_in_bake = 0;

#define ATLAS_MAX_PATCHES 4
struct AtlasPatch {
    void *addr;
    unsigned char saved[8];
    size_t len;
    int applied;
};
static AtlasPatch g_atlas_patches[ATLAS_MAX_PATCHES];
static int g_atlas_patch_count;

static int patch_byte(void *addr, unsigned char value);
static int patch_bytes(void *addr, const unsigned char *src, size_t len);

static CRITICAL_SECTION g_log_cs;
static unsigned char pack_row_byte(unsigned int row)
{
    float v;
    if (row >= kAtlasRows)
        row = kAtlasRows - 1;
    v = (((float)row + 0.5f) / (float)kAtlasRows) * 255.0f;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 255.0f)
        v = 255.0f;
    return (unsigned char)(int)v;
}

extern "C" unsigned int __fastcall pack_row_byte_c(unsigned int row)
{
    return (unsigned int)pack_row_byte(row);
}

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

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
    return (int)GetPrivateProfileIntW(L"arrow_colors", key, def, ini);
}

static uintptr_t ini_u32(const wchar_t *key, uintptr_t def)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"arrow_colors", key, L"", buf, 64, ini);
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

static void log_got_bytes(const char *name, uintptr_t rva, const void *addr, size_t n)
{
    unsigned char got[16];
    char hex[64];
    size_t i;
    size_t pos = 0;
    if (n > sizeof(got))
        n = sizeof(got);
    __try {
        memcpy(got, addr, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("%s insn mismatch at rva=0x%X (unreadable)", name, (unsigned)rva);
        return;
    }
    hex[0] = 0;
    for (i = 0; i < n && pos + 4 < sizeof(hex); i++)
        pos += (size_t)_snprintf_s(hex + pos, sizeof(hex) - pos, _TRUNCATE,
                                   "%02X%s", got[i], (i + 1 < n) ? " " : "");
    log_msg("%s insn mismatch at rva=0x%X got %s", name, (unsigned)rva, hex);
}

static void restore_atlas_patches(void)
{
    int i;
    for (i = g_atlas_patch_count - 1; i >= 0; i--) {
        if (g_atlas_patches[i].applied)
            patch_bytes(g_atlas_patches[i].addr, g_atlas_patches[i].saved,
                        g_atlas_patches[i].len);
        g_atlas_patches[i].applied = 0;
    }
    g_atlas_patch_count = 0;
}

/* Accept stock or already-patched bytes. Save stock/current for undo. */
static int apply_atlas_patch(const char *name, uintptr_t rva,
                             const unsigned char *stock, size_t stock_n,
                             const unsigned char *patch, size_t patch_n)
{
    void *addr;
    AtlasPatch *slot;
    if (g_atlas_patch_count >= ATLAS_MAX_PATCHES)
        return 0;
    if (!rva || patch_n == 0 || patch_n > sizeof(g_atlas_patches[0].saved))
        return 0;
    addr = (void *)(g_base + rva);
    if (bytes_match(addr, patch, patch_n)) {
        log_msg("%s already patched at rva=0x%X", name, (unsigned)rva);
        return 1;
    }
    if (g_verify_prologue && stock && stock_n == patch_n &&
        !bytes_match(addr, stock, stock_n)) {
        log_got_bytes(name, rva, addr, patch_n);
        return 0;
    }
    slot = &g_atlas_patches[g_atlas_patch_count];
    __try {
        memcpy(slot->saved, addr, patch_n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("%s unreadable at rva=0x%X", name, (unsigned)rva);
        return 0;
    }
    if (!patch_bytes(addr, patch, patch_n)) {
        log_msg("%s VirtualProtect/write failed at rva=0x%X", name, (unsigned)rva);
        return 0;
    }
    slot->addr = addr;
    slot->len = patch_n;
    slot->applied = 1;
    g_atlas_patch_count++;
    log_msg("%s patched at rva=0x%X", name, (unsigned)rva);
    return 1;
}

static int apply_atlas_byte(const char *name, uintptr_t rva, unsigned char stock,
                            unsigned char value)
{
    unsigned char s = stock;
    unsigned char p = value;
    return apply_atlas_patch(name, rva, &s, 1, &p, 1);
}

static unsigned int lerp_bgra(unsigned int a, unsigned int b, int t, int period)
{
    unsigned int out = 0;
    int i;
    if (period <= 0)
        period = 1;
    t %= period;
    if (t < 0)
        t += period;
    for (i = 0; i < 4; i++) {
        int ca = (int)((a >> (8 * i)) & 0xFFu);
        int cb = (int)((b >> (8 * i)) & 0xFFu);
        int v = ca + (cb - ca) * t / period;
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        out |= ((unsigned int)v) << (8 * i);
    }
    return out;
}

extern "C" unsigned int classify_row(unsigned int tick, unsigned int color_type)
{
    unsigned int t;
    unsigned int al;

    if (color_type != 1) {
        t = ((tick & 0x3FFu) + 0xDCu) >> 8;
        return (t & 3u) + 1u;
    }
    if (!g_expand) {
        t = tick & 0x3FFu;
        if (t == 0)
            return 1;
        if (t == 0x100 || t == 0x300)
            return 2;
        if (t == 0x200)
            return 3;
        return 4;
    }

    al = ((tick & 0x3FFu) * 3u + 0x20u) >> 6;
    if (al == 0 || al == 0x30)
        return 1;   /* 4th */
    if (al == 0x18)
        return 3;   /* 8th */
    if ((al & 0xF) == 0)                          /* 12th */
        return kWorldRow12th;
    if (al == 0x0C || al == 0x24)                 /* 16th */
        return 2;
    if ((al & 7) == 0)                            /* 24th */
        return kWorldRow24th;
    if (al % 6u == 0)                             /* 32nd */
        return kWorldRow32nd;
    if ((al & 3) == 0)                            /* 48th */
        return kWorldRow48th;
    if (al % 3u == 0)                             /* 64th */
        return kWorldRow64th;
    return 4;
}

extern "C" unsigned int bake_map_index(unsigned int row, unsigned int orig)
{
    static unsigned int s_bake_log_mask;
    static const int extra[21] = {
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
         2,  4,  5,  6,  7
    };

    g_bake_quant = -1;
    if (g_expand && row < kAtlasRows)
        g_bake_quant = extra[row];
    if ((row == 0 && !(s_bake_log_mask & 1u)) ||
        (row == 3 && !(s_bake_log_mask & 2u)) ||
        (row == 15 && !(s_bake_log_mask & 4u)) ||
        (row == 16 && !(s_bake_log_mask & 8u)) ||
        (row == 20 && !(s_bake_log_mask & 16u))) {
        if (row == 0) s_bake_log_mask |= 1u;
        if (row == 3) s_bake_log_mask |= 2u;
        if (row == 15) s_bake_log_mask |= 4u;
        if (row == 16) s_bake_log_mask |= 8u;
        if (row == 20) s_bake_log_mask |= 16u;
        log_msg("bake row=%u orig=%u quant=%d", row, orig, g_bake_quant);
    }
    if (g_expand && row >= kWorldRow12th && row <= kWorldRow64th)
        return 4;
    return orig;
}

extern "C" unsigned int bake_fill_arg(unsigned int row, unsigned int obj)
{
    return row - obj;
}

#ifdef _WIN64
extern "C" unsigned int bake_invoke_fill_reg(void *self, unsigned int a2, unsigned int a3,
                                             unsigned int a4)
{
    unsigned int r = 0;
    g_in_bake = 1;
    __try {
        if (g_bake_fill_fn)
            r = g_bake_fill_fn(self, a2, a3, a4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    g_in_bake = 0;
    g_bake_quant = -1;
    return r;
}
#else
extern "C" unsigned int __cdecl bake_invoke_fill_reg(void *self, unsigned int a2,
                                                     unsigned int a3, unsigned int a4)
{
    unsigned int r = 0;
    g_in_bake = 1;
    __try {
        if (g_bake_fill_fn)
            r = call_bake_fill3((void *)g_bake_fill_fn, self, a2, a3, a4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    g_in_bake = 0;
    g_bake_quant = -1;
    return r;
}
#endif

extern "C" void patch_freeze_head_vertex(void *vtx, void *player, void *extra)
{
    static unsigned int s_logged;
    unsigned int tick;
    unsigned int ctype;
    unsigned int row;
    unsigned char packed;
    int remain;
    char *obj;
    unsigned char *tex;
    unsigned short w;
    unsigned short h;
    uintptr_t note;

    if (!g_expand || !vtx || !extra)
        return;

    remain = 1;
    tick = 0;
    note = 0;
    __try {
        remain = *(int *)((char *)extra + EXTRA_REMAIN);
        note = *(uintptr_t *)extra;
        if (note)
            tick = *(unsigned int *)(note + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    /*
     * WORLD 0xE40 recolors only the approaching freeze head. Once remain is
     * non-negative, lane progress advances, or the dedicated hold state is
     * entered, leave the stock held-head geometry and freeze-row color alone.
     */
    if (remain >= 0 || g_freeze_lane_progress != 0 || g_freeze_state == 5)
        return;

    g_freeze_vtx = 0;

    ctype = 1;
    obj = NULL;
    __try {
        if (player)
            obj = *(char **)player;
        if (obj)
            ctype = *(unsigned int *)(obj + OBJ_CTYPE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        obj = NULL;
        ctype = 1;
    }

    row = classify_row(tick, ctype);
    packed = pack_row_byte(row);

    w = 256;
    h = 256;
    tex = NULL;
    __try {
        if (obj)
            tex = *(unsigned char **)(obj + OBJ_TEX);
        if (tex) {
            w = *(unsigned short *)(tex + 8);
            h = *(unsigned short *)(tex + 10);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w = 256;
        h = 256;
    }
    if (w == 0)
        w = 256;
    if (h == 0)
        h = 256;

    /* Approaching freeze heads use the tap-arrow sprite cell, not the hold strip. */
    __try {
        *(float *)((char *)vtx + 0x20) = 0.0f;
        *(float *)((char *)vtx + 0x24) = 0.0f;
        *(float *)((char *)vtx + 0x28) = 96.0f / (float)w;
        *(float *)((char *)vtx + 0x2C) = 96.0f / (float)h;
        *(unsigned char *)((char *)vtx + 0x30) = packed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (s_logged < 8) {
        s_logged++;
        log_msg("freeze-head tick=%u row=%u packed=0x%02X remain=%d vtx=%p",
                tick, row, packed, remain, vtx);
    }
}

static int patch_byte(void *addr, unsigned char value)
{
    DWORD old_protect;
    if (!VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_protect))
        return 0;
    *(volatile unsigned char *)addr = value;
    VirtualProtect(addr, 1, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), addr, 1);
    return 1;
}

static int patch_bytes(void *addr, const unsigned char *src, size_t len)
{
    DWORD old_protect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old_protect))
        return 0;
    memcpy(addr, src, len);
    VirtualProtect(addr, len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 1;
}

#ifdef _WIN64
static unsigned int __fastcall detour_other_border(void *self, unsigned int phase)
{
    int q = g_bake_quant;
    (void)self;
    (void)phase;
    if (!g_ready)
        return 0xFF80F880u;
    if (g_expand && g_in_bake && q >= 0 && q < 8)
        return g_border[q];
    return 0xFF80F880u;
}

static unsigned int __fastcall detour_other_fill(void *self, unsigned int a2,
                                                 unsigned int a3, unsigned int a4)
{
    int q = g_bake_quant;
    unsigned int orig;
    unsigned int t, r, g, b;

    if (!g_ready || !g_orig_other_fill)
        return 0;
    orig = g_orig_other_fill(self, a2, a3, a4);
#else
extern "C" unsigned int detour_other_border_c(void *self, unsigned int phase)
{
    int q = g_bake_quant;
    (void)self;
    (void)phase;
    if (!g_ready)
        return 0xFF80F880u;
    if (g_expand && g_in_bake && q >= 0 && q < 8)
        return g_border[q];
    return 0xFF80F880u;
}

extern "C" unsigned int detour_other_fill_c(void *self, unsigned int a2,
                                            unsigned int a3, unsigned int a4)
{
    int q = g_bake_quant;
    unsigned int orig;
    unsigned int t, r, g, b;

    if (!g_ready || !g_orig_other_fill)
        return 0;
    orig = call_orig_other_fill(g_orig_other_fill, self, a2, a3, a4);
#endif
    if (!g_expand || !g_in_bake || q < 0 || q > 7)
        return orig;
    if (g_solidify >= 3)
        return g_border[q];

    /* WORLD 0xD18 replaces NoteOther's final fill color. Native 4th/8th/16th
     * fills pulse the inner toward white (0xFFF8F8F8) so the outline reads
     * darker. Use the original fill's brightness as that mix. */
    r = (orig >> 16) & 0xFFu;
    g = (orig >> 8) & 0xFFu;
    b = orig & 0xFFu;
    t = r;
    if (g > t)
        t = g;
    if (b > t)
        t = b;
    if (t > 0xF8)
        t = 0xF8;
    return lerp_bgra(g_fill0[q], 0xFFF8F8F8u, (int)t, 0xF8);
}

static int wait_unpacked(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    int match_stable = 0;
    int mismatch_stable = 0;
    uintptr_t rva = g_rva_other_fill;

    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"gamemdx.dll");
        if (mod) {
            g_base = (uintptr_t)mod;
            if (!g_verify_prologue) {
                unsigned char b;
                __try {
                    b = *(unsigned char *)(g_base + rva);
                    (void)b;
                    if (++match_stable >= 3)
                        return 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    match_stable = 0;
                }
            } else if (bytes_match((void *)(g_base + rva), kOtherFillInsn,
                                   sizeof(kOtherFillInsn))) {
                mismatch_stable = 0;
                if (++match_stable >= 3)
                    return 1;
            } else {
                match_stable = 0;
                {
                    unsigned char b;
                    int readable = 0;
                    __try {
                        b = *(unsigned char *)(g_base + rva);
                        (void)b;
                        readable = 1;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        readable = 0;
                    }
                    if (readable) {
                        if (++mismatch_stable >= 10) {
                            log_msg("prologue mismatch at rva_other_fill=0x%X (wrong gamemdx build, not hooking)",
                                    (unsigned)rva);
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

static int hook_site(const char *name, uintptr_t rva, LPVOID cave,
                     const unsigned char *expect, size_t expect_n,
                     uintptr_t *cont, size_t insn_n)
{
    MH_STATUS st;
    unsigned char *at;

    if (!rva)
        return 1;
    at = (unsigned char *)(g_base + rva);
    if (g_verify_prologue && !bytes_match(at, expect, expect_n)) {
        log_got_bytes(name, rva, at, expect_n > 8 ? 8 : expect_n);
        return 0;
    }
    if (cont)
        *cont = g_base + rva + insn_n;
    st = MH_CreateHook((LPVOID)at, cave, NULL);
    if (st != MH_OK) {
        log_msg("MH_CreateHook %s: %s", name, MH_StatusToString(st));
        return 0;
    }
    log_msg("%s hook at %p (cont %p)", name, (void *)at, cont ? (void *)*cont : NULL);
    return 1;
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;
    int mh_inited = 0;
    static const wchar_t *border_keys[8] = {
        L"border_4th", L"border_8th", L"border_12th", L"border_16th",
        L"border_24th", L"border_32nd", L"border_48th", L"border_64th"
    };
    static const wchar_t *fill0_keys[8] = {
        L"fill_4th", L"fill_8th", L"fill_12th", L"fill_16th",
        L"fill_24th", L"fill_32nd", L"fill_48th", L"fill_64th"
    };
    int i;

    InitializeCriticalSection(&g_log_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 0);
    g_enabled = ini_int(L"enabled", 1);
    g_verify_prologue = ini_int(L"verify_prologue", 1);
    g_expand = ini_int(L"expand_note_colors", 1);
    g_solidify = ini_int(L"solidify_mode", 0);
    g_rva_classify = ini_u32(L"rva_classify", kDefaultClassify);
    g_rva_classify_cont = ini_u32(L"rva_classify_cont", kDefaultClassifyCont);
    g_rva_baker = ini_u32(L"rva_baker", kDefaultBaker);
    g_rva_freeze_extra = ini_u32(L"rva_freeze_extra", kDefaultFreezeExtra);
    g_rva_freeze_uv = ini_u32(L"rva_freeze_uv", kDefaultFreezeUv);
    g_rva_freeze_patch = ini_u32(L"rva_freeze_patch", kDefaultFreezePatch);
    g_rva_solidify_border = ini_u32(L"rva_solidify_border", kDefaultSolidifyBorder);
    g_rva_plus20 = ini_u32(L"rva_plus20", kDefaultPlus20);
    g_rva_plus18 = ini_u32(L"rva_plus18", kDefaultPlus18);
    g_rva_solidify_fill = ini_u32(L"rva_solidify_fill", kDefaultSolidifyFill);
    g_rva_other_border = ini_u32(L"rva_other_border", kDefaultOtherBorder);
    g_rva_other_fill = ini_u32(L"rva_other_fill", kDefaultOtherFill);
    g_rva_ctor_rows = ini_u32(L"rva_ctor_rows", kDefaultCtorRows);
    g_rva_baker_rows = ini_u32(L"rva_baker_rows", kDefaultBakerRows);
#ifdef _WIN64
    g_rva_ctor_r9 = ini_u32(L"rva_ctor_r9", kDefaultCtorR9);
    g_rva_ctor_r8 = ini_u32(L"rva_ctor_r8", kDefaultCtorR8);
    g_rva_tap_pack = ini_u32(L"rva_tap_pack", kDefaultTapPack);
    g_rva_body_pack = ini_u32(L"rva_body_pack", kDefaultBodyPack);
    g_rva_common_pack = ini_u32(L"rva_common_pack", kDefaultCommonPack);
#else
    g_rva_pack_scale = ini_u32(L"rva_pack_scale", kDefaultPackScale);
    g_rva_spot_pack = ini_u32(L"rva_spot_pack", kDefaultSpotPack);
#endif
    if (g_solidify < 0)
        g_solidify = 0;
    if (g_solidify > 3)
        g_solidify = 3;
    for (i = 0; i < 8; i++) {
        unsigned int v;
        g_border[i] = kDefaultBorder[i];
        g_fill0[i] = kDefaultFill0[i];
        v = (unsigned int)ini_u32(border_keys[i], 0);
        if (v)
            g_border[i] = v;
        v = (unsigned int)ini_u32(fill0_keys[i], 0);
        if (v)
            g_fill0[i] = v;
    }
    open_log();
#ifdef _WIN64
    log_msg("arrow_colors starting (x64 enabled=%d expand=%d solidify=%d "
            "rva_classify=0x%X rva_baker=0x%X rva_ctor_rows=0x%X rva_baker_rows=0x%X verify=%d)",
            g_enabled, g_expand, g_solidify,
            (unsigned)g_rva_classify, (unsigned)g_rva_baker,
            (unsigned)g_rva_ctor_rows, (unsigned)g_rva_baker_rows,
            g_verify_prologue);
#else
    log_msg("arrow_colors starting (x86 enabled=%d expand=%d solidify=%d "
            "rva_classify=0x%X rva_baker=0x%X rva_ctor_rows=0x%X rva_baker_rows=0x%X verify=%d)",
            g_enabled, g_expand, g_solidify,
            (unsigned)g_rva_classify, (unsigned)g_rva_baker,
            (unsigned)g_rva_ctor_rows, (unsigned)g_rva_baker_rows,
            g_verify_prologue);
#endif

    if (!wait_unpacked()) {
        if (g_verify_prologue)
            log_msg("not hooking (timeout or prologue mismatch)");
        else
            log_msg("timeout waiting for gamemdx.dll at rva_other_fill=0x%X",
                    (unsigned)g_rva_other_fill);
        return 1;
    }
    Sleep(200);
    log_msg("gamemdx at %p", (void *)g_base);

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }
    if (!g_expand && g_solidify == 0) {
        log_msg("expand off and solidify=0, nothing to hook");
        return 0;
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }
    mh_inited = 1;
#define FAIL_HOOK(msg) do { \
        log_msg("%s", msg); \
        restore_atlas_patches(); \
        if (mh_inited) MH_Uninitialize(); \
        return 1; \
    } while (0)

    if (g_expand) {
        if (!hook_site("classify", g_rva_classify, (LPVOID)classify_cave,
                       kClassifyInsn, sizeof(kClassifyInsn),
                       NULL, 0))
            FAIL_HOOK("classify hook failed");
        g_classify_cont = g_base + g_rva_classify_cont;
        log_msg("classify cont %p (skip stock 4th/8th/16th switch)",
                (void *)g_classify_cont);
        if (!hook_site("baker", g_rva_baker, (LPVOID)bake_cave,
                       kBakerInsn, sizeof(kBakerInsn),
                       &g_bake_cont, kBakerContOff))
            FAIL_HOOK("baker hook failed");
        if (!hook_site("freeze-extra", g_rva_freeze_extra, (LPVOID)freeze_extra_cave,
                       kFreezeExtraInsn, sizeof(kFreezeExtraInsn),
                       &g_freeze_extra_cont, sizeof(kFreezeExtraInsn)))
            FAIL_HOOK("freeze-extra hook failed");
#ifdef _WIN64
        if (!hook_site("freeze-uv", g_rva_freeze_uv, (LPVOID)freeze_uv_cave,
                       kFreezeUvInsn, sizeof(kFreezeUvInsn),
                       &g_freeze_uv_cont, 0x35))
            FAIL_HOOK("freeze-uv hook failed");
#else
        if (!hook_site("freeze-uv", g_rva_freeze_uv, (LPVOID)freeze_uv_cave,
                       kFreezeUvInsn, sizeof(kFreezeUvInsn),
                       &g_freeze_uv_cont, sizeof(kFreezeUvInsn)))
            FAIL_HOOK("freeze-uv hook failed");
#endif
        if (!hook_site("freeze-patch", g_rva_freeze_patch, (LPVOID)freeze_patch_cave,
                       kFreezePatchInsn, sizeof(kFreezePatchInsn),
                       &g_freeze_patch_cont, sizeof(kFreezePatchInsn)))
            FAIL_HOOK("freeze-patch hook failed");
#ifdef _WIN64
        if (!hook_site("tap-pack", g_rva_tap_pack, (LPVOID)tap_pack_cave,
                       kTapPackInsn, sizeof(kTapPackInsn),
                       &g_tap_pack_cont, 0x35))
            FAIL_HOOK("tap-pack hook failed");
        if (!hook_site("body-pack", g_rva_body_pack, (LPVOID)body_pack_cave,
                       kBodyPackInsn, sizeof(kBodyPackInsn),
                       &g_body_pack_cont, 0x45))
            FAIL_HOOK("body-pack hook failed");
        if (!hook_site("common-pack", g_rva_common_pack, (LPVOID)common_pack_cave,
                       kCommonPackInsn, sizeof(kCommonPackInsn),
                       &g_common_pack_cont, 0x46))
            FAIL_HOOK("common-pack hook failed");
#else
        if (!hook_site("pack-scale", g_rva_pack_scale, (LPVOID)pack_scale_cave,
                       kPackScaleInsn, sizeof(kPackScaleInsn),
                       &g_pack_scale_cont, kPackScaleInsnN))
            FAIL_HOOK("pack-scale hook failed");
        if (!hook_site("spot-pack", g_rva_spot_pack, (LPVOID)spot_pack_cave,
                       kPackScaleInsn, sizeof(kPackScaleInsn),
                       &g_spot_pack_cont, kPackScaleInsnN))
            FAIL_HOOK("spot-pack hook failed");
#endif

        if (g_verify_prologue &&
            !bytes_match((void *)(g_base + g_rva_other_border), kOtherBorderInsn,
                         sizeof(kOtherBorderInsn))) {
            log_got_bytes("NoteOther border", g_rva_other_border,
                          (void *)(g_base + g_rva_other_border),
                          sizeof(kOtherBorderInsn));
            FAIL_HOOK("NoteOther border insn mismatch");
        }
#ifdef _WIN64
        st = MH_CreateHook((LPVOID)(g_base + g_rva_other_border),
                           (LPVOID)detour_other_border, NULL);
#else
        st = MH_CreateHook((LPVOID)(g_base + g_rva_other_border),
                           (LPVOID)other_border_detour, NULL);
#endif
        if (st != MH_OK) {
            log_msg("MH_CreateHook NoteOther border: %s", MH_StatusToString(st));
            FAIL_HOOK("NoteOther border hook failed");
        }
#ifdef _WIN64
        st = MH_CreateHook((LPVOID)(g_base + g_rva_other_fill),
                           (LPVOID)detour_other_fill, (LPVOID *)&g_orig_other_fill);
#else
        st = MH_CreateHook((LPVOID)(g_base + g_rva_other_fill),
                           (LPVOID)other_fill_detour, (LPVOID *)&g_orig_other_fill);
#endif
        if (st != MH_OK) {
            log_msg("MH_CreateHook NoteOther fill: %s", MH_StatusToString(st));
            FAIL_HOOK("NoteOther fill hook failed");
        }
        log_msg("NoteOther fill/border hooks at 0x%X / 0x%X",
                (unsigned)g_rva_other_fill, (unsigned)g_rva_other_border);
    }

    if (g_solidify > 0) {
        g_plus20_epilogue = g_base + g_rva_plus20;
        g_plus18_epilogue = g_base + g_rva_plus18;
        if (!hook_site("solidify-border", g_rva_solidify_border,
                       (LPVOID)solidify_border_cave, kSolidifyBorderInsn,
                       sizeof(kSolidifyBorderInsn), NULL, 0))
            FAIL_HOOK("solidify-border hook failed");
#ifdef _WIN64
        if (!hook_site("solidify-fill", g_rva_solidify_fill,
                       (LPVOID)solidify_fill_cave, kSolidifyFillInsn,
                       sizeof(kSolidifyFillInsn), NULL, 0))
            FAIL_HOOK("solidify-fill hook failed");
#else
        if (!hook_site("solidify-fill", g_rva_solidify_fill,
                       (LPVOID)solidify_fill_cave, kSolidifyFillInsn,
                       sizeof(kSolidifyFillInsn),
                       &g_solidify_fill_cont, sizeof(kSolidifyFillInsn)))
            FAIL_HOOK("solidify-fill hook failed");
#endif
        log_msg("solidify caves +20=%p +18=%p",
                (void *)g_plus20_epilogue, (void *)g_plus18_epilogue);
    }

    if (g_expand) {
#ifdef _WIN64
        if (!apply_atlas_patch("ctor-rows", g_rva_ctor_rows,
                               kCtorRowsStock, sizeof(kCtorRowsStock),
                               kCtorRowsPatch, sizeof(kCtorRowsPatch)) ||
            !apply_atlas_patch("ctor-r9", g_rva_ctor_r9,
                               NULL, 0, kCtorR9Patch, sizeof(kCtorR9Patch)) ||
            !apply_atlas_patch("ctor-r8", g_rva_ctor_r8,
                               NULL, 0, kCtorR8Patch, sizeof(kCtorR8Patch)) ||
            !apply_atlas_byte("baker-rows", g_rva_baker_rows, 0x10,
                              (unsigned char)kAtlasRows))
            FAIL_HOOK("atlas patch failed");
#else
        if (!apply_atlas_byte("ctor-rows", g_rva_ctor_rows, 0x10,
                              (unsigned char)kAtlasRows) ||
            !apply_atlas_byte("baker-rows", g_rva_baker_rows, 0x10,
                              (unsigned char)kAtlasRows))
            FAIL_HOOK("atlas patch failed");
#endif
        log_msg("palette atlas widened to %u rows", kAtlasRows);
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        FAIL_HOOK("MH_EnableHook failed");
    }
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
