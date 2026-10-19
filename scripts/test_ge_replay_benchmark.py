"""Asset-free rejection and timing tests for the native replay runner."""
import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import benchmark_ge_replay as replay


class ReplayBenchmarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary_root = Path(__file__).resolve().parent.parent / 'build'
        cls.temporary_root.mkdir(exist_ok=True)

    def test_warmup_and_even_median(self):
        values = replay.frame_samples('frame_ms [999,3,1,4,2]\n', 5)
        self.assertEqual(replay.timing(values[1:]), dict(median_ms=2.5, p95_ms=4, max_ms=4))

    def test_invalid_samples(self):
        for text in ['', 'frame_ms [1]', 'frame_ms [1,0]', 'frame_ms [1,-2]',
                     'frame_ms [1,NaN]', 'frame_ms [true,1]', 'frame_ms [1,"2"]']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                replay.frame_samples(text, 2)
        with self.assertRaises(ValueError):
            replay.timing([])

    def test_stride_and_visible_channels(self):
        # A padded native framebuffer: first visible pixels are red/green/blue.
        raw = bytearray(512 * 272 * 2)
        raw[:6] = b'\x1f\x00\xe0\x07\x00\xf8'
        raw[480 * 2:512 * 2] = b'\xff' * 64
        rgb = replay.visible565(raw)
        self.assertEqual(rgb[:12], b'\xff\x00\x00\x00\xff\x00\x00\x00\xff\x00\x00\x00')
        self.assertEqual(len(rgb), 480 * 272 * 3)
        for invalid in [b'', b'123', b'\0' * (479 * 272 * 2)]:
            with self.assertRaises(ValueError):
                replay.visible565(invalid)

    def test_output_and_warmup_rejected_before_execution(self):
        with tempfile.TemporaryDirectory(dir=self.temporary_root) as directory, patch.object(replay, 'ROOT', Path(directory)):
            root = Path(directory); (root / 'build').mkdir()
            args = argparse.Namespace(output=Path('outside'), frames=32, warmup=8)
            with self.assertRaises(ValueError):
                replay.benchmark(args)
            args.output = Path('build/existing'); (root / args.output).mkdir()
            with self.assertRaises(ValueError):
                replay.benchmark(args)
            args.output = Path('build/new'); args.warmup = 32
            with self.assertRaises(ValueError):
                replay.benchmark(args)
            self.assertFalse((root / args.output).exists())

    def test_wrong_title_and_duplicate_scenes(self):
        with tempfile.TemporaryDirectory(dir=self.temporary_root) as directory, patch.object(replay, 'ROOT', Path(directory)):
            root = Path(directory); (root / 'build').mkdir()
            args = argparse.Namespace(output=Path('build/new'), frames=32, warmup=8, scenes=Path('scenes.json'))
            for manifest in [dict(disc_id='ULUS00000', guest_ram_mib=64),
                             dict(disc_id='ULUS10440', guest_ram_mib=32),
                             dict(disc_id='ULUS10440', guest_ram_mib=64, scenes=[dict(name='gt'), dict(name='gt')])]:
                (root / args.scenes).write_text(json.dumps(manifest), encoding='utf-8')
                with self.assertRaises(ValueError):
                    replay.benchmark(args)
            with self.assertRaises(ValueError):
                replay.validate_scene(dict(name='../outside'))


if __name__ == '__main__':
    unittest.main()
