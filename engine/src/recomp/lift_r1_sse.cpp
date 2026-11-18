// R1 sse family: SSE/SSE2/SSE3/SSSE3/SSE4.1 scalar and packed instructions.
// Conventions are in `lift_r1_ctx.h`.
//
// Not written yet: every instruction is reported as not handled here, so the core turns it into
// a call to the `unsupported` hook and the coverage meter counts it.

#include "lift_r1_ctx.h"

namespace wse::recomp::r1 {

Lifted lift_sse(Ctx& c, const DInsn& d) {
    (void)c;
    (void)d;
    return Lifted::not_mine;
}

}  // namespace wse::recomp::r1
