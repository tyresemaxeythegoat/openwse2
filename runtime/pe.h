/* Minimal PE32+ x86-64 image parser and mapper. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "guest.h"

typedef struct {
    char name[9];
    uint32_t rva;
    uint32_t vsize;
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t characteristics;
} Wse2PeSection;

typedef struct {
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t entry_rva;
    uint32_t import_rva;
    uint32_t import_size;
    uint32_t reloc_rva;
    uint32_t reloc_size;
    uint16_t machine;
    uint16_t section_count;
    Wse2PeSection sections[32];
} Wse2PeImage;

int wse2_pe_parse(const uint8_t *file, size_t len, Wse2PeImage *out,
                  char *err, size_t errcap);

/* Map headers and sections into `g`, then apply DIR64/HIGHLOW relocations for
 * the difference between the image's preferred base and g->guest_base. */
int wse2_pe_map(const uint8_t *file, size_t len, const GuestSpace *g,
                Wse2PeImage *out, uint32_t *relocs_applied,
                char *err, size_t errcap);
