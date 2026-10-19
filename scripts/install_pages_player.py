"""Verify a pinned release package and install its ISO-only player into Pages.

The source repository contains this installer and provenance, not game binaries.
The explicitly published package has compiled NCAA/boot data, never an ISO/save.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parent.parent
REQUIRED = {'index.html', 'index.js', 'index.wasm', 'index.data', 'local_iso.js',
            'static_bootstrap.js', 'coi-serviceworker.js', 'static-manifest.json',
            'THIRD_PARTY_NOTICES.md', 'HOSTING.md', '_headers', '.nojekyll',
            'licenses/project-MIT.txt', 'licenses/ATRAC-LGPL-2.1.txt',
            'licenses/coi-serviceworker-MIT.txt'}


def digest(stream):
    result = hashlib.sha256()
    while chunk := stream.read(1 << 20):
        result.update(chunk)
    return result.hexdigest()


def read_config(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    if (data.get('format') != 1 or data.get('discId') != 'ULUS10440'
            or not re.fullmatch(r'[0-9a-f]{64}', data.get('archiveSha256', ''))
            or not re.fullmatch(r'[0-9a-f]{40}', data.get('sourceCommit', ''))
            or not re.fullmatch(r'[A-Za-z0-9_.-]+', data.get('asset', ''))
            or not re.fullmatch(r'[A-Za-z0-9_.-]+', data.get('tag', ''))):
        raise ValueError('Invalid pinned player provenance.')
    return data


def install(archive, site, config):
    if not site.is_dir():
        raise ValueError('Pages output must already exist; assemble documentation with --player first.')
    with archive.open('rb') as stream:
        if digest(stream) != config['archiveSha256']:
            raise ValueError('Player release archive SHA-256 does not match the pinned version.')
    with zipfile.ZipFile(archive) as package:
        members = package.infolist()
        names = [item.filename for item in members]
        # A strict allowlist rejects traversal, extra disc files and duplicate
        # members before extracting anything. Emscripten may add worker scripts.
        if (len(names) > 64 or len(set(names)) != len(names) or not REQUIRED.issubset(names)
                or any(name not in REQUIRED and not re.fullmatch(r'index[\w.-]*\.worker\.js', name)
                       for name in names)
                or any(item.file_size > 256 * 1024 * 1024 or item.is_dir() for item in members)
                or sum(item.file_size for item in members) > 512 * 1024 * 1024):
            raise ValueError('Unexpected player package files or sizes.')
        if package.getinfo('static-manifest.json').file_size > 1024 * 1024:
            raise ValueError('Player manifest is too large.')
        manifest = json.loads(package.read('static-manifest.json'))
        if manifest.get('format') != 1 or manifest.get('discId') != 'ULUS10440':
            raise ValueError('Invalid player manifest identity.')
        files = manifest.get('files')
        if not isinstance(files, list) or not all(isinstance(item, dict) for item in files):
            raise ValueError('Invalid player manifest files.')
        listed = [item.get('path') for item in files]
        if len(set(listed)) != len(listed) or set(listed) != set(names) - {'static-manifest.json'}:
            raise ValueError('Player manifest must cover every package companion exactly once.')
        for item in files:
            info = package.getinfo(item['path'])
            with package.open(info) as stream:
                if (type(item.get('bytes')) is not int or item['bytes'] != info.file_size
                        or digest(stream) != item.get('sha256')):
                    raise ValueError('Player companion hash/size mismatch: ' + item['path'])
        with package.open('index.wasm') as stream:
            if stream.read(8) != b'\0asm\x01\0\0\0':
                raise ValueError('Player engine is not WebAssembly.')
        html = package.read('index.html').decode('utf-8')
        if ('window.PSPWEB_LOCAL_ISO' not in html or 'iso-only-v1' not in html
                or 'ULUS10440' not in html or 'static_bootstrap.js' not in html):
            raise ValueError('Player page is not the verified ISO-only launcher.')
        targets = [site.joinpath(*PurePosixPath(name).parts) for name in names]
        if any(not target.resolve().is_relative_to(site.resolve()) for target in targets):
            raise ValueError('Player destination escapes the Pages output.')
        provenance = site / 'player-provenance.json'
        if any(target.exists() or target.is_symlink() for target in [*targets, provenance]):
            raise ValueError('Player would replace existing Pages files; assemble with --player into a fresh output.')
        for name, target in zip(names, targets):
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(name) as source, target.open('xb') as destination:
                shutil.copyfileobj(source, destination)
    with provenance.open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(config, indent=2) + '\n')
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--site', required=True, type=Path)
    parser.add_argument('--config', type=Path, default=ROOT / 'docs/PAGES_PLAYER.json')
    args = parser.parse_args()
    site = args.site.resolve()
    if not site.is_relative_to(ROOT / 'build') or site == ROOT / 'build':
        parser.error('Pages output must be below build/.')
    files = install(args.archive, site, read_config(args.config))
    print(f'Installed {len(files)} verified player files for ULUS10440; no ISO or savedata.')


if __name__ == '__main__':
    main()
