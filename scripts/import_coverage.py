#!/usr/bin/env python3
# Import coverage for a decrypted PSP module: which PSP libraries it links
# and whether this profile implements them natively or via offline stubs.
#   scripts/import_coverage.py <EBOOT.BIN-decrypted>
#
# With a decrypted ELF it parses the section headers and reads the real PSP
# import tables (`.lib.stub` + `.rodata.sceNid`-style NID arrays), so the
# report lists exact NIDs per library -- the same data used to write
# profile/host/net.cpp.  Without section headers it falls back to a raw
# byte scan for library names.  NID-level runtime gaps still show up as
# "[hle] unimplemented" with PSPWEB_TRACE_HLE=1 (see docs/internals.md).
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Libraries with real implementations in profile/host (not just stubs).
NATIVE = {
    "ThreadManForUser", "SysMemUserForUser", "LoadExecForUser",
    "ModuleMgrForUser", "StdioForUser", "UtilsForUser", "Kernel_Library",
    "IoFileMgrForUser", "sceDisplay", "sceGe_user", "sceCtrl", "sceAudio",
    "sceUmdUser", "sceImpose", "sceRtc", "scePower", "sceUtility",
    "sceSuspendForUser", "sceMpeg", "sceSasCore", "sceAtrac3plus",
    "scePspNpDrm_user", "sceFont", "sceWlanDrv", "sceNet", "sceNetInet",
    "sceNetApctl", "sceNetAdhoc", "sceNetAdhocctl", "sceNetResolver",
    "sceOpenPSID", "sceReg", "sceSsl", "sceHttp", "sceMd5", "sceUsb",
    "InterruptManager",
}
# Known-offline stubs (net.cpp): game boots to single-player without them.
OFFLINE_OK = {
    "sceNet", "sceNetInet", "sceNetApctl", "sceNetAdhoc", "sceNetAdhocctl",
    "sceNetResolver", "sceWlanDrv", "sceSsl", "sceHttp", "sceUsb",
}
# sceGu/sceGum are pure user-level GE wrappers linked into game code: fine.
GAME_SIDE = {"sceGu", "sceGum", "sceGum_extra", "sceGe_user_extra"}

LIB_RE = re.compile(rb"sce[A-Za-z0-9_]{2,28}|UtilsForUser|ThreadManForUser|"
                    rb"SysMemUserForUser|IoFileMgrForUser|ModuleMgrForUser|"
                    rb"LoadExecForUser|StdioForUser|Kernel_Library")


def read_cstr(data, off):
    end = data.index(b"\x00", off)
    return data[off:end].decode("ascii", "replace")


def elf_imports(data):
    """Parse ELF section headers; return {lib: [nids]} or None."""
    if data[:4] != b"\x7fELF" or len(data) < 52 or data[4] != 1:
        return None  # not a 32-bit ELF
    try:
        _phoff, shoff, _flags, _ehsize, _phentsize, _phnum, shentsize, shnum, shstrndx = \
            struct.unpack("<IIIHHHHHH", data[0x1C:0x34])
    except struct.error:
        return None
    if shoff == 0 or shnum == 0 or shentsize < 40:
        return None
    try:
        sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            name, _type, _flags, addr, offset, size = struct.unpack("<IIIIII", data[off:off + 24])[:6]
            sections.append((name, offset, size, addr))
        shstr = data[sections[shstrndx][1]:sections[shstrndx][1] + sections[shstrndx][2]]
        named = {}
        for nidx, offset, size, addr in sections:
            end = shstr.index(b"\x00", nidx)
            named[shstr[nidx:end].decode("ascii", "replace")] = (offset, size, addr)
    except (IndexError, ValueError, struct.error):
        return None
    stub = named.get(".lib.stub")
    if stub is None:
        return None

    def file_of(addr):
        for _, offset, _size, base in sections:
            if base != 0 and base <= addr < base + 0x1000000:
                return addr - base + offset
        return addr + 0x100  # this profile's dumps map low addresses this way

    libs = {}
    off, size, _ = stub
    for i in range(size // 20):
        base = off + i * 20
        libname, _ver, _attr, cnt, nidtab, _jump = struct.unpack("<IIHHII", data[base:base + 20])
        try:
            name = read_cstr(data, file_of(libname))
            nids = struct.unpack("<%dI" % cnt, data[file_of(nidtab):file_of(nidtab) + 4 * cnt])
        except (ValueError, struct.error):
            continue
        libs[name] = list(nids)
    return libs or None


def analyze_imports(path):
    with open(path, "rb") as f:
        data = f.read()
    libs = elf_imports(data)
    if libs is not None:
        return libs, "ELF import tables (exact NIDs)"
    found = sorted({m.decode() for m in set(LIB_RE.findall(data))})
    return {lib: [] for lib in found}, "raw ELF scan (module names only)"


def main():
    if len(sys.argv) < 2:
        print("usage: import_coverage.py <decrypted EBOOT.BIN>")
        return 2
    path = sys.argv[1]
    if not os.path.isfile(path):
        print(f"no such file: {path}")
        return 1
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic == b"~PSP":
        print("EBOOT is still encrypted (~PSP). Decrypt it first (see docs/NCAA10.md).")
        return 1
    if magic != b"\x7fELF":
        print(f"warning: not an ELF file (magic {magic.hex()}), scan may be junk")

    libs, how = analyze_imports(path)
    print(f"imports via {how}: {len(libs)} libraries, "
          f"{sum(len(v) for v in libs.values())} functions")
    native, fallback, unknown = [], [], []
    for lib, nids in libs.items():
        tag = f"{lib} ({len(nids)})" if nids else lib
        if lib in NATIVE:
            (native if lib not in OFFLINE_OK else fallback).append(tag)
        elif lib in GAME_SIDE:
            native.append(f"{lib} (game-side, no HLE needed)")
        else:
            unknown.append(tag)
    print(f"\n[native] ({len(native)}):")
    for lib in native:
        print(f"  ok        {lib}")
    if fallback:
        print(f"\n[offline stubs] ({len(fallback)}; single-player safe, online disabled):")
        for lib in fallback:
            print(f"  stub      {lib}")
    if unknown:
        print(f"\n[NEEDS WORK] ({len(unknown)}; falls back to log-and-return-0):")
        for lib in unknown:
            print(f"  missing   {lib}")
        print("\nNext step: run the headless build, collect '[hle] unimplemented' lines,")
        print("and implement the NIDs the game actually calls (see docs/internals.md).")
    else:
        print("\nAll linked libraries have HLE coverage. Any remaining NID-level gaps")
        print("show up as '[hle] unimplemented' at runtime.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
