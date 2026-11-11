#!/usr/bin/env python3
"""Scoped x86-64 -> C lifter for the WSE2 proof.

Lifts one PE function, selected by RVA, into a C function `f_<rva>(CPU *c)`.
The output is deliberately strict: any instruction, operand, or control-flow
target outside the implemented subset aborts generation with the address and
mnemonic. No silent fallbacks.

This is a proof lifter, not the production catalog lifter. It exists to
validate the ABI, flag model, memory model, and generated-C shape on a real
WSE2 function before the catalog work starts.
"""
import argparse
import struct
import sys

import capstone
import pefile
from capstone import x86

# Absolute addresses emitted for RIP-relative data and pushed return addresses.
# 0 keeps the proof rig's flat-at-zero image layout. Set to 0x140000000 for the
# production runtime, which maps the image at its preferred base.
IMAGE_BASE = 0

# ---------------------------------------------------------------- registers

REGS = {
    "rax": 0, "rcx": 1, "rdx": 2, "rbx": 3, "rsp": 4, "rbp": 5, "rsi": 6, "rdi": 7,
    "r8": 8, "r9": 9, "r10": 10, "r11": 11, "r12": 12, "r13": 13, "r14": 14, "r15": 15,
}
R32 = {
    "eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esp": 4, "ebp": 5, "esi": 6, "edi": 7,
    "r8d": 8, "r9d": 9, "r10d": 10, "r11d": 11, "r12d": 12, "r13d": 13, "r14d": 14, "r15d": 15,
}
R16 = {
    "ax": 0, "cx": 1, "dx": 2, "bx": 3, "sp": 4, "bp": 5, "si": 6, "di": 7,
    "r8w": 8, "r9w": 9, "r10w": 10, "r11w": 11, "r12w": 12, "r13w": 13, "r14w": 14, "r15w": 15,
}
R8 = {
    "al": 0, "cl": 1, "dl": 2, "bl": 3, "spl": 4, "bpl": 5, "sil": 6, "dil": 7,
    "r8b": 8, "r9b": 9, "r10b": 10, "r11b": 11, "r12b": 12, "r13b": 13, "r14b": 14, "r15b": 15,
}
R8H = {"ah": 0, "ch": 1, "dh": 2, "bh": 3}


def reg_info(name):
    if name in REGS:
        return REGS[name], 8, False
    if name in R32:
        return R32[name], 4, False
    if name in R16:
        return R16[name], 2, False
    if name in R8:
        return R8[name], 1, False
    if name in R8H:
        return R8H[name], 1, True
    raise KeyError("register %s" % name)


def rreg(name):
    idx, size, high = reg_info(name)
    if size == 8:
        return "c->r[%d]" % idx
    if size == 4:
        return "(uint32_t)c->r[%d]" % idx
    if size == 2:
        return "(uint16_t)c->r[%d]" % idx
    if high:
        return "(uint8_t)(c->r[%d] >> 8)" % idx
    return "(uint8_t)c->r[%d]" % idx


def wreg(name, expr):
    idx, size, high = reg_info(name)
    if size == 8:
        return "c->r[%d] = (uint64_t)(%s);" % (idx, expr)
    if size == 4:
        return "c->r[%d] = (uint32_t)(%s);" % (idx, expr)
    if size == 2:
        return "c->r[%d] = (c->r[%d] & ~0xFFFFull) | (uint16_t)(%s);" % (idx, idx, expr)
    if high:
        return "c->r[%d] = (c->r[%d] & ~0xFF00ull) | ((uint64_t)(uint8_t)(%s) << 8);" % (idx, idx, expr)
    return "c->r[%d] = (c->r[%d] & ~0xFFull) | (uint8_t)(%s);" % (idx, idx, expr)


def xmm_idx(insn, op):
    """Return the XMM register index for an operand, or None."""
    if op.type != x86.X86_OP_REG:
        return None
    name = insn.reg_name(op.reg) or ""
    if name.startswith("xmm"):
        return int(name[3:])
    return None


