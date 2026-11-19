#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# duo-clade-decode: unpack the CLADE-compressed code of the Surface Duo (SM8150) modem firmware so it can be
# disassembled (llvm-objdump-18 -d, Hexagon). Own implementation from the public description of the line format
# (64-byte lines, dictionary words with "missing" bits re-inserted, US9300320) and of the high/low priority layout
# (US9823854), checked against this firmware.
#
# Firmware: modem.mdt + modem.bNN from /vendor/firmware_mnt/image (Android) or the Debian mount of the modem
# partition. The compressed data is segment 23 (VA 0xc6cb0000). The QuRT kernel gives the CLADE block the
# compressed base (PA 0x93cb0000 = VA 0xc6cb0000), window 0x5400000 and the exception area (VA 0xc87aa000).
#
# High priority lines: slot k (64 bytes at 0xc6cb0000 + 64k) decodes to VA 0xd8000000 + 64k, read LSB first:
#   bit 0 = 0: 16 words, each op(2) + dict index(11) + missing bits (op 0/1/2: 2/9/15 bits), op 3 = 32-bit literal;
#   bit 0 = 1: literal line: 12-bit exception index, 15 words inline, word 15 in the exception area (not located
#   yet: written as 0 here, 267 lines in this firmware).
# Low priority lines: line n (VA 0xdc000000 + 64n) sits in the unused tops ("holes") of slots 2n and 2n+1.
#   A hole is read from bit 511 downwards as an LSB-first bit stream: bits 0-4 = f (the hole is the top f+1
#   bytes), bits 5-16 = 12 bits of the overflow pointer, then data. Data of slot 2n, then of slot 2n+1, then
#   (if needed) from the byte at exception base + (ptr_even | ptr_odd << 12); ptr 0xffffff = no overflow.
#   The words use the same dictionaries and coding as the high lines.
# usage: duo-clade-decode.py FWDIR OUTDIR   -> OUTDIR/clade_high_d8000000.{bin,elf}, clade_low_dc000000.{bin,elf}
import os
import struct
import sys

NUM = (2, 9, 15)
POS = ((1, 15), (1, 2, 4, 5, 6, 7, 8, 9, 16), (0, 1, 2, 4, 5, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20))
SEG = 23
BASE = 0xc6cb0000          # compressed data (segment 23)
HIGH_END = 0xc87a9280      # end of the high slots (zeros follow up to the exception area)
EXC = 0xc87aa000           # exception area base (QuRT CLADE config)
DICTS = 0xc89b8000         # 3 dictionaries x 2048 words
HIGH_VA, LOW_VA = 0xd8000000, 0xdc000000


def insert_bits(dw, missing, pos):
    for bp in pos:
        dw = (((dw >> bp) << (bp + 1)) | (dw & ((1 << bp) - 1)) | ((missing & 1) << bp)) & 0xFFFFFFFF
        missing >>= 1
    return dw


class Words:
    def __init__(self, dicts):
        self.d = dicts

    def one(self, acc):
        """decode one word from the bit accumulator: (word, bits used)"""
        op = acc & 3
        if op == 3:
            return (acc >> 2) & 0xFFFFFFFF, 34
        n = NUM[op]
        return insert_bits(self.d[op][(acc >> 2) & 0x7FF], (acc >> 13) & ((1 << n) - 1), POS[op]), 13 + n


def rev(x, n):
    return int(format(x, '0%db' % n)[::-1], 2) if n else 0


def hole(slot):
    """top-of-slot hole as (length in bits, LSB-first stream)"""
    val = int.from_bytes(slot, 'little')
    n = 8 * (rev(val >> 507, 5) + 1)
    return n, rev(val >> (512 - n), n)


