#!/usr/bin/env python3
"""Read class layouts and globals from a Microsoft PDB (MSF 7.0) file.

The WSE2 PDB is a shipped debug artifact that describes the analysed image:
type records give exact class/struct sizes and member offsets, and the global
symbol stream names typed globals. This tool reads those records without
executing the target and without a Ghidra/DIA dependency.

Usage:
    python pdb_symbols.py mb_warband_wse2_x64.pdb --classes
    python pdb_symbols.py mb_warband_wse2_x64.pdb --layout rglString
    python pdb_symbols.py mb_warband_wse2_x64.pdb --globals --filter script
    python pdb_symbols.py mb_warband_wse2_x64.pdb --classes --json types.json

Addresses require the image: pass --exe to map global section offsets to VAs.
"""
import argparse
import collections
import json
import struct
import sys

import pefile

MSF7_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"

# TPI leaf kinds used by this tool.
LF_MODIFIER = 0x1001
LF_POINTER = 0x1002
LF_PROCEDURE = 0x1008
LF_MFUNCTION = 0x1009
LF_ARGLIST = 0x1201
LF_FIELDLIST = 0x1203
LF_BITFIELD = 0x1205
LF_BCLASS = 0x1400
LF_VBCLASS = 0x1401
LF_IVBCLASS = 0x1402
LF_INDEX = 0x1404
LF_VFUNCTAB = 0x1409
LF_ENUMERATE = 0x1502
LF_ARRAY = 0x1503
LF_CLASS = 0x1504
LF_STRUCTURE = 0x1505
LF_UNION = 0x1506
LF_ENUM = 0x1507
LF_ALIAS = 0x150B
LF_MEMBER = 0x150D
LF_STMEMBER = 0x150E
LF_METHOD = 0x150F
LF_NESTTYPE = 0x1510
LF_ONEMETHOD = 0x1511
LF_INTERFACE = 0x1519
LF_MEMBER_MOD = 0x1403  # not a real leaf; kept for clarity

CLASS_LIKE = {LF_CLASS, LF_STRUCTURE, LF_INTERFACE}

PRIMITIVES = {
    0x0003: "void",
    0x0010: "char",
    0x0020: "unsigned char",
    0x0068: "int8_t",
    0x0069: "uint8_t",
    0x0011: "short",
    0x0021: "unsigned short",
    0x0072: "int16_t",
    0x0073: "uint16_t",
    0x0012: "long",
    0x0022: "unsigned long",
    0x0074: "int32_t",
    0x0075: "uint32_t",
    0x0013: "__int64",
    0x0023: "unsigned __int64",
    0x0076: "int64_t",
    0x0077: "uint64_t",
    0x0040: "float",
    0x0041: "double",
    0x0042: "long double",
    0x0030: "bool",
    0x0031: "bool16",
    0x0032: "bool32",
    0x0033: "bool64",
    0x0070: "char",
    0x0071: "wchar_t",
    0x007A: "char16_t",
    0x007B: "char32_t",
    0x0014: "__int128",
    0x0024: "unsigned __int128",
    0x000F: "nullptr_t",
}

PRIMITIVE_SIZES = {
    0x0003: 0, 0x0010: 1, 0x0020: 1, 0x0068: 1, 0x0069: 1,
    0x0011: 2, 0x0021: 2, 0x0072: 2, 0x0073: 2,
    0x0012: 4, 0x0022: 4, 0x0074: 4, 0x0075: 4,
    0x0013: 8, 0x0023: 8, 0x0076: 8, 0x0077: 8,
    0x0040: 4, 0x0041: 8, 0x0042: 10, 0x0030: 1,
    0x0070: 1, 0x0071: 2, 0x007A: 2, 0x007B: 4,
    0x0014: 16, 0x0024: 16,
}

POINTER_SIZE = 8


class PdbError(Exception):
    pass


def read_u16(b, o):
    return struct.unpack_from("<H", b, o)[0], o + 2


