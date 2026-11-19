/* R4 corpus: strings, code pages, locale and memory through the shims (`docs/RECOMP_PLAN.md`
 * "R4 design", `wse/recomp/shim_strings.h`, `shim_memory.h`).
 *
 * Our own code. Self-checking like the other KERNEL32 corpora (0x600D when correct); every
 * expected value is what Windows answers for en-US (the same PE also runs natively on Windows as
 * the reference: locale calls name "en-US" with LOCALE_NOUSEROVERRIDE, so the user's own
 * settings do not matter).
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

__declspec(dllimport) int MultiByteToWideChar(u32 cp, u32 flags, const char* src, int n, u16* dst, int cap);
__declspec(dllimport) int WideCharToMultiByte(u32 cp, u32 flags, const u16* src, int n, char* dst, int cap,
                                              const char* def, int* used);
__declspec(dllimport) u32 GetACP(void);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) int LCMapStringEx(const u16* locale, u32 flags, const u16* src, int n, u16* dst, int cap, void* a,
                                        void* b, u64 c);
__declspec(dllimport) int CompareStringEx(const u16* locale, u32 flags, const u16* a, int na, const u16* b, int nb,
                                          void* x, void* y, u64 z);
__declspec(dllimport) int GetLocaleInfoEx(const u16* locale, u32 type, u16* buf, int cap);
__declspec(dllimport) int GetDateFormatEx(const u16* locale, u32 flags, const u16* st, const u16* fmt, u16* buf, int cap,
                                          const u16* cal);
__declspec(dllimport) int GetTimeFormatEx(const u16* locale, u32 flags, const u16* st, const u16* fmt, u16* buf, int cap);
__declspec(dllimport) u32 FormatMessageA(u32 flags, const void* source, u32 id, u32 lang, char* buf, u32 size, void* args);
__declspec(dllimport) void* LocalFree(void* p);
__declspec(dllimport) u64 GetProcessHeap(void);
__declspec(dllimport) u64 HeapCreate(u32 flags, u64 initial, u64 max);
__declspec(dllimport) int HeapDestroy(u64 heap);
__declspec(dllimport) void* HeapAlloc(u64 heap, u32 flags, u64 size);
__declspec(dllimport) void* HeapReAlloc(u64 heap, u32 flags, void* p, u64 size);
__declspec(dllimport) int HeapFree(u64 heap, u32 flags, void* p);
__declspec(dllimport) u64 HeapSize(u64 heap, u32 flags, const void* p);
__declspec(dllimport) void* GlobalAlloc(u32 flags, u64 size);
__declspec(dllimport) void* GlobalLock(void* h);
__declspec(dllimport) int GlobalUnlock(void* h);
__declspec(dllimport) void* GlobalFree(void* h);
__declspec(dllimport) void* VirtualAlloc(void* address, u64 size, u32 type, u32 protect);
__declspec(dllimport) int VirtualFree(void* address, u64 size, u32 type);
__declspec(dllimport) int VirtualProtect(void* address, u64 size, u32 protect, u32* old);
__declspec(dllimport) u64 VirtualQuery(const void* address, void* mbi, u64 length);

#define NOOVERRIDE 0x80000000u
static const u16 kEnUs[] = {'e', 'n', '-', 'U', 'S', 0};

static int wide_equals(const u16* a, const char* b) {
    for (; *b != 0; ++a, ++b) {
        if (*a != (u16)(u8)*b) {
            return 0;
        }
    }
    return *a == 0;
}

/* Code page 1252 both ways, UTF-8 with a surrogate pair, size queries and errors. The machine's
 * ANSI code page is a Windows setting (65001 on the PC this was checked on, with "Use Unicode
 * UTF-8 for worldwide language support"), so 1252 is named, not CP_ACP. */
