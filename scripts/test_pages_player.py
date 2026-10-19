"""Asset-free release integrity and Pages overlay regressions."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from install_pages_player import ROOT, REQUIRED, install, read_config


class PlayerTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'build/pages-player-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.site = self.root / 'site'
        self.site.mkdir()
        (self.site / 'project.html').write_text('preserved documentation')
        self.files = {name: b'synthetic' for name in REQUIRED - {'static-manifest.json'}}
        self.files['index.wasm'] = b'\0asm\1\0\0\0'
        self.files['index.html'] = b'<script>window.PSPWEB_LOCAL_ISO={discId:"ULUS10440",launcher:"iso-only-v1"}</script><script src="static_bootstrap.js"></script>'
        self.config = dict(format=1, discId='ULUS10440', tag='player-test', asset='player.zip', sourceCommit='a' * 40)
        self.archive = self.root / 'player.zip'

    def pack(self, extra=None, mutate=None):
        files = dict(self.files)
        manifest = dict(format=1, discId='ULUS10440', files=[dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) for name, data in sorted(files.items())])
        if mutate:
            mutate(manifest)
        files['static-manifest.json'] = json.dumps(manifest).encode()
        with zipfile.ZipFile(self.archive, 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, data)
            if extra:
                archive.writestr(extra, b'excluded')
        self.config['archiveSha256'] = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def test_install_preserves_docs_and_records_provenance(self):
        self.pack()
        self.assertEqual(set(install(self.archive, self.site, self.config)), REQUIRED)
        self.assertEqual((self.site / 'project.html').read_text(), 'preserved documentation')
        self.assertEqual((self.site / 'index.wasm').read_bytes(), self.files['index.wasm'])
        self.assertEqual(json.loads((self.site / 'player-provenance.json').read_text()), self.config)
        with self.assertRaisesRegex(ValueError, 'replace existing'):
            install(self.archive, self.site, self.config)

    def test_wrong_archive_hash_preserves_site(self):
        self.pack()
        self.config['archiveSha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            install(self.archive, self.site, self.config)
        self.assertEqual(list(self.site.iterdir()), [self.site / 'project.html'])

    def test_existing_provenance_is_not_overwritten(self):
        self.pack()
        provenance = self.site / 'player-provenance.json'
        provenance.write_text('existing deployment')
        with self.assertRaisesRegex(ValueError, 'replace existing'):
            install(self.archive, self.site, self.config)
        self.assertEqual(provenance.read_text(), 'existing deployment')
        self.assertFalse((self.site / 'index.html').exists())

    def test_disc_traversal_and_unlisted_files_rejected_before_extract(self):
        for name in ('game.iso', '../outside.js', '/absolute.js', 'extra.js'):
            with self.subTest(name=name):
                self.pack(extra=name)
                with self.assertRaisesRegex(ValueError, 'Unexpected'):
                    install(self.archive, self.site, self.config)
                self.assertFalse((self.site / 'index.html').exists())

    def test_bad_companion_hash_and_missing_coverage_rejected(self):
        for mutate in (lambda m: m['files'][0].update(sha256='0' * 64), lambda m: m['files'].pop()):
            self.pack(mutate=mutate)
            with self.assertRaisesRegex(ValueError, 'hash/size|cover every'):
                install(self.archive, self.site, self.config)
            self.assertFalse((self.site / 'index.html').exists())

    def test_wrong_engine_and_non_launcher_rejected(self):
        for name in ('index.wasm', 'index.html'):
            original = self.files[name]
            self.files[name] = b'invalid'
            self.pack()
            with self.assertRaisesRegex(ValueError, 'WebAssembly|ISO-only launcher'):
                install(self.archive, self.site, self.config)
            self.files[name] = original

    def test_pinned_metadata_rejects_shell_paths(self):
        self.pack()
        file = self.root / 'config.json'
        file.write_text(json.dumps(self.config))
        self.assertEqual(read_config(file), self.config)
        for field in ('tag', 'asset'):
            changed = dict(self.config, **{field: '../unexpected;command'})
            file.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'Invalid pinned'):
                read_config(file)


if __name__ == '__main__':
    unittest.main()
