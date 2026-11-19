// Win64 structured exception handling for translated code (Phase 5 R3, `docs/RECOMP_PLAN.md`
// "R3 design"): the part of Windows under the exe's own C++ exception logic. The exe's
// `__CxxFrameHandler3` (and the rest of its statically linked vcruntime) is translated like any
// other code; this runtime provides what it imports from KERNEL32:
//
//   RaiseException          phase 1: walk guest frames, call each frame's exception handler
//   RtlUnwindEx             phase 2: call termination handlers up to the target frame, run an
//                           unwind-consolidation callback (how MSVC calls catch blocks), resume
//   RtlUnwind               the same without a context record (R4: `lua5164`)
//   RtlRestoreContext       resume at a CONTEXT; STATUS_LONGJUMP takes the nonvolatile registers
//                           from a jump buffer, STATUS_UNWIND_CONSOLIDATE runs the callback first
//   RtlVirtualUnwind, RtlLookupFunctionEntry, RtlCaptureContext   over every translated image's
//                           .pdata/.xdata (the exe, then each DLL added with `add_image`, R4)
//
// Exceptions raised while one is being handled follow Windows: a handler that raises makes a
// nested exception (EXCEPTION_NESTED_CALL until the walk passes the frame whose handler raised); a
// raise or unwind from a termination handler collides with that unwind (the frame being unwound
// is examined again with its ScopeIndex, EXCEPTION_COLLIDED_UNWIND in phase 2); a rethrow from a
// catch block (an unwind-consolidation callback) walks on into the catch's frame (`Boundary`).
//
// Resuming a frame does not use host exceptions or setjmp: the runtime writes the target frame's
// context into `CpuState`, sets `unwind_rsp`, and returns; translated frames return through
// ordinary returns until the one whose call-site `rsp` matches resumes (`lift_r1.cpp`
// `guest_call`). Structure layouts are the documented x64 ones (winnt.h). Header-only.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/unwind.h"

namespace wse::recomp::seh {

// EXCEPTION_RECORD (x64).
namespace er {
inline constexpr std::size_t code = 0, flags = 4, record = 8, address = 16, nparams = 24, info = 32, size = 152;
}
// CONTEXT (x64, 16-aligned, 0x4D0 bytes).
namespace ctxo {
inline constexpr std::size_t flags = 0x30, mxcsr = 0x34, eflags = 0x44, gpr = 0x78, rip = 0xF8, xmm = 0x1A0,
                             size = 0x4D0;
}
// DISPATCHER_CONTEXT (x64).
namespace dco {
inline constexpr std::size_t control_pc = 0, image_base = 8, function_entry = 16, establisher = 24, target_ip = 32,
                             context = 40, language_handler = 48, handler_data = 56, history = 64, scope = 72,
                             size = 80;
}

inline constexpr std::uint32_t kNoncontinuable = 1;
inline constexpr std::uint32_t kUnwinding = 2;
inline constexpr std::uint32_t kExitUnwind = 4;
inline constexpr std::uint32_t kNestedCall = 0x10;      // EXCEPTION_NESTED_CALL
inline constexpr std::uint32_t kTargetUnwind = 0x20;
inline constexpr std::uint32_t kCollidedUnwind = 0x40;  // EXCEPTION_COLLIDED_UNWIND
inline constexpr std::uint32_t kStatusUnwind = 0xC0000027U;
inline constexpr std::uint32_t kStatusUnwindConsolidate = 0x80000029U;
inline constexpr std::uint32_t kStatusLongJump = 0x80000026U;
inline constexpr std::uint32_t kContextFull = 0x10000B;
inline constexpr std::uint32_t kContinueExecution = 0;
inline constexpr std::uint32_t kContinueSearch = 1;

namespace detail {

template <typename T>
inline void put(std::uint64_t va, T v) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &v, sizeof(T));
}
template <typename T>
inline T get(std::uint64_t va) noexcept {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(va), sizeof(T));
    return v;
}

