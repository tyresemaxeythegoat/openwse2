#!/usr/bin/env python3
"""Patch the FristOneRR PanVK ICD for in-process loading from an Android app.

PanVK links ``libhardware.so`` and calls ``hw_get_module("gralloc")`` while
setting up optional gralloc/video-buffer support. ``libhardware.so`` is a
VNDK-SP library and is not reachable from an ordinary app's linker namespace,
so the ICD fails to load before it can create a device.

The app ships a tiny ``libpanvk_hw.so`` (see runtime/android/src/hardware_stub.c)
that reports no gralloc module. PanVK treats that as a supported configuration
and falls back to Mesa's own gralloc path; swapchain rendering does not use the
vendor HAL.

This tool rewrites the ICD's DT_NEEDED string in place. ``libhardware.so`` and
``libpanvk_hw.so`` are both 14 bytes, so no offsets, hashes, or file layout
change.

Usage:
    python patch_panvk.py libvulkan_panfrost.so -o libvulkan_panfrost.patched.so

    python patch_panvk.py libvulkan_panfrost.so --in-place
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

ORIGINAL = b"libhardware.so"
REPLACEMENT = b"libpanvk_hw.so"

if len(ORIGINAL) != len(REPLACEMENT):
    raise SystemExit("internal error: replacement must keep the string length")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def dynstr_needed_offsets(elf):
    """Return the .dynstr offsets of every DT_NEEDED entry, in order."""
    section = elf.get_section_by_name(".dynstr")
    dynamic = elf.get_section_by_name(".dynamic")
    if section is None or dynamic is None:
        raise SystemExit("not a dynamically linked ELF: .dynstr/.dynamic missing")

    offsets = []
    for tag in dynamic.iter_tags():
        if tag.entry.d_tag == "DT_NEEDED":
            offsets.append(tag.entry.d_val)
    return section, offsets


def read_cstr(data, offset):
    end = data.find(b"\x00", offset)
    if end < 0:
        raise SystemExit(f"unterminated string at .dynstr offset {offset}")
    return data[offset:end]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="PanVK ICD (libvulkan_panfrost.so)")
    parser.add_argument("-o", "--output", type=Path,
                        help="patched output file")
    parser.add_argument("--in-place", action="store_true",
                        help="patch the input file directly")
    parser.add_argument("--drop-runpath", action="store_true",
                        help="blank the Termux DT_RUNPATH string")
    args = parser.parse_args(argv)

    if not args.input.is_file():
        parser.error(f"no such file: {args.input}")
    if args.in_place and args.output:
        parser.error("--in-place and --output are mutually exclusive")
    if not args.in_place and not args.output:
        parser.error("choose --output or --in-place")

    with open(args.input, "rb") as f:
        elf = ELFFile(f)
        if elf.elfclass != 64 or elf.little_endian is not True:
            raise SystemExit("expected a little-endian ELF64 image")
        if elf["e_machine"] != "EM_AARCH64":
            raise SystemExit(f"expected AArch64, got {elf['e_machine']}")

        dynstr, needed_offsets = dynstr_needed_offsets(elf)
        dynstr_data = dynstr.data()
        needed = [read_cstr(dynstr_data, offset) for offset in needed_offsets]
        dynstr_file_offset = dynstr["sh_offset"]

        patched_offsets = [offset for offset, name in zip(needed_offsets, needed)
                           if name == ORIGINAL]
        already = [offset for offset, name in zip(needed_offsets, needed)
                   if name == REPLACEMENT]

        if not patched_offsets and not already:
            raise SystemExit(f"{args.input} does not depend on {ORIGINAL.decode()}")

        if len(patched_offsets) > 1:
            raise SystemExit(f"refusing to patch {len(patched_offsets)} DT_NEEDED entries")

        if patched_offsets:
            start = patched_offsets[0]
            raw = dynstr_data[start:start + len(ORIGINAL)]
            if raw != ORIGINAL:
                raise SystemExit("internal error: .dynstr offset mismatch")

    output = args.input if args.in_place else args.output
    if output != args.input:
        if output.exists():
            output.unlink()
        shutil.copy2(args.input, output)

    with open(output, "r+b") as f:
        if patched_offsets:
            f.seek(dynstr_file_offset + patched_offsets[0])
            f.write(REPLACEMENT)

        if args.drop_runpath:
            with open(args.input, "rb") as original:
                elf = ELFFile(original)
                dynstr = elf.get_section_by_name(".dynstr")
                data = dynstr.data()
                for tag in elf.get_section_by_name(".dynamic").iter_tags():
                    if tag.entry.d_tag == "DT_RUNPATH":
                        f.seek(dynstr["sh_offset"] + tag.entry.d_val)
                        f.write(b"\x00")
                        break

    # Re-open and verify the result.
    with open(output, "rb") as f:
        elf = ELFFile(f)
        dynstr, needed_offsets = dynstr_needed_offsets(elf)
        needed = [read_cstr(dynstr.data(), offset).decode() for offset in needed_offsets]

    expected = [REPLACEMENT.decode() if name == ORIGINAL.decode() else name
                for name in needed]

    if ORIGINAL.decode() in expected:
        raise SystemExit("patch verification failed: libhardware.so is still needed")
    if REPLACEMENT.decode() not in expected:
        raise SystemExit("patch verification failed: libpanvk_hw.so is not needed")

    print(f"input       {args.input}")
    print(f"output      {output}")
    print(f"sha256      {sha256(output)}")
    print("DT_NEEDED   " + " ".join(expected))
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
