"""Render an asset-free red fixture and reject malformed native replay inputs."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from benchmark_ge_replay import ROOT, visible565

EXECUTABLE = None


class ReplayCliTests(unittest.TestCase):
    def test_red_frame_and_rejections(self):
        temporary_root = ROOT / 'build/replay-cli-tests'
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
            root = Path(directory)
            push = bytearray(struct.pack('<6f', 0, 0, 0, 480, 272, 0))
            records = [struct.pack('<BII', 2, 24, 0)]

            def record(kind, data):
                start = len(push)
                push.extend(data)
                records.append(struct.pack('<BII', kind, len(data), start))

            ops = [(0x9C, 0), (0x9D, 512), (0xD2, 0), (0xD4, 0), (0xD5, 479 | (271 << 10)),
                   (0x12, (3 << 7) | (1 << 23)), (0x1E, 0), (0x55, 255), (0x58, 255),
                   (0xD3, 0x101), (0x04, (6 << 16) | 2)]
            record(1, b''.join(struct.pack('<I', (cmd << 24) | value) for cmd, value in ops))
            record(9, struct.pack('<III', 0x04000000, 512, 0))
            commands, payload = root / 'commands.bin', root / 'pushbuffer.bin'
            commands.write_bytes(b''.join(records)); payload.write_bytes(push)
            env = os.environ.copy(); env['PSPWEB_DUMP_FRAME'] = '4294967295'
            base = [str(EXECUTABLE), str(commands), str(payload), str(root / 'red.ppm')]
            completed = subprocess.run(base + ['--frames', '2', '--warmup', '1', '--raw-display', str(root / 'red.bin')],
                                       env=env, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            expected = b'\xff\x00\x00' * (480 * 272)
            self.assertEqual(visible565((root / 'red.bin').read_bytes()), expected)
            self.assertEqual((root / 'red.ppm').read_bytes(), b'P6\n480 272\n255\n' + expected)
            for args in [['--frames', '2x'], ['--frames', '2', '--warmup', '2'], ['--frames', '121'], ['--bogus']]:
                with self.subTest(args=args):
                    self.assertNotEqual(subprocess.run(base + args, env=env, capture_output=True).returncode, 0)
            commands.write_bytes(b'123')
            self.assertNotEqual(subprocess.run(base + ['--frames', '1', '--warmup', '0'],
                                               env=env, capture_output=True).returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    args = parser.parse_args()
    EXECUTABLE = args.exe.resolve()
    if not EXECUTABLE.is_file():
        parser.error('Replay executable does not exist.')
    unittest.main(argv=[__file__])
