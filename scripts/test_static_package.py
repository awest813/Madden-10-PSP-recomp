"""Static package safety and wiring checks, using synthetic boot bytes."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from prepare_static_ncaa import ROOT, package, sha256


class StaticPackageTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parent.parent / 'build/static-package-tests'
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'input'
        self.build.mkdir()
        self.output = self.root / 'output'
        self.boot = b'synthetic boot bytes'
        self.config = dict(discId='ULUS10440', ebootHashes=['synthetic'],
                           bootSha256=hashlib.sha256(self.boot).hexdigest())
        self.metadata = dict(remote_package_size=len(self.boot), files=[
            dict(filename='/game/EBOOT.BIN', start=0, end=len(self.boot))])
        self.write_metadata()
        (self.build / 'index.data').write_bytes(self.boot)
        (self.build / 'index.wasm').write_bytes(b'\0asm\x01\0\0\0')
        (self.build / 'local_iso.js').write_bytes((ROOT / 'profile/web/local_iso.js').read_bytes())
        (self.build / 'index.html').write_text(
            '<head><title>PSP Web Recomp</title></head><body>'
            '<h1>PSP Web Recomp</h1><script>PspLocalIso.install();</script>'
            '<script src="index.js"></script></body>')
        (self.build / 'private.iso').write_bytes(b'do not copy')

    def write_metadata(self):
        (self.build / 'index.js').write_text('loadPackage(' + json.dumps(self.metadata) + ');')

    def test_package_is_self_contained_and_does_not_copy_unlisted_files(self):
        package(self.build, self.output, self.config)
        self.assertFalse((self.output / 'private.iso').exists())
        html = (self.output / 'index.html').read_text(encoding='utf-8')
        self.assertIn('window.PSPWEB_LOCAL_ISO', html)
        self.assertIn('"launcher": "iso-only-v1"', html)
        self.assertIn('data-runtime="index.js"', html)
        self.assertIn('<h1>NCAA Football 10</h1>', html)
        self.assertTrue((self.output / 'coi-serviceworker.js').is_file())
        self.assertTrue((self.output / 'licenses/coi-serviceworker-MIT.txt').is_file())
        notices = (self.output / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
        self.assertIn('(licenses/project-MIT.txt)', notices)
        self.assertNotIn('](profile/', notices)
        guide = (self.output / 'HOSTING.md').read_text(encoding='utf-8')
        self.assertIn('--directory .', guide)
        self.assertIn('unified, ISO-only player launcher', guide)
        manifest = json.loads((self.output / 'static-manifest.json').read_text())
        for item in manifest['files']:
            self.assertEqual(sha256(self.output / item['path']), item['sha256'])
        with self.assertRaisesRegex(ValueError, 'already exists'):
            package(self.build, self.output, self.config)

    def test_additional_preloaded_assets_are_rejected(self):
        self.metadata['files'].append(dict(filename='/game/private.bin', start=0, end=1))
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, 'additional game assets'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_wrong_boot_and_invalid_boundaries_are_rejected(self):
        (self.build / 'index.data').write_bytes(b'!' * len(self.boot))
        with self.assertRaisesRegex(ValueError, 'verified NCAA executable'):
            package(self.build, self.output, self.config)
        self.metadata['files'][0]['end'] = -1
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, 'boundaries'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_invalid_wasm_is_rejected(self):
        (self.build / 'index.wasm').write_bytes(b'not wasm')
        with self.assertRaisesRegex(ValueError, 'WebAssembly'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_stale_browser_helper_is_rejected(self):
        (self.build / 'local_iso.js').write_text('// stale helper')
        with self.assertRaisesRegex(ValueError, 'helper is stale'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_duplicate_preload_names_are_rejected_before_runtime_overwrite(self):
        # Emscripten would create this filename again, replacing the first,
        # hash-verified executable with the second entry's different bytes.
        self.metadata['files'].append(dict(filename='/game/EBOOT.BIN', start=len(self.boot), end=2 * len(self.boot)))
        self.metadata['remote_package_size'] = 2 * len(self.boot)
        self.write_metadata()
        (self.build / 'index.data').write_bytes(self.boot + b'!' * len(self.boot))
        with self.assertRaisesRegex(ValueError, 'Duplicate preload'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_unlisted_preload_bytes_are_rejected(self):
        self.metadata['remote_package_size'] += 6
        self.write_metadata()
        (self.build / 'index.data').write_bytes(self.boot + b'hidden')
        with self.assertRaisesRegex(ValueError, 'unlisted|coverage'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())

    def test_overlapping_preload_ranges_are_rejected(self):
        self.metadata['files'].append(dict(filename='/game/disc.manifest', start=0, end=1))
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, 'overlap|coverage'):
            package(self.build, self.output, self.config)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
