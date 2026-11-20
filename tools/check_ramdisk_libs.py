#!/usr/bin/env python3
"""Fail if a binary in the recovery ramdisk can't start.

Checks that every ELF's interpreter exists and every DT_NEEDED lib is found.
Recovery's ld.config.txt limits /system binaries to /system/lib64, so a lib
that only exists in /vendor/lib64 does not count for them. Everything else
gets the linker's default paths (/system, /odm, /vendor). Also lists init
services whose binary is missing (a warning: TWRP ships a few rc files for
services it never starts).

Usage: check_ramdisk_libs.py <recovery root dir>
"""
import os
import struct
import sys

SYSTEM_PATHS = ["system/lib64"]
DEFAULT_PATHS = ["system/lib64", "odm/lib64", "vendor/lib64"]


def needed(path):
    """(interpreter, DT_NEEDED list) of a 64-bit LE ELF, or None if not one."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads, dyn, interp = [], None, None
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + i * phentsize)
        if p_type == 3:
            interp = data[p_offset:p_offset + p_filesz].rstrip(b"\0").decode()
        elif p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dyn = (p_offset, p_filesz)
    if dyn is None:
        return interp, []

    def off(vaddr):
        for va, fo, sz in loads:
            if va <= vaddr < va + sz:
                return vaddr - va + fo
        raise ValueError(f"{path}: bad vaddr {vaddr:#x}")

    strtab, names = None, []
    for i in range(dyn[1] // 16):
        tag, val = struct.unpack_from("<qQ", data, dyn[0] + i * 16)
        if tag == 0:
            break
        if tag == 5:
            strtab = off(val)
        elif tag == 1:
            names.append(val)
    out = []
    for n in names:
        end = data.index(b"\0", strtab + n)
        out.append(data[strtab + n:end].decode())
    return interp, out


def rc_services(root):
    for dirpath, _, files in os.walk(root):
        for name in files:
            if not name.endswith(".rc"):
                continue
            with open(os.path.join(dirpath, name), errors="replace") as f:
                for line in f:
                    w = line.split()
                    if len(w) >= 3 and w[0] == "service":
                        yield name, w[1], w[2]


def main():
    root = sys.argv[1]
    missing = []
    for dirpath, _, files in os.walk(root):
        for name in files:
            path = os.path.join(dirpath, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            rel = os.path.relpath(path, root)
            try:
                elf = needed(path)
            except (ValueError, struct.error):
                continue
            if elf is None:
                continue
            interp, libs = elf
            if interp and not os.path.exists(os.path.join(root, interp.lstrip("/"))):
                missing.append(f"{rel}: interpreter {interp}")
            paths = SYSTEM_PATHS if rel.startswith("system/") else DEFAULT_PATHS
            for lib in libs:
                if not any(os.path.exists(os.path.join(root, p, lib)) for p in paths):
                    missing.append(f"{rel}: {lib} (searched {', '.join(paths)})")
    for rc, svc, binary in sorted(set(rc_services(root))):
        if not os.path.lexists(os.path.join(root, binary.lstrip("/"))):
            print(f"warning: service {svc} ({rc}): {binary} not in ramdisk")
    for m in sorted(missing):
        print("MISSING", m)
    print(f"{len(missing)} problems")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
