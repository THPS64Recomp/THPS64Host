"""ERZ decompressor for Edge of Reality's N64 THPS ports.

"ERZ" is Rob Northen Computing's ProPack ("RNC") format with the 3-byte magic
renamed. Reverse-engineered from the THPS1 N64 boot segment (handwritten MIPS
at 0x80001330 = method 1, 0x80000C40 = method 2), cross-checked against the
publicly documented RNC layout.

Header (18 bytes, big-endian):
  0x00  'E' 'R' 'Z' method(1|2)
  0x04  u32 unpacked size
  0x08  u32 packed size (bytes after this header)
  0x0C  u16 CRC-16 of unpacked data
  0x0E  u16 CRC-16 of packed data
  0x10  u8  leeway (bytes the output may overrun the input when unpacking in place)
  0x11  u8  number of blocks ("chunks") in the bit stream

Method 1 (Huffman LZ77): the bit stream is read LSB-first from little-endian
16-bit words. Each block = 3 Huffman tables (literal-run lengths, match
distances, match lengths; each: 5-bit symbol count, then 4-bit code length per
symbol, canonical codes) + 16-bit item count. Item = literal run (raw bytes
copied straight from the input stream, not via the bit reader) then, except
for the last item, a match (distance+1, length+2). Symbol values >= 2 encode
numbers as (1 << (v-1)) | (v-1 extra bits).

Method 2 (no Huffman): bits read MSB-first one at a time from bytes; literals
and short offsets are whole bytes from the same stream.

usage: python3 -I erz.py FILE OFFSET [OUT]   (decompress one ERZ chunk)
"""
import struct
import sys

MAGIC = b"ERZ"


def crc16(data: bytes) -> int:
    """CRC-16/ARC (poly 0xA001 reflected), as used by RNC ProPack."""
    table = crc16.table
    crc = 0
    for b in data:
        crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
    return crc


def _mk_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        t.append(c)
    return t


crc16.table = _mk_table()


class Header:
    def __init__(self, buf: bytes, off: int = 0):
        magic = buf[off:off + 3]
        if magic not in (MAGIC, b"RNC"):
            raise ValueError(f"bad magic {magic!r} at {off:#x}")
        self.method = buf[off + 3]
        (self.usize, self.psize, self.ucrc, self.pcrc,
         self.leeway, self.blocks) = struct.unpack_from(">IIHHBB", buf, off + 4)


# ---------------------------------------------------------------- method 1
class _Bits1:
    """Mirror of the MIPS bit reader: t6 = buffer, t7 = bits left of the
    current word, p (s3) = address of the lookahead word."""

    def __init__(self, buf, pos):
        self.buf, self.p = buf, pos
        self.bb = buf[pos] | (buf[pos + 1] << 8)  # lookahead word only
        self.left = 0

    def _word(self, pos):
        b = self.buf
        return (b[pos] if pos < len(b) else 0) | ((b[pos + 1] if pos + 1 < len(b) else 0) << 8)

    def peek(self, mask):
        return self.bb & mask

    def skip(self, n):
        self.left -= n
        if self.left < 0:                      # refill (0x800016B8)
            k = self.left + n                  # bits that were left
            self.bb >>= k
            self.p += 2
            self.bb |= self._word(self.p) << 16
            n -= k
            self.left = 16 - n
        self.bb = (self.bb >> n) & 0xFFFFFFFF

    def read(self, n):
        v = self.bb & ((1 << n) - 1)
        self.skip(n)
        return v

    def literals(self, n):
        """Raw bytes start at the lookahead word; then reload lookahead."""
        out = self.buf[self.p:self.p + n]
        self.p += n
        self.bb = (self.bb & ((1 << self.left) - 1)) | (self._word(self.p) << self.left)
        return out


def _read_table(bits, old):
    num = bits.read(5)
    if num == 0:
        return old  # MIPS leaves the previous table in place
    lens = [bits.read(4) for _ in range(num)]
    table, code = [], 0
    for L in range(1, 17):
        for sym, l in enumerate(lens):
            if l == L:
                rev = int(f"{code:0{L}b}"[::-1], 2)  # canonical MSB code -> LSB-first
                table.append(((1 << L) - 1, rev, L, sym))
                code += 1
        code <<= 1
    return table


