/* Differential harness for ?QueryInterface@rglEffectStateManager (0x281B50).
 *
 * The function loads its vtable from [rcx], then calls slot 1 through an
 * indirect call. The fake vtable points slot 1 at setLocalScaling (0x43E640),
 * another lifted function, so the call exercises wse2_dispatch_call and the
 * guest return-address push/pop across two lifted frames.
 */
#include "cpu.h"

uint32_t f_281B50(CPU *c);
uint32_t f_43E640(CPU *c);

static uint8_t membuf[MEM_SIZE];
uint8_t *MEM = membuf;

#define OBJ      0x1000u
#define VT       0x2000u
#define VEC      0x3000u
#define PPV      0x4000u
#define STACKSP  0xFFF8ull
#define SENTINEL 0xDEADBEEFull

static unsigned dispatch_calls;
static uint64_t dispatch_last;

uint32_t wse2_dispatch_call(CPU *c, uint64_t guest_va) {
    dispatch_calls++;
    dispatch_last = guest_va;
    if (guest_va == 0x43E640ull) {
        return f_43E640(c);
    }
    fprintf(stderr, "harness: unexpected indirect target 0x%llX\n",
            (unsigned long long)guest_va);
    exit(2);
}

static uint32_t parse_word(const char *s) {
    return (uint32_t)strtoul(s, NULL, 16);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s image.bin vectors.txt actual.txt\n", argv[0]);
        return 2;
    }
    FILE *img = fopen(argv[1], "rb");
    FILE *in = fopen(argv[2], "r");
    FILE *out = fopen(argv[3], "w");
    if (!img || !in || !out) {
        fprintf(stderr, "open failed\n");
        return 2;
    }

    char sx[32], sy[32], sz[32];
    unsigned cases = 0;
    memset(membuf, 0, sizeof membuf);
    size_t nimg = fread(membuf, 1, sizeof membuf, img);
    fclose(img);
    if (nimg == 0) {
        fprintf(stderr, "empty image\n");
        return 2;
    }

    while (fscanf(in, "%31s %31s %31s", sx, sy, sz) == 3) {
        uint32_t x = parse_word(sx), y = parse_word(sy), z = parse_word(sz);

        memset(membuf + OBJ, 0, 0x40);
        st64(OBJ, VT);              /* object -> fake vtable */
        st64(VT + 0, 0);            /* slot 0 unused */
        st64(VT + 8, 0x43E640ull);  /* slot 1 -> lifted setLocalScaling */
        st64(PPV, 0);
        st32(VEC + 0, x);
        st32(VEC + 4, y);
        st32(VEC + 8, z);
        st32(VEC + 12, 0xDEADBEEFu);
        st32(OBJ + 0x20, 0xCCCCCCCCu);
        st32(OBJ + 0x24, 0xCCCCCCCCu);
        st32(OBJ + 0x28, 0xCCCCCCCCu);
        st32(OBJ + 0x2C, 0xCCCCCCCCu);
        st64(STACKSP, SENTINEL);

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[RSP] = STACKSP;
        c.r[RCX] = OBJ;
        c.r[RDX] = VEC;
        c.r[R8] = PPV;
        dispatch_calls = 0;
        dispatch_last = 0;

        uint32_t rc = f_281B50(&c);
        if (rc != 0) {
            fprintf(stderr, "lifted function returned %u\n", rc);
            return 2;
        }
        if (c.rip != SENTINEL || c.r[RSP] != STACKSP + 8) {
            fprintf(stderr, "ABI fault at case %u: rip=0x%llX rsp=0x%llX\n",
                    cases, (unsigned long long)c.rip, (unsigned long long)c.r[RSP]);
            return 2;
        }
        if (dispatch_calls != 1 || dispatch_last != 0x43E640ull) {
            fprintf(stderr, "dispatch fault at case %u: calls=%u last=0x%llX\n",
                    cases, dispatch_calls, (unsigned long long)dispatch_last);
            return 2;
        }

        fprintf(out, "%x %x %u %u %x %x %x %x %u\n",
                (unsigned)(c.r[RAX] & 0xFFFFFFFFu),
                c.flags & (FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_SF | FLAG_OF),
                ld64(PPV) == OBJ,
                ld64(OBJ) == VT,
                ld32(OBJ + 0x20), ld32(OBJ + 0x24),
                ld32(OBJ + 0x28), ld32(OBJ + 0x2C),
                dispatch_calls);
        cases++;
    }

    fclose(in);
    fclose(out);
    fprintf(stderr, "harness: %u cases\n", cases);
    return 0;
}
