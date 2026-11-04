#!/usr/bin/env python3
"""Bring a RomFS mod made for an older game version (most mods target 1.2.1 or earlier) to 1.4.3, ready for
make_mod_manifest.py.

- Pack/*.pack.zs (SARC archives): member by member, a three-way merge with 1.0.0 as the base. A member the mod left as
  1.0.0 or as 1.4.3 has keeps 1.4.3's; one only the mod changed takes the mod's. When both changed it, a BYML member is
  compared field by field: if the merge equals the mod's or 1.4.3's member, that one is used. Otherwise (and for other
  formats) 1.4.3's is kept and the member is reported. A pack left equal to 1.4.3's is dropped.
- System/Resource/ResourceSizeTable.*: the mod's tables for other versions are dropped, and 1.4.3's is written again. Each
  file of the mod gets the largest size of 1.4.3's entry, the mod's own tables and, for a merged pack, 1.4.3's entry
  grown by the pack's growth. A file no table lists is left out (the game sizes it itself).
- Every other file is copied, unless it equals 1.0.0's while 1.4.3 has its own (a stale copy).

usage: port_mod.py <mod romfs folder> <output folder> --extracted <work folder with v0/ and v143/> [--base v0]
"""
import argparse, json, shutil, struct, sys, zlib
from pathlib import Path
try:
    import compression.zstd as zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd): run it with python3.14')

RSTB_143 = 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'


class Romfs:
    def __init__(self, folder):
        index = json.loads((folder/'index.json').read_text())
        self.files = {f['path']: f for f in index['files']}
        self.image = (folder/'romfs.bin').open('rb')

    def read(self, path):
        f = self.files.get(path)
        if f is None: return None
        self.image.seek(f['offset']); data = self.image.read(f['size']); assert len(data) == f['size']
        return data


