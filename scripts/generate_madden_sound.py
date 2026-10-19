"""AOT compile Allegrex routines embedded in Madden NFL 10's EA audio banks.

The ABKC program table stores code offsets followed by data offsets. Programs
are deduplicated by their unlinked bytes. J/JAL target relocations remain guest
linker data; no guest instruction is decoded or compiled at runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('disc', type=Path)
parser.add_argument('codegen', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
programs = {}
signature = bytes.fromhex('f8ffbd270000b0af0400bfaf21808000')
for path in sorted((args.disc / 'PSP_GAME/USRDIR/data/audio/aems').rglob('*')):
    if not path.is_file():
        continue
    data = path.read_bytes()
    search = 0
    while (bank := data.find(b'ABKC', search)) >= 0:
        search = bank + 4
        first = data.find(signature, bank)
        if first < 0:
            continue
        entries = []
        for offset in range(bank + 0x70, first, 4):
            kind, code, payload = struct.unpack_from('<3I', data, offset)
            if 0x10100 <= kind <= 0x101ff and bank + code < len(data):
                if data[bank + code:bank + code + 16] == signature:
                    entries.append((code, payload))
        if not entries:
            continue
        starts = sorted(set(code for code, _ in entries))
        end = min(payload for _, payload in entries)
        for index, start in enumerate(starts):
            stop = starts[index + 1] if index + 1 < len(starts) else end
            blob = data[bank + start:bank + stop]
            if not blob or len(blob) % 4 or len(blob) > 16384:
                raise ValueError(f'Invalid code range in {path}: {start:x}-{stop:x}')
            programs.setdefault(blob, []).append(f'{path.relative_to(args.disc)}@{bank + start:x}')

if not programs:
    raise ValueError('No EA audio programs found; supply the complete verified Madden NFL 10 disc.')

manifest = []
source = ['#include "psprecomp/runtime.hpp"', '#include <bit>', '#include <cmath>',
          '#include <cstdint>', '#include <limits>', 'namespace psprecomp {']
for index, (blob, origins) in enumerate(programs.items()):
    name = f'madden_sound_{index:03d}'
    raw = args.output / f'{name}.bin'
    inc = args.output / f'{name}.inc'
    raw.write_bytes(blob)
    subprocess.run([str(args.codegen.resolve()), '--blob', str(raw), str(inc), name], check=True)
    source.append(f'#include "{name}.inc"')
    words = struct.unpack(f'<{len(blob) // 4}I', blob)
    source.append(f'static const std::uint32_t words_{index}[] = {{' +
                  ','.join(f'0x{word:08x}u' for word in words) + '};')
    manifest.append({'name': name, 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest(), 'origins': origins})
source.extend(['const MaddenSoundProgram madden_sound_programs[] = {'])
for index in range(len(programs)):
    source.append(f'{{words_{index}, sizeof(words_{index}) / 4u, &madden_sound_{index:03d}_entry}},')
source.extend(['};', f'const std::uint32_t madden_sound_program_count = {len(programs)}u;', '}'])
source.insert(0, '#include "../../host/madden_sound.hpp"')
(args.output / 'madden_sound.cpp').write_text('\n'.join(source) + '\n')
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(f'Compiled {len(programs)} unique audio programs from {sum(map(len, programs.values()))} bank entries')