inline std::uint32_t eflags_of(const CpuState& s) noexcept {
    return 0x202U | (s.cf ? 1U : 0U) | (s.pf ? 4U : 0U) | (s.af ? 0x10U : 0U) | (s.zf ? 0x40U : 0U) |
           (s.sf ? 0x80U : 0U) | (s.df ? 0x400U : 0U) | (s.of ? 0x800U : 0U);
}

inline void write_context(std::uint64_t va, const CpuState& s) noexcept {
    std::memset(reinterpret_cast<void*>(va), 0, ctxo::size);
    put<std::uint32_t>(va + ctxo::flags, kContextFull);
    put<std::uint32_t>(va + ctxo::mxcsr, s.mxcsr);
    put<std::uint32_t>(va + ctxo::eflags, eflags_of(s));
    std::memcpy(reinterpret_cast<void*>(va + ctxo::gpr), s.gpr, sizeof(s.gpr));
    put<std::uint64_t>(va + ctxo::rip, s.rip);
    std::memcpy(reinterpret_cast<void*>(va + ctxo::xmm), s.xmm, sizeof(s.xmm));
    put<std::uint32_t>(va + 0x100 + 0x18, s.mxcsr);  // FltSave.MxCsr
}

inline void read_context(std::uint64_t va, CpuState& s) noexcept {
    std::memcpy(s.gpr, reinterpret_cast<const void*>(va + ctxo::gpr), sizeof(s.gpr));
    s.rip = get<std::uint64_t>(va + ctxo::rip);
    std::memcpy(s.xmm, reinterpret_cast<const void*>(va + ctxo::xmm), sizeof(s.xmm));
    s.mxcsr = get<std::uint32_t>(va + ctxo::mxcsr);
    const std::uint32_t f = get<std::uint32_t>(va + ctxo::eflags);
    s.cf = (f & 1U) != 0;
    s.pf = (f & 4U) != 0;
    s.af = (f & 0x10U) != 0;
    s.zf = (f & 0x40U) != 0;
    s.sf = (f & 0x80U) != 0;
    s.df = (f & 0x400U) != 0;
    s.of = (f & 0x800U) != 0;
}

// The context of a shim's caller as if the shim had returned (`rip` = return address).
inline CpuState caller_context(const CpuState& s) noexcept {
    CpuState c = s;
    c.rip = get<std::uint64_t>(s.gpr[4]);
    c.gpr[4] = s.gpr[4] + 8;
    return c;
}

}  // namespace detail

class Exceptions {
public:
    Exceptions(Runtime& rt, const PeImage& img, const std::uint8_t* image) : rt_(rt) { add_image(img, image); }

    // Adds another translated image mapped at `img.image_base` (R4: a DLL). Frames are looked up in
    // the image that contains their `rip`. Call before guest code runs.
    void add_image(const PeImage& img, const std::uint8_t* image) {
        auto m = std::make_unique<Image>();
        m->pe = img;
        m->u = unwind::Image{image, img.size_of_image, img.image_base, &m->pe.functions};
        images_.push_back(std::move(m));
    }

    // Binds the shims to the exe's imports (by name; KERNEL32 or ntdll spellings).
    std::size_t install() {
        std::size_t n = 0;
        for (const char* dll : {"KERNEL32.dll", "ntdll.dll"}) {
            n += rt_.set_import(dll, "RaiseException", [this](CpuState& s) { raise(s); });
            n += rt_.set_import(dll, "RtlUnwindEx", [this](CpuState& s) { unwind_ex(s, true); });
            n += rt_.set_import(dll, "RtlUnwind", [this](CpuState& s) { unwind_ex(s, false); });
            n += rt_.set_import(dll, "RtlRestoreContext", [this](CpuState& s) { restore_context(s); });
            n += rt_.set_import(dll, "RtlVirtualUnwind", [this](CpuState& s) { virtual_unwind(s); });
            n += rt_.set_import(dll, "RtlLookupFunctionEntry", [this](CpuState& s) { lookup(s); });
            n += rt_.set_import(dll, "RtlCaptureContext", [this](CpuState& s) { capture(s); });
        }
        return n;
    }

