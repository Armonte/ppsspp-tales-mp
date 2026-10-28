#!/usr/bin/env python3
"""Diff two PPSSPP savestate buffers (as written by the rollback diagnostics) section by section.

Section boundaries are found by scanning for the section markers PointerWrap::Section writes: a 16-byte
zero-padded title (strncpy) followed by an int32 version. Titles come from every p.Section("...") in the source.

usage: state_diff.py <repo root> <first.bin> <resim.bin> [max ranges]
"""
import os, re, struct, sys

def section_titles(root):
    titles = set()
    for d in ('Core', 'GPU', 'Common', 'UI'):
        for dp, _, fns in os.walk(os.path.join(root, d)):
            for fn in fns:
                if fn.endswith(('.cpp', '.h')):
                    with open(os.path.join(dp, fn), encoding='utf-8', errors='ignore') as f:
                        titles.update(re.findall(r'\.Section\(\s*"([^"]+)"', f.read()))
    return titles

def find_sections(buf, titles):
    hits = []
    for t in titles:
        key = t.encode()[:16].ljust(16, b'\0')
        i = buf.find(key)
        while i >= 0:
            if i + 20 <= len(buf):
                v = struct.unpack_from('<i', buf, i + 16)[0]
                if 0 < v < 64:
                    hits.append((i, t, v))
            i = buf.find(key, i + 1)
    hits.sort()
    return hits

def section_at(hits, off):
    lo, hi, best = 0, len(hits) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if hits[mid][0] <= off:
            best = hits[mid]; lo = mid + 1
        else:
            hi = mid - 1
    return best

def main():
    root, a_path, b_path = sys.argv[1:4]
    max_ranges = int(sys.argv[4]) if len(sys.argv) > 4 else 60
    a = open(a_path, 'rb').read(); b = open(b_path, 'rb').read()
    titles = section_titles(root)
    hits = find_sections(a, titles)
    # Skip hits inside RAM/VRAM: only trust markers outside the big Memory section's payload.
    mem = next((h for h in hits if h[1] == 'Memory'), None)
    if mem:
        mem_end = mem[0] + 32 * 1024 * 1024 + 2 * 1024 * 1024 + 16 * 1024 + 64
        hits = [h for h in hits if h[0] <= mem[0] or h[0] >= mem_end]
    print(f'sizes {len(a)} / {len(b)}, {len(hits)} section markers')
    n = min(len(a), len(b)); ranges = []; i = 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            ranges.append((i, j)); i = j
        else:
            i += 1
    print(f'{len(ranges)} differing ranges, {sum(e - s for s, e in ranges)} bytes')
    by_section = {}
    for s, e in ranges:
        sec = section_at(hits, s)
        key = f'{sec[1]} v{sec[2]} @+{sec[0]:#x}' if sec else '(before any section)'
        by_section.setdefault(key, []).append((s, e))
    for key, rs in by_section.items():
        print(f'== {key}: {len(rs)} ranges, {sum(e - s for s, e in rs)} bytes')
        for s, e in rs[:max_ranges]:
            sec = section_at(hits, s)
            rel = s - sec[0] if sec else s
            print(f'   +{s:#010x} (+{rel:#x} in section) len {e - s}: {a[s:min(e, s + 16)].hex()} -> {b[s:min(e, s + 16)].hex()}')

main()