def read_u32(b, o):
    return struct.unpack_from("<I", b, o)[0], o + 4


def read_i32(b, o):
    return struct.unpack_from("<i", b, o)[0], o + 4


def read_cstr(b, o):
    end = b.find(b"\0", o)
    if end < 0:
        end = len(b)
    return b[o:end].decode("utf-8", errors="replace"), end + 1


def read_numeric(b, o):
    """Numeric leaf: an inline u16 value or a typed LEAF immediately after."""
    value, o = read_u16(b, o)
    if value < 0x8000:
        return value, o
    if value == 0x8000:
        return struct.unpack_from("<b", b, o)[0], o + 1
    if value == 0x8001:
        return struct.unpack_from("<h", b, o)[0], o + 2
    if value == 0x8002:
        return struct.unpack_from("<H", b, o)[0], o + 2
    if value == 0x8003:
        return struct.unpack_from("<i", b, o)[0], o + 4
    if value == 0x8004:
        return struct.unpack_from("<I", b, o)[0], o + 4
    if value == 0x8009:
        return struct.unpack_from("<q", b, o)[0], o + 8
    if value == 0x800A:
        return struct.unpack_from("<Q", b, o)[0], o + 8
    raise PdbError("unsupported numeric leaf 0x%x" % value)


def msf_streams(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(MSF7_MAGIC):
        raise PdbError("not an MSF 7.0 PDB")
    block_size, _free, num_blocks, dir_bytes, _unknown, block_map = \
        struct.unpack_from("<IIIIII", data, 32)
    if block_size == 0:
        raise PdbError("invalid block size")
    def block(i):
        start = i * block_size
        return data[start:start + block_size]
    # The block map lists the blocks that hold the stream directory.
    map_bytes = block(block_map)
    n_dir_blocks = (dir_bytes + block_size - 1) // block_size
    dir_block_ids = struct.unpack_from("<%dI" % n_dir_blocks, map_bytes, 0)
    directory = b"".join(block(i) for i in dir_block_ids)[:dir_bytes]
    num_streams = struct.unpack_from("<I", directory, 0)[0]
    sizes = list(struct.unpack_from("<%dI" % num_streams, directory, 4))
    sizes = [0 if s == 0xFFFFFFFF else s for s in sizes]
    pos = 4 + 4 * num_streams
    streams = {}
    for i, size in enumerate(sizes):
        n = (size + block_size - 1) // block_size
        ids = struct.unpack_from("<%dI" % n, directory, pos)
        pos += 4 * n
        streams[i] = b"".join(block(b) for b in ids)[:size]
    return streams


def parse_tpi(stream):
    version, header_size = struct.unpack_from("<II", stream, 0)
    ti_min, ti_max, rec_bytes = struct.unpack_from("<III", stream, 8)
    records = {}
    pos = header_size
    index = ti_min
    end = header_size + rec_bytes
    while pos + 4 <= end and index < ti_max:
        rec_len, leaf = struct.unpack_from("<HH", stream, pos)
        payload = stream[pos + 4:pos + 2 + rec_len]
        records[index] = (leaf, payload, pos)
        if rec_len == 0:
            break
        pos += 2 + rec_len
        index += 1
    return records


class TypeDb:
    def __init__(self, records):
        self.records = records
        self._names = {}
        self._layouts = {}

    def leaf(self, idx):
        rec = self.records.get(idx)
        return rec[0] if rec else None

    def name(self, idx, depth=0):
        if depth > 16:
            return "..."
        if idx is None:
            return "void"
        if idx < 0x1000:
            if 0x400 <= idx < 0x500 or 0x600 <= idx < 0x700:
                # Primitive pointer encodings reuse the low byte for the
                # pointed-to primitive: T_64PRCHAR 0x670 -> char *.
                base = idx & 0xFF
                return PRIMITIVES.get(base, "T_0x%x" % base) + " *"
            return PRIMITIVES.get(idx, "T_0x%x" % idx)
        cached = self._names.get(idx)
        if cached is not None:
            return cached
        rec = self.records.get(idx)
        if rec is None:
            name = "missing_0x%x" % idx
        else:
            leaf, b, _pos = rec
            try:
                name = self._name_of(leaf, b, idx, depth)
            except (PdbError, struct.error):
                name = "unparsed_0x%x" % idx
        self._names[idx] = name
        return name

    def _name_of(self, leaf, b, idx, depth):
        if leaf in CLASS_LIKE:
            return self._class_header(b, leaf)["name"]
        if leaf == LF_UNION:
            return self._union_header(b)["name"]
        if leaf == LF_ENUM:
            return self._enum_header(b)["name"]
        if leaf == LF_POINTER:
            utype, attr = struct.unpack_from("<II", b, 0)
            base = self.name(utype, depth + 1)
            if (attr & 0xF) == 0:
                return base + "*"
            if (attr & 0xF) == 1:
                return base + "&"
            if (attr & 0xF) == 2:
                return base + "&&"
            return base + "*"
        if leaf == LF_MODIFIER:
            utype, mods = struct.unpack_from("<IH", b, 0)
            base = self.name(utype, depth + 1)
            tags = []
            if mods & 0x1:
                tags.append("const")
            if mods & 0x2:
                tags.append("volatile")
            if mods & 0x4:
                tags.append("unaligned")
            return (" ".join(tags) + " " + base).strip()
        if leaf == LF_ARRAY:
            elem, _idxtype = struct.unpack_from("<II", b, 0)
            size, _ = read_numeric(b, 8)
            elem_size = self.size_of(elem)
            count = size // elem_size if elem_size else size
            return "%s[%d]" % (self.name(elem, depth + 1), count)
        if leaf == LF_BITFIELD:
            utype, length, position = struct.unpack_from("<IBB", b, 0)
            return "%s:%d" % (self.name(utype, depth + 1), length)
        if leaf == LF_ALIAS:
            utype = struct.unpack_from("<I", b, 0)[0]
            return self.name(utype, depth + 1)
        if leaf == LF_PROCEDURE:
            rvtype = struct.unpack_from("<I", b, 0)[0]
            return self.name(rvtype, depth + 1) + " (*)(...)"
        if leaf == LF_MFUNCTION:
            rvtype, cls = struct.unpack_from("<II", b, 0)
            return self.name(rvtype, depth + 1) + " (%s::*)(...)" % self.name(cls, depth + 1)
        return "leaf_0x%x" % leaf

    @staticmethod
    def _class_header(b, leaf):
        count, prop, field, derived, vshape = struct.unpack_from("<HHIII", b, 0)
        size, pos = read_numeric(b, 16)
        name, _ = read_cstr(b, pos)
        return {
            "kind": "class" if leaf == LF_CLASS else ("interface" if leaf == LF_INTERFACE else "struct"),
            "name": name or "__unnamed",
            "size": size,
            "field": field,
            "derived": derived,
            "vshape": vshape,
            "prop": prop,
            "count": count,
        }

    @staticmethod
    def _union_header(b):
        count, prop, field = struct.unpack_from("<HHI", b, 0)
        size, pos = read_numeric(b, 8)
        name, _ = read_cstr(b, pos)
        return {"kind": "union", "name": name or "__unnamed", "size": size,
                "field": field, "prop": prop, "count": count}

    @staticmethod
    def _enum_header(b):
        count, prop, utype, field = struct.unpack_from("<HHII", b, 0)
        name, _ = read_cstr(b, 12)
        return {"kind": "enum", "name": name or "__unnamed", "underlying": utype,
                "field": field, "prop": prop, "count": count}

    # Field lists ---------------------------------------------------------
    def size_of(self, idx, depth=0):
        if depth > 8:
            return 0
        if idx is None:
            return 0
        if idx < 0x1000:
            if 0x400 <= idx < 0x700:
                return POINTER_SIZE
            return PRIMITIVE_SIZES.get(idx, 0)
        rec = self.records.get(idx)
        if rec is None:
            return 0
        leaf, b, _pos = rec
        if leaf == LF_POINTER:
            return POINTER_SIZE
        if leaf == LF_MODIFIER:
            return self.size_of(struct.unpack_from("<I", b, 0)[0], depth + 1)
        if leaf == LF_ARRAY:
            return read_numeric(b, 8)[0]
        if leaf in CLASS_LIKE:
            return self._class_header(b, leaf)["size"]
        if leaf == LF_UNION:
            return self._union_header(b)["size"]
        if leaf == LF_ENUM:
            return self.size_of(self._enum_header(b)["underlying"], depth + 1)
        if leaf == LF_BITFIELD:
            return self.size_of(struct.unpack_from("<I", b, 0)[0], depth + 1)
        if leaf == LF_ALIAS:
            return self.size_of(struct.unpack_from("<I", b, 0)[0], depth + 1)
        return 0

    def fields(self, field_idx):
        out = []
        seen = set()
        cur = field_idx
        while cur and cur not in seen:
            seen.add(cur)
            rec = self.records.get(cur)
            if rec is None or rec[0] != LF_FIELDLIST:
                break
            b = rec[1]
            pos = 0
            while pos < len(b):
                # LF_PAD0..LF_PAD15 are single pad bytes that also encode how
                # many bytes of padding they introduce.
                if b[pos] >= 0xF0:
                    pos += max(1, b[pos] & 0x0F)
                    continue
                if pos + 2 > len(b):
                    break
                leaf, pos = read_u16(b, pos)
                if leaf == LF_INDEX:
                    _pad, pos = read_u16(b, pos)
                    nxt, pos = read_u32(b, pos)
                    cur = nxt
                    break
                if leaf == LF_MEMBER:
                    attr, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    off, pos = read_numeric(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "member", "name": name, "type_index": typ,
                                "type": self.name(typ), "offset": off, "attr": attr})
                elif leaf == LF_STMEMBER:
                    attr, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "static", "name": name, "type_index": typ,
                                "type": self.name(typ), "attr": attr})
                elif leaf == LF_BCLASS:
                    attr, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    off, pos = read_numeric(b, pos)
                    out.append({"kind": "base", "name": self.name(typ), "type_index": typ,
                                "offset": off, "attr": attr})
                elif leaf in (LF_VBCLASS, LF_IVBCLASS):
                    attr, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    vbptr_type, pos = read_u32(b, pos)
                    vbptr, pos = read_numeric(b, pos)
                    vtable, pos = read_numeric(b, pos)
                    out.append({"kind": "vbase", "name": self.name(typ), "type_index": typ,
                                "vbptr_type_index": vbptr_type,
                                "vbptr_offset": vbptr, "vtable_offset": vtable, "attr": attr})
                elif leaf == LF_METHOD:
                    _count, pos = read_u16(b, pos)
                    _mlist, pos = read_u32(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "method", "name": name})
                elif leaf == LF_ONEMETHOD:
                    attr, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    # Method property lives in bits 2..4; intro variants carry
                    # a vftable offset.
                    if ((attr >> 2) & 0x7) in (0x4, 0x6):
                        _vbaseoff, pos = read_u32(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "method", "name": name, "type_index": typ,
                                "type": self.name(typ), "attr": attr})
                elif leaf == LF_NESTTYPE:
                    _pad, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "nested", "name": name, "type_index": typ,
                                "type": self.name(typ)})
                elif leaf == LF_VFUNCTAB:
                    _pad, pos = read_u16(b, pos)
                    typ, pos = read_u32(b, pos)
                    out.append({"kind": "vftable", "type_index": typ, "type": self.name(typ)})
                elif leaf == LF_ENUMERATE:
                    attr, pos = read_u16(b, pos)
                    val, pos = read_numeric(b, pos)
                    name, pos = read_cstr(b, pos)
                    out.append({"kind": "enumerator", "name": name, "value": val, "attr": attr})
                else:
                    raise PdbError("unknown fieldlist leaf 0x%x at %d near %s" % (
                        leaf, pos - 2, b[max(0, pos - 18):pos + 30].hex()))
            else:
                break
        return out

    def layout(self, idx):
        if idx in self._layouts:
            return self._layouts[idx]
        rec = self.records.get(idx)
        if rec is None:
            return None
        leaf, b, _ = rec
        if leaf in CLASS_LIKE:
            head = self._class_header(b, leaf)
        elif leaf == LF_UNION:
            head = self._union_header(b)
        elif leaf == LF_ENUM:
            head = self._enum_header(b)
        else:
            return None
        head.setdefault("size", 0)
        head["fields"] = self.fields(head.get("field", 0))
        self._layouts[idx] = head
        return head


