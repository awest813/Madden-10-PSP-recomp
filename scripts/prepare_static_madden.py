"""Package an existing Madden NFL 10 web build for static hosting and local ISO reads.

No disc image, extracted assets, saves or server program is copied. The
compiled Madden 10 Wasm and verified boot data are still part of this game build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from verify_madden10 import EXPECTED_EBOOT_SHA256, read_sfo

ROOT = Path(__file__).resolve().parent.parent
SOURCE_URL = 'https://github.com/awest813/NCAA-10-PSP-Recomp/blob/main/'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def build_config(game):
    metadata = read_sfo(game / 'disc/PSP_GAME/PARAM.SFO')
    if metadata.get('DISC_ID') != 'ULUS10441' or 'MADDEN NFL 10' not in metadata.get('TITLE', '').upper():
        raise ValueError('This static package requires Madden NFL 10 PSP US (ULUS10441).')
    if sha256(game / 'EBOOT.BIN') != EXPECTED_EBOOT_SHA256:
        raise ValueError('The decrypted executable does not match the Madden 10 AOT build.')
    encrypted = sha256(game / 'disc/PSP_GAME/SYSDIR/EBOOT.BIN')
    return dict(discId='ULUS10441', title='Madden NFL 10', ebootHashes=sorted({encrypted, EXPECTED_EBOOT_SHA256}),
                bootSha256=EXPECTED_EBOOT_SHA256)


def package_notices():
    notices_path = ROOT / 'THIRD_PARTY_NOTICES.md'
    if not notices_path.is_file():
        return '# Third Party Notices\n'
    notices = notices_path.read_text(encoding='utf-8')
    licenses = {'LICENSE': 'licenses/project-MIT.txt',
                'profile/third_party/at3_standalone/COPYING.LGPLv2.1': 'licenses/ATRAC-LGPL-2.1.txt',
                'profile/third_party/coi_serviceworker/LICENSE': 'licenses/coi-serviceworker-MIT.txt'}
    def link(match):
        target = match.group(1)
        return '](' + (target if '://' in target else licenses.get(target, SOURCE_URL + target)) + ')'
    notices = re.sub(r'\]\(([^)]+)\)', link, notices)
    return notices.replace(
        'Local NCAA disc files, generated translations, game WebAssembly, RAM dumps and savedata are not included.',
        'This static package includes Madden NFL 10 WebAssembly and verified boot data. Full disc images, extracted disc assets, RAM dumps and savedata are not included.')


def package(build, output, config):
    if output.exists():
        raise ValueError('Output already exists; choose a new directory to preserve the earlier package.')
    required = ['index.html', 'index.js', 'index.wasm', 'index.data', 'local_iso.js']
    for name in required:
        if not (build / name).is_file():
            raise ValueError(f'Missing web build file: {name}. Rebuild the current Madden 10 web target first.')
    if (build / 'local_iso.js').read_bytes() != (ROOT / 'profile/web/local_iso.js').read_bytes():
        raise ValueError('The built local ISO helper is stale; rebuild the current Madden 10 web target first.')
    with (build / 'index.wasm').open('rb') as stream:
        if stream.read(8) != b'\0asm\x01\0\0\0':
            raise ValueError('Invalid WebAssembly build.')
    javascript = (build / 'index.js').read_text(encoding='utf-8')
    match = re.search(r'\bloadPackage\(\s*(\{)', javascript)
    if not match:
        raise ValueError('Cannot verify the Emscripten preload metadata for this build.')
    metadata, _ = json.JSONDecoder().raw_decode(javascript, match.start(1))
    if (not isinstance(metadata, dict) or type(metadata.get('remote_package_size')) is not int
            or not isinstance(metadata.get('files'), list)
            or not all(isinstance(item, dict) and isinstance(item.get('filename'), str)
                       for item in metadata['files'])):
        raise ValueError('Invalid preload metadata.')
    files = metadata['files']
    names = [item['filename'] for item in files]
    if len(set(names)) != len(names):
        raise ValueError('Duplicate preload filenames could overwrite verified boot data.')
    if any(item['filename'] not in ('/game/EBOOT.BIN', '/game/disc.manifest') for item in files):
        raise ValueError('Preload contains additional game assets or state; use the source-only boot preload.')
    boot = next((item for item in files if item['filename'] == '/game/EBOOT.BIN'), None)
    if not boot or (build / 'index.data').stat().st_size != metadata['remote_package_size']:
        raise ValueError('Incomplete boot preload.')
    size = metadata['remote_package_size']
    for item in files:
        if not (type(item.get('start')) is int and type(item.get('end')) is int
                and 0 <= item['start'] <= item['end'] <= size):
            raise ValueError('Invalid preload file boundaries.')
    covered = 0
    for item in sorted(files, key=lambda item: (item['start'], item['end'])):
        if item['start'] != covered:
            raise ValueError('Preload coverage contains gaps or overlapping ranges.')
        covered = item['end']
    if covered != size:
        raise ValueError('Preload contains unlisted bytes outside the verified file ranges.')
    with (build / 'index.data').open('rb') as stream:
        stream.seek(boot['start'])
        data = stream.read(boot['end'] - boot['start'])
    if hashlib.sha256(data).hexdigest() != config['bootSha256']:
        raise ValueError('Built preload does not contain the verified Madden 10 executable; rebuild before packaging.')
    html = (build / 'index.html').read_text(encoding='utf-8')
    if 'PspLocalIso.install' not in html:
        raise ValueError('This shell predates local ISO support; rebuild first.')
    runtime_tag = re.compile(r'<script\b[^>]*\bsrc=["\']index\.js["\'][^>]*>\s*</script>')
    if len(runtime_tag.findall(html)) != 1:
        raise ValueError('Expected one external index.js runtime script in the built shell.')
    if html.count('<head>') != 1:
        raise ValueError('Expected one head element in the built shell.')
    launcher_config = dict(config, launcher='iso-only-v1')
    html = html.replace('<head>', '<head>\n<script>window.PSPWEB_LOCAL_ISO = ' + json.dumps(launcher_config) +
                        ';document.documentElement.classList.add("iso-launcher");</script>', 1)
    html = html.replace('<title>PSP Web Recomp</title>', '<title>Madden NFL 10 — Load ISO</title>')
    html = html.replace('<h1>PSP Web Recomp</h1>', '<h1>Madden NFL 10</h1>')
    html = html.replace('PSP games recompiled for the browser, with graphics, audio, controls and local saves.',
                        'Select your Madden NFL 10 PSP US ISO to play. Graphics, audio, controls and saves run in your browser.')
    html = html.replace('<section id="disc-loader" class="disc-loader" hidden',
                        '<section id="disc-loader" class="disc-loader"')
    html = html.replace('<label for="disc-file">Load NCAA Football 10</label>',
                        '<label for="disc-file">Load Madden NFL 10</label>')
    html = html.replace('<input id="disc-file" type="file"', '<input id="disc-file" disabled type="file"')
    html = html.replace('<p id="disc-message" role="status"></p>',
                        '<p id="disc-message" role="status">Preparing browser support. Then choose or drop an uncompressed .iso (ULUS10441). The ISO stays on your device.</p><p>After loading, press <strong>Enter for Start</strong>, or use the on-screen Start control.</p>')
    html = runtime_tag.sub('<script src="static_bootstrap.js" data-runtime="index.js"></script>', html)
    output.mkdir(parents=True)
    for name in required:
        shutil.copy2(build / name, output / name)
    for worker in build.glob('index*.worker.js'):
        shutil.copy2(worker, output / worker.name)
    (output / 'index.html').write_text(html, encoding='utf-8', newline='\n')
    shutil.copy2(ROOT / 'profile/web/static_bootstrap.js', output / 'static_bootstrap.js')
    shutil.copy2(ROOT / 'profile/third_party/coi_serviceworker/coi-serviceworker.js', output / 'coi-serviceworker.js')
    (output / '_headers').write_text('/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n  Cross-Origin-Resource-Policy: same-origin\n', encoding='utf-8')
    (output / '.nojekyll').write_text('', encoding='utf-8')
    (output / 'licenses').mkdir()
    for source, target in [('LICENSE', 'project-MIT.txt'),
                           ('profile/third_party/at3_standalone/COPYING.LGPLv2.1', 'ATRAC-LGPL-2.1.txt'),
                           ('profile/third_party/coi_serviceworker/LICENSE', 'coi-serviceworker-MIT.txt')]:
        if (ROOT / source).is_file():
            shutil.copy2(ROOT / source, output / 'licenses' / target)
    (output / 'THIRD_PARTY_NOTICES.md').write_text(package_notices(), encoding='utf-8', newline='\n')
    (output / 'HOSTING.md').write_text('''# Madden NFL 10 local-ISO static build

Upload this directory unchanged to a static host over HTTPS. The package works
at the domain root or a subdirectory; keep all companion files together.
Netlify and Cloudflare Pages can use `_headers`. Hosts without custom headers
use the included same-origin isolation service worker and reload once before
loading the engine. WebAssembly requires the `application/wasm` MIME type.

This is the unified, ISO-only player launcher. Open index.html through your
host URL, choose or drop a matching Madden NFL 10 PSP US ISO (ULUS10441), then
press Enter for Start. Use an uncompressed `.iso`; ZIP/CSO/ZSO are unsupported
in the browser picker. The ISO is read locally in ranges and is not uploaded.
Choose it again after a reload. Export saves before changing the hosting origin.
Browser isolation is prepared before file selection. The ISO is checked before
the large engine downloads; a wrong image can be replaced on the same page.
Players do not need the separate build importer, translator or development tools.

This is the Madden NFL 10 AOT build, not a generic PSP emulator. It contains
compiled game-specific WebAssembly and verified boot data, but no full ISO,
extracted disc assets or saves.

For a local preview, open a terminal in this package directory and run:
python -m http.server 8623 --bind 127.0.0.1 --directory .

Open http://127.0.0.1:8623/ in your browser. This plain static server also
tests the service-worker fallback without server-added isolation headers.
For header-enabled preview from the source checkout, use scripts/serve.py
with the actual package directory path and an available port.

Use localhost/127.0.0.1 for preview; opening the HTML through file:// will fail.
No /disc mount or backend is required. Do not copy games/ into the host directory.
''', encoding='utf-8', newline='\n')
    manifest = []
    for path in sorted(output.rglob('*')):
        if path.is_file():
            manifest.append(dict(path=path.relative_to(output).as_posix(), bytes=path.stat().st_size, sha256=sha256(path)))
    (output / 'static-manifest.json').write_text(json.dumps(dict(format=1, discId='ULUS10441', files=manifest), indent=2) + '\n', encoding='utf-8')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/web-madden10/profiles/web'))
    parser.add_argument('--game-root', type=Path, default=Path('games/madden10/root'))
    parser.add_argument('--output', type=Path, default=Path('build/static-madden10'))
    args = parser.parse_args()
    output = (ROOT / args.output).resolve()
    build_root = (ROOT / 'build').resolve()
    if not output.is_relative_to(build_root) or output == build_root:
        parser.error('Output must be a new directory below build/.')
    try:
        config = build_config((ROOT / args.game_root).resolve())
        files = package((ROOT / args.build).resolve(), output, config)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'Static preparation failed: {error}\n')
    print(json.dumps(dict(output=str(output), files=len(files), iso_included=False, boot_data_included=True), indent=2))


if __name__ == '__main__':
    main()