def addr_expr(insn, op):
    m = op.mem
    # x86-64 keeps the FS/GS bases outside the GPR file. The WSE2 engine only
    # uses GS (the Win64 TEB); refuse anything else instead of silently
    # dropping the override and reading the wrong address.
    seg = insn.reg_name(m.segment) if m.segment else ""
    if seg and seg != "gs":
        raise SystemExit("segment override %s: unsupported at 0x%X" % (seg, insn.address))
    if m.base == x86.X86_REG_RIP:
        target = IMAGE_BASE + insn.address + insn.size + m.disp
        expr = "0x%Xull" % (target & 0xFFFFFFFFFFFFFFFF)
    else:
        parts = []
        if m.base:
            name = insn.reg_name(m.base)
            idx, _, _ = reg_info(name)
            parts.append("c->r[%d]" % idx)
        if m.index:
            name = insn.reg_name(m.index)
            idx, _, _ = reg_info(name)
            parts.append("c->r[%d] * %d" % (idx, m.scale))
        if m.disp or not parts:
            if m.disp < 0:
                parts.append("-0x%Xull" % (-m.disp))
            else:
                parts.append("0x%Xull" % m.disp)
        expr = "(" + " + ".join(parts) + ")"
    if seg == "gs":
        expr = "(c->teb + %s)" % expr
    return expr


def read_op(insn, op, size=None):
    if op.type == x86.X86_OP_IMM:
        v = op.imm & 0xFFFFFFFFFFFFFFFF
        n = size or op.size
        if n and n < 8:
            # Immediate operands are encoded at the operation width; x86
            # truncates (and for extended ops sign-extends) them there.
            v &= (1 << (n * 8)) - 1
        return "0x%Xull" % v
    if op.type == x86.X86_OP_REG:
        return rreg(insn.reg_name(op.reg))
    if op.type == x86.X86_OP_MEM:
        n = size or op.size
        return "ld%d(%s)" % (n * 8, addr_expr(insn, op))
    raise SystemExit("unsupported read operand type %d at 0x%X" % (op.type, insn.address))


def write_op(insn, op, size, expr):
    if op.type == x86.X86_OP_REG:
        return wreg(insn.reg_name(op.reg), expr)
    if op.type == x86.X86_OP_MEM:
        return "st%d(%s, %s);" % (size * 8, addr_expr(insn, op), expr)
    raise SystemExit("unsupported write operand type %d at 0x%X" % (op.type, insn.address))


def mask_for(bits):
    return "0xFFFFFFFFFFFFFFFFull" if bits == 64 else "0x%Xull" % ((1 << bits) - 1)


# ---------------------------------------------------------------- conditions

COND = {
    "je": "ZF(c)", "jz": "ZF(c)",
    "jne": "!ZF(c)", "jnz": "!ZF(c)",
    "jb": "CF(c)", "jnae": "CF(c)", "jc": "CF(c)",
    "jae": "!CF(c)", "jnb": "!CF(c)", "jnc": "!CF(c)",
    "jbe": "CF(c) || ZF(c)", "jna": "CF(c) || ZF(c)",
    "ja": "!CF(c) && !ZF(c)", "jnbe": "!CF(c) && !ZF(c)",
    "js": "SF(c)", "jns": "!SF(c)",
    "jo": "OF(c)", "jno": "!OF(c)",
    "jp": "PF(c)", "jpe": "PF(c)",
    "jnp": "!PF(c)", "jpo": "!PF(c)",
    "jl": "SF(c) != OF(c)", "jnge": "SF(c) != OF(c)",
    "jge": "SF(c) == OF(c)", "jnl": "SF(c) == OF(c)",
    "jle": "ZF(c) || (SF(c) != OF(c))", "jng": "ZF(c) || (SF(c) != OF(c))",
    "jg": "!ZF(c) && (SF(c) == OF(c))", "jnle": "!ZF(c) && (SF(c) == OF(c))",
}

