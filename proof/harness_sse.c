/* Native differential harness for the SSE + RIP-relative proof function. */
#include "cpu.h"

uint32_t f_43E640(CPU *c);

static uint8_t membuf[MEM_SIZE];
uint8_t *MEM = membuf;

#define INPUT   0x1000u
#define OBJ     0x2000u
#define STACKSP 0x8008ull
#define SENTINEL 0xDEADBEEFull

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    for (int k = 0; k < 8; k++) {
        int h = hexval((unsigned char)s[k]);
        if (h < 0) {
            fprintf(stderr, "bad hex word: %s\n", s);
            exit(2);
        }
        v = (v << 4) | (uint32_t)h;
    }
    if (s[8] != 0) {
        fprintf(stderr, "hex word too long: %s\n", s);
        exit(2);
    }
    return v;
}

static void xmm_pattern(CPU *c) {
    for (int i = 0; i < 16; i++) {
        uint8_t *p = (uint8_t *)&c->xmm[i];
        for (int k = 0; k < 16; k++)
            p[k] = (uint8_t)(0xA0 + i);
    }
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
    memset(membuf, 0, sizeof membuf);
    size_t nimg = fread(membuf, 1, sizeof membuf, img);
    fclose(img);
    if (nimg == 0) {
        fprintf(stderr, "empty image\n");
        return 2;
    }

    char sa[32], sb[32], sc[32];
    unsigned cases = 0;
    while (fscanf(in, "%31s %31s %31s", sa, sb, sc) == 3) {
        /* Clear the object and the frame scratch. */
        memset(membuf + OBJ, 0, 0x100);
        memset(membuf + 0x7F00, 0, 0x200);

        st32(INPUT + 0, parse_u32(sa));
        st32(INPUT + 4, parse_u32(sb));
        st32(INPUT + 8, parse_u32(sc));

        CPU c;
        memset(&c, 0, sizeof c);
        xmm_pattern(&c);
        c.r[RSP] = STACKSP;
        st64(STACKSP, SENTINEL);
        c.r[RCX] = OBJ;
        c.r[RDX] = INPUT;
        c.r[R8] = 0x1111111111111111ull;
        c.r[R9] = 0x2222222222222222ull;

        uint32_t rc = f_43E640(&c);
        if (rc != 0) {
            fprintf(stderr, "lifted function returned %u\n", rc);
            return 2;
        }
        if (c.rip != SENTINEL || c.r[RSP] != STACKSP + 8) {
            fprintf(stderr, "ABI fault at case %u: rip=0x%llX rsp=0x%llX\n",
                    cases, (unsigned long long)c.rip, (unsigned long long)c.r[RSP]);
            return 2;
        }

        for (int k = 0; k < 16; k++)
            fprintf(out, "%02x", membuf[OBJ + 0x20 + k]);
        for (int i = 0; i < 16; i++)
            fprintf(out, " %016llx%016llx",
                    (unsigned long long)c.xmm[i].q[0],
                    (unsigned long long)c.xmm[i].q[1]);
        fputc('\n', out);
        cases++;
    }

    fclose(in);
    fclose(out);
    fprintf(stderr, "harness: %u cases\n", cases);
    return 0;
}
