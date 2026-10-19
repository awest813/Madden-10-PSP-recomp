#!/usr/bin/env python3
# NID-level audit: every (library, NID) a decrypted module imports must have
# a handler registered in profile/host, or it lands in the log-and-return-0
# fallback in kernel.cpp.
#   scripts/nid_audit.py <decrypted EBOOT.BIN> [profile/host dir]
# Exit 0 when nothing is missing, 1 otherwise.  Used by port scripts and by
# hand after adding HLE: the NCAA 10 bring-up drove this to zero gaps.
import os
import re
import struct
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILELIB = {'atrac': 'sceAtrac3plus', 'mpeg': 'sceMpeg', 'sas': 'sceSasCore',
           'savedata': 'sceUtility'}


def read_cstr(data, off):
    return data[off:data.index(b'\x00', off)].decode('ascii', 'replace')


def module_imports(data):
    """{lib: set(nids)} from the ELF `.lib.stub` tables."""
    _phoff, shoff, _fl, _eh, _phe, _phn, shent, shnum, shstr = \
        struct.unpack('<IIIHHHHHH', data[0x1C:0x34])
    secs = []
    for i in range(shnum):
        row = struct.unpack('<IIIIII', data[shoff + i * shent:shoff + i * shent + 24])
        secs.append((row[0], row[4], row[5], row[3]))
    strings = data[secs[shstr][1]:secs[shstr][1] + secs[shstr][2]]
    named = {}
    for (nidx, off, size, addr) in secs:
        named[strings[nidx:strings.index(b'\x00', nidx)].decode()] = (off, size, addr)

    def file_of(addr):
        for _, off, _sz, base in secs:
            if base != 0 and base <= addr < base + 0x1000000:
                return addr - base + off
        return addr + 0x100

    need = {}
    off, size, _ = named['.lib.stub']
    for i in range(size // 20):
        base = off + i * 20
        lib, _v, _a, cnt, nidtab, _j = struct.unpack('<IIHHII', data[base:base + 20])
        nids = struct.unpack('<%dI' % cnt, data[file_of(nidtab):file_of(nidtab) + 4 * cnt])
        need.setdefault(read_cstr(data, file_of(lib)), set()).update(nids)
    return need


def hle_coverage(host):
    """{lib: set(nids)} registered by profile/host sources."""
    have = {}
    for path in glob.glob(os.path.join(host, '*.cpp')):
        key = next((v for k, v in FILELIB.items() if k in os.path.basename(path)), None)
        curlib, alias = None, {}
        for ln in open(path, encoding='utf-8', errors='replace'):
            m = re.search(r'const char \*(\w+)\s*=\s*"([A-Za-z0-9_]+)"', ln)
            if m:
                alias[m.group(1)] = m.group(2)
            lm = re.search(r'(?:hle|register_hle)\(\s*(?:"([A-Za-z0-9_]+)"|(\w+))\s*,', ln)
            if lm:
                curlib = lm.group(1) or alias.get(lm.group(2))
            am = re.search(r'(?:add|simple)\(\s*(0x[0-9A-Fa-f]{8})', ln)
            if am and key:
                have.setdefault(key, set()).add(int(am.group(1), 16))
            elif curlib and re.search(r'0x[0-9A-Fa-f]{8}', ln):
                for nm in re.finditer(r'(0x[0-9A-Fa-f]{8})', ln):
                    if int(nm.group(1), 16) != 0:
                        have.setdefault(curlib, set()).add(int(nm.group(1), 16))
    return have


def main():
    if len(sys.argv) < 2:
        print('usage: nid_audit.py <decrypted EBOOT.BIN> [profile/host dir]')
        return 2
    data = open(sys.argv[1], 'rb').read()
    if data[:4] == b'~PSP':
        print('EBOOT is still encrypted (~PSP). Decrypt it first (see docs/NCAA10.md).')
        return 1
    host = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'profile', 'host')
    need = module_imports(data)
    have = hle_coverage(host)
    missing = 0
    for lib in sorted(need):
        gap = sorted(need[lib] - have.get(lib, set()))
        print(('GAP ' if gap else 'ok   ') + '%-18s %3d imports -> %s'
              % (lib, len(need[lib]), ' '.join('0x%08X' % g for g in gap)))
        missing += len(gap)
    print('total missing:', missing)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
