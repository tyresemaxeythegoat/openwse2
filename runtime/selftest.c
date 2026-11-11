/* Offline self-test for the runtime skeleton.
 *
 * Builds a synthetic PE32+ image in memory (one import, one DIR64 reloc), maps
 * it into a reserved guest range, binds the import, builds a TEB/PEB/TLS block,
 * and exercises the two real gs-relative access patterns.
 */
#include "guest.h"
#include "pe.h"
#include "dispatch.h"
#include "imports.h"
#include "shim.h"
#include "teb.h"

#include <stdio.h>
#include <string.h>

#define FILE_SIZE 0xA00u
#define HDR_SIZE 0x400u
#define TEXT_RVA 0x1000u
#define TEXT_RAW 0x400u
#define DATA_RVA 0x2000u
#define DATA_RAW 0x600u
#define RDATA_RVA 0x3000u
#define RDATA_RAW 0x800u
#define IMAGE_SIZE 0x4000u
#define PREFERRED_BASE 0x140000000ull

static void w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void build_pe(uint8_t *f) {
    memset(f, 0, FILE_SIZE);
    f[0] = 'M';
    f[1] = 'Z';
    w32(f + 0x3C, 0x80);

    uint8_t *pe = f + 0x80;
    w32(pe, 0x00004550u);
    w16(pe + 4, 0x8664);
    w16(pe + 6, 3);
    w16(pe + 20, 0xF0);
    w16(pe + 22, 0x22);

    uint8_t *opt = pe + 24; /* 0x98 */
    w16(opt, 0x20B);
    w32(opt + 0x10, TEXT_RVA);
    w64(opt + 0x18, PREFERRED_BASE);
    w32(opt + 0x20, 0x1000);
    w32(opt + 0x24, 0x200);
    w32(opt + 0x38, IMAGE_SIZE);
    w32(opt + 0x3C, HDR_SIZE);
    w16(opt + 0x44, 3);
    w32(opt + 0x6C, 16);
    /* import directory, index 1 */
    w32(opt + 0x70 + 8, RDATA_RVA);
    w32(opt + 0x70 + 12, 40);
    /* base reloc directory, index 5 */
    w32(opt + 0x70 + 40, RDATA_RVA + 0xA0);
    w32(opt + 0x70 + 44, 12);

    uint8_t *sec = opt + 0xF0; /* 0x188 */
    memcpy(sec, ".text\0\0\0", 8);
    w32(sec + 8, 0x100);
    w32(sec + 12, TEXT_RVA);
    w32(sec + 16, 0x200);
    w32(sec + 20, TEXT_RAW);
    w32(sec + 36, 0x60000020);

    sec += 40;
    memcpy(sec, ".data\0\0\0", 8);
    w32(sec + 8, 0x100);
    w32(sec + 12, DATA_RVA);
    w32(sec + 16, 0x200);
    w32(sec + 20, DATA_RAW);
    w32(sec + 36, 0xC0000040);

    sec += 40;
    memcpy(sec, ".rdata\0\0", 8);
    w32(sec + 8, 0x200);
    w32(sec + 12, RDATA_RVA);
    w32(sec + 16, 0x200);
    w32(sec + 20, RDATA_RAW);
    w32(sec + 36, 0x40000040);
}

static void build_payload(uint8_t *f) {
    uint8_t *text = f + TEXT_RAW;
    text[0] = 0x55;
    text[1] = 0x48;
    text[2] = 0x89;
    text[3] = 0xE5;
    text[4] = 0x90;
    text[5] = 0xC3;

    uint8_t *data = f + DATA_RAW;
    w64(data + 0x20, PREFERRED_BASE + TEXT_RVA); /* reloc target */
    w64(data + 0x40, 0);                          /* IAT slot */

    uint8_t *rdata = f + RDATA_RAW;
    w32(rdata + 0x00, RDATA_RVA + 0x40); /* OriginalFirstThunk */
    w32(rdata + 12, RDATA_RVA + 0x60);   /* Name */
    w32(rdata + 16, DATA_RVA + 0x40);    /* FirstThunk */
    w32(rdata + 0x40, RDATA_RVA + 0x80); /* INT[0] */
    w32(rdata + 0x44, 0);
    memcpy(rdata + 0x60, "KERNEL32.dll", 13);
    w16(rdata + 0x80, 0); /* hint */
    memcpy(rdata + 0x82, "GetTickCount64", 15);

    w32(rdata + 0xA0, DATA_RVA);
    w32(rdata + 0xA4, 12);
    w16(rdata + 0xA8, (10u << 12) | 0x20u); /* DIR64 at page+0x20 */
    w16(rdata + 0xAA, 0);
}

static void require(int cond, const char *what) {
    if (!cond) {
        wse2_fatal("selftest failed: %s", what);
    }
}