EXPORT NOINLINE u64 t_str_codepages(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const char ansi[] = {'A', (char)0x80, (char)0xE9, (char)0x9F, 0};
    u16 w[8];
    if (GetACP() == 0 || MultiByteToWideChar(1252, 0, ansi, -1, w, 8) != 5 || w[1] != 0x20AC || w[2] != 0xE9 ||
        w[3] != 0x178 || w[4] != 0) {
        return 1;
    }
    char back[8];
    int used = 0;
    const u16 cjk[] = {'x', 0x4E2D, 0x20AC, 0};
    if (WideCharToMultiByte(1252, 0, cjk, -1, back, 8, 0, &used) != 4 || back[1] != '?' || back[2] != (char)0x80 ||
        !used) {
        return 2;
    }
    const char utf8[] = {(char)0xF0, (char)0x9F, (char)0x98, (char)0x80, 'z', 0}; /* U+1F600 z */
    if (MultiByteToWideChar(65001, 0, utf8, -1, 0, 0) != 4 || MultiByteToWideChar(65001, 0, utf8, -1, w, 8) != 4 ||
        w[0] != 0xD83D || w[1] != 0xDE00 || w[2] != 'z') {
        return 3;
    }
    char u8back[8];
    if (WideCharToMultiByte(65001, 0, w, -1, u8back, 8, 0, 0) != 6 || u8back[0] != (char)0xF0 || u8back[3] != (char)0x80) {
        return 4;
    }
    const char badutf8[] = {'a', (char)0xC0, (char)0x80, 0};
    if (MultiByteToWideChar(65001, 8 /* MB_ERR_INVALID_CHARS */, badutf8, -1, w, 8) != 0 || GetLastError() != 1113) {
        return 5;
    }
    if (MultiByteToWideChar(65001, 0, utf8, -1, w, 2) != 0 || GetLastError() != 122) {
        return 6;
    }
    return 0x600D;
}

/* Case mapping, comparison and locale data for en-US. */
EXPORT NOINLINE u64 t_str_locale(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const u16 mixed[] = {'a', 'B', 0xE9, 0xFF, 0x3B1, 0x416, 0};
    u16 out[8];
    if (LCMapStringEx(kEnUs, 0x200 /* UPPERCASE */, mixed, -1, out, 8, 0, 0, 0) != 7 || out[0] != 'A' || out[1] != 'B' ||
        out[2] != 0xC9 || out[3] != 0x178 || out[4] != 0x391 || out[5] != 0x416) {
        return 1;
    }
    const u16 x[] = {'a', 'b', 0}, y[] = {'a', '-', 'b', 0}, z[] = {'A', 'B', 0}, u[] = {'_', 'a', 0};
    if (CompareStringEx(kEnUs, 0, x, -1, y, -1, 0, 0, 0) != 1 || CompareStringEx(kEnUs, 1, x, -1, z, -1, 0, 0, 0) != 2 ||
        CompareStringEx(kEnUs, 0, x, -1, z, -1, 0, 0, 0) != 1 || CompareStringEx(kEnUs, 0, u, -1, x, -1, 0, 0, 0) != 1) {
        return 2;
    }
    u16 buf[64];
    if (GetLocaleInfoEx(kEnUs, NOOVERRIDE | 0x0E, buf, 64) != 2 || !wide_equals(buf, ".") ||
        GetLocaleInfoEx(kEnUs, NOOVERRIDE | 0x0F, buf, 64) != 2 || !wide_equals(buf, ",") ||
        GetLocaleInfoEx(kEnUs, NOOVERRIDE | 0x38, buf, 64) != 8 || !wide_equals(buf, "January") ||
        GetLocaleInfoEx(kEnUs, NOOVERRIDE | 0x31, buf, 64) != 4 || !wide_equals(buf, "Mon") ||
        GetLocaleInfoEx(kEnUs, 0x5C, buf, 64) != 6 || !wide_equals(buf, "en-US")) {
        return 3;
    }
    if (GetLocaleInfoEx(kEnUs, 0x20000000 | 0x1004, buf, 64) != 2 || *(u32*)buf != 1252) {
        return 4;
    }
    return 0x600D;
}

/* Date and time pictures for a fixed SYSTEMTIME (Tuesday 2000-02-29 13:05:09). */
EXPORT NOINLINE u64 t_str_datetime(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const u16 st[8] = {2000, 2, 2, 29, 13, 5, 9, 0};
    const u16 dfmt[] = {'d', 'd', 'd', 'd', ',', ' ', 'M', 'M', 'M', 'M', ' ', 'd', ',', ' ', 'y', 'y', 'y', 'y', ' ',
                        '(', 'M', 'M', '/', 'd', 'd', '/', 'y', 'y', ')', 0};
    const u16 tfmt[] = {'h', 'h', ':', 'm', 'm', ':', 's', 's', ' ', 't', 't', ' ', 'H', ':', 'm', 0};
    u16 buf[64];
    if (GetDateFormatEx(kEnUs, 0, st, dfmt, buf, 64, 0) == 0 || !wide_equals(buf, "Tuesday, February 29, 2000 (02/29/00)")) {
        return 1;
    }
    if (GetTimeFormatEx(kEnUs, 0, st, tfmt, buf, 64) == 0 || !wide_equals(buf, "01:05:09 PM 13:5")) {
        return 2;
    }
    return 0x600D;
}

