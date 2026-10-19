"""Compare native GE replay executables with identical output and ABBA timing.

Supply local extracted NCAA GE records and a known-good RGB565 capture in a
scene manifest. Nothing is downloaded. Results stay in a new build directory;
native replay timing is not browser/gameplay FPS.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import os

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def frame_samples(log, frames):
    match = re.search(r'^frame_ms (\[[^\n]+\])$', log, re.MULTILINE)
    if not match:
        raise ValueError('Replay did not report frame samples.')
    values = json.loads(match[1])
    if (len(values) != frames or any(isinstance(v, bool) or not isinstance(v, (int, float))
                                   or not math.isfinite(v) or v <= 0 for v in values)):
        raise ValueError('Replay reported invalid frame samples.')
    return values


def timing(values):
    ordered = sorted(values)
    if not ordered:
        raise ValueError('No measured samples after warmup.')
    return dict(median_ms=statistics.median(ordered),
                p95_ms=ordered[math.ceil(len(ordered) * .95) - 1],
                max_ms=ordered[-1])


def visible565(raw):
    if len(raw) % (272 * 2):
        raise ValueError('Invalid RGB565 framebuffer length.')
    stride = len(raw) // (272 * 2)
    if not 480 <= stride <= 1024:
        raise ValueError('Invalid RGB565 framebuffer stride.')
    output = bytearray(480 * 272 * 3)
    for y in range(272):
        for x in range(480):
            at = (y * stride + x) * 2
            word = raw[at] | (raw[at + 1] << 8)
            red, green, blue = word & 31, (word >> 5) & 63, word >> 11
            at = (y * 480 + x) * 3
            output[at:at + 3] = bytes(((red << 3) | (red >> 2),
                                      (green << 2) | (green >> 4), (blue << 3) | (blue >> 2)))
    return bytes(output)


def validate_scene(scene):
    if not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', scene.get('name', '')):
        raise ValueError('Scene names must be unique lowercase slugs.')
    paths = {key: (ROOT / scene[key]).resolve() for key in ('commands', 'pushbuffer', 'expected_display')}
    if not all(p.is_file() for p in paths.values()):
        raise ValueError('Missing scene input or expected display.')
    visible565(paths['expected_display'].read_bytes())
    return paths


def benchmark(args):
    output = (ROOT / args.output).resolve()
    if not output.is_relative_to((ROOT / 'build').resolve()) or output.exists():
        raise ValueError('Output must be a new directory below build/.')
    if not 1 <= args.frames <= 120 or not 0 <= args.warmup < args.frames:
        raise ValueError('Require 1..120 frames and warmup less than frames.')
    manifest = json.loads((ROOT / args.scenes).read_text(encoding='utf-8'))
    if manifest.get('disc_id') != 'ULUS10440' or manifest.get('guest_ram_mib') != 64:
        raise ValueError('Expected NCAA ULUS10440 / 64 MiB scene manifest.')
    scenes = manifest.get('scenes', [])
    if not scenes or len({s.get('name') for s in scenes}) != len(scenes):
        raise ValueError('Expected nonempty, uniquely named scenes.')
    inputs = [validate_scene(scene) for scene in scenes]
    executables = {label: (ROOT / getattr(args, label)).resolve() for label in ('baseline', 'candidate')}
    if not all(p.is_file() for p in executables.values()):
        raise ValueError('Missing baseline/candidate executable.')
    output.mkdir(parents=True)
    # Disable legacy primitive dumps and all optional replay mutations. Native
    # options are parsed once before timing, never queried per record.
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('PSPWEB_REPLAY_', 'PSPWEB_SKIP_PRIMS', 'PSPWEB_GE_PROFILE')):
            env.pop(key)
    env.update(PSPWEB_DUMP_FRAME='4294967295', PSPWEB_FRAME_PROFILE='1')
    report = dict(schema_version=1, disc_id='ULUS10440', guest_ram_mib=64,
                  frames=args.frames, warmup=args.warmup, order='ABBA',
                  power_throttling='disabled for benchmark process only' if args.no_eco else 'unchanged',
                  method='Native parser/render/presentation/EGL swap/GPU finish; final reads excluded. Not browser FPS.',
                  executables={k: dict(path=str(v), sha256=digest(v)) for k, v in executables.items()}, scenes=[])
    for scene, paths in zip(scenes, inputs):
        expected = paths['expected_display'].read_bytes()
        expected_rgb = visible565(expected)
        item = dict(name=scene['name'], inputs={k: dict(path=str(v), sha256=digest(v)) for k, v in paths.items()}, runs=[])
        for label, number in [('baseline', 1), ('candidate', 1), ('candidate', 2), ('baseline', 2)]:
            stem = output / f"{scene['name']}-{label}-{number}"
            ppm, raw = stem.with_suffix('.ppm'), stem.with_suffix('.bin')
            command = [str(executables[label]), str(paths['commands']), str(paths['pushbuffer']), str(ppm),
                       '--frames', str(args.frames), '--warmup', str(args.warmup), '--raw-display', str(raw)]
            if args.no_eco:
                command.append('--no-eco')
            with stem.with_suffix('.log').open('w', encoding='utf-8') as log:
                completed = subprocess.run(command, env=env, stdout=log, stderr=log)
            text = stem.with_suffix('.log').read_text(encoding='utf-8')
            if completed.returncode:
                raise ValueError(f'Replay failed; see {stem.name}.log.')
            channels = re.search(r'EGL channel bits: (\d+) (\d+) (\d+) (\d+)', text)
            if not channels or any(int(v) < 8 for v in channels.groups()) or 'guest_ram_mib 64' not in text:
                raise ValueError('Replay did not verify RGBA8 / 64 MiB.')
            if args.no_eco and 'power_disabled_for_this_process 1' not in text:
                raise ValueError('Replay did not confirm process-local power control.')
            if raw.read_bytes() != expected:
                raise ValueError(f'{stem.name}: direct framebuffer differs from the accepted capture.')
            if ppm.read_bytes() != b'P6\n480 272\n255\n' + expected_rgb:
                raise ValueError(f'{stem.name}: RGBA8 presentation differs from direct RGB565.')
            samples = frame_samples(text, args.frames)
            counters = re.search(r'prims (\d+) triangles (\d+) draws (\d+) downloads (\d+) uploads (\d+)', text)
            copies = re.search(r'blend_copies (\d+) blend_pixels (\d+)', text)
            bandwidth = re.search(r'plane_upload_bytes_total (\d+)', text)
            if not counters or not copies or not bandwidth:
                raise ValueError('Replay did not report rendering counters.')
            counts = dict(zip(('prims', 'triangles', 'draws', 'downloads', 'uploads'), map(int, counters.groups())))
            counts.update(blend_copies=int(copies[1]), blend_pixels=int(copies[2]),
                          plane_bytes_per_frame=int(bandwidth[1]) / args.frames)
            item['runs'].append(dict(label=label, run=number, frames_ms=samples,
                                     counters=counts, framebuffer_sha256=digest(raw), **timing(samples[args.warmup:])))
        for label in executables:
            item[label] = timing([value for run in item['runs'] if run['label'] == label
                                  for value in run['frames_ms'][args.warmup:]])
        item['median_change_percent'] = (item['candidate']['median_ms'] / item['baseline']['median_ms'] - 1) * 100
        report['scenes'].append(item)
        (output / 'performance.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({key: item[key] for key in ('name', 'baseline', 'candidate', 'median_change_percent')}), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--scenes', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=32)
    parser.add_argument('--warmup', type=int, default=8)
    parser.add_argument('--no-eco', action='store_true', help='Windows: opt only the replay process out of speed throttling')
    args = parser.parse_args()
    try:
        benchmark(args)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Replay benchmark failed: {error}\n')


if __name__ == '__main__':
    main()
