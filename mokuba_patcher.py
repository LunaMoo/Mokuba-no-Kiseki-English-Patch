#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mokuba no Kiseki English patcher - Python version (any OS, Python 3.8+, no extra modules).

  python3 mokuba_patcher.py <game.iso>                                   Standard edition
  python3 mokuba_patcher.py <game.iso> mokuba_english_ppsspp.patch [decrypted EBOOT]
                                                                         PPSSPP edition

Same patch files and same result as mokuba_patcher.exe (the recompressed textures of the
PPSSPP edition are packed with zlib here, so those bytes differ but unpack identically).
The input ISO is never modified; the output is written next to it.
"""
import os, struct, sys, zlib, gzip, io, shutil

SECTOR = 2048
FPAC_SECTOR = 0x800
HERE = os.path.dirname(os.path.abspath(__file__))


def die(msg):
    print('\nERROR: ' + msg, file=sys.stderr)
    sys.exit(1)


def crc(b): return zlib.crc32(b) & 0xffffffff


# ---------------------------------------------------------------- ISO9660
class Iso:
    def __init__(self, f):
        self.f = f
        f.seek(16 * SECTOR); pvd = f.read(SECTOR)
        if pvd[0] != 1 or pvd[1:6] != b'CD001':
            die('input is not an ISO9660 image (CSO/CHD must be converted to ISO first)')
        self.root = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]

    def find(self, path):
        """-> (lba, size, offset of the directory record) or None"""
        lba, size = self.root; rec_off = None
        for part in path.split('/'):
            self.f.seek(lba * SECTOR); d = self.f.read(size)
            o = 0; found = None
            while o < len(d):
                l = d[o]
                if l == 0: o = (o // SECTOR + 1) * SECTOR; continue
                nl = d[o + 32]; name = d[o + 33:o + 33 + nl].decode('latin1').split(';')[0]
                if name.upper() == part.upper():
                    found = (struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0], lba * SECTOR + o)
                    break
                o += l
            if not found: return None
            lba, size, rec_off = found
        return lba, size, rec_off

    def read(self, lba, size):
        self.f.seek(lba * SECTOR); return self.f.read(size)


# ---------------------------------------------------------------- FPAC
def fpac_parse(d):
    if d[:4] != b'FPAC': die('archive is not FPAC (wrong ISO?)')
    ents = []; n = 0
    while 8 + 8 * (n + 1) <= len(d) and struct.unpack_from('<I', d, 8 + 8 * n)[0] != 0:
        ents.append(struct.unpack_from('<IHH', d, 8 + 8 * n)); n += 1
    minsec = min(e[2] for e in ents)
    base = None
    for p in range(0, len(d) - 1, FPAC_SECTOR):
        if p >= 8 + 8 * n and d[p] == 0x1f and d[p + 1] == 0x8b:
            base = p - minsec * FPAC_SECTOR; break
    if base is None: die('cannot parse FPAC archive')
    return base, ents


def fpac_entry(d, base, ents, i):
    cs, fl, sec = ents[i]; o = base + sec * FPAC_SECTOR
    return d[o:o + cs]


def fpac_rebuild(d, reps):
    base, ents = fpac_parse(d)
    out = bytearray(d[:base]); pos = base
    for i, (cs, fl, sec) in enumerate(ents):
        blob = reps.get(i)
        if blob is None: blob = fpac_entry(d, base, ents, i)
        s = (pos - base) // FPAC_SECTOR
        if s > 0xffff: die('FPAC sector overflow')
        out += blob; out += b'\0' * ((-len(blob)) % FPAC_SECTOR)
        struct.pack_into('<IHH', out, 8 + 8 * i, len(blob), fl, s)
        pos = len(out)
    return bytes(out)


# ---------------------------------------------------------------- gzip members
def gz_header_len(g):
    if g[:3] != b'\x1f\x8b\x08': die('FPAC entry is not gzip')
    fl = g[3]; p = 10
    if fl & 4: p += 2 + struct.unpack_from('<H', g, p)[0]
    if fl & 8: p = g.index(b'\0', p) + 1
    if fl & 16: p = g.index(b'\0', p) + 1
    if fl & 2: p += 2
    return p


def gz_unpack(g):
    return gzip.decompress(g)


def gz_pack(like, data):
    h = bytearray(like[:gz_header_len(like)]); h[3] &= ~2 & 0xff
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return bytes(h) + c.compress(data) + c.flush() + struct.pack('<II', crc(data), len(data) & 0xffffffff)


# ---------------------------------------------------------------- GIM layout (PPSSPP edition)
# Every GIM image gets a power-of-two row stride (pitch_align) and row count (height_align),
# zero padded. Swizzled images are stored as 16x8-byte blocks, one block row after another, so
# widening a swizzled image just appends zero blocks to every block row.
BPP = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8}


def p2(x):
    v = 1
    while v < x: v *= 2
    return v


def _align(x, a): return (x + a - 1) // a * a


def _blocks(d):
    out = []

    def walk(o, limit, parents):
        while o + 16 <= limit:
            bid, _, size, nxt, doff = struct.unpack_from('<HHIII', d, o)
            if size == 0 or o + size > limit: break
            out.append((o, bid, size, nxt, doff, tuple(parents)))
            if bid in (2, 3): walk(o + doff, o + size, parents + [o])
            o += size
    walk(16, 16 + struct.unpack_from('<I', d, 20)[0], [])
    return out


def fix_gim(g):
    """returns the re-laid-out GIM, or None if nothing changes"""
    if len(g) < 32 or g[:11] != b'MIG.00.1PSP': return None
    d = bytearray(g); changed = False
    for o, bid, size, nxt, doff, parents in sorted([b for b in _blocks(d) if b[1] == 4], reverse=True):
        ih = o + doff
        if ih + 48 > len(d): continue
        fmt, order, w, h, _bpp, pa, hal = struct.unpack_from('<7H', d, ih + 4)
        idx, ps, pe = struct.unpack_from('<3I', d, ih + 24)
        lvc = struct.unpack_from('<H', d, ih + 42)[0]; frc = struct.unpack_from('<H', d, ih + 46)[0]
        if fmt not in BPP or lvc != 1 or order > 1 or not pa or not hal or not frc: continue
        bp = BPP[fmt]
        os_ = _align((w * bp + 7) // 8, pa); orows = _align(h, hal)
        ns = max(p2(w) * bp // 8, 16); nrows = max(p2(h), hal)
        if order == 1 and (orows % 8 or os_ % 16): continue
        fsz = os_ * orows; nsz = ns * nrows
        for k in range(frc):
            if struct.unpack_from('<I', d, ih + idx + 4 * k)[0] != ps + k * fsz: die('unexpected GIM frame layout')
        if pe < ps + frc * fsz or ih + pe > o + size: die('bad GIM pixel data')
        if (os_, orows) == (ns, nrows) and pa == ns and hal == nrows and pe == ps + frc * nsz: continue
        parts = []
        for k in range(frc):
            fr = bytes(d[ih + ps + k * fsz:ih + ps + (k + 1) * fsz])
            if order == 1:   # block rows of 8 lines
                zb = b'\0' * ((ns - os_) * 8)
                for by in range(orows // 8):
                    parts.append(fr[by * os_ * 8:(by + 1) * os_ * 8]); parts.append(zb)
            else:
                zr = b'\0' * (ns - os_)
                for y in range(orows):
                    parts.append(fr[y * os_:(y + 1) * os_]); parts.append(zr)
            parts.append(b'\0' * ((nrows - orows) * ns))
        out = b''.join(parts)
        delta = len(out) - (pe - ps)
        d[ih + ps:ih + pe] = out
        struct.pack_into('<HH', d, ih + 14, ns, nrows)
        struct.pack_into('<I', d, ih + 32, ps + len(out))
        for k in range(frc): struct.pack_into('<I', d, ih + idx + 4 * k, ps + k * nsz)
        struct.pack_into('<I', d, o + 4, size + delta)
        if nxt == size: struct.pack_into('<I', d, o + 8, nxt + delta)
        for p in parents:
            struct.pack_into('<I', d, p + 4, struct.unpack_from('<I', d, p + 4)[0] + delta)
        changed = True
    return bytes(d) if changed else None


def fix_entry(d):
    """FPAC entry (FCHN chain or bare GIM) -> new bytes, or None if unchanged"""
    if d[:4] == b'MIG.' and len(d) >= 24:
        gl = 16 + struct.unpack_from('<I', d, 20)[0]
        if gl > len(d): return None
        g = fix_gim(d[:gl])
        return None if g is None else g + d[gl:]
    if d[:4] != b'FCHN': return None
    hs, cnt = struct.unpack_from('<HH', d, 4)
    if hs != 8 + 8 * cnt: return None
    ents = [struct.unpack_from('<II', d, 8 + 8 * i) for i in range(cnt)]
    order = sorted(range(cnt), key=lambda i: (ents[i][1], ents[i][0]))
    body = bytearray(); new = [None] * cnt; pos = 0; changed = False
    for i in order:
        sz, of = ents[i]
        if of < pos: die('overlapping FCHN members')
        if of > pos: body += d[hs + pos:hs + of]
        m = d[hs + of:hs + of + sz]
        if m[:4] == b'MIG.' and len(m) >= 24:
            gl = 16 + struct.unpack_from('<I', m, 20)[0]
            if gl <= sz:
                g = fix_gim(m[:gl])
                if g is not None: m = g + m[gl:]; changed = True
        new[i] = (len(m), len(body)); body += m; pos = of + sz
    if not changed: return None
    hdr = bytearray(d[:hs])
    for i, (sz, of) in enumerate(new): struct.pack_into('<II', hdr, 8 + 8 * i, sz, of)
    return bytes(hdr) + bytes(body) + d[hs + pos:]


# ---------------------------------------------------------------- EBOOT ops
def apply_ops(src, nops, blob, out_size):
    out = bytearray(); p = 0
    for _ in range(nops):
        t = blob[p]; p += 1
        if t == 0:
            o, l = struct.unpack_from('<II', blob, p); p += 8; out += src[o:o + l]
        elif t == 1:
            l = struct.unpack_from('<I', blob, p)[0]; p += 4; out += blob[p:p + l]; p += l
        elif t == 2:
            l = struct.unpack_from('<I', blob, p)[0]; p += 4; out += b'\0' * l
        else: die('bad EBOOT patch')
    if len(out) != out_size: die('bad EBOOT patch')
    return bytes(out)


def find_dec(iso_path, arg, size, want):
    cands = [arg] if arg else []
    folder = os.path.dirname(os.path.abspath(iso_path))
    cands += [os.path.join(folder, n) for n in sorted(os.listdir(folder))]
    for p in cands:
        try:
            if os.path.isfile(p) and os.path.getsize(p) == size:
                b = open(p, 'rb').read()
                if crc(b) == want:
                    print('Using decrypted EBOOT:', p); return b
        except OSError: pass
    print('''
This edition needs the game's decrypted EBOOT.BIN (it is not included in the patch).
  1. In PPSSPP: Settings > Tools > Developer tools > enable
     "Dump decrypted EBOOT.BIN on game boot".
  2. Start the original Japanese game once, until the title screen.
  3. Copy the dumped file (in PPSSPP's memstick folder, PSP/SYSTEM/DUMP,
     e.g. NPJH50442_EBOOT.BIN) into the same folder as the ISO.
  4. Run the patcher again.''', file=sys.stderr)
    die('decrypted EBOOT.BIN not found next to the ISO')


# ---------------------------------------------------------------- main
def read_records(data):
    if data[:7] != b'MKPATCH' or data[7:8] not in (b'1', b'2'): die('not a valid patch file')
    ver = data[7] - 0x30
    n = struct.unpack_from('<I', data, 8)[0]; p = 12; recs = []
    for _ in range(n):
        t, pl = struct.unpack_from('<BH', data, p); p += 3
        path = data[p:p + pl].decode(); p += pl
        osz, ocrc = struct.unpack_from('<II', data, p); p += 8
        r = {'type': t, 'path': path, 'osz': osz, 'ocrc': ocrc}
        if t == 0:
            l = struct.unpack_from('<I', data, p)[0]; p += 4; r['data'] = data[p:p + l]; p += l
        elif t in (1, 2):
            k = struct.unpack_from('<I', data, p)[0]; p += 4; reps = {}
            for _ in range(k):
                i, l = struct.unpack_from('<II', data, p); p += 8; reps[i] = data[p:p + l]; p += l
            r['reps'] = reps; r['pads'] = []
            if t == 2:
                if ver < 2: die('this patch file is outdated, get the current one')
                m = struct.unpack_from('<I', data, p)[0]; p += 4
                r['pads'] = list(struct.unpack_from('<%dI' % m, data, p)); p += 4 * m
        elif t == 3:
            ds, dc, os_, oc, nops, bl = struct.unpack_from('<6I', data, p); p += 24
            r.update(ds=ds, dc=dc, out_size=os_, out_crc=oc, nops=nops, blob=data[p:p + bl]); p += bl
        else: die('unknown patch record')
        recs.append(r)
    return recs


def main(argv):
    print('Mobile Suit Gundam: Path of the Trojan Horse - English patcher (Python)')
    print('=' * 70 + '\n')
    if len(argv) < 2:
        print(__doc__); sys.exit(1)
    in_path = argv[1]
    patch_path = argv[2] if len(argv) >= 3 else os.path.join(HERE, 'mokuba_english.patch')
    dec_arg = argv[3] if len(argv) >= 4 else None
    if not os.path.exists(patch_path): die('patch file missing: ' + patch_path)
    recs = read_records(open(patch_path, 'rb').read())
    stem = os.path.basename(patch_path)
    suffix = '_english' if len(argv) < 3 else '_' + os.path.splitext(stem[7:] if stem.startswith('mokuba_') else stem)[0]
    out_path = os.path.splitext(in_path)[0] + suffix + '.iso'

    with open(in_path, 'rb') as f:
        iso = Iso(f)
        print('Checking input ISO...')
        dec = None
        for r in recs:
            fi = iso.find(r['path'])
            if not fi: die('missing %s - this is not the expected ISO' % r['path'])
            b = iso.read(fi[0], fi[1])
            if fi[1] != r['osz'] or crc(b) != r['ocrc']:
                die('ISO does not match (%s).\n       Use a clean dump of Kidou Senshi Gundam - Mokuba no Kiseki (Japan) [NPJH-50442].' % r['path'])
            if r['type'] == 3 and dec is None:
                dec = find_dec(in_path, dec_arg, r['ds'], r['dc'])

    print('Copying ISO to %s ...' % out_path)
    shutil.copyfile(in_path, out_path)
    with open(out_path, 'r+b') as f:
        iso = Iso(f)
        f.seek(0, 2); end_lba = (f.tell() + SECTOR - 1) // SECTOR
        for r in recs:
            lba, size, rec_off = iso.find(r['path'])
            t = r['type']
            if t == 0:
                data = r['data']
            elif t == 3:
                data = apply_ops(dec, r['nops'], r['blob'], r['out_size'])
                if crc(data) != r['out_crc']: die('patched EBOOT does not verify')
            else:
                orig = iso.read(lba, size)
                reps = dict(r['reps'])
                if r['pads']:
                    base, ents = fpac_parse(orig)
                    for n, i in enumerate(r['pads']):
                        g = fpac_entry(orig, base, ents, i)
                        padded = fix_entry(gz_unpack(g))
                        if padded is not None: reps[i] = gz_pack(g, padded)
                        if n % 16 == 15 or n + 1 == len(r['pads']):
                            print('\r  %-60s %d/%d' % (r['path'], n + 1, len(r['pads'])), end='', flush=True)
                    print('\r' + ' ' * 79 + '\r', end='')
                data = fpac_rebuild(orig, reps)
            old_secs = (size + SECTOR - 1) // SECTOR; new_secs = (len(data) + SECTOR - 1) // SECTOR
            if new_secs > old_secs:
                new_lba = end_lba; end_lba += new_secs
            else:
                new_lba = lba
            f.seek(new_lba * SECTOR); f.write(data + b'\0' * (new_secs * SECTOR - len(data)))
            if new_lba == lba and old_secs > new_secs:
                f.write(b'\0' * ((old_secs - new_secs) * SECTOR))
            rec = struct.pack('<I', new_lba) + struct.pack('>I', new_lba) + struct.pack('<I', len(data)) + struct.pack('>I', len(data))
            f.seek(rec_off + 2); f.write(rec)
            print('  %-60s %s' % (r['path'], 'in place' if new_lba == lba else 'relocated'))
        f.seek(0, 2)
        if f.tell() < end_lba * SECTOR:
            f.seek(end_lba * SECTOR - 1); f.write(b'\0')
        f.seek(16 * SECTOR + 80); f.write(struct.pack('<I', end_lba) + struct.pack('>I', end_lba))
    print('\nDone! Created:\n  ' + out_path)


if __name__ == '__main__':
    main(sys.argv)
