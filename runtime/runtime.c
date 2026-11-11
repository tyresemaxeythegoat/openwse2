/* Runtime globals and the fail-loud path. */
#include "cpu.h"

#include <stdarg.h>

uint8_t *MEM = NULL;
uint64_t MEM_BIAS = 0;
uint64_t MEM_LIMIT = 0;

void wse2_fatal(const char *fmt, ...) {
    va_list ap;
    fputs("wse2: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