/* FormatMessageA: a system message into an allocated buffer, and inserts from an array. */
EXPORT NOINLINE u64 t_str_format_message(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    char* text = 0;
    const u32 n = FormatMessageA(0x100 | 0x200 | 0x1000, 0, 2, 0x409, (char*)&text, 0, 0);
    const char want[] = "The system cannot find the file specified.\r\n";
    if (n != sizeof(want) - 1 || text == 0) {
        return 1;
    }
    for (u32 i = 0; i < n; ++i) {
        if (text[i] != want[i]) {
            return 2;
        }
    }
    if (LocalFree(text) != 0) {
        return 3;
    }
    const char* args[2] = {"left", "right"};
    char buf[64];
    const u32 m = FormatMessageA(0x400 | 0x2000, "%1 and %2%%", 0, 0, buf, 64, (void*)args);
    const char want2[] = "left and right%";
    if (m != sizeof(want2) - 1) {
        return 4;
    }
    for (u32 i = 0; i <= m; ++i) {
        if (buf[i] != want2[i]) {
            return 5;
        }
    }
    return 0x600D;
}

/* Heaps and Global memory: zeroed allocation, size, growth keeping the contents, private heaps. */
EXPORT NOINLINE u64 t_mem_heap(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 heap = GetProcessHeap();
    u8* p = (u8*)HeapAlloc(heap, 8 /* HEAP_ZERO_MEMORY */, 100);
    if (p == 0 || ((u64)p & 15) != 0 || HeapSize(heap, 0, p) != 100) {
        return 1;
    }
    for (int i = 0; i < 100; ++i) {
        if (p[i] != 0) {
            return 2;
        }
        p[i] = (u8)(a + i);
    }
    p = (u8*)HeapReAlloc(heap, 0, p, 5000);
    if (p == 0 || HeapSize(heap, 0, p) != 5000 || p[99] != (u8)(a + 99)) {
        return 3;
    }
    if (!HeapFree(heap, 0, p)) {
        return 4;
    }
    const u64 mine = HeapCreate(0, 0, 0);
    void* q = HeapAlloc(mine, 0, 32);
    if (mine == 0 || q == 0 || !HeapFree(mine, 0, q) || !HeapDestroy(mine)) {
        return 5;
    }
    void* g = GlobalAlloc(0x42 /* GHND: moveable, zeroed */, 64);
    u8* gp = (u8*)GlobalLock(g);
    if (g == 0 || gp == 0 || gp[63] != 0) {
        return 6;
    }
    gp[0] = 7;
    GlobalUnlock(g);
    if (GlobalFree(g) != 0) {
        return 7;
    }
    return 0x600D;
}

/* Virtual memory: reserve, commit part, use it, query, protect, release. */
EXPORT NOINLINE u64 t_mem_virtual(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    u8* base = (u8*)VirtualAlloc(0, 1 << 20, 0x2000 /* MEM_RESERVE */, 0x04);
    if (base == 0 || ((u64)base & 0xFFFF) != 0) {
        return 1;
    }
    u8* page = (u8*)VirtualAlloc(base + 0x10000, 0x2000, 0x1000 /* MEM_COMMIT */, 0x04);
    if (page != base + 0x10000) {
        return 2;
    }
    page[0] = (u8)a;
    page[0x1FFF] = 0x5A;
    u64 mbi[6];
    if (VirtualQuery(page, mbi, sizeof(mbi)) != 48 || mbi[0] != (u64)page || mbi[1] != (u64)base ||
        (u32)mbi[4] != 0x1000 /* MEM_COMMIT */) {
        return 3;
    }
    u32 old = 0;
    if (!VirtualProtect(page, 0x1000, 0x02 /* PAGE_READONLY */, &old) || old != 0x04) {
        return 4;
    }
    if (page[0] != (u8)a || page[0x1FFF] != 0x5A || !VirtualFree(base, 0, 0x8000 /* MEM_RELEASE */)) {
        return 5;
    }
    return 0x600D;
}
