"""Read-only EA MAIN heap audit of a 32 or 64 MiB PSP RAM capture.

Addresses are guest addresses. Allocation sizes in allocated nodes are requested
payload bytes; sizes in free nodes include the 16-byte header. Physical spans
come from the next-physical pointer, never from a guessed alignment rule.
"""
import argparse
import json
import re
import struct
from pathlib import Path

BASE = 0x08000000
HEAP_TABLE = 0x08EC5AF0


def ledger(path):
    result = []
    for line in Path(path).read_text(errors="replace").splitlines():
        if "[ncaa-heap-capture]" in line and "-target.ram" in line:
            break
        if "event=alloc " in line or "EA_ALLOC " in line:
            fields = dict(re.findall(r"([\w-]+)=((?:0x)?[0-9a-fA-F]{8})\b", line))
            aliases = {"ptr": "pointer", "align": "alignment", "return": "continuation"}
            result.append({aliases.get(k, k): int(v, 16) for k, v in fields.items()})
    return result


def audit(path, heap_index=0, history=None):
    data = Path(path).read_bytes()
    if len(data) not in (0x02000000, 0x04000000):
        raise ValueError("Expected a complete 32 or 64 MiB PSP RAM image")

    def words(addr, count):
        offset = addr - BASE
        if offset < 0 or offset + 4 * count > len(data):
            raise ValueError(f"Out-of-range structure at {addr:08X}")
        return struct.unpack_from("<" + "I" * count, data, offset)

    heap = words(HEAP_TABLE + heap_index * 4, 1)[0]
    header = words(heap, 16)
    start, end = header[2:4]
    blocks, errors, seen = [], [], set()
    address, previous = start, 0
    by_pointer = {e["pointer"]: e for e in (history or []) if "pointer" in e}
    while address:
        if address in seen or len(seen) >= 100000:
            errors.append(f"Physical-chain cycle/limit at {address:08X}")
            break
        seen.add(address)
        flags, size, following, back = words(address, 4)
        if back != previous:
            errors.append(f"Back-link mismatch at {address:08X}: {back:08X} != {previous:08X}")
        kind = "free" if flags == (0x40004246 | heap_index << 16) else "allocated" if flags & 0xFFFF == 0x424D else "control"
        span = following - address if following else 0
        item = dict(address=address, flags=flags, kind=kind, size_field=size,
                    next_physical=following, previous_physical=back, span=span)
        if following and (following <= address or following > end):
            errors.append(f"Invalid next physical pointer at {address:08X}")
            blocks.append(item)
            break
        if kind == "free":
            item.update(usable=size - 16, next_free=words(address + 16, 1)[0],
                        previous_free=words(address + 20, 1)[0])
            if size != span:
                errors.append(f"Free size/span mismatch at {address:08X}")
        elif kind == "allocated":
            item.update(pointer=address + 16, requested=size, header_and_padding=span-size)
            if size + 16 > span:
                errors.append(f"Allocated payload exceeds physical span at {address:08X}")
            if address + 16 in by_pointer and by_pointer[address + 16].get("size") == size:
                item["allocation_event"] = by_pointer[address + 16]
        blocks.append(item)
        if address == end:
            if following:
                errors.append("End sentinel has a next pointer")
            break
        previous, address = address, following
    if not blocks or blocks[-1]["address"] != end:
        errors.append("Physical chain does not reach end sentinel")
    frees = [b for b in blocks if b["kind"] == "free"]
    allocated = [b for b in blocks if b["kind"] == "allocated"]
    adjacent = [[a["address"], b["address"]] for a, b in zip(blocks, blocks[1:])
                if a["kind"] == b["kind"] == "free"]
    sentinel, free_seen, free_address = heap + 16, set(), header[8]
    free_previous = sentinel
    free_by_address = {b["address"]: b for b in frees}
    while free_address != sentinel:
        if free_address in free_seen or free_address not in free_by_address:
            errors.append(f"Invalid free-list entry/cycle at {free_address:08X}")
            break
        free_seen.add(free_address)
        b = free_by_address[free_address]
        if b["previous_free"] != free_previous:
            errors.append(f"Free-list back-link mismatch at {free_address:08X}")
        free_previous, free_address = free_address, b["next_free"]
    if free_previous != header[9]:
        errors.append("Free-list tail does not match heap header")
    if free_seen != set(free_by_address):
        errors.append("Free-list membership differs from physical free nodes")
    summary = dict(heap=heap, start=start, end=end, physical_bytes=end-start,
                   block_count=len(blocks), allocated_count=len(allocated), free_count=len(frees),
                   requested_allocated_bytes=sum(b["requested"] for b in allocated),
                   allocated_header_and_padding=sum(b["header_and_padding"] for b in allocated),
                   control_span=sum(b["span"] for b in blocks if b["kind"] == "control"),
                   total_free_span=sum(b["span"] for b in frees),
                   total_free_usable=sum(b["usable"] for b in frees),
                   largest_free_span=max((b["span"] for b in frees), default=0),
                   largest_free_usable=max((b["usable"] for b in frees), default=0),
                   adjacent_free_pairs=adjacent, integrity_errors=errors)
    return dict(source=str(Path(path).resolve()), summary=summary, header=list(header), blocks=blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ram")
    parser.add_argument("--heap-index", type=int, default=0)
    parser.add_argument("--ledger")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    report = audit(args.ram, args.heap_index, ledger(args.ledger) if args.ledger else None)
    Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))
    if report["summary"]["integrity_errors"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
