/* R4 corpus: files, directories and file mappings through the shims (`docs/RECOMP_PLAN.md`
 * "R4 design", `wse/recomp/shim_files.h`).
 *
 * Our own code. Self-checking (0x600D when correct). Paths are relative to the current directory
 * or under GetTempPath, so the same PE runs natively on Windows (real folders, cleaned up after)
 * and under the shims (the install overlay and the writable layer).
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)
#define INVALID ((u64)~0ULL)

__declspec(dllimport) u64 CreateFileA(const char* name, u32 access, u32 share, void* sa, u32 disp, u32 flags, u64 tmpl);
__declspec(dllimport) int ReadFile(u64 h, void* buf, u32 n, u32* got, void* ov);
__declspec(dllimport) int WriteFile(u64 h, const void* buf, u32 n, u32* put, void* ov);
__declspec(dllimport) u32 SetFilePointer(u64 h, int lo, int* hi, u32 method);
__declspec(dllimport) u32 GetFileSize(u64 h, u32* hi);
__declspec(dllimport) int SetEndOfFile(u64 h);
__declspec(dllimport) int CloseHandle(u64 h);
__declspec(dllimport) int DeleteFileA(const char* name);
__declspec(dllimport) u32 GetFileAttributesA(const char* name);
__declspec(dllimport) int GetFileAttributesExA(const char* name, int level, void* data);
__declspec(dllimport) int CreateDirectoryA(const char* name, void* sa);
__declspec(dllimport) int RemoveDirectoryA(const char* name);
__declspec(dllimport) u64 FindFirstFileA(const char* spec, void* data);
__declspec(dllimport) int FindNextFileA(u64 h, void* data);
__declspec(dllimport) int FindClose(u64 h);
__declspec(dllimport) u32 GetTempPathA(u32 n, char* buf);
__declspec(dllimport) u32 GetCurrentDirectoryA(u32 n, char* buf);
__declspec(dllimport) int SetCurrentDirectoryA(const char* name);
__declspec(dllimport) u32 GetFullPathNameA(const char* name, u32 n, char* buf, char** part);
__declspec(dllimport) int CopyFileA(const char* from, const char* to, int fail_if_exists);
__declspec(dllimport) int MoveFileExA(const char* from, const char* to, u32 flags);
__declspec(dllimport) u64 CreateFileMappingA(u64 file, void* sa, u32 protect, u32 hi, u32 lo, const char* name);
__declspec(dllimport) u64 OpenFileMappingA(u32 access, int inherit, const char* name);
__declspec(dllimport) void* MapViewOfFile(u64 mapping, u32 access, u32 hi, u32 lo, u64 bytes);
__declspec(dllimport) int UnmapViewOfFile(const void* p);
__declspec(dllimport) u32 GetLastError(void);

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u

static u32 slen(const char* s) {
    u32 n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}
static void scat(char* dst, const char* src) {
    u32 n = slen(dst);
    for (u32 i = 0; src[i] != 0; ++i) {
        dst[n++] = src[i];
    }
    dst[n] = 0;
}
static int seq(const char* a, const char* b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static int put_file(const char* name, const char* text) {
    const u64 h = CreateFileA(name, GENERIC_WRITE, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    if (h == INVALID) {
        return 0;
    }
    u32 put = 0;
    const int ok = WriteFile(h, text, slen(text), &put, 0) && put == slen(text);
    return CloseHandle(h) && ok;
}

/* Create, write, reopen, read, seek, open dispositions, delete. */
EXPORT NOINLINE u64 t_file_rw(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    char path[300];
    if (GetTempPathA(260, path) == 0) {
        return 1;
    }
    scat(path, "wse_corpus_rw.txt");
    if (!put_file(path, "hello world")) {
        return 2;
    }
    u64 h = CreateFileA(path, GENERIC_READ, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    char buf[16];
    u32 got = 0;
    if (h == INVALID || GetFileSize(h, 0) != 11 || !ReadFile(h, buf, 5, &got, 0) || got != 5 || buf[0] != 'h' ||
        buf[4] != 'o') {
        return 3;
    }
    if (SetFilePointer(h, -5, 0, 2 /* FILE_END */) != 6 || !ReadFile(h, buf, 16, &got, 0) || got != 5 || buf[0] != 'w' ||
        !ReadFile(h, buf, 16, &got, 0) || got != 0) {
        return 4;
    }
    CloseHandle(h);
    if (CreateFileA(path, GENERIC_READ, 1, 0, 1 /* CREATE_NEW */, 0x80, 0) != INVALID || GetLastError() != 80) {
        return 5;
    }
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, 4 /* OPEN_ALWAYS */, 0x80, 0);
    if (h == INVALID || GetLastError() != 183 || SetFilePointer(h, 5, 0, 0) != 5 || !SetEndOfFile(h)) {
        return 6;
    }
    CloseHandle(h);
    u64 info[5];
    if (!GetFileAttributesExA(path, 0, info) || ((u32*)info)[8] != 5 /* nFileSizeLow */) {
        return 7;
    }
    if (!DeleteFileA(path) || DeleteFileA(path) || GetLastError() != 2 || GetFileAttributesA(path) != 0xFFFFFFFFu ||
        GetLastError() != 2) {
        return 8;
    }
    return 0x600D;
}