def sarc_read(data):
    """[(name, name hash, attribute, bytes, alignment)] in table order, and the version field."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, _, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count*16 + 8
    out = []
    for i in range(count):
        h, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i*16)
        offset = names + (attribute & 0xffffff)*4
        align = 8
        while align < 256 and (start + lo) % (align*2) == 0: align *= 2
        out.append((data[offset:data.index(0, offset, start)].decode(), h, attribute, data[start+lo:start+hi], align))
    return out, data[16:20]


def sarc_hash(name):
    """The SFAT name hash (key 0x65, signed characters)."""
    h = 0
    for c in name.encode(): h = (h*0x65 + (c - 256 if c > 127 else c)) & 0xffffffff
    return h


def sarc_write(members, version):
    """A SARC with the members in the given order (the game's own layout: data and every member 8-byte aligned)."""
    names = b''; table = []
    for name, h, _, body, align in members:
        table.append((h, 0x01000000 | len(names)//4)); names += name.encode() + b'\0'; names += b'\0'*(-len(names) % 4)
    head = 0x14 + 0xc + 16*len(members) + 8 + len(names)
    start = head + (-head % 8)
    data = bytearray(); offsets = []
    for _, _, _, body, align in members:
        data += b'\0'*(-len(data) % 8); offsets.append((len(data), len(data) + len(body))); data += body
    out = bytearray(b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, start + len(data), start) + version)
    out += b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65)
    for (h, attribute), (lo, hi) in zip(table, offsets): out += struct.pack('<4I', h, attribute, lo, hi)
    out += b'SFNT' + struct.pack('<HH', 8, 0) + names
    out += b'\0'*(start - len(out)) + data
    return bytes(out)


def byml_flat(b):
    """A BYML v7 document as {path: value}, or None when it is not BYML."""
    if b[:2] != b'YB': return None
    key_table, string_table, root = struct.unpack_from('<III', b, 4)
    def table(off):
        if not off: return []
        n = struct.unpack_from('<I', b, off)[0] >> 8; offs = struct.unpack_from(f'<{n + 1}I', b, off + 4)
        return [b[off+o:b.index(0, off + o)].decode() for o in offs[:n]]
    keys, strings = table(key_table), table(string_table); out = {}
    def value(path, t, v):
        if t in (0xc0, 0xc1): node(path, v)
        elif t == 0xa0: out[path] = strings[v]
        elif t in (0xd4, 0xd5, 0xd6): out[path] = (t, b[v:v + 8])
        else: out[path] = (t, v)
    def node(path, o):
        t = b[o]; n = struct.unpack_from('<I', b, o)[0] >> 8
        if t == 0xc1:
            for i in range(n):
                e = o + 4 + 8*i
                value(f'{path}.{keys[struct.unpack_from("<I", b, e)[0] & 0xffffff]}', b[e + 3], struct.unpack_from('<I', b, e + 4)[0])
        elif t == 0xc0:
            base = o + 4 + ((n + 3) & ~3)
            for i in range(n): value(f'{path}[{i}]', b[o + 4 + i], struct.unpack_from('<I', b, base + 4*i)[0])
        else: out[path] = ('node', t)
    if root: node('', root)
    return out


def merge_member(base, new, mod):
    """(bytes, how) for one pack member: base = 1.0.0's (None if absent), new = 1.4.3's, mod = the mod's."""
    if mod == base or mod == new: return new, None
    if new == base or new is None: return mod, 'mod'
    fb, fn, fm = (byml_flat(x) if x is not None else {} for x in (base, new, mod))
    if fb is None or fn is None or fm is None: return new, 'conflict (not BYML): kept 1.4.3'
    merged = {}
    for k in set(fb) | set(fn) | set(fm):
        b, n, m = fb.get(k), fn.get(k), fm.get(k)
        v = n if m == b else m if n == b else n
        if v is not None: merged[k] = v
    if merged == fm: return mod, 'mod (has 1.4.3\'s changes)'
    if merged == fn: return new, None
    return new, 'conflict (both changed it): kept 1.4.3'


def restbl_read(data):
    assert data[:6] == b'RESTBL'
    version, slen, ncrc, nname = struct.unpack_from('<IIII', data, 6)
    crc = dict(struct.unpack_from('<II', data, 22 + 8*i) for i in range(ncrc))
    base = 22 + 8*ncrc; names = {}
    for i in range(nname):
        e = base + i*(slen + 4)
        names[data[e:e + slen].split(b'\0')[0].decode()] = struct.unpack_from('<I', data, e + slen)[0]
    return version, slen, crc, names


def restbl_write(version, slen, crc, names):
    out = bytearray(b'RESTBL' + struct.pack('<IIII', version, slen, len(crc), len(names)))
    for k in sorted(crc): out += struct.pack('<II', k, crc[k])
    for name in sorted(names): out += name.encode().ljust(slen, b'\0') + struct.pack('<I', names[name])
    return bytes(out)


def rstb_key(path):
    for suffix in ('.zs', '.mc'):
        if path.endswith(suffix): path = path[:-len(suffix)]
    return path


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mod', type=Path); p.add_argument('out', type=Path)
    p.add_argument('--extracted', type=Path, required=True); p.add_argument('--base', default='v0')
    a = p.parse_args()
    old, new = Romfs(a.extracted/a.base/'romfs'), Romfs(a.extracted/'v143'/'romfs')
    dicts = {}
    for name, _, _, body, _ in sarc_read(zstd.decompress(new.read('Pack/ZsDic.pack.zs')))[0]:
        d = zstd.ZstdDict(body); dicts[d.dict_id] = d
    def unzs(raw):
        info = zstd.get_frame_info(raw); return zstd.decompress(raw, zstd_dict=dicts.get(info.dictionary_id)) if info.dictionary_id else zstd.decompress(raw)
    if a.out.exists(): sys.exit(f'{a.out} exists; remove it first')
    files = sorted(f.relative_to(a.mod).as_posix() for f in a.mod.rglob('*') if f.is_file())
    tables = [restbl_read(unzs((a.mod/f).read_bytes())) for f in files if f.startswith('System/Resource/ResourceSizeTable.')]
    version, slen, crc, names = restbl_read(unzs(new.read(RSTB_143)))
    def entry(t, path): return t[3].get(path, t[2].get(zlib.crc32(path.encode())))
    def size(path, at_least=0):
        key = rstb_key(path)
        sizes = [s for s in [entry((version, slen, crc, names), key), at_least] + [entry(t, key) for t in tables] if s]
        if not sizes: return
        if key in names: names[key] = max(sizes)
        else: crc[zlib.crc32(key.encode())] = max(sizes)
    for f in files:
        if f.startswith('System/Resource/ResourceSizeTable.'): print(f'  dropped {f} (other versions\' table)'); continue
        body = (a.mod/f).read_bytes(); original, base = new.read(f), old.read(f)
        dest = a.out/f
        if f.endswith('.pack.zs') and original is not None:
            mine, (theirs, sarc_version), (ancestor, _) = sarc_read(unzs(body))[0], sarc_read(unzs(original)), sarc_read(unzs(base)) if base else ([], b'')
            mine, ancestor = {m[0]: m[3] for m in mine}, {m[0]: m[3] for m in ancestor}
            out = []
            for name, h, attribute, payload, align in theirs:
                chosen, how = merge_member(ancestor.get(name), payload, mine.get(name, payload))
                if how: print(f'  {f}: {name}: {how}')
                out.append((name, h, attribute, chosen, align))
            # members the mod adds (neither 1.0.0 nor 1.4.3 has them) are kept; ones 1.4.3 removed stay removed
            have = {m[0] for m in theirs}
            added = sorted(n for n in mine if n not in have and n not in ancestor)
            removed = sorted(n for n in mine if n not in have and n in ancestor)
            for name in added: out.append((name, sarc_hash(name), 0, mine[name], 8))
            if added: print(f'  {f}: {len(added)} members added by the mod')
            if removed: print(f'  {f}: {len(removed)} members 1.4.3 removed were left out: {", ".join(removed[:4])}')
            out.sort(key=lambda m: m[1])
            if not added and all(o[3] == t[3] for o, t in zip(out, theirs)): print(f'  dropped {f} (nothing left to change)'); continue
            packed = sarc_write(out, sarc_version)
            info = zstd.get_frame_info(original)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(zstd.compress(packed, zstd_dict=dicts[info.dictionary_id]) if info.dictionary_id else zstd.compress(packed))
            grown = (entry((version, slen, crc, names), rstb_key(f)) or 0) + max(0, len(packed) - info.decompressed_size)
            size(f, (grown + 31) & ~31)
            print(f'  merged {f}')
            continue
        if base is not None and body == base and original != base: print(f'  dropped {f} (1.0.0\'s file; 1.4.3 has its own)'); continue
        dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(a.mod/f, dest); size(f)
    table = restbl_write(version, slen, crc, names)
    (a.out/RSTB_143).parent.mkdir(parents=True, exist_ok=True); (a.out/RSTB_143).write_bytes(zstd.compress(table))
    print(f'wrote {a.out} ({sum(1 for _ in a.out.rglob("*") if _.is_file())} files); next: make_mod_manifest.py {a.out} --romfs <v143 romfs>')


if __name__ == '__main__':
    main()
