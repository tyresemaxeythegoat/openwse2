/* Hardware-vs-lifted differential driver for integer/SSE operand widths.
 *
 * flagdiff.py generates flagdiff_lifted.c (synthetic `instruction; ret`
 * functions lifted by proof/lift.py) and flagdiff_cases.h (case table).
 * This driver runs each instruction on the host CPU through hw_x86.S and
 * through the lifted C, then compares registers, flags, and XMM state.
 */
#include "cpu.h"

#include <windows.h>

#define STACK_TOP 0x100000ull
#define SENTINEL  0xDEADBEEFull

uint8_t *MEM;
static uint8_t membuf[MEM_SIZE];

uint64_t g_in[16];
uint64_t g_out[16];
uint8_t g_xmm_in[256];
uint8_t g_xmm_out[256];
uint64_t g_eflags_in;
uint64_t g_flags;
uint64_t g_host_rsp;
uint8_t g_host_xmm[160];
const void *g_code_ptr;

void run_x86(void);

enum { KIND_SHIFT = 0, KIND_IMUL = 1, KIND_CMOV = 2, KIND_XMM = 3 };

#include "flagdiff_cases.h"

#define FLAG_MASK (FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)

static uint32_t flags_from_hw(uint64_t e) {
    uint32_t f = 0;
    if (e & (1ull << 0)) f |= FLAG_CF;
    if (e & (1ull << 2)) f |= FLAG_PF;
    if (e & (1ull << 4)) f |= FLAG_AF;
    if (e & (1ull << 6)) f |= FLAG_ZF;
    if (e & (1ull << 7)) f |= FLAG_SF;
    if (e & (1ull << 11)) f |= FLAG_OF;
    return f;
}

static uint64_t flags_to_hw(uint32_t f) {
    uint64_t e = 0x2; /* reserved bit 1 */
    if (f & FLAG_CF) e |= 1ull << 0;
    if (f & FLAG_PF) e |= 1ull << 2;
    if (f & FLAG_AF) e |= 1ull << 4;
    if (f & FLAG_ZF) e |= 1ull << 6;
    if (f & FLAG_SF) e |= 1ull << 7;
    if (f & FLAG_OF) e |= 1ull << 11;
    return e;
}