def rawelf(path, va, data):
    shstr = b'\0.x\0.shstrtab\0'
    off = 0x34
    body = data + b'\0' * (-len(data) % 4)
    shoff_str = off + len(body)
    body += shstr + b'\0' * (-(off + len(body) + len(shstr)) % 4)
    shoff = off + len(body)
    sh = struct.pack('<10I', *([0] * 10))
    sh += struct.pack('<10I', 1, 1, 0x6, va, off, len(data), 0, 0, 4, 0)
    sh += struct.pack('<10I', 4, 3, 0, 0, shoff_str, len(shstr), 0, 0, 1, 0)
    eh = b'\x7fELF\x01\x01\x01' + b'\0' * 9 + struct.pack('<HHIIIIIHHHHHH', 2, 164, 1, va, 0, shoff, 0x66, 0x34,
                                                          32, 0, 40, 3, 2)
    with open(path, 'wb') as f:
        f.write(eh + body + sh)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: duo-clade-decode.py FWDIR OUTDIR')
    fw, out = sys.argv[1], sys.argv[2]
    mdt = open(os.path.join(fw, 'modem.mdt'), 'rb').read()
    phoff, = struct.unpack_from('<I', mdt, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', mdt, 0x2a)
    vaddr, = struct.unpack_from('<I', mdt, phoff + SEG * phentsize + 8)
    if vaddr != BASE:
        sys.exit('segment %d is at 0x%x, not 0x%x: different firmware layout' % (SEG, vaddr, BASE))
    b = open(os.path.join(fw, 'modem.b%02d' % SEG), 'rb').read()
    w = Words([list(struct.unpack_from('<2048I', b, DICTS - BASE + k * 0x2000)) for k in range(3)])
    nslots = (HIGH_END - BASE) // 64
    os.makedirs(out, exist_ok=True)

    high = bytearray()
    nlit = 0
    for k in range(nslots):
        acc = int.from_bytes(b[64 * k:64 * k + 64], 'little')
        if acc & 1:
            nlit += 1
            x = acc >> 13
            words = [(x >> (32 * i)) & 0xFFFFFFFF for i in range(15)] + [0]
        else:
            acc >>= 1
            words = []
            while len(words) < 16:
                v, u = w.one(acc)
                words.append(v)
                acc >>= u
        high += struct.pack('<16I', *words)

    low = bytearray()
    stats = {'empty': 0, 'overflow': 0, 'broken': 0}
    for n in range(nslots // 2):
        (n0, s0), (n1, s1) = hole(b[128 * n:128 * n + 64]), hole(b[128 * n + 64:128 * n + 128])
        if n0 == 8 and n1 == 8 and s0 == 0 and s1 == 0:
            stats['empty'] += 1
            low += bytes(64)
            continue
        ptr = 0
        for i, (nn, s) in enumerate(((n0, s0), (n1, s1))):
            m = min(nn, 17) - 5
            ptr |= ((s >> 5) & ((1 << m) - 1)) << (12 * i)
        l0, l1 = max(n0 - 17, 0), max(n1 - 17, 0)
        acc = ((s0 >> 17) & ((1 << l0) - 1)) | (((s1 >> 17) & ((1 << l1) - 1)) << l0)
        avail, used, words = l0 + l1, 0, []
        while len(words) < 16:
            v, u = w.one(acc)
            if used + u > avail:
                if ptr == 0xffffff or ptr >= HIGH_VA:  # no (valid) overflow pointer
                    stats['broken'] += 1
                    words += [0] * (16 - len(words))
                    break
                a = EXC + ptr - BASE
                acc |= int.from_bytes(b[a:a + 80], 'little') << (avail - used)
                avail, ptr = 1 << 30, 0xffffff
                stats['overflow'] += 1
                continue
            words.append(v)
            acc >>= u
            used += u
        low += struct.pack('<16I', *words)

    for name, va, data in (('clade_high', HIGH_VA, high), ('clade_low', LOW_VA, low)):
        base = os.path.join(out, '%s_%08x' % (name, va))
        open(base + '.bin', 'wb').write(data)
        rawelf(base + '.elf', va, bytes(data))
    print('high: %d lines (%d literal, word 15 unknown) -> 0x%08x..0x%08x' %
          (nslots, nlit, HIGH_VA, HIGH_VA + len(high)))
    print('low: %d lines (%d empty, %d with overflow, %d undecodable) -> 0x%08x..0x%08x' %
          (nslots // 2, stats['empty'], stats['overflow'], stats['broken'], LOW_VA, LOW_VA + len(low)))


if __name__ == '__main__':
    main()
