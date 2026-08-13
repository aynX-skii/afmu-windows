"""src/QrCode.cpp 的 Python 对照实现，结构一一对应（同样的表、同样的函数划分）。

它不是权威 —— 权威是 segno。它存在的意义是把两类错误分开：gen_qr_vectors.py 先让
这份实现和 segno 逐位对齐（算法对了），再把向量交给 C++ 测试（抄对了）。只有一份
实现的话，出错时只知道"不一样"，不知道错在哪一层。

只做字节模式，因为配对 URI 已经是 percent-encoded 的纯字节，用不上数字/字母模式。
"""

ECC_CODEWORDS_PER_BLOCK = {
    'L': [7,10,15,20,26,18,20,24,30,18,20,24,26,30,22,24,28,30,28,28,28,28,30,30,26,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
    'M': [10,16,26,18,24,16,18,22,22,26,30,22,22,24,24,28,28,26,26,26,26,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28],
    'Q': [13,22,18,26,18,24,18,22,20,24,28,26,24,20,30,24,28,28,26,30,28,30,30,30,30,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
    'H': [17,28,22,16,22,28,26,26,24,28,24,28,22,24,24,30,28,28,26,28,30,24,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
}
NUM_BLOCKS = {
    'L': [1,1,1,1,1,2,2,2,2,4,4,4,4,4,6,6,6,6,7,8,8,9,9,10,12,12,12,13,14,15,16,17,18,19,19,20,21,22,24,25],
    'M': [1,1,1,2,2,4,4,4,5,5,5,8,9,9,10,10,11,13,14,16,17,17,18,20,21,23,25,26,28,29,31,33,35,37,38,40,43,45,47,49],
    'Q': [1,1,2,2,4,4,6,6,8,8,8,10,12,16,12,17,16,18,21,20,23,23,25,27,29,34,34,35,38,40,43,45,48,51,53,56,59,62,65,68],
    'H': [1,1,2,4,4,4,5,6,8,8,11,11,16,16,18,16,19,21,25,25,25,34,30,32,35,37,40,42,45,48,51,54,57,60,63,66,70,74,77,81],
}
ECC_FORMAT_BITS = {'L': 1, 'M': 0, 'Q': 3, 'H': 2}


def num_raw_data_modules(ver):
    result = (16 * ver + 128) * ver + 64
    if ver >= 2:
        num_align = ver // 7 + 2
        result -= (25 * num_align - 10) * num_align - 55
        if ver >= 7:
            result -= 36
    return result


def num_data_codewords(ver, ecc):
    return (num_raw_data_modules(ver) // 8
            - ECC_CODEWORDS_PER_BLOCK[ecc][ver - 1] * NUM_BLOCKS[ecc][ver - 1])


def align_positions(ver):
    if ver == 1:
        return []
    num_align = ver // 7 + 2
    step = 26 if ver == 32 else (ver * 4 + num_align * 2 + 1) // (num_align * 2 - 2) * 2
    pos = []
    p = ver * 4 + 10
    for _ in range(num_align - 1):
        pos.insert(0, p)
        p -= step
    pos.insert(0, 6)
    return pos


# ---------------------------------------------------------------- Reed-Solomon

def rs_multiply(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z & 0xFF


def rs_divisor(degree):
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = rs_multiply(result[j], root)
            if j + 1 < degree:
                result[j] ^= result[j + 1]
        root = rs_multiply(root, 0x02)
    return result


def rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for b in data:
        factor = b ^ result[0]
        result.pop(0)
        result.append(0)
        for i in range(len(divisor)):
            result[i] ^= rs_multiply(divisor[i], factor)
    return result


# ---------------------------------------------------------------- encoding

def char_count_bits(ver):
    return 8 if ver <= 9 else 16


def pick_version(n, ecc):
    for ver in range(1, 41):
        cap = num_data_codewords(ver, ecc) * 8
        need = 4 + char_count_bits(ver) + 8 * n
        if need <= cap:
            return ver
    return 0


def make_data_codewords(data, ver, ecc):
    bits = []

    def append(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)

    append(0x4, 4)
    append(len(data), char_count_bits(ver))
    for b in data:
        append(b, 8)

    capacity = num_data_codewords(ver, ecc) * 8
    append(0, min(4, capacity - len(bits)))
    append(0, (8 - len(bits) % 8) % 8)
    pad = 0xEC
    while len(bits) < capacity:
        append(pad, 8)
        pad ^= 0xEC ^ 0x11

    out = bytearray(len(bits) // 8)
    for i, bit in enumerate(bits):
        out[i >> 3] |= bit << (7 - (i & 7))
    return out


def interleave(data, ver, ecc):
    num_blocks = NUM_BLOCKS[ecc][ver - 1]
    ecc_len = ECC_CODEWORDS_PER_BLOCK[ecc][ver - 1]
    raw = num_raw_data_modules(ver) // 8
    short_len = raw // num_blocks - ecc_len
    num_short = num_blocks - raw % num_blocks

    divisor = rs_divisor(ecc_len)
    blocks = []
    k = 0
    for i in range(num_blocks):
        n = short_len + (0 if i < num_short else 1)
        dat = data[k:k + n]
        k += n
        blocks.append((bytes(dat), rs_remainder(dat, divisor)))

    out = bytearray()
    for i in range(short_len + 1):
        for j, (dat, _) in enumerate(blocks):
            if i < short_len or j >= num_short:
                out.append(dat[i])
    for i in range(ecc_len):
        for _, e in blocks:
            out.append(e[i])
    return out


# ---------------------------------------------------------------- matrix

class Matrix:
    def __init__(self, ver):
        self.ver = ver
        self.size = ver * 4 + 17
        self.mods = [[False] * self.size for _ in range(self.size)]
        self.func = [[False] * self.size for _ in range(self.size)]

    def set(self, x, y, dark, is_func=False):
        self.mods[y][x] = dark
        if is_func:
            self.func[y][x] = True

    def draw_function_patterns(self):
        size = self.size
        for i in range(size):
            self.set(6, i, i % 2 == 0, True)
            self.set(i, 6, i % 2 == 0, True)
        for (cx, cy) in ((3, 3), (size - 4, 3), (3, size - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < size and 0 <= y < size:
                        d = max(abs(dx), abs(dy))
                        self.set(x, y, d != 2 and d != 4, True)
        pos = align_positions(self.ver)
        n = len(pos)
        for i in range(n):
            for j in range(n):
                if (i == 0 and j == 0) or (i == 0 and j == n - 1) or (i == n - 1 and j == 0):
                    continue
                cx, cy = pos[i], pos[j]
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        self.set(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1, True)
        # reserve format/version areas
        self.draw_format_bits(0)
        self.draw_version()

    def draw_format_bits(self, mask, ecc='L'):
        data = ECC_FORMAT_BITS[ecc] << 3 | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = ((data << 10) | rem) ^ 0x5412

        for i in range(6):
            self.set(8, i, (bits >> i) & 1 != 0, True)
        self.set(8, 7, (bits >> 6) & 1 != 0, True)
        self.set(8, 8, (bits >> 7) & 1 != 0, True)
        self.set(7, 8, (bits >> 8) & 1 != 0, True)
        for i in range(9, 15):
            self.set(14 - i, 8, (bits >> i) & 1 != 0, True)

        size = self.size
        for i in range(8):
            self.set(size - 1 - i, 8, (bits >> i) & 1 != 0, True)
        for i in range(8, 15):
            self.set(8, size - 15 + i, (bits >> i) & 1 != 0, True)
        self.set(8, size - 8, True, True)

    def draw_version(self):
        if self.ver < 7:
            return
        rem = self.ver
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        bits = (self.ver << 12) | rem
        for i in range(18):
            dark = (bits >> i) & 1 != 0
            a, b = self.size - 11 + i % 3, i // 3
            self.set(a, b, dark, True)
            self.set(b, a, dark, True)

    def draw_codewords(self, data):
        size = self.size
        i = 0
        right = size - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(size):
                for j in range(2):
                    x = right - j
                    upward = ((right + 1) & 2) == 0
                    y = size - 1 - vert if upward else vert
                    if not self.func[y][x] and i < len(data) * 8:
                        self.mods[y][x] = (data[i >> 3] >> (7 - (i & 7))) & 1 != 0
                        i += 1
            right -= 2

    def apply_mask(self, mask):
        for y in range(self.size):
            for x in range(self.size):
                if self.func[y][x]:
                    continue
                if mask == 0:
                    inv = (x + y) % 2 == 0
                elif mask == 1:
                    inv = y % 2 == 0
                elif mask == 2:
                    inv = x % 3 == 0
                elif mask == 3:
                    inv = (x + y) % 3 == 0
                elif mask == 4:
                    inv = (x // 3 + y // 2) % 2 == 0
                elif mask == 5:
                    inv = x * y % 2 + x * y % 3 == 0
                elif mask == 6:
                    inv = (x * y % 2 + x * y % 3) % 2 == 0
                else:
                    inv = ((x + y) % 2 + x * y % 3) % 2 == 0
                if inv:
                    self.mods[y][x] = not self.mods[y][x]

    def _line_n3(self, seq):
        """N3: 1:1:3:1:1 with a 4-module light area on either side."""
        size = self.size
        pattern = [True, False, True, True, True, False, True]
        count = 0
        i = 0
        while i + 7 <= size:
            if seq[i:i + 7] != pattern:
                i += 1
                continue
            after = i + 7
            if i == 0 or i == size - 7 \
                    or not any(seq[max(i - 4, 0):i]) \
                    or not any(seq[after:min(after + 4, size)]):
                count += 40
                i = after
            else:
                i += 4
        return count

    def penalty(self):
        size = self.size
        n1 = n2 = n3 = 0
        dark = 0
        for y in range(size):
            row = self.mods[y]
            col = [self.mods[x][y] for x in range(size)]
            row_prev = col_prev = None
            row_run = col_run = 0
            for x in range(size):
                dark += 1 if row[x] else 0
                if row[x] == row_prev:
                    row_run += 1
                else:
                    if row_run >= 5:
                        n1 += row_run - 2
                    row_run = 1
                if col[x] == col_prev:
                    col_run += 1
                else:
                    if col_run >= 5:
                        n1 += col_run - 2
                    col_run = 1
                if y and x and row[x] == row_prev == self.mods[y - 1][x] == self.mods[y - 1][x - 1]:
                    n2 += 3
                row_prev = row[x]
                col_prev = col[x]
            if row_run >= 5:
                n1 += row_run - 2
            if col_run >= 5:
                n1 += col_run - 2
            n3 += self._line_n3(row)
            n3 += self._line_n3(col)

        percent = dark * 100.0 / (size * size)
        n4 = 10 * int(abs(percent - 50) / 5)
        return n1 + n2 + n3 + n4


def encode(data, ecc='M'):
    ver = pick_version(len(data), ecc)
    if ver == 0:
        return None
    dc = make_data_codewords(data, ver, ecc)
    cw = interleave(dc, ver, ecc)

    m = Matrix(ver)
    m.draw_function_patterns()
    m.draw_codewords(cw)

    best = None
    best_penalty = None
    for mask in range(8):
        m.apply_mask(mask)
        m.draw_format_bits(mask, ecc)
        p = m.penalty()
        if best_penalty is None or p < best_penalty:
            best_penalty = p
            best = mask
        m.apply_mask(mask)
    m.apply_mask(best)
    m.draw_format_bits(best, ecc)
    m.mask = best
    return m