    // Unhandled exceptions are reported here (code, address); the default faults the runtime.
    std::uint32_t last_unhandled_code = 0;

private:
    // Scratch structures go below the guest `rsp`, like a native dispatcher's frame.
    struct Scratch {
        std::uint64_t context = 0;
        std::uint64_t record = 0;
        std::uint64_t dispatcher = 0;
        std::uint64_t rsp = 0;  // where calls made from here may start
    };
    static Scratch carve(std::uint64_t rsp) noexcept {
        Scratch sc;
        std::uint64_t p = (rsp - 0x200) & ~std::uint64_t{15};
        p -= ctxo::size;
        p &= ~std::uint64_t{15};
        sc.context = p;
        p -= er::size;
        p &= ~std::uint64_t{15};
        sc.record = p;
        p -= dco::size;
        p &= ~std::uint64_t{15};
        sc.dispatcher = p;
        sc.rsp = p - 0x40;
        return sc;
    }

    struct Image {
        PeImage pe;
        unwind::Image u;
    };
    // The image containing `pc`; the first image when none does (its lookup then finds no
    // function, and the frame unwinds as a leaf).
    const Image& image_for(std::uint64_t pc) const noexcept {
        for (const auto& m : images_) {
            if (pc >= m->u.base && pc - m->u.base < m->u.size) {
                return *m;
            }
        }
        return *images_.front();
    }
    std::uint64_t function_entry_va(std::uint64_t pc) const noexcept {
        const Image& m = image_for(pc);
        const PeFunctionRange* f = unwind::lookup_function(m.u, pc);
        return f != nullptr && f->entry != 0 ? m.pe.image_base + f->entry : 0;
    }

    // Calls a language handler (`EXCEPTION_DISPOSITION h(record, frame, context, dispatcher)`).
    std::uint32_t call_handler(CpuState& s, std::uint64_t handler, std::uint64_t record, std::uint64_t frame,
                               std::uint64_t context, std::uint64_t dispatcher, std::uint64_t below, bool* ok) {
        const std::uint64_t rsp = s.gpr[4];
        s.gpr[4] = below;
        const std::uint64_t r = rt_.call_guest(s, handler, {record, frame, context, dispatcher}, ok);
        if (s.unwind_rsp == 0) {
            s.gpr[4] = rsp;
        }
        return static_cast<std::uint32_t>(r);
    }

    void fill_dispatcher(std::uint64_t dc, const CpuState& frame_ctx, const unwind::Result& r, std::uint64_t context,
                         std::uint64_t target_ip) const noexcept {
        std::memset(reinterpret_cast<void*>(dc), 0, dco::size);
        detail::put<std::uint64_t>(dc + dco::control_pc, frame_ctx.rip);
        detail::put<std::uint64_t>(dc + dco::image_base, image_for(frame_ctx.rip).pe.image_base);
        detail::put<std::uint64_t>(dc + dco::function_entry, function_entry_va(frame_ctx.rip));
        detail::put<std::uint64_t>(dc + dco::establisher, r.establisher_frame);
        detail::put<std::uint64_t>(dc + dco::target_ip, target_ip);
        detail::put<std::uint64_t>(dc + dco::context, context);
        detail::put<std::uint64_t>(dc + dco::language_handler, r.handler);
        detail::put<std::uint64_t>(dc + dco::handler_data, r.handler_data);
    }