SETCC = {
    "sete": "ZF(c)", "setz": "ZF(c)", "setne": "!ZF(c)", "setnz": "!ZF(c)",
    "setb": "CF(c)", "setc": "CF(c)", "setnae": "CF(c)",
    "setae": "!CF(c)", "setnb": "!CF(c)", "setnc": "!CF(c)",
    "setbe": "CF(c) || ZF(c)", "setna": "CF(c) || ZF(c)",
    "seta": "!CF(c) && !ZF(c)", "setnbe": "!CF(c) && !ZF(c)",
    "sets": "SF(c)", "setns": "!SF(c)",
    "setl": "SF(c) != OF(c)", "setnge": "SF(c) != OF(c)",
    "setge": "SF(c) == OF(c)", "setnl": "SF(c) == OF(c)",
    "setle": "ZF(c) || (SF(c) != OF(c))", "setng": "ZF(c) || (SF(c) != OF(c))",
    "setg": "!ZF(c) && (SF(c) == OF(c))", "setnle": "!ZF(c) && (SF(c) == OF(c))",
}

CMOV = {
    "cmove": "ZF(c)", "cmovz": "ZF(c)", "cmovne": "!ZF(c)", "cmovnz": "!ZF(c)",
    "cmovb": "CF(c)", "cmovc": "CF(c)", "cmovnae": "CF(c)",
    "cmovae": "!CF(c)", "cmovnb": "!CF(c)", "cmovnc": "!CF(c)",
    "cmovbe": "CF(c) || ZF(c)", "cmovna": "CF(c) || ZF(c)",
    "cmova": "!CF(c) && !ZF(c)", "cmovnbe": "!CF(c) && !ZF(c)",
    "cmovs": "SF(c)", "cmovns": "!SF(c)",
    "cmovl": "SF(c) != OF(c)", "cmovnge": "SF(c) != OF(c)",
    "cmovge": "SF(c) == OF(c)", "cmovnl": "SF(c) == OF(c)",
    "cmovle": "ZF(c) || (SF(c) != OF(c))", "cmovng": "ZF(c) || (SF(c) != OF(c))",
    "cmovg": "!ZF(c) && (SF(c) == OF(c))", "cmovnle": "!ZF(c) && (SF(c) == OF(c))",
}


