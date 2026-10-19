"""Prepare an audited source snapshot and ZIP locally; never push to GitHub."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
from check_source_tree import ROOT, candidates, audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='build/github-ready', help='new snapshot directory inside build/')
    args = parser.parse_args()
    output = (ROOT / args.output).resolve()
    build = (ROOT / 'build').resolve()
    if not output.is_relative_to(build) or output == build:
        parser.error('output must be a new directory below build/')
    if output.exists():
        parser.error('output already exists; choose a new directory to preserve the earlier snapshot')
    archive = output.with_suffix('.zip')
    if archive.exists():
        parser.error('archive already exists; choose another output name')
    files = candidates(ROOT)
    result = audit(ROOT, files)
    if result['errors'] or not files:
        print(json.dumps(result, indent=2))
        raise SystemExit(1)
    output.mkdir(parents=True)
    manifest = []
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as zipped:
        for relative in files:
            source, target = ROOT / relative, output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            data = target.read_bytes()
            manifest.append(dict(path=relative, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
            info = zipfile.ZipInfo.from_file(target, arcname=f'NCAA-10-PSP-Recomp/{relative}')
            if relative.endswith('.sh'):
                info.external_attr = 0o100755 << 16
            zipped.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
    report = dict(repository='https://github.com/awest813/NCAA-10-PSP-Recomp',
                  snapshot=str(output), archive=str(archive), audit=result, files=manifest)
    report_path = output.with_name(output.name + '-manifest.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(snapshot=str(output), archive=str(archive), manifest=str(report_path), **result), indent=2))


if __name__ == '__main__':
    main()
