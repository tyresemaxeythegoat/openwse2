#!/usr/bin/env python3
"""Static checks for the patched PanVK ICD and its libhardware stub.

Usage:
    python check_panvk.py libvulkan_panfrost.patched.so \
        --stub libpanvk_hw.so

    python check_panvk.py libvulkan_panfrost.so --allow-unpatched

The checker is intentionally offline: it validates the same properties the
Android dynamic linker relies on (ELF class/machine, SONAME, DT_NEEDED, and
exported ICD entry points) without needing a phone or an emulator.
"""
import argparse
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

ICD_EXPORTS = (
    "vk_icdGetInstanceProcAddr",
    "vk_icdNegotiateLoaderICDInterfaceVersion",
    "vk_icdGetPhysicalDeviceProcAddr",
)
STUB_EXPORTS = ("hw_get_module",)
STUB_SONAME = "libpanvk_hw.so"


class ElfFacts:
    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        self.elf = ELFFile(self.file)

    def close(self):
        self.file.close()

    @property
    def needed(self):
        dynamic = self.elf.get_section_by_name(".dynamic")
        if dynamic is None:
            return []
        return [tag.needed for tag in dynamic.iter_tags()
                if tag.entry.d_tag == "DT_NEEDED"]

    @property
    def soname(self):
        dynamic = self.elf.get_section_by_name(".dynamic")
        if dynamic is None:
            return None
        for tag in dynamic.iter_tags():
            if tag.entry.d_tag == "DT_SONAME":
                return tag.soname
        return None

    @property
    def exports(self):
        dynsym = self.elf.get_section_by_name(".dynsym")
        if dynsym is None:
            return set()
        return {symbol.name for symbol in dynsym.iter_symbols()
                if symbol.name and symbol["st_shndx"] != "SHN_UNDEF"
                and symbol["st_info"]["bind"] != "STB_LOCAL"}

    @property
    def imports(self):
        dynsym = self.elf.get_section_by_name(".dynsym")
        if dynsym is None:
            return set()
        return {symbol.name for symbol in dynsym.iter_symbols()
                if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"}


def check_common(facts, problems):
    elf = facts.elf
    if elf.elfclass != 64:
        problems.append(f"{facts.path.name}: not ELF64")
    if elf.little_endian is not True:
        problems.append(f"{facts.path.name}: not little-endian")
    if elf["e_machine"] != "EM_AARCH64":
        problems.append(f"{facts.path.name}: machine is {elf['e_machine']}, "
                        "expected EM_AARCH64")
    if elf["e_type"] != "ET_DYN":
        problems.append(f"{facts.path.name}: type is {elf['e_type']}, "
                        "expected ET_DYN")


def check_icd(path, allow_unpatched):
    problems = []
    facts = ElfFacts(path)
    try:
        check_common(facts, problems)

        exports = facts.exports
        missing = [name for name in ICD_EXPORTS if name not in exports]
        if missing:
            problems.append("ICD is missing exports: " + ", ".join(missing))

        needed = facts.needed
        if "libhardware.so" in needed:
            if allow_unpatched:
                print("note: ICD is not patched (libhardware.so is still needed)")
            else:
                problems.append("ICD still needs libhardware.so; run "
                                "tools/patch_panvk.py")
        if not allow_unpatched and STUB_SONAME not in needed:
            problems.append(f"ICD does not need {STUB_SONAME}; run "
                            "tools/patch_panvk.py")
        if "libvulkan.so" in needed:
            problems.append("ICD must not depend on the system libvulkan.so")
        if facts.soname is None:
            problems.append("ICD has no DT_SONAME")
        elif "libvulkan" not in facts.soname:
            problems.append(f"unexpected ICD SONAME: {facts.soname}")

        if "hw_get_module" not in facts.imports:
            problems.append("ICD does not import hw_get_module; the stub is "
                            "not needed for this build")

        print(f"ICD         {path}")
        print(f"  soname    {facts.soname}")
        print(f"  needed    {' '.join(needed)}")
        print(f"  exports   {' '.join(sorted(exports & set(ICD_EXPORTS)))}")
        print(f"  hw import {'yes' if 'hw_get_module' in facts.imports else 'no'}")
    finally:
        facts.close()
    return problems


def check_stub(path):
    problems = []
    facts = ElfFacts(path)
    try:
        check_common(facts, problems)

        exports = facts.exports
        missing = [name for name in STUB_EXPORTS if name not in exports]
        if missing:
            problems.append("stub is missing exports: " + ", ".join(missing))

        if facts.soname != STUB_SONAME:
            problems.append(f"stub SONAME is {facts.soname!r}, expected "
                            f"{STUB_SONAME!r}")
        if facts.needed:
            problems.append("stub must not need other libraries: "
                            + " ".join(facts.needed))

        print(f"stub        {path}")
        print(f"  soname    {facts.soname}")
        print(f"  exports   {' '.join(sorted(exports))}")
    finally:
        facts.close()
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("icd", type=Path)
    parser.add_argument("--stub", type=Path,
                        help="check the companion libpanvk_hw.so")
    parser.add_argument("--allow-unpatched", action="store_true",
                        help="accept an unpatched ICD (libhardware.so needed)")
    args = parser.parse_args(argv)

    if not args.icd.is_file():
        parser.error(f"no such file: {args.icd}")

    problems = check_icd(args.icd, args.allow_unpatched)
    if args.stub:
        if not args.stub.is_file():
            parser.error(f"no such file: {args.stub}")
        problems += check_stub(args.stub)

    if problems:
        print()
        for problem in problems:
            print(f"FAIL: {problem}")
        return 1

    print()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