/* Directories: create, list with wildcards (with "." and ".."), refuse to remove when full. */
EXPORT NOINLINE u64 t_file_dirs(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const char* dir = "wse_corpus_dir";
    if (!CreateDirectoryA(dir, 0) || CreateDirectoryA(dir, 0) || GetLastError() != 183 ||
        (GetFileAttributesA(dir) & 0x10) == 0) {
        return 1;
    }
    if (!put_file("wse_corpus_dir\\a.txt", "1") || !put_file("wse_corpus_dir\\B.dat", "22") ||
        !put_file("wse_corpus_dir/c.txt", "333")) {
        return 2;
    }
    u8 fd[320];
    u64 h = FindFirstFileA("wse_corpus_dir\\*.txt", fd);
    int n = 0;
    int saw_a = 0, saw_c = 0;
    if (h == INVALID) {
        return 3;
    }
    do {
        const char* name = (const char*)fd + 44;
        saw_a |= seq(name, "a.txt") && *(u32*)(fd + 32) == 1;
        saw_c |= seq(name, "c.txt") && *(u32*)(fd + 32) == 3;
        ++n;
    } while (FindNextFileA(h, fd));
    if (GetLastError() != 18 || !FindClose(h) || n != 2 || !saw_a || !saw_c) {
        return 4;
    }
    h = FindFirstFileA("wse_corpus_dir\\*", fd);
    n = 0;
    int dots = 0;
    if (h == INVALID) {
        return 5;
    }
    do {
        const char* name = (const char*)fd + 44;
        dots += (seq(name, ".") || seq(name, "..")) && (*(u32*)fd & 0x10) != 0;
        ++n;
    } while (FindNextFileA(h, fd));
    FindClose(h);
    if (n != 5 || dots != 2) {
        return 6;
    }
    if (FindFirstFileA("wse_corpus_dir\\*.none", fd) != INVALID || GetLastError() != 2 ||
        FindFirstFileA("wse_no_such_dir\\*", fd) != INVALID || GetLastError() != 3) {
        return 7;
    }
    if (RemoveDirectoryA(dir) || GetLastError() != 145) {
        return 8;
    }
    if (!DeleteFileA("wse_corpus_dir\\a.txt") || !DeleteFileA("wse_corpus_dir\\b.DAT") ||
        !DeleteFileA("WSE_CORPUS_DIR\\C.TXT") || !RemoveDirectoryA(dir)) {
        return 9;
    }
    return 0x600D;
}

/* Paths: current directory, full path names with their file part, copy and move. */
EXPORT NOINLINE u64 t_file_paths(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    char cwd[300];
    const u32 n = GetCurrentDirectoryA(300, cwd);
    if (n == 0 || n != slen(cwd) || GetCurrentDirectoryA(2, cwd + 299) != n + 1) {
        return 1;
    }
    char full[300];
    char* part = 0;
    char want[300];
    want[0] = 0;
    scat(want, cwd);
    scat(want, "\\x.txt");
    if (GetFullPathNameA("sub\\..\\x.txt", 300, full, &part) != slen(want) || !seq(full, want) || part == 0 ||
        !seq(part, "x.txt")) {
        return 2;
    }
    char temp[300];
    GetTempPathA(260, temp);
    if (!SetCurrentDirectoryA(temp) || !put_file("wse_corpus_copy_a.txt", "abc")) {
        return 3;
    }
    if (!CopyFileA("wse_corpus_copy_a.txt", "wse_corpus_copy_b.txt", 1) ||
        CopyFileA("wse_corpus_copy_a.txt", "wse_corpus_copy_b.txt", 1) || GetLastError() != 80) {
        return 4;
    }
    if (!MoveFileExA("wse_corpus_copy_b.txt", "wse_corpus_copy_c.txt", 0) ||
        GetFileAttributesA("wse_corpus_copy_b.txt") != 0xFFFFFFFFu || GetFileAttributesA("wse_corpus_copy_c.txt") == 0xFFFFFFFFu) {
        return 5;
    }
    if (!DeleteFileA("wse_corpus_copy_a.txt") || !DeleteFileA("wse_corpus_copy_c.txt") || !SetCurrentDirectoryA(cwd)) {
        return 6;
    }
    return 0x600D;
}

/* File mappings: named shared memory seen through two handles, and a mapped file's contents. */
EXPORT NOINLINE u64 t_file_mapping(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 m = CreateFileMappingA(INVALID, 0, 0x04 /* PAGE_READWRITE */, 0, 65536, "Local\\wse_corpus_map");
    u8* p = (u8*)MapViewOfFile(m, 0xF001F /* FILE_MAP_ALL_ACCESS */, 0, 0, 0);
    if (m == 0 || p == 0 || p[100] != 0) {
        return 1;
    }
    p[100] = (u8)(a | 1);
    const u64 m2 = OpenFileMappingA(0xF001F, 0, "Local\\wse_corpus_map");
    u8* q = (u8*)MapViewOfFile(m2, 0xF001F, 0, 0, 0);
    if (m2 == 0 || q == 0 || q[100] != (u8)(a | 1)) {
        return 2;
    }
    UnmapViewOfFile(q);
    UnmapViewOfFile(p);
    CloseHandle(m2);
    CloseHandle(m);
    if (OpenFileMappingA(0xF001F, 0, "Local\\wse_corpus_map") != 0 || GetLastError() != 2) {
        return 3;
    }
    char path[300];
    GetTempPathA(260, path);
    scat(path, "wse_corpus_map.bin");
    if (!put_file(path, "mapped bytes")) {
        return 4;
    }
    const u64 f = CreateFileA(path, GENERIC_READ, 1, 0, 3, 0x80, 0);
    const u64 fm = CreateFileMappingA(f, 0, 0x02 /* PAGE_READONLY */, 0, 0, 0);
    const char* v = (const char*)MapViewOfFile(fm, 0x4 /* FILE_MAP_READ */, 0, 0, 0);
    if (f == INVALID || fm == 0 || v == 0 || v[0] != 'm' || v[7] != 'b' || v[11] != 's') {
        return 5;
    }
    UnmapViewOfFile(v);
    CloseHandle(fm);
    CloseHandle(f);
    return DeleteFileA(path) ? 0x600D : 6;
}