class Lifter:
    def __init__(self, insns, start, end):
        self.insns = insns
        self.start = start
        self.end = end
        self.addrs = {i.address for i in insns}
        self.out = []
        self.unsupported = []

    def emit(self, s=""):
        self.out.append(s)

    def lift(self):
        self.emit("/* Generated by proof/lift.py. Do not hand-edit. */")
        self.emit("#include \"cpu.h\"")
        self.emit("")
        callees = sorted({
            i.operands[0].imm for i in self.insns
            if i.mnemonic == "call" and i.operands and i.operands[0].type == x86.X86_OP_IMM
        })
        for target in callees:
            self.emit("uint32_t f_%X(CPU *c);" % target)
        indirect = any(
            i.mnemonic == "call" and i.operands and i.operands[0].type != x86.X86_OP_IMM
            for i in self.insns
        )
        if indirect:
            self.emit("uint32_t wse2_dispatch_call(CPU *c, uint64_t guest_va);")
        if callees or indirect:
            self.emit("")
        self.emit("uint32_t f_%X(CPU *c)" % self.start)
        self.emit("{")
        for insn in self.insns:
            self.emit("L_%X: ;" % insn.address)
            # Never drop an instruction silently. A missing opcode must fail
            # the lift, otherwise the generated code looks valid but computes
            # the wrong thing.
            self.one(insn)
        self.emit("    return 0;")
        self.emit("}")
        return "\n".join(self.out) + "\n"

    def one(self, i):
        m = i.mnemonic
        ops = i.operands

        if m in ("nop", "endbr64", "endbr32", "pause"):
            self.emit("    /* %s */" % m)
            return
        if m in ("mov", "movabs"):
            dst, src = ops[0], ops[1]
            size = dst.size
            self.emit("    " + write_op(i, dst, size, read_op(i, src, size)))
            return
        if m == "movzx":
            dst, src = ops[0], ops[1]
            st = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[src.size]
            self.emit("    " + write_op(i, dst, dst.size, "(%s)%s" % (st, read_op(i, src, src.size))))
            return
        if m in ("movsx", "movsxd"):
            dst, src = ops[0], ops[1]
            st = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[src.size]
            self.emit("    " + write_op(i, dst, dst.size, "(uint64_t)(int64_t)(%s)%s" % (st, read_op(i, src, src.size))))
            return
        if m == "lea":
            dst, src = ops[0], ops[1]
            self.emit("    " + write_op(i, dst, dst.size, addr_expr(i, src)))
            return
        if m in ("add", "sub", "and", "or", "xor", "cmp", "test", "adc", "sbb"):
            dst, src = ops[0], ops[1]
            size = dst.size
            bits = size * 8
            a = read_op(i, dst, size)
            b = read_op(i, src, size)
            mask = mask_for(bits)
            if m in ("and", "or", "xor", "test"):
                op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
                expr = "((a_) %s (b_)) & %s" % (op, mask)
                self.emit("    { uint64_t a_ = %s, b_ = %s; uint64_t r_ = %s; flags_logic(c, r_, %d);%s }" % (
                    a, b, expr, bits, "" if m == "test" else " " + write_op(i, dst, size, "r_")))
            else:
                op = {"add": "+", "sub": "-", "cmp": "-", "adc": "+", "sbb": "-"}[m]
                ci = "(CF(c) ? 1u : 0u)" if m in ("adc", "sbb") else "0u"
                expr = "((a_ %s b_ + ci_)) & %s" % (op, mask) if m in ("adc",) else \
                       "((a_ %s b_ - ci_)) & %s" % (op, mask) if m in ("sbb",) else \
                       "((a_ %s b_)) & %s" % (op, mask)
                fn = "flags_add" if m in ("add", "adc") else "flags_sub"
                self.emit("    { uint64_t a_ = %s, b_ = %s, ci_ = %s; uint64_t r_ = %s; %s(c, a_, b_, ci_, r_, %d);%s }" % (
                    a, b, ci, expr, fn, bits, "" if m == "cmp" else " " + write_op(i, dst, size, "r_")))
            return
        if m in ("inc", "dec"):
            dst = ops[0]
            size = dst.size
            bits = size * 8
            mask = mask_for(bits)
            a = read_op(i, dst, size)
            op = "+" if m == "inc" else "-"
            fn = "flags_inc" if m == "inc" else "flags_dec"
            self.emit("    { uint64_t a_ = %s; uint64_t r_ = (a_ %s 1) & %s; %s(c, a_, r_, %d); %s }" % (
                a, op, mask, fn, bits, write_op(i, dst, size, "r_")))
            return
        if m in ("neg", "not"):
            dst = ops[0]
            size = dst.size
            bits = size * 8
            mask = mask_for(bits)
            a = read_op(i, dst, size)
            if m == "neg":
                self.emit("    { uint64_t a_ = %s; uint64_t r_ = (0 - a_) & %s; flags_sub(c, 0, a_, 0, r_, %d); %s }" % (
                    a, mask, bits, write_op(i, dst, size, "r_")))
            else:
                self.emit("    { uint64_t a_ = %s; uint64_t r_ = (~a_) & %s; %s }" % (
                    a, mask, write_op(i, dst, size, "r_")))
            return
        if m in ("shl", "sal", "shr", "sar"):
            dst, src = ops[0], ops[1]
            size = dst.size
            bits = size * 8
            mask = mask_for(bits)
            cnt = read_op(i, src, src.size)
            kind = 0 if m in ("shl", "sal") else (1 if m == "shr" else 2)
            cnt_mask = 63 if bits == 64 else 31
            if kind == 0:
                rexpr = "(a_ << cnt_) & %s" % mask
            elif kind == 1:
                rexpr = "(a_ >> cnt_) & %s" % mask
            else:
                rexpr = "sar_bits(a_, cnt_, %d)" % bits
            self.emit("    { unsigned cnt_ = (unsigned)(%s) & %d; uint64_t a_ = %s; "
                      "uint64_t r_ = %s; if (cnt_) flags_shift(c, a_, r_, cnt_, %d, %d); %s }" % (
                          cnt, cnt_mask, read_op(i, dst, size), rexpr,
                          bits, kind, write_op(i, dst, size, "r_")))
            return
        if m == "imul":
            if len(ops) == 1:
                raise SystemExit("one-operand imul unsupported at 0x%X" % i.address)
            dst = ops[0]
            size = dst.size
            bits = size * 8
            mask = mask_for(bits)
            if len(ops) == 2:
                a, b = read_op(i, dst, size), read_op(i, ops[1], size)
            else:
                a, b = read_op(i, ops[1], size), read_op(i, ops[2], size)
            self.emit("    { uint64_t a_ = %s, b_ = %s; uint64_t r_ = (a_ * b_) & %s; "
                      "flags_imul(c, a_, b_, r_, %d); %s }" % (
                          a, b, mask, bits, write_op(i, dst, size, "r_")))
            return
        if m == "bswap":
            dst = ops[0]
            size = dst.size
            fn = {4: "__builtin_bswap32", 8: "__builtin_bswap64"}[size]
            self.emit("    " + write_op(i, dst, size, "%s(%s)" % (fn, read_op(i, dst, size))))
            return
        if m in SETCC:
            self.emit("    " + write_op(i, ops[0], 1, "(%s ? 1 : 0)" % SETCC[m]))
            return
        if m in CMOV:
            dst, src = ops[0], ops[1]
            if dst.size == 4:
                # A 32-bit cmov writes EAX even when the condition is false,
                # which clears bits 32..63 of the full register.
                self.emit("    c->r[%d] = %s ? (uint32_t)(%s) : (uint32_t)c->r[%d];" % (
                    reg_info(i.reg_name(dst.reg))[0], CMOV[m],
                    read_op(i, src, 4), reg_info(i.reg_name(dst.reg))[0]))
            else:
                self.emit("    if (%s) { %s }" % (
                    CMOV[m], write_op(i, dst, dst.size, read_op(i, src, dst.size))))
            return
        if m in COND:
            target = ops[0].imm
            if target not in self.addrs:
                raise SystemExit("branch target 0x%X outside lifted function 0x%X..0x%X" % (target, self.start, self.end))
            self.emit("    if (%s) goto L_%X;" % (COND[m], target))
            return
        if m == "jmp":
            target = ops[0].imm
            if target not in self.addrs:
                raise SystemExit("jump target 0x%X outside lifted function 0x%X..0x%X" % (target, self.start, self.end))
            self.emit("    goto L_%X;" % target)
            return
        if m == "ret":
            self.emit("    c->rip = pop64(c); return 0;")
            return
        if m == "push":
            src = ops[0]
            self.emit("    { uint64_t v_ = %s; c->r[RSP] -= 8; st64(c->r[RSP], v_); }" % read_op(i, src, src.size))
            return
        if m == "pop":
            dst = ops[0]
            self.emit("    { uint64_t v_ = ld64(c->r[RSP]); c->r[RSP] += 8; %s }" % write_op(i, dst, dst.size, "v_"))
            return
        if m == "leave":
            self.emit("    c->r[RSP] = c->r[RBP]; c->r[RBP] = pop64(c);")
            return
        if m == "cdqe":
            self.emit("    c->r[RAX] = (uint64_t)(int64_t)(int32_t)c->r[RAX];")
            return
        if m == "cwde":
            self.emit("    c->r[RAX] = (uint32_t)(int32_t)(int16_t)c->r[RAX];")
            return
        if m == "cbw":
            self.emit("    c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | (uint16_t)(int16_t)(int8_t)c->r[RAX];")
            return
        if m == "xchg":
            a, b = ops[0], ops[1]
            if a.type != x86.X86_OP_REG or b.type != x86.X86_OP_REG:
                raise SystemExit("xchg memory unsupported at 0x%X" % i.address)
            an, bn = i.reg_name(a.reg), i.reg_name(b.reg)
            self.emit("    { uint64_t t_ = %s; %s %s }" % (
                rreg(an), wreg(an, rreg(bn)), wreg(bn, "t_")))
            return
        if m == "call":
            dst = ops[0]
            retaddr = i.address + i.size
            if dst.type == x86.X86_OP_IMM:
                self.emit("    { c->r[RSP] -= 8; st64(c->r[RSP], 0x%Xull); f_%X(c); }" % (
                    IMAGE_BASE + retaddr, dst.imm))
            else:
                target = read_op(i, dst, 8)
                self.emit("    { uint64_t t_ = %s; c->r[RSP] -= 8; st64(c->r[RSP], 0x%Xull); "
                          "wse2_dispatch_call(c, t_); }" % (target, IMAGE_BASE + retaddr))
            return
        if m in ("bt", "btc", "btr", "bts"):
            dst, src = ops[0], ops[1]
            size = dst.size
            bits = size * 8
            a = read_op(i, dst, size)
            b = read_op(i, src, size)
            if m == "bt":
                self.emit("    flags_bt(c, %s, %s, %d);" % (a, b, bits))
            else:
                op = {"btc": "^", "btr": "& ~", "bts": "|"}[m]
                self.emit("    { uint64_t a_ = %s; unsigned b_ = (unsigned)(%s) & %d; flags_bt(c, a_, b_, %d); uint64_t r_ = a_ %s (1ull << b_); %s }" % (
                    a, b, bits - 1, bits, op, write_op(i, dst, size, "r_")))
            return
        if m in ("movss", "movsd"):
            dst, src = ops
            n = 4 if m == "movss" else 8
            d = xmm_idx(i, dst)
            s = xmm_idx(i, src)
            if d is not None:
                if s is not None:
                    if n == 4:
                        self.emit("    xmm_set32_keep(c, %d, xmm32(c, %d));" % (d, s))
                    else:
                        self.emit("    c->xmm[%d].q[0] = c->xmm[%d].q[0];" % (d, s))
                elif n == 4:
                    self.emit("    xmm_set32(c, %d, (uint32_t)ld32(%s));" % (d, addr_expr(i, src)))
                else:
                    self.emit("    xmm_set64(c, %d, ld64(%s));" % (d, addr_expr(i, src)))
            else:
                if s is None:
                    raise SystemExit("movs* needs an xmm operand at 0x%X" % i.address)
                if n == 4:
                    self.emit("    st32(%s, xmm32(c, %d));" % (addr_expr(i, dst), s))
                else:
                    self.emit("    st64(%s, xmm64(c, %d));" % (addr_expr(i, dst), s))
            return
        if m in ("movaps", "movapd", "movdqa", "movups", "movupd", "movdqu"):
            dst, src = ops
            d, s = xmm_idx(i, dst), xmm_idx(i, src)
            if d is not None:
                if s is not None:
                    self.emit("    c->xmm[%d] = c->xmm[%d];" % (d, s))
                else:
                    self.emit("    xmm_ld128(c, %d, %s);" % (d, addr_expr(i, src)))
            else:
                if s is None:
                    raise SystemExit("aligned move needs an xmm operand at 0x%X" % i.address)
                self.emit("    xmm_st128(c, %d, %s);" % (s, addr_expr(i, dst)))
            return
        if m in ("andps", "orps", "xorps", "andnps", "pand", "por", "pxor", "pandn"):
            dst, src = ops
            d = xmm_idx(i, dst)
            if d is None:
                raise SystemExit("%s destination must be xmm at 0x%X" % (m, i.address))
            s = xmm_idx(i, src)
            if s is not None:
                b0, b1 = "c->xmm[%d].q[0]" % s, "c->xmm[%d].q[1]" % s
            else:
                base = addr_expr(i, src)
                b0, b1 = "ld64(%s)" % base, "ld64((%s) + 8)" % base
            a0, a1 = "c->xmm[%d].q[0]" % d, "c->xmm[%d].q[1]" % d
            if m in ("andps", "pand"):
                e0, e1 = "%s & (%s)" % (a0, b0), "%s & (%s)" % (a1, b1)
            elif m in ("orps", "por"):
                e0, e1 = "%s | (%s)" % (a0, b0), "%s | (%s)" % (a1, b1)
            elif m in ("xorps", "pxor"):
                e0, e1 = "%s ^ (%s)" % (a0, b0), "%s ^ (%s)" % (a1, b1)
            else:  # andnps / pandn: ~dst & src
                e0, e1 = "(~%s) & (%s)" % (a0, b0), "(~%s) & (%s)" % (a1, b1)
            self.emit("    { c->xmm[%d].q[0] = %s; c->xmm[%d].q[1] = %s; }" % (d, e0, d, e1))
            return
        if m in ("movd", "movq"):
            dst, src = ops
            n = 4 if m == "movd" else 8
            d = xmm_idx(i, dst)
            s = xmm_idx(i, src)
            if d is not None:
                # Moves into XMM clear the rest of the 128-bit register.
                if s is not None:
                    src_v = ("xmm32(c, %d)" % s) if n == 4 else ("xmm64(c, %d)" % s)
                    setter = "xmm_set32" if n == 4 else "xmm_set64"
                    self.emit("    %s(c, %d, %s);" % (setter, d, src_v))
                elif src.type == x86.X86_OP_REG:
                    src_v = rreg(i.reg_name(src.reg))
                    if n == 8:
                        self.emit("    xmm_set64(c, %d, %s);" % (d, src_v))
                    else:
                        self.emit("    xmm_set32(c, %d, (uint32_t)%s);" % (d, src_v))
                elif src.type == x86.X86_OP_MEM:
                    if n == 4:
                        self.emit("    xmm_set32(c, %d, ld32(%s));" % (d, addr_expr(i, src)))
                    else:
                        self.emit("    xmm_set64(c, %d, ld64(%s));" % (d, addr_expr(i, src)))
                else:
                    raise SystemExit("movd/movq unsupported source at 0x%X" % i.address)
            else:
                if s is None:
                    raise SystemExit("movd/movq needs an xmm source at 0x%X" % i.address)
                src_v = ("xmm32(c, %d)" % s) if n == 4 else ("xmm64(c, %d)" % s)
                if dst.type == x86.X86_OP_REG:
                    self.emit("    " + wreg(i.reg_name(dst.reg), src_v))
                elif dst.type == x86.X86_OP_MEM:
                    self.emit("    st%d(%s, %s);" % (n * 8, addr_expr(i, dst), src_v))
                else:
                    raise SystemExit("movd/movq unsupported destination at 0x%X" % i.address)
            return
        raise SystemExit("unsupported instruction 0x%X %s %s" % (i.address, m, i.op_str))