    // A host-to-guest call this runtime is making on the current thread (innermost last). A frame
    // walk that reaches `Runtime::kCallbackReturn` has unwound out of that call and crosses into
    // the frames Windows would find there (its RtlpExecuteHandlerFor* / consolidation frames):
    //   dispatch     a phase-1 language handler: continue at that raise's thrower; a phase-1 walk
    //                is a nested exception (EXCEPTION_NESTED_CALL up to the handler's frame)
    //   unwind       a termination handler: a collided unwind; continue at the frame being
    //                unwound, whose handler runs again with its saved ScopeIndex
    //   consolidate  an unwind-consolidation callback (a C++ catch block): continue at the
    //                target frame, which the finished unwind left
    struct Boundary {
        enum class Kind { dispatch, unwind, consolidate } kind = Kind::dispatch;
        CpuState next;                  // where the walk continues
        std::uint64_t establisher = 0;  // dispatch / unwind: the frame whose handler is running
        std::uint64_t dispatcher = 0;   // unwind: its DISPATCHER_CONTEXT (ScopeIndex)
    };
    static std::vector<Boundary>& boundaries() {
        thread_local std::vector<Boundary> stack;  // guest threads run on their own host threads
        return stack;
    }
    struct Pushed {
        explicit Pushed(const Boundary& b) { boundaries().push_back(b); }
        ~Pushed() { boundaries().pop_back(); }
        Pushed(const Pushed&) = delete;
        Pushed& operator=(const Pushed&) = delete;
    };

    // One frame walk (phase 1 or 2) over guest frames and the boundaries below it.
    struct Walk {
        CpuState ctx;
        std::size_t level = 0;            // boundaries not yet crossed: [0, level)
        std::uint32_t flags = 0;          // EXCEPTION_NESTED_CALL / EXCEPTION_COLLIDED_UNWIND added here
        std::uint64_t nested_frame = 0;   // phase 1: the nested region ends at this establisher
        std::uint64_t scope_frame = 0;    // a collided frame: restore its ScopeIndex once
        std::uint32_t scope = 0;
    };

    // Advances `w` to the next guest frame: crosses boundaries, then unwinds one frame (`frame` is
    // the context in that frame, `r` its unwind result). False when the walk ends.
    bool next_frame(Walk& w, std::uint8_t mask, bool phase1, CpuState& frame, unwind::Result& r) {
        while (w.ctx.rip == Runtime::kCallbackReturn) {
            if (w.level == 0) {
                return false;
            }
            const Boundary& b = boundaries()[--w.level];
            w.ctx = b.next;
            if (b.kind == Boundary::Kind::dispatch && phase1) {
                w.flags |= kNestedCall;
                w.nested_frame = std::max(w.nested_frame, b.establisher);
            } else if (b.kind == Boundary::Kind::unwind) {
                if (!phase1) {
                    w.flags |= kCollidedUnwind;
                }
                w.scope_frame = b.establisher;
                w.scope = detail::get<std::uint32_t>(b.dispatcher + dco::scope);
            }
        }
        frame = w.ctx;
        r = unwind::virtual_unwind(image_for(w.ctx.rip).u, w.ctx, mask);
        return r.ok && w.ctx.rip != 0;
    }
    // Fills the dispatcher context for `frame`, restoring a collided frame's ScopeIndex.
    void prepare_call(Walk& w, std::uint64_t dc, const CpuState& frame, const unwind::Result& r, std::uint64_t context,
                      std::uint64_t target_ip) {
        fill_dispatcher(dc, frame, r, context, target_ip);
        if (w.scope_frame != 0 && r.establisher_frame == w.scope_frame) {
            detail::put<std::uint32_t>(dc + dco::scope, w.scope);
            w.scope_frame = 0;
        }
    }

