"""Writes the streamed-disc manifest: manifest.py <disc_dir> <out_file>
Lines are "D <path>" for directories and "F <size> <path>" for files."""
import os, sys
root, out = sys.argv[1], sys.argv[2]
lines = []
for directory, dirs, files in os.walk(root):
    dirs.sort()
    rel = os.path.relpath(directory, root).replace(os.sep, '/')
    if rel != '.':
        lines.append(f"D {rel}")
    for name in sorted(files):
        path = os.path.join(directory, name)
        lines.append(f"F {os.path.getsize(path)} {os.path.relpath(path, root).replace(os.sep, '/')}")
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
print(f"{len(lines)} entries -> {out}")