def find_function(pe, rva):
    sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".pdata")
    data = sec.get_data()
    for k in range(len(data) // 12):
        beg, end, unwind = struct.unpack_from("<III", data, k * 12)
        if beg == rva:
            return beg, end
    raise SystemExit("RVA 0x%X not found in .pdata" % rva)


def main():
    global IMAGE_BASE
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("rva", type=lambda s: int(s, 0))
    ap.add_argument("out")
    ap.add_argument("--image-base", type=lambda s: int(s, 0), default=0,
                    help="base added to RIP-relative targets and pushed return addresses")
    args = ap.parse_args()
    IMAGE_BASE = args.image_base

    pe = pefile.PE(args.exe, fast_load=True)
    beg, end = find_function(pe, args.rva)
    sec = next(s for s in pe.sections if s.VirtualAddress <= beg < s.VirtualAddress + s.Misc_VirtualSize)
    if sec.Name.rstrip(b"\0") != b".text":
        raise SystemExit("function 0x%X is not in .text" % beg)
    data = sec.get_data()[beg - sec.VirtualAddress:end - sec.VirtualAddress]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    insns = list(md.disasm(data, beg))
    if not insns:
        raise SystemExit("no instructions decoded")
    if sum(i.size for i in insns) != len(data):
        raise SystemExit("disassembly did not consume the whole function")
    lifter = Lifter(insns, beg, end)
    text = lifter.lift()
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("lifted 0x%X..0x%X (%d bytes, %d instructions) -> %s" % (beg, end, end - beg, len(insns), args.out))


if __name__ == "__main__":
    main()