int main(void) {
    uint8_t file[FILE_SIZE];
    build_pe(file);
    build_payload(file);

    GuestSpace g;
    require(wse2_guest_reserve(&g, PREFERRED_BASE, 0x1000000) == 0, "guest reserve");
    wse2_guest_attach(&g);

    Wse2PeImage img;
    uint32_t relocs = 0;
    char err[256];
    require(wse2_pe_map(file, sizeof file, &g, &img, &relocs, err, sizeof err) == 0, err);
    require(img.entry_rva == TEXT_RVA, "entry RVA");
    require(img.section_count == 3, "section count");
    require(relocs == 1, "one DIR64 reloc");
    require(ld32(g.guest_base + TEXT_RVA) == 0xE5894855u, "text mapping");
    require(ld64(g.guest_base + DATA_RVA + 0x20) == g.guest_base + TEXT_RVA,
            "relocation applied");

    require(wse2_dispatch_init(256) == 0, "dispatch init");
    require(wse2_imports_init(g.guest_base + 0x100000, 0x10000) == 0, "import arena");
    wse2_shim_set_image_base(g.guest_base);

    Wse2ImportStats st;
    require(wse2_imports_bind(&g, &img, &st) == 0, "bind imports");
    require(st.total == 1 && st.bound == 1 && st.missing == 0, "import stats");

    uint64_t iat = ld64(g.guest_base + DATA_RVA + 0x40);
    require(iat != 0, "IAT patched");
    require(iat == wse2_imports_find("KERNEL32.dll", "GetTickCount64"), "import lookup");

    Wse2Fn fn = NULL;
    const char *name = NULL;
    require(wse2_dispatch_resolve(iat, &fn, &name) == 1, "dispatch resolve");
    require(fn != NULL && name && strstr(name, "GetTickCount64") != NULL, "shim bound");

    CPU c;
    memset(&c, 0, sizeof c);
    require(wse2_dispatch_call(&c, iat) == 0, "call import");
    require(c.r[RAX] != 0, "GetTickCount64 returns nonzero");

    uint64_t teb_va = g.guest_base + 0x200000;
    uint64_t peb_va = g.guest_base + 0x201000;
    uint64_t tls_array = g.guest_base + 0x202000;
    uint64_t tls_block = g.guest_base + 0x203000;
    uint64_t stack_base = g.guest_base + 0x400000;
    uint64_t stack_limit = g.guest_base + 0x300000;
    const uint8_t tls_template[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

    Wse2Teb teb;
    require(wse2_teb_build(teb_va, peb_va, g.guest_base, stack_base, stack_limit,
                           4242, 1717, tls_array, tls_block,
                           tls_template, sizeof tls_template, &teb) == 0, "teb build");
    require(ld64(teb_va + WSE2_TEB_SELF) == teb_va, "TEB self pointer");
    require(ld64(teb_va + WSE2_TEB_STACK_LIMIT) == stack_limit, "TEB stack limit");
    require(ld64(teb_va + WSE2_TEB_STACK_BASE) == stack_base, "TEB stack base");
    require(ld64(teb_va + WSE2_TEB_PEB) == peb_va, "TEB PEB pointer");
    require(ld64(teb_va + WSE2_TEB_CLIENT_ID) == 4242, "TEB process id");
    require(ld64(teb_va + WSE2_TEB_CLIENT_ID + 8) == 1717, "TEB thread id");
    require(ld64(peb_va + WSE2_PEB_IMAGE_BASE) == g.guest_base, "PEB image base");

    /* Emulate: mov rax, gs:[0x58]; mov rdx, [rax + _tls_index*8] */
    c.teb = teb_va;
    uint64_t tls_ptr = ld64(c.teb + WSE2_TEB_TLS_POINTER);
    require(tls_ptr == tls_array, "TLS pointer");
    uint64_t block = ld64(tls_ptr + 0 * 8); /* _tls_index == 0 => .data is zero */
    require(block == tls_block, "TLS block slot");
    require(ld32(block) == 0xEFBEADDEu, "TLS template copied");

    /* Emulate: mov r11, gs:[0x10]; cmp r10, r11; jae. */
    uint64_t rsp = stack_base - 0x1000;
    require(rsp - 0xF00 >= ld64(c.teb + WSE2_TEB_STACK_LIMIT), "stack probe passes");
    require(rsp - 0x200000 < ld64(c.teb + WSE2_TEB_STACK_LIMIT), "stack probe triggers");

    /* Relocation path: map the same image at a different base. */
    GuestSpace g2;
    require(wse2_guest_reserve(&g2, PREFERRED_BASE + 0x1000000, 0x800000) == 0,
            "second reservation");
    Wse2PeImage img2;
    uint32_t relocs2 = 0;
    require(wse2_pe_map(file, sizeof file, &g2, &img2, &relocs2, err, sizeof err) == 0, err);
    require(relocs2 == 1, "second reloc applied");
    uint64_t rebased_ptr = 0;
    memcpy(&rebased_ptr, g2.base + DATA_RVA + 0x20, 8);
    require(rebased_ptr == g2.guest_base + TEXT_RVA, "rebased pointer");
    wse2_guest_release(&g2);

    printf("wse2 runtime selftest: PASS\n");
    printf("  image base      0x%llX%s\n", (unsigned long long)g.guest_base,
           g.rebased ? " (rebased)" : "");
    printf("  sections        %u, relocs %u\n", img.section_count, relocs);
    printf("  imports         %u total, %u bound, %u missing\n", st.total, st.bound, st.missing);
    printf("  teb             0x%llX  tls slot0 0x%llX\n",
           (unsigned long long)teb_va, (unsigned long long)tls_block);

    wse2_guest_release(&g);
    return 0;
}
