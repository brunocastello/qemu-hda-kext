#!/usr/bin/env python3
"""
macho-compat.py - make a modern-linker x86_64 kext loadable by old kxld.

Mac OS X 10.6's kernel linker (kxld, xnu-1504) only understands a handful of
load commands in an MH_KEXT_BUNDLE and refuses the kext ("Invalid load
command") when it sees newer ones such as LC_VERSION_MIN_MACOSX,
LC_FUNCTION_STARTS, LC_DATA_IN_CODE, LC_SOURCE_VERSION or LC_BUILD_VERSION.
Those are pure metadata for a kext, so this script removes them from the
load command list in place. The bytes they pointed to in __LINKEDIT are left
untouched (harmless orphan data).

Any other unexpected load command is reported as an error, because silently
removing something functional (e.g. chained fixups) would produce a kext
that panics instead of one that fails to load.

Usage: macho-compat.py <path-to-kext-binary> [--check]
  --check   only report, do not modify (exit 1 if changes would be needed)

Standard library only (runs on the GitHub macOS runner and on Linux).
SPDX-License-Identifier: MIT
"""

import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
MH_KEXT_BUNDLE = 0xB
CPU_TYPE_X86_64 = 0x01000007

LC_NAMES = {
    0x2: "LC_SYMTAB",
    0xB: "LC_DYSYMTAB",
    0x19: "LC_SEGMENT_64",
    0x1B: "LC_UUID",
    0x1D: "LC_CODE_SIGNATURE",
    0x22: "LC_DYLD_INFO",
    0x80000022: "LC_DYLD_INFO_ONLY",
    0x24: "LC_VERSION_MIN_MACOSX",
    0x26: "LC_FUNCTION_STARTS",
    0x29: "LC_DATA_IN_CODE",
    0x2A: "LC_SOURCE_VERSION",
    0x32: "LC_BUILD_VERSION",
    0x80000033: "LC_DYLD_EXPORTS_TRIE",
    0x80000034: "LC_DYLD_CHAINED_FIXUPS",
}

KEEP = {0x2, 0xB, 0x19, 0x1B, 0x1D}
STRIP = {0x24, 0x26, 0x29, 0x2A, 0x32}


def name(cmd):
    return LC_NAMES.get(cmd, "0x%x" % cmd)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    check_only = "--check" in argv[2:]

    with open(path, "rb") as f:
        data = bytearray(f.read())

    magic, cputype, _sub, filetype, ncmds, sizeofcmds, _flags, _res = \
        struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64:
        print("error: %s is not a thin 64-bit Mach-O (magic 0x%x)" % (path, magic))
        return 1
    if cputype != CPU_TYPE_X86_64:
        print("error: cputype 0x%x is not x86_64" % cputype)
        return 1
    if filetype != MH_KEXT_BUNDLE:
        print("error: filetype %d is not MH_KEXT_BUNDLE" % filetype)
        return 1

    header_size = 32
    off = header_size
    kept = []
    stripped = []
    unknown = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        blob = bytes(data[off:off + cmdsize])
        if cmd in KEEP:
            kept.append((cmd, blob))
        elif cmd in STRIP:
            stripped.append(cmd)
        else:
            unknown.append(cmd)
            kept.append((cmd, blob))
        off += cmdsize

    print("load commands: " + ", ".join(name(c) for c, _ in kept) +
          ("" if not stripped else "  | removable: " + ", ".join(name(c) for c in stripped)))

    if unknown:
        print("error: unexpected load command(s) for a 10.6 kext: " +
              ", ".join(name(c) for c in unknown))
        return 1

    if not stripped:
        print("ok: nothing to strip")
        return 0
    if check_only:
        print("check: %d load command(s) would be stripped" % len(stripped))
        return 1

    new_cmds = b"".join(blob for _, blob in kept)
    old_end = header_size + sizeofcmds
    # Rewrite the command area, zero-fill the freed tail (stays inside the
    # header padding, so no section offsets move).
    data[header_size:old_end] = new_cmds + b"\0" * (sizeofcmds - len(new_cmds))
    struct.pack_into("<II", data, 16, len(kept), len(new_cmds))

    with open(path, "wb") as f:
        f.write(data)
    print("stripped: " + ", ".join(name(c) for c in stripped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
