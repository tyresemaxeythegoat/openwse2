/* Guest address-space reservation.
 *
 * The engine image wants 0x140000000. A 39-bit Android user VA layout can hold
 * that, and it is below the typical 47/48-bit PC layout too. If the preferred
 * range is unavailable the reservation still succeeds elsewhere and reports
 * `rebased`; the loader then applies base relocations.
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t *base;       /* host pointer to the start of the reservation */
    uint64_t guest_base; /* guest VA that `base` represents */
    uint64_t size;
    int rebased;         /* nonzero if guest_base != requested base */
} GuestSpace;

int wse2_guest_reserve(GuestSpace *g, uint64_t preferred, uint64_t size);
void wse2_guest_release(GuestSpace *g);

/* Point the CPU memory layer at this reservation. */
void wse2_guest_attach(const GuestSpace *g);
