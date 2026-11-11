/* Minimal Windows TEB/PEB/TLS layout for the two real gs-relative accesses in
 * the engine:
 *
 *   0x471ACC  mov rax, qword ptr gs:[0x58]   ; TEB->ThreadLocalStoragePointer
 *   0x482C1C  mov r11, qword ptr gs:[0x10]   ; TEB->NtTib.StackLimit
 *
 * plus the TEB self pointer (0x30) and ClientId (0x40) that shims rely on.
 */
#pragma once
#include <stdint.h>

#define WSE2_TEB_SIZE 0x1000u
#define WSE2_PEB_SIZE 0x1000u
#define WSE2_TLS_SLOTS 64u
#define WSE2_TLS_BLOCK_SIZE 0x1000u

#define WSE2_TEB_EXCEPTION_LIST 0x00u
#define WSE2_TEB_STACK_BASE 0x08u
#define WSE2_TEB_STACK_LIMIT 0x10u
#define WSE2_TEB_SELF 0x30u
#define WSE2_TEB_CLIENT_ID 0x40u   /* process id at +0x40, thread id at +0x48 */
#define WSE2_TEB_TLS_POINTER 0x58u
#define WSE2_TEB_PEB 0x60u
#define WSE2_TEB_LAST_ERROR 0x68u

#define WSE2_PEB_IMAGE_BASE 0x10u
#define WSE2_PEB_OS_MAJOR 0x118u
#define WSE2_PEB_OS_MINOR 0x11Cu
#define WSE2_PEB_OS_BUILD 0x120u
#define WSE2_PEB_OS_PLATFORM 0x124u

typedef struct {
    uint64_t teb_va;
    uint64_t peb_va;
    uint64_t tls_array_va;
    uint64_t tls_block_va;
    uint32_t process_id;
    uint32_t thread_id;
} Wse2Teb;

int wse2_teb_build(uint64_t teb_va, uint64_t peb_va, uint64_t image_base,
                   uint64_t stack_base, uint64_t stack_limit,
                   uint32_t pid, uint32_t tid,
                   uint64_t tls_array_va, uint64_t tls_block_va,
                   const uint8_t *tls_template, uint32_t tls_size,
                   Wse2Teb *out);