def _decode(bits, table):
    for mask, code, L, sym in table:
        if bits.peek(mask) == code:
            bits.skip(L)
            if sym < 2:
                return sym
            return (1 << (sym - 1)) | bits.read(sym - 1)
    raise ValueError("Huffman decode error")


def _unpack1(buf, pos, usize, stats):
    bits = _Bits1(buf, pos)
    bits.read(2)  # lock / key flags
    out = bytearray()
    raw = dist = length = []
    while len(out) < usize:
        stats["blocks"] += 1
        raw = _read_table(bits, raw)
        dist = _read_table(bits, dist)
        length = _read_table(bits, length)
        count = bits.read(16)
        count = (count - 1) & 0xFFFF
        while True:
            n = _decode(bits, raw)
            if n:
                out += bits.literals(n)
            if count == 0:
                break
            count -= 1
            d = _decode(bits, dist) + 1
            n = _decode(bits, length) + 2
            src = len(out) - d
            if src < 0:
                raise ValueError(f"match before start: dist {d} at {len(out)}")
            for i in range(n):           # byte-wise: overlaps are intended
                out.append(out[src + i])
    stats["end"] = bits.p
    return bytes(out)


# ---------------------------------------------------------------- method 2
class _Bits2:
    """MSB-first single-bit reader over bytes; byte reads share the stream."""

    def __init__(self, buf, pos):
        self.buf, self.p = buf, pos
        self.bb, self.n = 0, 0

    def byte(self):
        v = self.buf[self.p]
        self.p += 1
        return v

    def bit(self):
        if self.n == 0:
            self.bb, self.n = self.byte(), 8
        self.n -= 1
        v = (self.bb >> 7) & 1
        self.bb = (self.bb << 1) & 0xFF
        return v


def _offset2(b):
    off = 0
    if b.bit():
        off = b.bit()
        if b.bit():
            off = (off << 1) | b.bit() | 4
            if not b.bit():
                off = (off << 1) | b.bit()
        elif off == 0:
            off = 2 + b.bit()
    return (off << 8) + b.byte() + 1


def _unpack2(buf, pos, usize, stats):
    b = _Bits2(buf, pos)
    b.bit(); b.bit()  # lock / key flags
    out = bytearray()
    while True:  # like the MIPS: stop only at an end-of-block marker + 0 bit
        if len(out) > usize + 0x10000:
            raise ValueError("runaway method-2 stream")
        if not b.bit():
            out.append(b.byte())
            continue
        if not b.bit():
            n = 4 + b.bit()
            if b.bit():
                n = ((n - 1) << 1) + b.bit()
                if n == 9:  # literal run
                    n = (((b.bit() << 3) | (b.bit() << 2) | (b.bit() << 1) | b.bit()) + 3) * 4
                    out += buf[b.p:b.p + n]
                    b.p += n
                    continue
            off = _offset2(b)
        elif not b.bit():
            n, off = 2, b.byte() + 1
        else:
            if not b.bit():
                n = 3
                off = _offset2(b)
            else:
                n = b.byte() + 8
                if n == 8:  # end of block; next bit = more blocks follow
                    stats["blocks"] += 1
                    if not b.bit():
                        break
                    continue
                off = _offset2(b)
        src = len(out) - off
        if src < 0:
            raise ValueError(f"match before start: dist {off} at {len(out)}")
        for i in range(n):
            out.append(out[src + i])
    stats["end"] = b.p
    return bytes(out)


def decompress(buf: bytes, off: int = 0, check: bool = True):
    """Return (data, info) for the ERZ chunk at buf[off]."""
    h = Header(buf, off)
    body = off + 18
    packed = buf[body:body + h.psize]
    info = {"method": h.method, "usize": h.usize, "psize": h.psize,
            "blocks_hdr": h.blocks, "blocks": 0}
    if check:
        info["pcrc_ok"] = crc16(packed) == h.pcrc
    # pad so the bit reader's lookahead can run past the end safely
    src = bytes(packed) + b"\0" * 8
    if h.method == 1:
        data = _unpack1(src, 0, h.usize, info)
    elif h.method == 2:
        data = _unpack2(src, 0, h.usize, info)
    else:
        raise ValueError(f"unknown method {h.method}")
    info["overrun"] = len(data) - h.usize
    data = data[:h.usize]
    if check:
        info["ucrc_ok"] = crc16(data) == h.ucrc
    return data, info


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    d, info = decompress(rom, int(sys.argv[2], 0))
    print(info)
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(d)
