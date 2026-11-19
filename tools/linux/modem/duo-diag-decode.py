#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# duo-diag-decode: print a DIAG capture (.qmdl from duo-diag-cap) as text: F3 debug messages with their format
# strings (plain 0x79, and QShrink 4 0x99 looked up in the modem's qdsp6m.qdb), event ids, log codes.
# The qdb layout is the one SCAT (fgsect/scat) reads: 64-byte header starting "\x7fQDB", then a zlib-compressed
# text whose <Content> section has lines "hash:subsys_mask:ssid:line:file:format".
# usage: duo-diag-decode.py cap.qmdl [qdsp6m.qdb] [--grep REGEX] [--last SECONDS]
import re
import struct
import sys
import zlib


def load_qdb(path):
    with open(path, 'rb') as f:
        hdr = f.read(64)
        if hdr[:4] != b'\x7fQDB':
            sys.exit(f'{path}: not a QDB file')
        text = zlib.decompress(f.read()).decode(errors='backslashreplace')
    table, mode = {}, None
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] == '#':
            continue
        m = re.match(r'^<\\?(\w+)>$', line)
        if m:
            mode = None if line.startswith('<\\') else m.group(1)
            continue
        if mode == 'Content':
            p = line.split(':', 5)
            if len(p) == 6:
                table[int(p[0])] = (int(p[2]), int(p[3]), p[4], p[5])  # ssid, line, file, fmt
    return table


CFMT = re.compile(r'%(?:[-+0 #]{0,5})(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|w|I64|I32|I|z|j|t)?[diouxXpcs%]')


def cformat(fmt, args):
    out, i = [], 0

    def rep(m):
        nonlocal i
        s = m.group(0)
        if s == '%%':
            return '%'
        if i >= len(args):
            return s
        v = args[i]
        i += 1
        conv = s[-1]
        spec = re.sub(r'(hh|h|ll|l|w|I64|I32|I|z|j|t)?[diouxXpcs]$', '', s[1:]).replace('*', '')
        try:
            if conv in 'di':
                v = v - (1 << 32) if v >= 1 << 31 else v
                return ('%' + spec + 'd') % v
            if conv in 'ouxX':
                return ('%' + spec + conv) % v
            if conv == 'p':
                return '0x%x' % v
            if conv == 'c':
                return chr(v & 0xff) if 32 <= (v & 0xff) < 127 else '?'
            return '<str@0x%x>' % v
        except (ValueError, TypeError):
            return s
    return CFMT.sub(rep, fmt)


def frames(data):
    for raw in data.split(b'\x7e'):
        if len(raw) < 3:
            continue
        out, esc = bytearray(), False
        for b in raw:
            if esc:
                out.append(b ^ 0x20)
                esc = False
            elif b == 0x7d:
                esc = True
            else:
                out.append(b)
        crc = 0xffff
        for b in out[:-2]:
            crc ^= b
            for _ in range(8):
                crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
        if (~crc & 0xffff) != struct.unpack('<H', out[-2:])[0]:
            yield None
            continue
        yield bytes(out[:-2])


def ts_sec(t):
    return (t >> 16) / 800.0


def decode(pkt, qdb):
    c = pkt[0]
    if c == 0x98 and len(pkt) > 8:  # multi-SIM wrapper
        return decode(pkt[8:], qdb)
    if c == 0x79 and len(pkt) >= 20:
        _, _, na, drop, ts, line, ssid, lvl = struct.unpack('<BBBBQHHI', pkt[:20])
        args = list(struct.unpack('<%dI' % na, pkt[20:20 + 4 * na]))
        rest = pkt[20 + 4 * na:].split(b'\0')
        fmt = rest[0].decode(errors='replace')
        fn = rest[1].decode(errors='replace') if len(rest) > 1 else ''
        return ts, f'F3 {ssid}/{lvl:x} {fn}:{line}{" DROP%d" % drop if drop else ""}  {cformat(fmt, args)}'
    if c == 0x99 and len(pkt) >= 18:
        _, _, nsz, drop, ts, h, _ = struct.unpack('<BBBBQIH', pkt[:18])
        na, sz = nsz >> 4, nsz & 15
        args = []
        for i in range(na):
            if sz:
                args.append(int.from_bytes(pkt[18 + i * sz:18 + (i + 1) * sz], 'little'))
        e = qdb.get(h)
        if e:
            ssid, line, fn, fmt = e
            return ts, f'F3 {ssid} {fn}:{line}{" DROP%d" % drop if drop else ""}  {cformat(fmt, args)}'
        return ts, f'F3 qsr4 hash 0x{h:08x} args {" ".join("%x" % a for a in args)}'
    if c == 0x92 and len(pkt) >= 24:
        _, _, na, _, ts, line, ssid, lvl, h = struct.unpack('<BBBBQHHII', pkt[:24])
        args = list(struct.unpack('<%dI' % na, pkt[24:24 + 4 * na]))
        return ts, f'F3 qsr2 {ssid} line {line} hash 0x{h:08x} args {" ".join("%x" % a for a in args)}'
    if c == 0x10 and len(pkt) >= 16:
        _, _, ln, ln2, code, ts = struct.unpack('<BBHHHQ', pkt[:16])
        return ts, f'LOG 0x{code:04x} len {ln2}'
    if c == 0x60 and len(pkt) >= 3:
        evs, o = [], 3
        end = min(len(pkt), 3 + struct.unpack('<H', pkt[1:3])[0])
        ts0 = None
        while o + 2 <= end:
            w = struct.unpack('<H', pkt[o:o + 2])[0]
            eid, plen, trunc = w & 0xfff, (w >> 13) & 3, (w >> 15) & 1
            o += 2
            if trunc:
                o += 2
            else:
                if ts0 is None and o + 8 <= end:
                    ts0 = struct.unpack('<Q', pkt[o:o + 8])[0]
                o += 8
            if plen == 3:
                if o >= end:
                    break
                plen = pkt[o] + 1
            payload = pkt[o:o + plen] if plen else b''
            o += plen
            evs.append(f'{eid}' + (':' + payload.hex() if payload else ''))
        return ts0, 'EVENT ' + ' '.join(evs)
    return None, f'PKT 0x{c:02x} len {len(pkt)} {pkt[:24].hex()}'


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__ or 'usage: duo-diag-decode.py cap.qmdl [qdsp6m.qdb] [--grep RE] [--last S]')
    grep = last = None
    if '--grep' in a:
        i = a.index('--grep')
        grep = re.compile(a[i + 1], re.I)
        del a[i:i + 2]
    if '--last' in a:
        i = a.index('--last')
        last = float(a[i + 1])
        del a[i:i + 2]
    qdb = load_qdb(a[1]) if len(a) > 1 else {}
    if qdb:
        print(f'# qdb: {len(qdb)} messages', file=sys.stderr)
    data = open(a[0], 'rb').read()
    rows, bad, t0 = [], 0, None
    for pkt in frames(data):
        if pkt is None:
            bad += 1
            continue
        ts, txt = decode(pkt, qdb)
        s = ts_sec(ts) if ts else None
        if s is not None and t0 is None:
            t0 = s
        rows.append((s, txt))
    tend = max((s for s, _ in rows if s is not None), default=0)
    for s, txt in rows:
        if last is not None and s is not None and s < tend - last:
            continue
        if grep and not grep.search(txt):
            continue
        rel = f'{s - t0:10.3f}' if s is not None and t0 is not None else ' ' * 10
        print(f'{rel}  {txt}')
    print(f'# {len(rows)} packets, {bad} bad frames', file=sys.stderr)


if __name__ == '__main__':
    main()
