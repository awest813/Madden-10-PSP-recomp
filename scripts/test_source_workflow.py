"""Source-workflow regressions using synthetic ISO data, never game files."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

from prepare_ncaa_disc import prepare

SCRIPTS = Path(__file__).resolve().parent


def iso_fixture(payload=b'test data', declared_size=None):
    data = bytearray(21 * 2048)
    data[16 * 2048:16 * 2048 + 7] = b'\x01CD001\x01'
    root = bytearray(34)
    root[0], root[25], root[32] = 34, 2, 1
    struct.pack_into('<I', root, 2, 20)
    struct.pack_into('<I', root, 10, 2048)
    data[16 * 2048 + 156:16 * 2048 + 190] = root
    record = bytearray(42)
    record[0], record[32] = 42, 8
    struct.pack_into('<I', record, 2, 21)
    struct.pack_into('<I', record, 10, len(payload) if declared_size is None else declared_size)
    record[33:41] = b'TEST.BIN'
    data[20 * 2048:20 * 2048 + 42] = record
    return bytes(data) + payload


class SourceWorkflowTests(unittest.TestCase):
    def setUp(self):
        temporary_root = SCRIPTS.parent / 'build/source-workflow-tests'
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='ncaa-source-', dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'game.iso'
        self.source.write_bytes(iso_fixture())
        self.disc = self.root / 'disc'

    def test_iso_copy_reuse_and_extracted_source(self):
        result = prepare(self.source, self.disc)
        raw = self.disc / '__umd.iso'
        self.assertEqual(raw.read_bytes(), self.source.read_bytes())
        modified = raw.stat().st_mtime_ns
        self.assertEqual(prepare(self.source, self.disc), result)
        self.assertEqual(prepare(self.disc, self.disc), result)
        self.assertEqual(raw.stat().st_mtime_ns, modified)

    def test_zip_unpacks_iso_bytes(self):
        archive = self.root / 'game.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as zipped:
            zipped.write(self.source, 'folder/game.iso')
        prepare(archive, self.disc)
        self.assertEqual((self.disc / '__umd.iso').read_bytes(), self.source.read_bytes())

    def test_same_size_mismatch_preserves_existing_image(self):
        prepare(self.source, self.disc)
        original = (self.disc / '__umd.iso').read_bytes()
        self.source.write_bytes(original[:-1] + b'!')
        with self.assertRaisesRegex(ValueError, 'differs'):
            prepare(self.source, self.disc)
        self.assertEqual((self.disc / '__umd.iso').read_bytes(), original)

    def test_invalid_source_leaves_no_partial_destination(self):
        self.source.write_bytes(b'not an ISO')
        with self.assertRaisesRegex(ValueError, 'ISO9660'):
            prepare(self.source, self.disc)
        self.assertEqual(list(self.disc.iterdir()), [])

    def test_ambiguous_zip_is_rejected(self):
        archive = self.root / 'two.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.write(self.source, 'one.iso')
            zipped.write(self.source, 'two.iso')
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            prepare(archive, self.disc)
        self.assertFalse((self.disc / '__umd.iso').exists())

    def test_extractor_writes_payload_and_rejects_truncation(self):
        command = [sys.executable, '-I', str(SCRIPTS / 'extract_iso.py'), str(self.source), str(self.disc)]
        subprocess.run(command, check=True, capture_output=True, timeout=10)
        self.assertEqual((self.disc / 'TEST.BIN').read_bytes(), b'test data')
        self.source.write_bytes(iso_fixture(declared_size=2048))
        result = subprocess.run(command, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Truncated ISO', result.stderr)

    def test_missing_audio_banks_fail_before_codegen(self):
        output = self.root / 'audio-output'
        result = subprocess.run([sys.executable, '-I', str(SCRIPTS / 'generate_ncaa_sound.py'),
                                 str(self.disc), str(self.root / 'no-codegen'), str(output)],
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'No EA audio programs found', result.stderr)
        self.assertFalse((output / 'ncaa_sound.cpp').exists())


if __name__ == '__main__':
    unittest.main()
