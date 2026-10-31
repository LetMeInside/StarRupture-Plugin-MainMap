"""Pinned A2.2 inputs and PE resources; stdlib only, never loads plugin code."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
PIN = Path(__file__).with_name('rendering-inputs.json')
BASELINE = ROOT / 'Content/Cook/material-baseline.json'
RESOURCE_IDS = {30101: 'pak', 30102: 'utoc', 30103: 'ucas'}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def check(path, size, sha):
    data = path.read_bytes()
    if len(data) != size or digest(data) != sha:
        raise ValueError(f'Input identity mismatch: {path}')
    return data

def resources(path):
    """Read the PE resource tree as data, including numeric type/name/language."""
    data = path.read_bytes()
    def u16(p): return struct.unpack_from('<H', data, p)[0]
    def u32(p): return struct.unpack_from('<I', data, p)[0]
    if data[:2] != b'MZ': raise ValueError('Not a PE image')
    pe = u32(0x3c)
    if data[pe:pe+4] != b'PE\0\0': raise ValueError('Not a PE image')
    opt = pe + 24
    dd = opt + (112 if u16(opt) == 0x20b else 96)
    sections = opt + u16(pe + 20)
    def offset(rva):
        for n in range(u16(pe + 6)):
            s = sections + n * 40
            va, rawsize, raw = u32(s+12), u32(s+16), u32(s+20)
            if va <= rva < va + rawsize: return raw + rva - va
        raise ValueError(f'Unmapped RVA {rva:x}')
    base = offset(u32(dd + 16))
    found = {}
    def walk(rel, keys):
        p = base + rel
        for n in range(u16(p+12) + u16(p+14)):
            name, target = u32(p+16+n*8), u32(p+20+n*8)
            if name & 0x80000000:
                q = base + (name & 0x7fffffff)
                name = data[q+2:q+2+2*u16(q)].decode('utf-16le')
            k = keys + (name,)
            if target & 0x80000000: walk(target & 0x7fffffff, k)
            else:
                q = base + target
                start, size = offset(u32(q)), u32(q+4)
                found[k] = data[start:start+size]
    walk(0, ())
    return found

def validate_inputs():
    pin = json.loads(PIN.read_text(encoding='utf-8'))
    baseline = json.loads(BASELINE.read_text(encoding='utf-8'))
    entries = pin['resources']
    if pin['schema'] != 1 or entries != baseline['resources']:
        raise ValueError('Approved package manifests disagree')
    if len(entries) != 3 or {e['id'] for e in entries} != set(RESOURCE_IDS):
        raise ValueError('Expected exactly resources 30101, 30102, 30103')
    for e in entries:
        expected = f"Resources/Materials/MainMapMaterials.{RESOURCE_IDS[e['id']]}"
        if e['file'] != expected or e['bytes'] <= 0:
            raise ValueError(f'Invalid resource declaration: {e}')
        check(ROOT / e['file'], e['bytes'], e['sha256'])
    return entries

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stamp', type=Path)
    parser.add_argument('--dll', type=Path)
    args = parser.parse_args()
    entries = validate_inputs()
    if args.stamp:
        text = '// Validated pinned A2.2 resource digest\n#define MAINMAP_RESOURCE_DIGEST "' + digest(
            json.dumps(entries, sort_keys=True).encode()) + '"\n'
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        if not args.stamp.exists() or args.stamp.read_text() != text:
            args.stamp.write_text(text)
    if args.dll:
        found = resources(args.dll)
        for entry in entries:
            rid, size, sha = entry['id'], entry['bytes'], entry['sha256']
            leaves = [(k, v) for k, v in found.items() if k[:2] == (10, rid)]
            if len(leaves) != 1: raise ValueError(f'Expected one RT_RCDATA {rid}')
            key, data = leaves[0]
            if len(data) != size or digest(data) != sha: raise ValueError(f'Embedded mismatch: {key}')
            original = (ROOT / entry['file']).read_bytes()
            if data != original: raise ValueError(f'Embedded bytes differ: {key}')
            print(f'RT_RCDATA {rid}: {size} bytes SHA256={sha} byte-for-byte PASS')
    print('Approved MainMap material resource validation: PASS')

if __name__ == '__main__':
    main()
