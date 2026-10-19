"""Check the publication file list without reading ignored local game data."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ROOT_FILES = {
    '.gitignore', '.gitattributes', 'LICENSE', 'README.md', 'AGENTS.md',
    'ENGINEERING_LOG.md', 'HANDOFF.md', 'OTHER_RECOMPS.md',
    'GITHUB_READY.md', 'THIRD_PARTY_NOTICES.md',
}
SOURCE_DIRS = ('docs', 'patches', 'profile', 'scripts', '.github')
BLOCKED_PARTS = {'games', 'build', 'PSPRecomp', 'tools', 'generated',
                 '__pycache__', '.git', '.codex', '.agents', '.claude', 'ms0', 'memstick'}
TEXT_EXTENSIONS = {'.md', '.txt', '.cpp', '.hpp', '.h', '.c', '.py', '.mjs',
                   '.js', '.html', '.css', '.sh', '.ps1', '.patch', '.yml', '.yaml', '.json'}
SPECIAL_NAMES = {'CMakeLists.txt', 'COPYING.LGPLv2.1', 'SOURCE_COMMIT', 'LICENSE'}
DEMO_FILES = {'docs/media/demo.webp', 'docs/media/demo.mp4'}
SECRET_PATTERNS = {
    'private-key header': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'GitHub token': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'AWS access-key ID': re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
}


def allowed(relative):
    path = Path(relative)
    if any(part in BLOCKED_PARTS for part in path.parts):
        return False
    if relative in ROOT_FILES or relative in DEMO_FILES:
        return True
    return (path.parts[0] in SOURCE_DIRS and
            (path.suffix.lower() in TEXT_EXTENSIONS or path.name in SPECIAL_NAMES))


def candidates(root):
    files = [name for name in sorted(ROOT_FILES) if (root / name).is_file()]
    for directory in SOURCE_DIRS:
        for path in (root / directory).rglob('*'):
            if path.is_file() and not path.is_symlink():
                relative = path.relative_to(root).as_posix()
                if allowed(relative):
                    files.append(relative)
    return sorted(set(files))


def audit(root, files):
    errors, total = [], 0
    for relative in files:
        path = root / relative
        if not allowed(relative) or path.is_symlink() or not path.is_file():
            errors.append({'file': relative, 'reason': 'outside source publication policy'})
            continue
        size = path.stat().st_size
        total += size
        if size >= 95 * 1024 * 1024:
            errors.append({'file': relative, 'reason': 'oversized file'})
        if relative not in DEMO_FILES:
            try:
                text = path.read_text(encoding='utf-8')
            except UnicodeError:
                errors.append({'file': relative, 'reason': 'unexpected binary or non-UTF-8 source'})
                continue
            if '\0' in text:
                errors.append({'file': relative, 'reason': 'unexpected binary source'})
            for label, pattern in SECRET_PATTERNS.items():
                if pattern.search(text):
                    errors.append({'file': relative, 'reason': label})
    return dict(file_count=len(files), total_bytes=total, errors=errors,
                note='Allowlist and selected credential patterns; not a complete secret detector.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tracked', action='store_true', help='check every Git index entry, including forbidden files')
    args = parser.parse_args()
    if args.tracked:
        output = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT, check=True, capture_output=True).stdout
        files = [name.decode('utf-8') for name in output.split(b'\0') if name]
    else:
        files = candidates(ROOT)
    result = audit(ROOT, files)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result['errors']) or not files)


if __name__ == '__main__':
    main()
