#include "pe.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_FILE_MACHINE_AMD64 0x8664u
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20Bu
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGHLOW 3
#define IMAGE_REL_BASED_DIR64 10

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static int fail(char *err, size_t errcap, const char *fmt, ...) {
    if (err && errcap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errcap, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int wse2_pe_parse(const uint8_t *file, size_t len, Wse2PeImage *out,
                  char *err, size_t errcap) {
    memset(out, 0, sizeof *out);

    if (len < 0x40 || file[0] != 'M' || file[1] != 'Z') {
        return fail(err, errcap, "not an MZ image");
    }
    uint32_t pe_off = rd32(file + 0x3C);
    if (pe_off > len || len - pe_off < 24) {
        return fail(err, errcap, "e_lfanew out of range");
    }
    const uint8_t *pe = file + pe_off;
    if (rd32(pe) != 0x00004550u) {
        return fail(err, errcap, "missing PE signature");
    }
    out->machine = rd16(pe + 4);
    if (out->machine != IMAGE_FILE_MACHINE_AMD64) {
        return fail(err, errcap, "machine 0x%X is not x86-64", out->machine);
    }
    out->section_count = rd16(pe + 6);
    uint16_t opt_size = rd16(pe + 20);
    if (out->section_count == 0 || out->section_count > 32) {
        return fail(err, errcap, "bad section count %u", out->section_count);
    }

    uint32_t opt_off = pe_off + 24;
    if (opt_off > len || len - opt_off < opt_size || opt_size < 0x70) {
        return fail(err, errcap, "optional header out of range");
    }
    const uint8_t *opt = file + opt_off;
    if (rd16(opt) != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return fail(err, errcap, "not PE32+ (magic 0x%X)", rd16(opt));
    }
    out->entry_rva = rd32(opt + 0x10);
    out->image_base = rd64(opt + 0x18);
    out->size_of_image = rd32(opt + 0x38);
    out->size_of_headers = rd32(opt + 0x3C);
    if (out->size_of_image == 0 || out->size_of_headers > len ||
        out->size_of_headers > out->size_of_image) {
        return fail(err, errcap, "bad image/header size");
    }
    if (out->entry_rva >= out->size_of_image) {
        return fail(err, errcap, "entry RVA outside image");
    }

    uint32_t nrva = rd32(opt + 0x6C);
    const uint8_t *dirs = opt + 0x70;
    if (nrva > 16) nrva = 16;
    if (opt_size < 0x70 + nrva * 8) {
        return fail(err, errcap, "data directories exceed optional header");
    }
    if (nrva > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        out->import_rva = rd32(dirs + IMAGE_DIRECTORY_ENTRY_IMPORT * 8);
        out->import_size = rd32(dirs + IMAGE_DIRECTORY_ENTRY_IMPORT * 8 + 4);
        out->reloc_rva = rd32(dirs + IMAGE_DIRECTORY_ENTRY_BASERELOC * 8);
        out->reloc_size = rd32(dirs + IMAGE_DIRECTORY_ENTRY_BASERELOC * 8 + 4);
    }
    if (out->import_size &&
        (out->import_rva > out->size_of_image ||
         out->import_size > out->size_of_image - out->import_rva)) {
        return fail(err, errcap, "import directory outside image");
    }
    if (out->reloc_size &&
        (out->reloc_rva > out->size_of_image ||
         out->reloc_size > out->size_of_image - out->reloc_rva)) {
        return fail(err, errcap, "reloc directory outside image");
    }

    uint32_t sec_off = opt_off + opt_size;
    if (sec_off > len || (uint64_t)out->section_count * 40 > len - sec_off) {
        return fail(err, errcap, "section table out of range");
    }
    for (uint16_t i = 0; i < out->section_count; i++) {
        const uint8_t *s = file + sec_off + (uint32_t)i * 40;
        Wse2PeSection *d = &out->sections[i];
        memcpy(d->name, s, 8);
        d->name[8] = 0;
        d->vsize = rd32(s + 8);
        d->rva = rd32(s + 12);
        d->raw_size = rd32(s + 16);
        d->raw_ptr = rd32(s + 20);
        d->characteristics = rd32(s + 36);
        if (d->vsize && (d->rva > out->size_of_image ||
                         d->vsize > out->size_of_image - d->rva)) {
            return fail(err, errcap, "section %s outside image", d->name);
        }
        if (d->raw_size && (d->raw_ptr > len || d->raw_size > len - d->raw_ptr)) {
            return fail(err, errcap, "section %s raw data outside file", d->name);
        }
    }
    return 0;
}

int wse2_pe_map(const uint8_t *file, size_t len, const GuestSpace *g,
                Wse2PeImage *out, uint32_t *relocs_applied,
                char *err, size_t errcap) {
    if (relocs_applied) *relocs_applied = 0;
    if (wse2_pe_parse(file, len, out, err, errcap) != 0) {
        return -1;
    }
    if (g->size < out->size_of_image) {
        return fail(err, errcap, "guest reservation too small (%llu < %u)",
                    (unsigned long long)g->size, out->size_of_image);
    }

    uint8_t *img = g->base;
    memset(img, 0, out->size_of_image);
    memcpy(img, file, out->size_of_headers);

    for (uint16_t i = 0; i < out->section_count; i++) {
        const Wse2PeSection *s = &out->sections[i];
        uint32_t n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        if (n) {
            memcpy(img + s->rva, file + s->raw_ptr, n);
        }
    }

    int64_t delta = (int64_t)(g->guest_base - out->image_base);
    uint32_t applied = 0;
    if (out->reloc_size) {
        const uint8_t *rel = img + out->reloc_rva;
        uint32_t off = 0;
        while (off + 8 <= out->reloc_size) {
            uint32_t page = rd32(rel + off);
            uint32_t block = rd32(rel + off + 4);
            if (block < 8 || (block & 1) || block > out->reloc_size - off) {
                return fail(err, errcap, "bad relocation block at rva 0x%X", out->reloc_rva + off);
            }
            for (uint32_t e = 8; e + 2 <= block; e += 2) {
                uint16_t ent = rd16(rel + off + e);
                uint32_t type = ent >> 12;
                uint32_t target = page + (ent & 0xFFF);
                if (type == IMAGE_REL_BASED_ABSOLUTE) {
                    continue;
                }
                if (type == IMAGE_REL_BASED_DIR64) {
                    if (target > out->size_of_image ||
                        out->size_of_image - target < 8) {
                        return fail(err, errcap, "DIR64 reloc outside image");
                    }
                    wr64(img + target, rd64(img + target) + (uint64_t)delta);
                    applied++;
                } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    if (target > out->size_of_image ||
                        out->size_of_image - target < 4) {
                        return fail(err, errcap, "HIGHLOW reloc outside image");
                    }
                    wr32(img + target, (uint32_t)(rd32(img + target) + (uint32_t)delta));
                    applied++;
                } else {
                    return fail(err, errcap, "unsupported relocation type %u", type);
                }
            }
            off += block;
        }
        if (off != out->reloc_size) {
            return fail(err, errcap, "trailing bytes in relocation directory");
        }
    }
    if (relocs_applied) *relocs_applied = applied;
    return 0;
}