def load_globals(streams, pe):
    dbi = streams.get(3)
    if not dbi:
        return []
    (_sig, _ver, _age, glob_stream, _build, _pub_stream, _pdbver,
     _sym_stream, _rbld, _modi, _sectcontrib, _sectmap, _src, _tst,
     _mfc, _optdbg, _ec, _flags, _machine, _pad) = struct.unpack_from("<IIIHHHHHHIIIIIIIIHHI", dbi, 0)
    # The DBI global index points at a GSI hash table whose 8-byte entries
    # carry offsets into the symbol-record stream.
    gsi = streams.get(glob_stream, b"")
    rec_stream = streams.get(_sym_stream, b"")
    offsets = []
    if len(gsi) >= 16:
        sig, hdr, hr_size, buckets = struct.unpack_from("<IIII", gsi, 0)
        if sig == 0xFFFF0000 and 16 + hr_size <= len(gsi):
            for i in range(16, 16 + hr_size, 8):
                off = struct.unpack_from("<I", gsi, i)[0]
                if off < len(rec_stream):
                    offsets.append(off)
    rows = []
    if offsets:
        _collect_symbols(rec_stream, offsets, pe, rows)
        # The GSI index covers typed globals only; a sequential walk also
        # recovers S_PUB32 records (string literals, data labels).
        _collect_symbols(rec_stream, None, pe, rows)
    else:
        _collect_symbols(rec_stream, None, pe, rows)
    # Deduplicate: one address can appear in both the global index and a
    # module stream.
    seen = set()
    uniq = []
    for r in rows:
        key = (r["kind"], r.get("va"), r["name"])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(r)
    return uniq


