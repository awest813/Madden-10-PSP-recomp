"""GitHub Pages documentation site checks, using a synthetic checkout."""
from pathlib import Path
import tempfile
import unittest

from build_pages_site import LAUNCHER, LAUNCHER_FILES, ROOT, TRANSLATOR_BUILD_FILES, build

REPOSITORY = 'example/NCAA-10-PSP-Recomp'


class PagesSiteTests(unittest.TestCase):
    def setUp(self):
        root = ROOT / 'build/pages-site-tests'
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.checkout = Path(self.temporary.name) / 'checkout'
        self.output = Path(self.temporary.name) / 'site'
        self.write('README.md', '# Title\n\nSee [hosting](docs/STATIC_HOSTING.md#host-requirements), '
                   '[license](LICENSE), [profile](profile) and <img src="docs/media/demo.webp">.\n\n'
                   'Keep `[code](docs/STATIC_HOSTING.md)` literal.\n\n'
                   '```text\n[fenced](HANDOFF.md) {{ liquid }}\n```\n')
        self.write('HANDOFF.md', '# Handoff\n\n[Build](README.md#build) and [missing](build/x.log).\n')
        self.write('docs/STATIC_HOSTING.md', '# Static hosting\n\n[Log](../HANDOFF.md) [web](https://example.com)\n')
        self.write('docs/media/demo.webp', 'image')
        self.write('LICENSE', 'MIT')
        self.write('profile/web/index.wasm', 'game code')
        self.write('games/ncaa10/root/EBOOT.BIN', 'game executable')
        self.write('build/static-ncaa10/index.data', 'boot preload')
        for name in LAUNCHER_FILES:
            self.write(f'{LAUNCHER}/{name}', (ROOT / LAUNCHER / name).read_text(encoding='utf-8'))
        self.write('profile/web/local_iso.js', '// iso reader')

    def write(self, relative, text):
        path = self.checkout / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def read(self, relative):
        return (self.output / relative).read_text(encoding='utf-8')

    def test_site_contains_only_documentation_media_and_launcher(self):
        written, _ = build(self.checkout, self.output, REPOSITORY, 'abc123')
        self.assertEqual(written, ['HANDOFF.md', '_config.yml', 'docs/STATIC_HOSTING.md',
                                   'docs/media/demo.webp', 'index.md', 'play/game/index.html',
                                   'play/index.html', 'play/launcher-sw.js', 'play/launcher.js',
                                   'play/translate/index.html', 'play/translate/local_iso.js',
                                   'play/translate/translate-worker.js', 'play/translate/translator.js'])
        for name in LAUNCHER_FILES:
            self.assertEqual(self.read(f'play/{name}'), (ROOT / LAUNCHER / name).read_text(encoding='utf-8'))
        on_disk = sorted(path.relative_to(self.output).as_posix()
                         for path in self.output.rglob('*') if path.is_file())
        self.assertEqual(on_disk, written)
        self.assertIn(f'repository: {REPOSITORY}', self.read('_config.yml'))

    def test_translator_build_is_published_only_when_given(self):
        translator = Path(self.temporary.name) / 'translator-web'
        translator.mkdir()
        (translator / 'pspweb_translator.js').write_text('// module')
        with self.assertRaisesRegex(SystemExit, 'pspweb_translator.wasm is missing'):
            build(self.checkout, self.output, REPOSITORY, 'abc123', translator)
        (translator / 'pspweb_translator.wasm').write_bytes(b'\0asm\1\0\0\0')
        written, _ = build(self.checkout, self.output.with_name('with-translator'), REPOSITORY, 'abc123', translator)
        for name in TRANSLATOR_BUILD_FILES:
            self.assertIn(f'play/translate/{name}', written)

    def test_links_point_at_site_pages_or_pinned_source(self):
        _, unresolved = build(self.checkout, self.output, REPOSITORY, 'abc123')
        index = self.read('index.md')
        self.assertIn('[hosting](docs/STATIC_HOSTING.html#host-requirements)', index)
        self.assertIn(f'[license](https://github.com/{REPOSITORY}/blob/abc123/LICENSE)', index)
        self.assertIn(f'[profile](https://github.com/{REPOSITORY}/tree/abc123/profile)', index)
        self.assertIn('<img src="docs/media/demo.webp">', index)
        self.assertIn('`[code](docs/STATIC_HOSTING.md)`', index)
        self.assertIn('[fenced](HANDOFF.md) {{ liquid }}', index)
        self.assertIn('[Build](index.html#build)', self.read('HANDOFF.md'))
        hosting = self.read('docs/STATIC_HOSTING.md')
        self.assertIn('[Log](../HANDOFF.html)', hosting)
        self.assertIn('[web](https://example.com)', hosting)
        self.assertEqual(unresolved, ['HANDOFF.md: build/x.log'])

    def test_pages_are_liquid_quoted_and_carry_the_documentation_notice(self):
        build(self.checkout, self.output, REPOSITORY, 'abc123')
        index = self.read('index.md')
        self.assertTrue(index.startswith('---\nlayout: default\ntitle: "Title"\n---\n{% raw %}\n# Title\n\n> '))
        self.assertIn('does not host the game', index)
        self.assertIn('[launcher](play/)', index)
        self.assertIn('(docs/STATIC_HOSTING.html)', index)
        self.assertTrue(index.endswith('{% endraw %}\n'))
        self.assertNotIn('does not host the game', self.read('HANDOFF.md'))

    def test_existing_output_and_liquid_escapes_are_rejected(self):
        build(self.checkout, self.output, REPOSITORY, 'abc123')
        with self.assertRaisesRegex(SystemExit, 'already exists'):
            build(self.checkout, self.output, REPOSITORY, 'abc123')
        self.write('HANDOFF.md', '# Handoff\n\n{% endraw %}{{ site }}\n')
        with self.assertRaisesRegex(SystemExit, 'endraw'):
            build(self.checkout, self.output.with_name('again'), REPOSITORY, 'abc123')

    def test_repository_documentation_links_resolve(self):
        output = Path(self.temporary.name) / 'repository-site'
        _, unresolved = build(ROOT, output, REPOSITORY, 'abc123')
        self.assertEqual(unresolved, [])

    def test_player_reserves_the_homepage_and_rewrites_readme_links(self):
        written, _ = build(self.checkout, self.output, REPOSITORY, 'abc123', player=True)
        self.assertIn('project.md', written)
        self.assertNotIn('index.md', written)
        self.assertIn('[Build](project.html#build)', self.read('HANDOFF.md'))
        self.assertIn('[ISO-only player](./)', self.read('project.md'))
        self.assertNotIn('does not host the game', self.read('project.md'))


if __name__ == '__main__':
    unittest.main()
