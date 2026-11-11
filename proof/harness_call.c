/* Differential harness for std::string::compare calling memcmp. */
#include "cpu.h"

uint32_t f_124A0(CPU *c);

static uint8_t membuf[MEM_SIZE];
uint8_t *MEM = membuf;

#define OBJ_A   0x1000u
#define OBJ_B   0x2000u
#define BUF_A   0x4000u
#define BUF_B   0x5000u
/* Same low 32 bits as Unicorn's guest stack, so the epilogue `add rsp, 0x20`
   produces identical SF/OF/PF and the final flag comparison is meaningful. */
#define STACKSP 0xFFF8ull
#define SENTINEL 0xDEADBEEFull

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *s, uint8_t *out, size_t cap) {
    if (s[0] == '-' && s[1] == 0) return 0;
    size_t n = 0;
    while (s[0] && s[1]) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0 || n >= cap) {
            fprintf(stderr, "bad hex: %s\n", s);
            exit(2);
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    if (s[0]) {
        fprintf(stderr, "odd hex length\n");
        exit(2);
    }
    return n;
}

/* MSVC std::string x64: buffer[16] @0, size @0x10, capacity @0x18. */
static void build_string(uint64_t obj, uint64_t ext, const uint8_t *data, size_t n) {
    memset(MEM + obj, 0, 0x20);
    if (n < 16) {
        memcpy(MEM + obj, data, n);
        st64(obj + 0x10, n);
        st64(obj + 0x18, 15);
    } else {
        st64(obj, ext);
        memcpy(MEM + ext, data, n);
        st64(obj + 0x10, n);
        st64(obj + 0x18, n);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s vectors.txt actual.txt\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    FILE *out = fopen(argv[2], "w");
    if (!in || !out) {
        fprintf(stderr, "open failed\n");
        return 2;
    }

    char sa[4096], sb[4096];
    uint8_t da[2048], db[2048];
    unsigned cases = 0;
    /* Buffers are rebuilt per case and memcmp never reads past min(lenA,lenB),
       so one clear is enough; the stack scratch is rewritten each call. */
    memset(membuf, 0, sizeof membuf);
    while (fscanf(in, "%4095s %4095s", sa, sb) == 2) {
        size_t na = parse_hex(sa, da, sizeof da);
        size_t nb = parse_hex(sb, db, sizeof db);

        build_string(OBJ_A, BUF_A, da, na);
        build_string(OBJ_B, BUF_B, db, nb);
        st64(STACKSP, SENTINEL);

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[RSP] = STACKSP;
        c.r[RCX] = OBJ_A;
        c.r[RDX] = OBJ_B;
        c.r[R8] = 0x1111111111111111ull;

        uint32_t rc = f_124A0(&c);
        if (rc != 0) {
            fprintf(stderr, "lifted function returned %u\n", rc);
            return 2;
        }
        if (c.rip != SENTINEL || c.r[RSP] != STACKSP + 8) {
            fprintf(stderr, "ABI fault at case %u: rip=0x%llX rsp=0x%llX\n",
                    cases, (unsigned long long)c.rip, (unsigned long long)c.r[RSP]);
            return 2;
        }

        fprintf(out, "%x %x\n",
                (unsigned)(c.r[RAX] & 0xFFFFFFFFu),
                c.flags & (FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_SF | FLAG_OF));
        cases++;
    }

    fclose(in);
    fclose(out);
    fprintf(stderr, "harness: %u cases\n", cases);
    return 0;
}