def _symbol_walk(data):
    """Yield record starts of a CodeView symbol-record stream."""
    pos = 0
    while pos + 4 <= len(data):
        yield pos
        rec_len = struct.unpack_from("<H", data, pos)[0]
        if rec_len < 2 or pos + 2 + rec_len > len(data):
            break
        pos += 2 + rec_len


def _collect_symbols(data, offsets, pe, out):
    base = pe.OPTIONAL_HEADER.ImageBase if pe else 0
    secs = list(pe.sections) if pe else []
    kinds = {0x110C: "local", 0x110D: "global", 0x110E: "public",
             0x1107: "constant", 0x1108: "udt"}
    positions = offsets if offsets is not None else _symbol_walk(data)
    visited = set()
    for pos in positions:
        if pos in visited or pos + 4 > len(data):
            continue
        visited.add(pos)
        rec_len, kind = struct.unpack_from("<HH", data, pos)
        if rec_len < 2 or pos + 2 + rec_len > len(data):
            continue
        body = data[pos + 4:pos + 2 + rec_len]
        try:
            if kind in (0x110C, 0x110D):
                typ, off, seg = struct.unpack_from("<IIH", body, 0)
                name, _ = read_cstr(body, 10)
            elif kind == 0x110E:
                # S_PUB32: flags, then offset/segment, then the name.
                typ = None
                _flags, off, seg = struct.unpack_from("<IIH", body, 0)
                name, _ = read_cstr(body, 10)
            if kind in (0x110C, 0x110D, 0x110E):
                va = None
                if seg and seg <= len(secs):
                    s = secs[seg - 1]
                    va = base + s.VirtualAddress + off
                out.append({"kind": kinds[kind], "name": name, "type_index": typ,
                            "va": va, "section": seg, "offset": off})
            elif kind == 0x1107:
                typ = struct.unpack_from("<I", body, 0)[0]
                val, pos2 = read_numeric(body, 4)
                name, _ = read_cstr(body, pos2)
                out.append({"kind": "constant", "name": name, "type_index": typ, "value": val})
            elif kind == 0x1108:
                typ = struct.unpack_from("<I", body, 0)[0]
                name, _ = read_cstr(body, 4)
                out.append({"kind": "udt", "name": name, "type_index": typ})
        except (PdbError, struct.error):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb")
    ap.add_argument("--exe")
    ap.add_argument("--classes", action="store_true")
    ap.add_argument("--layout")
    ap.add_argument("--globals", action="store_true")
    ap.add_argument("--filter", default=None)
    ap.add_argument("--json")
    ap.add_argument("--limit", type=int, default=60)
    args = ap.parse_args()

    streams = msf_streams(args.pdb)
    records = parse_tpi(streams.get(2, b""))
    db = TypeDb(records)
    print("tpi types %d (streams %d)" % (len(records), len(streams)))

    payload = {}
    if args.classes:
        import re
        rx = re.compile(args.filter) if args.filter else None
        rows = []
        for idx, (leaf, b, _pos) in sorted(records.items()):
            if leaf not in CLASS_LIKE and leaf not in (LF_UNION, LF_ENUM):
                continue
            lay = db.layout(idx)
            if lay is None or lay["size"] == 0:
                continue
            if rx and not rx.search(lay["name"]):
                continue
            rows.append({
                "type_index": idx,
                "kind": lay["kind"],
                "name": lay["name"],
                "size": lay["size"],
                "base": next((f["name"] for f in lay["fields"] if f["kind"] == "base"), None),
                "members": sum(1 for f in lay["fields"] if f["kind"] in ("member", "static")),
            })
        rows.sort(key=lambda r: r["name"])
        payload["classes"] = rows
        print("classes/enums with a size: %d" % len(rows))
        for r in rows[:args.limit]:
            print("  %-52s %-7s size=%-6d base=%s" % (
                r["name"][:52], r["kind"], r["size"], r["base"] or "-"))

    if args.layout:
        candidates = []
        for i in records:
            if db.leaf(i) in CLASS_LIKE | {LF_UNION, LF_ENUM} and db.name(i) == args.layout:
                lay = db.layout(i)
                if lay is not None:
                    candidates.append((lay.get("size", 0), i))
        if not candidates:
            print("layout not found:", args.layout)
            return 1
        _size, idx = max(candidates)
        lay = db.layout(idx)
        payload["layout"] = lay
        print("\n%s %s size=%d type_index=0x%x" % (lay["kind"], lay["name"], lay["size"], idx))
        for f in lay["fields"]:
            if f["kind"] == "member":
                print("  +0x%04x  %-40s %s" % (f["offset"], f["name"], f["type"]))
            elif f["kind"] == "base":
                print("  base @0x%04x %s" % (f["offset"], f["name"]))
            elif f["kind"] in ("method", "static", "nested", "vftable"):
                print("  %-7s %s" % (f["kind"], f.get("name", f.get("type", ""))))

    if args.globals:
        pe = pefile.PE(args.exe, fast_load=True) if args.exe else None
        rows = load_globals(streams, pe)
        import re
        rx = re.compile(args.filter) if args.filter else None
        if rx:
            rows = [r for r in rows if rx.search(r["name"])]
        payload["globals"] = rows
        print("\nglobals %d" % len(rows))
        for r in rows[:args.limit]:
            va = "0x%x" % r["va"] if r.get("va") else "-"
            print("  %-20s %-8s %-46s %s" % (va, r["kind"], r["name"][:46], db.name(r["type_index"])))

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=1)
            f.write("\n")
        print("wrote", args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
