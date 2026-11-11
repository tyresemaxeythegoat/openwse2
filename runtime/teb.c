#include "teb.h"
#include "cpu.h"

#include <string.h>

int wse2_teb_build(uint64_t teb_va, uint64_t peb_va, uint64_t image_base,
                   uint64_t stack_base, uint64_t stack_limit,
                   uint32_t pid, uint32_t tid,
                   uint64_t tls_array_va, uint64_t tls_block_va,
                   const uint8_t *tls_template, uint32_t tls_size,
                   Wse2Teb *out) {
    if (tls_size > WSE2_TLS_BLOCK_SIZE) {
        wse2_fatal("tls block too large: %u", tls_size);
    }
    if (stack_base <= stack_limit) {
        wse2_fatal("bad stack range: base 0x%llX limit 0x%llX",
                   (unsigned long long)stack_base, (unsigned long long)stack_limit);
    }
    mem_check(teb_va, WSE2_TEB_SIZE);
    mem_check(peb_va, WSE2_PEB_SIZE);
    mem_check(tls_array_va, WSE2_TLS_SLOTS * 8);
    mem_check(tls_block_va, WSE2_TLS_BLOCK_SIZE);

    memset(mem_ptr(teb_va), 0, WSE2_TEB_SIZE);
    st64(teb_va + WSE2_TEB_EXCEPTION_LIST, ~0ull); /* ExceptionList = -1 */
    st64(teb_va + WSE2_TEB_STACK_BASE, stack_base);
    st64(teb_va + WSE2_TEB_STACK_LIMIT, stack_limit);
    st64(teb_va + WSE2_TEB_SELF, teb_va);
    st64(teb_va + WSE2_TEB_CLIENT_ID, pid);
    st64(teb_va + WSE2_TEB_CLIENT_ID + 8, tid);
    st64(teb_va + WSE2_TEB_TLS_POINTER, tls_array_va);
    st64(teb_va + WSE2_TEB_PEB, peb_va);

    memset(mem_ptr(peb_va), 0, WSE2_PEB_SIZE);
    st64(peb_va + WSE2_PEB_IMAGE_BASE, image_base);
    /* Report Windows 10 22H2 to version checks that read the PEB directly. */
    st32(peb_va + WSE2_PEB_OS_MAJOR, 10);
    st32(peb_va + WSE2_PEB_OS_MINOR, 0);
    st16(peb_va + WSE2_PEB_OS_BUILD, 19045);
    st32(peb_va + WSE2_PEB_OS_PLATFORM, 2); /* VER_PLATFORM_WIN32_NT */

    memset(mem_ptr(tls_array_va), 0, WSE2_TLS_SLOTS * 8);
    st64(tls_array_va, tls_block_va); /* slot for _tls_index == 0 */

    memset(mem_ptr(tls_block_va), 0, WSE2_TLS_BLOCK_SIZE);
    if (tls_template && tls_size) {
        memcpy(mem_ptr(tls_block_va), tls_template, tls_size);
    }

    if (out) {
        memset(out, 0, sizeof *out);
        out->teb_va = teb_va;
        out->peb_va = peb_va;
        out->tls_array_va = tls_array_va;
        out->tls_block_va = tls_block_va;
        out->process_id = pid;
        out->thread_id = tid;
    }
    return 0;
}
