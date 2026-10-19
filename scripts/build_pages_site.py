"""Assemble the GitHub Pages site from tracked Markdown and game-free web tools.

The site publishes project documentation, the inherited demo media, the
launcher under play/ and the browser translator under play/translate/. It never
copies game files or build outputs from the checkout. With --player, README
becomes project.html and a separate verified-release installer supplies the
explicitly published ISO-only player at the root after Jekyll runs.
"""
import argparse
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REPOSITORY = 'awest813/NCAA-10-PSP-Recomp'
ROOT_PAGES = ('README.md', 'ENGINEERING_LOG.md', 'HANDOFF.md', 'AGENTS.md',
              'OTHER_RECOMPS.md', 'GITHUB_READY.md', 'THIRD_PARTY_NOTICES.md')
MEDIA = ('docs/media/demo.webp', 'docs/media/demo.mp4')
LAUNCHER = 'profile/web/launcher'
LAUNCHER_SITE = 'play'
LAUNCHER_FILES = ('index.html', 'launcher.js', 'launcher-sw.js', 'game/index.html',
                  'translate/index.html', 'translate/translate-worker.js', 'translate/translator.js')
# Game-free translator companions: the shared ISO reader and the Wasm module
# built by scripts/build_translator.sh (PSPRecomp code generator + decrypter).
TRANSLATOR_SOURCES = {'translate/local_iso.js': 'profile/web/local_iso.js'}
TRANSLATOR_BUILD_FILES = ('pspweb_translator.js', 'pspweb_translator.wasm')
SITE_TITLE = 'NCAA Football 10 PSP Web Recomp'
SITE_DESCRIPTION = 'Static recompilation of NCAA Football 10 (PSP, ULUS10440) for the web'
NOTICE = ('> This site hosts the project documentation and a [launcher](play/) for your own build. '
          'It does not host the game, compiled game code, disc data or saves: the launcher imports a '
          'package you built locally and opens it with your ISO, all inside your browser. See '
          '[Static hosting]({static}).')
FENCE = re.compile(r'^\s*(```|~~~)')
MARKDOWN_LINK = re.compile(r'(\]\()([^)\s]+)((?:\s+"[^"]*")?\))')
HTML_LINK = re.compile(r'((?:src|href)=")([^"]+)(")')
EXTERNAL = re.compile(r'^(?:[a-z][a-z0-9+.-]*:|//|#|/)', re.IGNORECASE)


def page_sources(root):
    pages = [name for name in ROOT_PAGES if (root / name).is_file()]
    pages += sorted(path.relative_to(root).as_posix() for path in (root / 'docs').glob('*.md'))
    return pages


def page_destination(source):
    return 'index.md' if source == 'README.md' else source


def html_name(destination):
    return str(PurePosixPath(destination).with_suffix('.html'))


class LinkRewriter:
    def __init__(self, root, pages, repository, ref, player=False):
        self.root = root
        self.player = player
        self.site = {source: html_name(page_destination(source)) for source in pages}
        if player:
            self.site['README.md'] = 'project.html'
        self.site.update({name: name for name in MEDIA})
        self.github = f'https://github.com/{repository}'
        self.ref = ref
        self.unresolved = []

    def target(self, source, link):
        if EXTERNAL.match(link):
            return link
        path, hash_mark, fragment = link.partition('#')
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
        anchor = hash_mark + fragment
        if resolved in self.site:
            here = posixpath.dirname(self.site[source])
            return posixpath.relpath(self.site[resolved], here or '.') + anchor
        if resolved.startswith('..') or not (self.root / resolved).exists():
            self.unresolved.append(f'{source}: {link}')
        kind = 'tree' if (self.root / resolved).is_dir() else 'blob'
        return f'{self.github}/{kind}/{self.ref}/{resolved}{anchor}'

    def line(self, source, text):
        # Leave inline code spans untouched: only even-numbered backtick segments are prose.
        segments = text.split('`')
        for index in range(0, len(segments), 2):
            replace = lambda match: match.group(1) + self.target(source, match.group(2)) + match.group(3)
            segments[index] = HTML_LINK.sub(replace, MARKDOWN_LINK.sub(replace, segments[index]))
        return '`'.join(segments)

    def document(self, source, text):
        lines, fenced = [], False
        for line in text.splitlines():
            if FENCE.match(line):
                fenced = not fenced
                lines.append(line)
            else:
                lines.append(line if fenced else self.line(source, line))
        return '\n'.join(lines) + '\n'