    // RaiseException(code, flags, nargs, args): phase 1.
    void raise(CpuState& s) {
        const auto code = static_cast<std::uint32_t>(win64_arg(s, 0));
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 1)) & kNoncontinuable;
        const std::uint32_t nargs = std::min<std::uint32_t>(static_cast<std::uint32_t>(win64_arg(s, 2)), 15);
        const std::uint64_t args = win64_arg(s, 3);
        const CpuState thrower = detail::caller_context(s);
        const Scratch sc = carve(s.gpr[4]);
        std::memset(reinterpret_cast<void*>(sc.record), 0, er::size);
        detail::put<std::uint32_t>(sc.record + er::code, code);
        detail::put<std::uint32_t>(sc.record + er::flags, flags);
        detail::put<std::uint64_t>(sc.record + er::address, thrower.rip);
        detail::put<std::uint32_t>(sc.record + er::nparams, args != 0 ? nargs : 0);
        if (args != 0) {
            std::memcpy(reinterpret_cast<void*>(sc.record + er::info), reinterpret_cast<const void*>(args), 8ULL * nargs);
        }
        detail::write_context(sc.context, thrower);
        Walk w;
        w.ctx = thrower;
        w.level = boundaries().size();
        for (int depth = 0; depth < 4096; ++depth) {
            CpuState frame_ctx;
            unwind::Result r;
            if (!next_frame(w, unwind::kFlagExceptionHandler, true, frame_ctx, r)) {
                break;
            }
            if (r.handler != 0) {
                detail::put<std::uint32_t>(sc.record + er::flags, flags | w.flags);
                prepare_call(w, sc.dispatcher, frame_ctx, r, sc.context, 0);
                bool ok = true;
                std::uint32_t disp = kContinueSearch;
                {
                    Boundary b;
                    b.kind = Boundary::Kind::dispatch;
                    b.next = thrower;
                    b.establisher = r.establisher_frame;
                    const Pushed pushed(b);
                    disp = call_handler(s, r.handler, sc.record, r.establisher_frame, sc.context, sc.dispatcher, sc.rsp,
                                        &ok);
                }
                if (s.unwind_rsp != 0) {
                    return;  // a handler unwound to a target frame (or the thread is exiting)
                }
                if (disp == kContinueExecution && (flags & kNoncontinuable) == 0) {
                    detail::read_context(sc.context, s);  // resume where the handler left the context
                    return;
                }
            }
            // The end of a nested region: the frame whose handler raised has been examined.
            if (w.nested_frame != 0 && r.establisher_frame == w.nested_frame) {
                w.flags &= ~kNestedCall;
                w.nested_frame = 0;
            }
        }
        last_unhandled_code = code;
        s.hooks->unsupported(&s, thrower.rip, "unhandled guest exception");
    }

    // RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, ContextRecord, HistoryTable);
    // RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) when `has_context` is false.
    void unwind_ex(CpuState& s, bool has_context) {
        const std::uint64_t target_frame = win64_arg(s, 0);
        const std::uint64_t target_ip = win64_arg(s, 1);
        std::uint64_t record = win64_arg(s, 2);
        const std::uint64_t return_value = win64_arg(s, 3);
        const std::uint64_t context_arg = has_context ? win64_arg(s, 4) : 0;
        const Scratch sc = carve(s.gpr[4]);
        if (record == 0) {
            record = sc.record;
            std::memset(reinterpret_cast<void*>(record), 0, er::size);
            detail::put<std::uint32_t>(record + er::code, kStatusUnwind);
            detail::put<std::uint64_t>(record + er::address, detail::caller_context(s).rip);
        }
        std::uint32_t flags = detail::get<std::uint32_t>(record + er::flags) | kUnwinding;
        if (target_frame == 0) {
            flags |= kExitUnwind;
        }
        const std::uint64_t context = context_arg != 0 ? context_arg : sc.context;
        Walk w;
        w.ctx = detail::caller_context(s);
        w.level = boundaries().size();
        CpuState target_ctx;
        bool found = false;
        std::string walk;  // for the "not found" report: rip/establisher of each frame
        for (int depth = 0; depth < 4096; ++depth) {
            CpuState frame_ctx;
            unwind::Result r;
            const bool stepped = next_frame(w, unwind::kFlagTerminationHandler, false, frame_ctx, r);
            if (walk.size() < 600) {
                char line[96];
                std::snprintf(line, sizeof(line), " [rip %llx est %llx -> %llx level %zu%s]",
                              static_cast<unsigned long long>(frame_ctx.rip),
                              static_cast<unsigned long long>(r.establisher_frame),
                              static_cast<unsigned long long>(w.ctx.rip), w.level, r.ok ? "" : " !ok");
                walk += line;
            }
            if (!stepped) {
                break;
            }
            const bool at_target = r.establisher_frame == target_frame;
            if (r.handler != 0) {
                detail::put<std::uint32_t>(record + er::flags, flags | w.flags | (at_target ? kTargetUnwind : 0));
                prepare_call(w, sc.dispatcher, frame_ctx, r, context, target_ip);
                bool ok = true;
                {
                    Boundary b;
                    b.kind = Boundary::Kind::unwind;
                    b.next = frame_ctx;
                    b.establisher = r.establisher_frame;
                    b.dispatcher = sc.dispatcher;
                    const Pushed pushed(b);
                    (void)call_handler(s, r.handler, record, r.establisher_frame, context, sc.dispatcher, sc.rsp, &ok);
                }
                if (s.unwind_rsp != 0) {
                    return;  // a nested or collided unwind took over (or the thread is exiting)
                }
            }
            if (at_target) {
                target_ctx = frame_ctx;
                found = true;
                break;
            }
        }
        detail::put<std::uint32_t>(record + er::flags, flags);
        if (!found) {
            char head[64];
            std::snprintf(head, sizeof(head), "RtlUnwindEx: target frame %llx not found:",
                          static_cast<unsigned long long>(target_frame));
            const std::string what = head + walk;
            s.hooks->unsupported(&s, detail::caller_context(s).rip, what.c_str());
            return;
        }
        std::uint64_t resume_ip = target_ip;
        if (detail::get<std::uint32_t>(record + er::code) == kStatusUnwindConsolidate) {
            // Unwind consolidation: the callback in ExceptionInformation[0] runs on the target
            // frame's stack and returns the address to resume at (MSVC: the catch block, then
            // its continuation).
            const std::uint64_t callback = detail::get<std::uint64_t>(record + er::info);
            CpuState cs = target_ctx;
            cs.hooks = s.hooks;
            cs.teb = s.teb;
            bool ok = true;
            {
                // A rethrow from the catch block walks on into the target frame.
                Boundary b;
                b.kind = Boundary::Kind::consolidate;
                b.next = target_ctx;
                const Pushed pushed(b);
                resume_ip = rt_.call_guest(cs, callback, {record}, &ok);
            }
            if (cs.unwind_rsp != 0) {
                s = cs;  // the callback started another unwind
                return;
            }
            target_ctx.gpr[0] = resume_ip;
        } else {
            target_ctx.gpr[0] = return_value;
        }
        target_ctx.rip = resume_ip;
        target_ctx.hooks = s.hooks;
        const std::uint64_t site_rsp = target_ctx.gpr[4];
        s = target_ctx;
        s.unwind_rsp = site_rsp;
    }

    // RtlVirtualUnwind(HandlerType, ImageBase, ControlPc, FunctionEntry, ContextRecord,
    //                  *HandlerData, *EstablisherFrame, ContextPointers) -> handler.
    void virtual_unwind(CpuState& s) {
        const auto type = static_cast<std::uint8_t>(win64_arg(s, 0));
        const std::uint64_t pc = win64_arg(s, 2);
        const std::uint64_t context = win64_arg(s, 4);
        const std::uint64_t handler_data = win64_arg(s, 5);
        const std::uint64_t establisher = win64_arg(s, 6);
        CpuState c;
        detail::read_context(context, c);
        c.rip = pc;
        const unwind::Result r = unwind::virtual_unwind(image_for(pc).u, c, type);
        std::memcpy(reinterpret_cast<void*>(context + ctxo::gpr), c.gpr, sizeof(c.gpr));
        detail::put<std::uint64_t>(context + ctxo::rip, c.rip);
        std::memcpy(reinterpret_cast<void*>(context + ctxo::xmm), c.xmm, sizeof(c.xmm));
        if (handler_data != 0) {
            detail::put<std::uint64_t>(handler_data, r.handler_data);
        }
        if (establisher != 0) {
            detail::put<std::uint64_t>(establisher, r.establisher_frame);
        }
        win64_return(s, r.handler);
    }

    // RtlLookupFunctionEntry(ControlPc, *ImageBase, HistoryTable) -> RUNTIME_FUNCTION*.
    void lookup(CpuState& s) {
        const std::uint64_t pc = win64_arg(s, 0);
        const std::uint64_t base_out = win64_arg(s, 1);
        const std::uint64_t entry = function_entry_va(pc);
        if (base_out != 0) {
            detail::put<std::uint64_t>(base_out, entry != 0 ? image_for(pc).pe.image_base : 0);
        }
        win64_return(s, entry);
    }

    // RtlRestoreContext(ContextRecord, ExceptionRecord): continues at the context. With
    // STATUS_LONGJUMP the nonvolatile registers, rsp and rip come from the _JUMP_BUFFER in
    // ExceptionInformation[0] (how MSVC's longjmp ends); with STATUS_UNWIND_CONSOLIDATE the callback
    // in ExceptionInformation[0] runs on the context's stack and returns where to continue. Like
    // RtlUnwindEx, the target frame resumes through `unwind_rsp`.
    void restore_context(CpuState& s) {
        const std::uint64_t context = win64_arg(s, 0);
        const std::uint64_t record = win64_arg(s, 1);
        CpuState c = s;
        detail::read_context(context, c);
        const std::uint32_t code = record != 0 ? detail::get<std::uint32_t>(record + er::code) : 0;
        if (code == kStatusLongJump) {
            // _JUMP_BUFFER (x64): Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip, MxCsr, FpCsr, Spare,
            // Xmm6-Xmm15.
            const std::uint64_t jb = detail::get<std::uint64_t>(record + er::info);
            static constexpr int kRegs[] = {3, 4, 5, 6, 7, 12, 13, 14, 15};
            for (std::size_t i = 0; i < sizeof(kRegs) / sizeof(kRegs[0]); ++i) {
                c.gpr[kRegs[i]] = detail::get<std::uint64_t>(jb + 8 + 8 * i);
            }
            c.rip = detail::get<std::uint64_t>(jb + 0x50);
            c.mxcsr = detail::get<std::uint32_t>(jb + 0x58);
            std::memcpy(c.xmm[6], reinterpret_cast<const void*>(jb + 0x60), 10 * 16);
        } else if (code == kStatusUnwindConsolidate) {
            const std::uint64_t callback = detail::get<std::uint64_t>(record + er::info);
            CpuState cs = c;
            bool ok = true;
            Boundary b;
            b.kind = Boundary::Kind::consolidate;
            b.next = c;
            {
                const Pushed pushed(b);
                c.rip = rt_.call_guest(cs, callback, {record}, &ok);
            }
            if (cs.unwind_rsp != 0) {
                s = cs;  // the callback started another unwind
                return;
            }
        }
        c.hooks = s.hooks;
        c.teb = s.teb;
        s = c;
        s.unwind_rsp = c.gpr[4];
    }

    // RtlCaptureContext(ContextRecord): the caller's context as if this call had returned.
    void capture(CpuState& s) {
        detail::write_context(win64_arg(s, 0), detail::caller_context(s));
        win64_return(s, 0);
    }

    Runtime& rt_;
    std::vector<std::unique_ptr<Image>> images_;
};

}  // namespace wse::recomp::seh
