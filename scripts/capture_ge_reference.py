"""Capture a GE dump twice with a hash-pinned PPSSPP headless software renderer.

Requires Pillow. Outputs and a fresh memory stick stay in a new build/ directory.
This is reference-only tooling, never a guest execution path in the web port.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def capture(args):
    from PIL import Image

    emulator, dump, output = (path.resolve() for path in (args.emulator, args.dump, args.output))
    build = (ROOT / 'build').resolve()
    if not output.is_relative_to(build) or output == build or output.exists():
        raise ValueError('Output must be a new directory below this checkout\'s build/.')
    if sha256(emulator) != args.exe_sha256.lower():
        raise ValueError('Emulator SHA-256 does not match; nothing was executed.')
    with dump.open('rb') as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b'PPSSPPGE':
        raise ValueError('Not a GE frame dump.')
    disc_id = header[12:24].split(b'\0', 1)[0].decode('ascii')
    if args.disc_id and disc_id != args.disc_id:
        raise ValueError('Dump disc ID does not match the requested title.')
    version = subprocess.run([str(emulator), '--version'], check=True,
                             capture_output=True, text=True, timeout=10).stdout.strip()
    if args.source_commit[:10].lower() not in version.lower():
        raise ValueError('Emulator version does not identify the requested source commit.')
    output.mkdir(parents=True)
    memstick = output / 'memstick'
    memstick.mkdir()
    report = dict(kind='Reference frame-dump replay; not gameplay verification',
                  source_commit=args.source_commit.lower(), emulator=str(emulator),
                  exe_sha256=args.exe_sha256.lower(), version=version, dump=str(dump),
                  dump_sha256=sha256(dump), disc_id=disc_id, captures=[])
    for attempt in (1, 2):
        png = output / f'frame-{attempt}.png'
        command = [str(emulator), '--graphics=software', '--resolution-scale=1',
                   f'--memstick={memstick}', f'--screenshot-save={png}',
                   f'--timeout-wall={args.timeout}', str(dump)]
        with (output / f'run-{attempt}.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, check=True, stdout=log, stderr=log,
                           timeout=args.timeout + 5)
        with Image.open(png) as im:
            if im.width < 480 or im.height < 272:
                raise ValueError('Screenshot is smaller than the PSP visible frame.')
            rgb = im.convert('RGB')
            if not any(high for low, high in rgb.getextrema()):
                raise ValueError('Black reference capture excluded; inspect the retained log.')
            visible = rgb.crop((0, 0, 480, 272))
            visible.save(output / f'visible-{attempt}.png')
            row = dict(args=command, dimensions=[im.width, im.height], png_sha256=sha256(png),
                       visible_rgb_sha256=hashlib.sha256(visible.tobytes()).hexdigest())
            if args.expect_rgb565:
                packed = bytearray()
                for red, green, blue in struct.iter_unpack('BBB', rgb.tobytes()):
                    r, g, b = red >> 3, green >> 2, blue >> 3
                    if (red, green, blue) != ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)):
                        raise ValueError('Screenshot channels are not expanded RGB565 values.')
                    packed.extend(struct.pack('<H', r | (g << 5) | (b << 11)))
                (output / f'decoded565-{attempt}.bin').write_bytes(packed)
                row['decoded565_sha256'] = hashlib.sha256(packed).hexdigest()
            report['captures'].append(row)
    first, second = report['captures']
    for key in ('dimensions', 'png_sha256', 'visible_rgb_sha256', 'decoded565_sha256'):
        if first.get(key) != second.get(key):
            raise ValueError(f'Repeated reference captures disagree: {key}')
    report['repeated_captures_match'] = True
    (output / 'reference-manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True, help='PPSSPPHeadless.exe, not the GUI executable')
    parser.add_argument('--exe-sha256', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--disc-id', help='ULUS10440 for NCAA Football 10 US')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-rgb565', action='store_true')
    parser.add_argument('--timeout', type=int, default=20, choices=range(1, 31))
    args = parser.parse_args()
    if not re.fullmatch(r'[a-fA-F0-9]{64}', args.exe_sha256) or not re.fullmatch(r'[a-fA-F0-9]{40}', args.source_commit):
        parser.error('Provide a full SHA-256 and a full source commit SHA.')
    try:
        result = capture(args)
    except (OSError, ValueError, ImportError, UnicodeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Reference capture failed: {error}\n')
    print(json.dumps(dict(version=result['version'], disc_id=result['disc_id'],
                         output=str(args.output), repeated_captures_match=True), indent=2))


if __name__ == '__main__':
    main()