def title_of(text, fallback):
    for line in text.splitlines():
        if line.startswith('# '):
            return line[2:].strip()
    return fallback


def render_page(rewriter, source, text):
    if '{% endraw' in text:
        raise SystemExit(f'{source}: contains a Liquid endraw tag that would escape page quoting')
    body = rewriter.document(source, text)
    if source == 'README.md':
        static = rewriter.target(source, 'docs/STATIC_HOSTING.md')
        heading, _, rest = body.partition('\n')
        notice = (f'> Open the [ISO-only player](./). This site includes the compiled NCAA runtime and verified boot preload; '
                  f'bring your own matching ISO, which stays local. See [Static hosting]({static}).'
                  if rewriter.player else NOTICE.format(static=static))
        body = f'{heading}\n\n{notice}\n{rest}'
    title = title_of(text, PurePosixPath(source).stem).replace('"', '\\"')
    # Wrap content in raw so Jekyll's Liquid pass never interprets documentation text.
    return (f'---\nlayout: default\ntitle: "{title}"\n---\n'
            f'{{% raw %}}\n{body}{{% endraw %}}\n')


def config(repository):
    return '\n'.join([
        f'title: "{SITE_TITLE}"',
        f'description: "{SITE_DESCRIPTION}"',
        'theme: jekyll-theme-cayman',
        'markdown: kramdown',
        'kramdown:',
        '  input: GFM',
        '  hard_wrap: false',
        f'repository: {repository}',
        '',
    ])


def current_ref(root):
    if os.environ.get('GITHUB_SHA'):
        return os.environ['GITHUB_SHA']
    try:
        return subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root, check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'main'


def build(root, output, repository, ref, translator=None, player=False):
    if output.exists():
        raise SystemExit(f'{output} already exists; choose a new output directory')
    pages = page_sources(root)
    rewriter = LinkRewriter(root, pages, repository, ref, player)
    written = []
    for source in pages:
        name = 'project.md' if player and source == 'README.md' else page_destination(source)
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        text = (root / source).read_text(encoding='utf-8')
        destination.write_text(render_page(rewriter, source, text), encoding='utf-8', newline='\n')
        written.append(name)
    for name in MEDIA:
        if (root / name).is_file():
            (output / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, output / name)
            written.append(name)
    for name in LAUNCHER_FILES:
        destination = output / LAUNCHER_SITE / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / LAUNCHER / name, destination)
        written.append(f'{LAUNCHER_SITE}/{name}')
    for name, source in TRANSLATOR_SOURCES.items():
        shutil.copyfile(root / source, output / LAUNCHER_SITE / name)
        written.append(f'{LAUNCHER_SITE}/{name}')
    if translator is not None:
        for name in TRANSLATOR_BUILD_FILES:
            if not (translator / name).is_file():
                raise SystemExit(f'{translator / name} is missing; run scripts/build_translator.sh')
            shutil.copyfile(translator / name, output / LAUNCHER_SITE / 'translate' / name)
            written.append(f'{LAUNCHER_SITE}/translate/{name}')
    (output / '_config.yml').write_text(config(repository), encoding='utf-8', newline='\n')
    written.append('_config.yml')
    return sorted(written), rewriter.unresolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/pages-site')
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY', DEFAULT_REPOSITORY))
    parser.add_argument('--ref', help='commit or branch for links to source files (default: HEAD)')
    parser.add_argument('--strict', action='store_true', help='fail on links to files absent from the checkout')
    parser.add_argument('--translator', type=Path, help='translator build directory to publish (scripts/build_translator.sh)')
    parser.add_argument('--player', action='store_true', help='reserve index.html for the verified player; put README at project.html')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build') or output == ROOT / 'build':
        raise SystemExit('the Pages site output must be a new directory below build/')
    written, unresolved = build(ROOT, output, args.repository, args.ref or current_ref(ROOT),
                                args.translator.resolve() if args.translator else None, args.player)
    for entry in unresolved:
        print(f'unresolved link: {entry}')
    print(f'wrote {len(written)} files to {output}')
    raise SystemExit(1 if args.strict and unresolved else 0)


if __name__ == '__main__':
    main()
