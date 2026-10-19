# Converts a binary PPM (P6) to PNG, optionally scaled: ppm2png.py in.ppm out.png [scale]
import sys, zlib, struct
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); pixels = parts[3]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = []
for y in range(h):
    row = pixels[y * w * 3:(y + 1) * w * 3]
    row = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
    rows.extend([b'\x00' + row] * scale)
def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
