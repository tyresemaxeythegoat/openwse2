/* Native differential harness for the lifted WSE2 function. */
#include "cpu.h"

uint32_t f_485390(CPU *c);

static uint8_t membuf[MEM_SIZE];
uint8_t *MEM = membuf;

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *s, uint8_t *out) {
    if (s[0] == '-' && s[1] == 0) return 0;
    size_t n = 0;
    while (s[0] && s[1]) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) {
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

    char sa[1024], sb[1024];
    int len;
    unsigned cases = 0;
    while (fscanf(in, "%d %1023s %1023s", &len, sa, sb) == 3) {
        if (len < 0 || len > 512) {
            fprintf(stderr, "bad length %d\n", len);
            return 2;
        }
        memset(membuf, 0, sizeof membuf);
        parse_hex(sa, membuf + 0x1000);
        parse_hex(sb, membuf + 0x1100);

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[RSP] = 0x8000;
        st64(0x8000, 0xDEADBEEFull);
        c.r[RCX] = 0x1000;
        c.r[RDX] = 0x1100;
        c.r[R8] = (uint64_t)len;
        c.r[R9] = 0x123456789ABCDEF0ull;
        c.r[R10] = 0xFEDCBA9876543210ull;
        c.r[R11] = 0x0F0F0F0F0F0F0F0Full;

        uint32_t rc = f_485390(&c);
        if (rc != 0) {
            fprintf(stderr, "lifted function returned %u\n", rc);
            return 2;
        }
        if (c.rip != 0xDEADBEEFull || c.r[RSP] != 0x8008) {
            fprintf(stderr, "ABI fault at case %u: rip=0x%llX rsp=0x%llX\n",
                    cases, (unsigned long long)c.rip, (unsigned long long)c.r[RSP]);
            return 2;
        }

        fprintf(out, "%llx %x\n",
                (unsigned long long)(c.r[RAX] & 0xFFFFFFFFu),
                c.flags & (FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_SF | FLAG_OF));
        cases++;
    }

    fclose(in);
    fclose(out);
    fprintf(stderr, "harness: %u cases\n", cases);
    return 0;
}