static void seed_xmm(uint8_t *p) {
    for (int r = 0; r < 16; r++)
        for (int k = 0; k < 16; k++)
            p[r * 16 + k] = (uint8_t)(0xA0 + r);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s vectors.txt\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    MEM = membuf;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsigned char *exec_buf = (unsigned char *)VirtualAlloc(
        NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!exec_buf) {
        fprintf(stderr, "VirtualAlloc failed\n");
        return 2;
    }
    static uint8_t hw_stack[0x20000] __attribute__((aligned(16)));
    const uint64_t hw_stack_top = (uint64_t)(hw_stack + sizeof hw_stack);

    unsigned counts[MAX_CASES] = {0};
    unsigned bad[MAX_CASES] = {0};
    unsigned id;
    unsigned long long a, b, fl;
    unsigned total = 0, total_bad = 0;

    while (fscanf(in, "%u %llx %llx %llx", &id, &a, &b, &fl) == 4) {
        if (id >= MAX_CASES) {
            fprintf(stderr, "bad case id %u\n", id);
            return 2;
        }
        const Case *k = &cases[id];
        counts[id]++;
        total++;

        /* Shared initial register state: recognizable values everywhere. */
        for (int i = 0; i < 16; i++) {
            g_in[i] = 0x0101010101010101ull * (uint64_t)(i + 1);
        }
        g_in[0] = a;
        g_in[1] = b;
        g_in[4] = hw_stack_top;
        seed_xmm(g_xmm_in);

        g_eflags_in = flags_to_hw((uint32_t)fl);
        memcpy(exec_buf, k->code, k->code_len);
        g_code_ptr = exec_buf;
        run_x86();

        CPU c;
        memset(&c, 0, sizeof c);
        for (int i = 0; i < 16; i++) c.r[i] = g_in[i];
        c.r[4] = STACK_TOP - 8;
        seed_xmm((uint8_t *)c.xmm);
        st64(STACK_TOP - 8, SENTINEL);
        c.flags = (uint32_t)fl;

        uint32_t rc = k->fn(&c);
        if (rc != 0 || c.rip != SENTINEL || c.r[4] != STACK_TOP) {
            fprintf(stderr, "ABI fault case %s: rc=%u rip=0x%llX rsp=0x%llX\n",
                    k->name, rc, (unsigned long long)c.rip,
                    (unsigned long long)c.r[4]);
            return 2;
        }

        uint32_t hwf = flags_from_hw(g_flags);
        uint32_t flag_mask = FLAG_MASK;
        if (k->kind == KIND_SHIFT) {
            unsigned cnt = (unsigned)b & (k->width == 64 ? 63u : 31u);
            flag_mask &= ~(uint32_t)FLAG_AF; /* undefined for shifts */
            if (cnt == 0) {
                /* flags unchanged */
            } else if (cnt >= k->width) {
                flag_mask &= ~(uint32_t)(FLAG_CF | FLAG_OF);
            } else if (cnt != 1) {
                flag_mask &= ~(uint32_t)FLAG_OF;
            }
        } else if (k->kind == KIND_IMUL) {
            /* Only CF and OF are defined; AF/PF/ZF/SF hold stale CPU state. */
            flag_mask &= ~(uint32_t)(FLAG_AF | FLAG_PF | FLAG_ZF | FLAG_SF);
        }

        int mismatch = 0;
        for (int i = 0; i < 16; i++) {
            if (i == 4) continue; /* host vs guest stack address */
            if (g_out[i] != c.r[i]) mismatch = 1;
        }
        if ((hwf & flag_mask) != ((uint32_t)c.flags & flag_mask)) mismatch = 1;
        if (memcmp(g_xmm_out, c.xmm, sizeof g_xmm_out) != 0) mismatch = 1;

        if (mismatch) {
            bad[id]++;
            total_bad++;
            if (bad[id] <= 3) {
                fprintf(stderr, "MISMATCH %s a=%016llx b=%016llx flags=%03llx\n",
                        k->name, a, b, fl);
                for (int i = 0; i < 16; i++) {
                    if (i != 4 && g_out[i] != c.r[i]) {
                        fprintf(stderr, "  r%-2d hw=%016llx lifted=%016llx\n", i,
                                (unsigned long long)g_out[i],
                                (unsigned long long)c.r[i]);
                    }
                }
                if ((hwf & flag_mask) != ((uint32_t)c.flags & flag_mask)) {
                    fprintf(stderr, "  flags hw=%03x lifted=%03x mask=%03x kind=%u width=%u b=%llx\n",
                            hwf & flag_mask, (unsigned)c.flags & flag_mask, flag_mask,
                            k->kind, k->width, b);
                }
                if (memcmp(g_xmm_out, c.xmm, sizeof g_xmm_out) != 0) {
                    fprintf(stderr, "  xmm0 hw=%016llx%016llx lifted=%016llx%016llx\n",
                            (unsigned long long)((uint64_t *)g_xmm_out)[1],
                            (unsigned long long)((uint64_t *)g_xmm_out)[0],
                            (unsigned long long)c.xmm[0].q[1],
                            (unsigned long long)c.xmm[0].q[0]);
                }
            }
        }
    }
    fclose(in);

    for (unsigned i = 0; i < MAX_CASES; i++) {
        if (counts[i]) {
            printf("%-16s %6u vectors  %6u mismatches\n", cases[i].name, counts[i], bad[i]);
        }
    }
    printf("total: %u vectors, %u mismatches\n", total, total_bad);
    return total_bad == 0 ? 0 : 1;
}
