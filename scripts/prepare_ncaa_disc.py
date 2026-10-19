"""Preserve the raw UMD image required by NCAA's sector reads.

Accept an ISO, a ZIP containing one ISO, or an extracted disc directory that
already contains __umd.iso. An existing destination must match byte-for-byte;
it is never silently replaced, even when both images have the same size.
"""
import argparse
from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import tempfile
import zipfile


def image_digest(stream, output=None):
    header = stream.read(16 * 2048 + 7)
    if header[16 * 2048:16 * 2048 + 7] != b'\x01CD001\x01':
        raise ValueError('The raw UMD source must be an ISO9660 image, not a ZIP or CSO.')
    digest = hashlib.sha256(header)
    size = len(header)
    if output:
        output.write(header)
    while chunk := stream.read(1 << 20):
        digest.update(chunk)
        size += len(chunk)
        if output:
            output.write(chunk)
    return size, digest.hexdigest()


def prepare(source, disc):
    source, disc = Path(source).resolve(), Path(disc).resolve()
    if source.is_dir():
        source = source / '__umd.iso'
    if not source.is_file():
        raise ValueError('Supply the original ISO/ZIP, or an extracted disc containing __umd.iso.')
    destination = disc / '__umd.iso'
    disc.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with ExitStack() as stack:
            if source.suffix.lower() == '.zip':
                archive = stack.enter_context(zipfile.ZipFile(source))
                images = [item for item in archive.infolist()
                          if not item.is_dir() and item.filename.lower().endswith('.iso')]
                if len(images) != 1:
                    raise ValueError('The ZIP must contain exactly one ISO image.')
                stream = stack.enter_context(archive.open(images[0]))
            else:
                stream = stack.enter_context(source.open('rb'))
            if destination.exists():
                size, digest = image_digest(stream)
                with destination.open('rb') as existing:
                    if (size, digest) != image_digest(existing):
                        raise ValueError('Existing __umd.iso differs from the source. Preserve it and use a separate game folder.')
            else:
                with tempfile.NamedTemporaryFile(dir=disc, prefix='.umd-', suffix='.tmp', delete=False) as output:
                    temporary = Path(output.name)
                    size, digest = image_digest(stream, output)
                if destination.exists():
                    raise FileExistsError('The raw UMD destination appeared during preparation; retry to verify it.')
                os.replace(temporary, destination)
                temporary = None
        return size, digest
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('disc', type=Path)
    args = parser.parse_args()
    try:
        size, digest = prepare(args.source, args.disc)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Raw UMD preparation failed: {error}\n')
    print(f'Raw UMD verified: {size} bytes, SHA-256 {digest}')


if __name__ == '__main__':
    main()
