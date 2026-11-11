/* Win32 shim registry.
 *
 * Imports are bound by name against this table. Unimplemented imports still get
 * a thunk address; calling one aborts with the DLL!function name rather than
 * running with fabricated semantics.
 */
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef uint32_t (*Wse2Fn)(CPU *c);

Wse2Fn wse2_shim_lookup(const char *dll, const char *name);
void wse2_shim_set_image_base(uint64_t base);
uint64_t wse2_shim_image_base(void);
