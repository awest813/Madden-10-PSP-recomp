"""Verify the US Madden NFL 10 disc and the executable this AOT port targets."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

EXPECTED_EBOOT_SHA256 = '5c5064c142139b9bd9bb5fff7ab8421f8364cc188a86c2b6eca06ffd69735da3'


def read_sfo(path):
    data = path.read_bytes()
    magic, version, keys, values, count = struct.unpack_from('<5I', data)
    if magic != 0x46535000:
        raise ValueError('Invalid PARAM.SFO')
    result = {}
    for i in range(count):
        key, fmt, length, capacity, offset = struct.unpack_from('<HHIII', data, 20 + i * 16)
        name = data[keys + key:].split(b'\0', 1)[0].decode('utf-8')
        value = data[values + offset:values + offset + length]
        result[name] = value.rstrip(b'\0').decode('utf-8') if fmt == 0x204 else value.hex()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='games/madden10/root')
    args = parser.parse_args()
    metadata = read_sfo(args.root / 'disc/PSP_GAME/PARAM.SFO')
    if metadata.get('DISC_ID') != 'ULUS10441' or 'MADDEN NFL 10' not in metadata.get('TITLE', '').upper():
        raise ValueError(f'This port requires US Madden NFL 10 (ULUS10441): {metadata}')
    eboot = (args.root / 'EBOOT.BIN').read_bytes()
    digest = hashlib.sha256(eboot).hexdigest()
    if digest != EXPECTED_EBOOT_SHA256:
        raise ValueError(f'Unexpected decrypted executable SHA-256: {digest}. Do not link another game against this corpus.')
    print(json.dumps({'title': metadata['TITLE'], 'disc_id': metadata['DISC_ID'],
                      'system_version': metadata.get('PSP_SYSTEM_VER'),
                      'executable_sha256': digest, 'executable_bytes': len(eboot)}, indent=2))


if __name__ == '__main__':
    main()
