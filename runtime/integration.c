/* Runtime integration test.
 *
 * Requires the retail executable at runtime (never committed):
 *
 *   integration.exe <path-to>mb_warband_wse2_x64.exe
 *
 * It maps the real PE32+ image at 0x140000000 through the runtime loader, then
 * calls two production-mode lifts (image-base addressing) through the indirect
 * dispatch table:
 *
 *   f_485390  memcmp
 *   f_43E640  btConvexInternalShape::setLocalScaling  (RIP-relative mask)
 */
#include "guest.h"
#include "pe.h"
#include "dispatch.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t f_485390(CPU *c);
uint32_t f_43E640(CPU *c);

#define GUEST_BASE 0x140000000ull
#define GUEST_SIZE 0x4000000ull

#define BUF_A (GUEST_BASE + 0x1000000ull)
#define BUF_B (GUEST_BASE + 0x1100000ull)
#define VEC   (GUEST_BASE + 0x1200000ull)
#define OBJ   (GUEST_BASE + 0x1210000ull)
#define STACK (GUEST_BASE + 0x2000000ull)
#define SENTINEL (GUEST_BASE + 0x3000000ull)

static void require(int cond, const char *what) {
    if (!cond) {
        wse2_fatal("integration: %s", what);
    }
}

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        wse2_fatal("cannot open %s", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) wse2_fatal("seek failed");
    long n = ftell(f);
    if (n <= 0) wse2_fatal("empty file");
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) wse2_fatal("out of memory");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) wse2_fatal("short read");
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static void prep_call(CPU *c) {
    memset(c, 0, sizeof *c);
    c->r[RSP] = STACK;
    st64(STACK, SENTINEL);
}

static void check_return(CPU *c, const char *what) {
    require(c->rip == SENTINEL, what);
    require(c->r[RSP] == STACK + 8, "stack pointer restored");
}

static int32_t run_memcmp(CPU *c, uint64_t a, uint64_t b, uint64_t n) {
    prep_call(c);
    c->r[RCX] = a;
    c->r[RDX] = b;
    c->r[R8] = n;
    wse2_dispatch_call(c, GUEST_BASE + 0x485390ull);
    check_return(c, "memcmp return");
    return (int32_t)(uint32_t)c->r[RAX];
}

static void check_scaling(CPU *c, uint32_t x, uint32_t y, uint32_t z) {
    st32(VEC + 0, x);
    st32(VEC + 4, y);
    st32(VEC + 8, z);
    memset(mem_ptr(OBJ), 0, 0x40);

    prep_call(c);
    c->r[RCX] = OBJ;
    c->r[RDX] = VEC;
    wse2_dispatch_call(c, GUEST_BASE + 0x43E640ull);
    check_return(c, "setLocalScaling return");

    require(ld32(OBJ + 0x20) == (x & 0x7FFFFFFFu), "abs(x)");
    require(ld32(OBJ + 0x24) == (y & 0x7FFFFFFFu), "abs(y)");
    require(ld32(OBJ + 0x28) == (z & 0x7FFFFFFFu), "abs(z)");
    require(ld32(OBJ + 0x2C) == 0, "w cleared");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mb_warband_wse2_x64.exe>\n", argv[0]);
        return 2;
    }
    size_t file_len = 0;
    uint8_t *file = read_file(argv[1], &file_len);

    GuestSpace g;
    require(wse2_guest_reserve(&g, GUEST_BASE, GUEST_SIZE) == 0, "guest reservation");
    require(!g.rebased, "preferred base unavailable; production lifts need 0x140000000");
    wse2_guest_attach(&g);

    Wse2PeImage img;
    uint32_t relocs = 0;
    char err[256];
    require(wse2_pe_map(file, file_len, &g, &img, &relocs, err, sizeof err) == 0, err);
    require(img.image_base == GUEST_BASE, "image base");
    require(img.entry_rva == 0x481CE0u, "entry RVA");
    require(img.section_count == 7, "section count");

    require(wse2_dispatch_init(256) == 0, "dispatch init");
    require(wse2_dispatch_register(GUEST_BASE + 0x485390ull, f_485390, "memcmp") == 0,
            "register memcmp");
    require(wse2_dispatch_register(GUEST_BASE + 0x43E640ull, f_43E640, "setLocalScaling") == 0,
            "register scaling");

    uint8_t *a = mem_ptr(BUF_A);
    uint8_t *b = mem_ptr(BUF_B);
    for (int i = 0; i < 256; i++) {
        a[i] = (uint8_t)(i * 7 + 3);
        b[i] = a[i];
    }

    CPU c;
    require(run_memcmp(&c, BUF_A, BUF_B, 0) == 0, "memcmp len 0");
    require(run_memcmp(&c, BUF_A, BUF_B, 256) == 0, "memcmp equal 256");

    b[0] = (uint8_t)(a[0] + 1);
    require(run_memcmp(&c, BUF_A, BUF_B, 256) < 0, "memcmp first byte lower");
    b[0] = a[0];
    b[200] = (uint8_t)(a[200] + 1);
    require(run_memcmp(&c, BUF_A, BUF_B, 256) < 0, "memcmp tail lower");
    b[200] = (uint8_t)(a[200] - 1);
    require(run_memcmp(&c, BUF_A, BUF_B, 256) > 0, "memcmp tail higher");
    b[200] = a[200];
    require(run_memcmp(&c, BUF_A, BUF_B, 200) == 0, "memcmp prefix equal");

    check_scaling(&c, 0xBF800000u, 0x7FC00001u, 0x80000000u);
    check_scaling(&c, 0x3F800000u, 0xFF7FFFFFu, 0x00800000u);
    check_scaling(&c, 0xFFFFFFFFu, 0x00000000u, 0x7F7FFFFFu);

    printf("wse2 runtime integration: PASS\n");
    printf("  image           0x%llX, %zu bytes, %u sections, %u relocs\n",
           (unsigned long long)img.image_base, file_len, img.section_count, relocs);
    printf("  lifted calls    memcmp 5 cases, setLocalScaling 3 cases\n");
    printf("  dispatch        indirect calls through guest VAs\n");

    free(file);
    wse2_guest_release(&g);
    return 0;
}
