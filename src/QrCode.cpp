#include "QrCode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace afmu {
namespace {

// ---------------------------------------------------------------- 规格表
//
// ISO/IEC 18004 表 13–22。下标 [纠错级别][版本]，版本从 1 开始，所以每行头上补一个 0。
// 这两张表 + numRawDataModules() 决定了每个版本能装多少字节 —— 抄错一个数字的表现
// 是「某些长度的二维码扫不出来」，不是编译错误，所以 tests/qrcode_test.cpp 里
// 用独立实现生成的向量把 40 个版本 × 4 个级别全钉住了。

constexpr std::uint8_t kEccCodewordsPerBlock[4][41] = {
    { 0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
    { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28 },
    { 0, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
    { 0, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
};

constexpr std::uint8_t kNumBlocks[4][41] = {
    { 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25 },
    { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49 },
    { 0, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68 },
    { 0, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81 },
};

// 格式信息里的纠错级别编码，顺序不是 L<M<Q<H 而是 M,L,H,Q（ISO 表 12）
constexpr int kFormatEccBits[4] = { 1, 0, 3, 2 };

int eccIndex(QrCode::Ecc ecc) { return int(ecc); }

/** 版本 ver 的符号里，除去功能图形之后剩下多少个可放数据的模块。 */
int numRawDataModules(int ver)
{
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7)
            result -= 36; // 版本信息两块 6×3
    }
    return result;
}

int numDataCodewords(int ver, int ecc)
{
    return numRawDataModules(ver) / 8
        - int(kEccCodewordsPerBlock[ecc][ver]) * int(kNumBlocks[ecc][ver]);
}

/** 校正图形的中心坐标（ISO 表 E.1 的生成式，和表里的值逐个核对过）。 */
std::vector<int> alignPositions(int ver)
{
    std::vector<int> pos;
    if (ver == 1)
        return pos;
    const int numAlign = ver / 7 + 2;
    // 版本 32 是唯一不满足通式的特例，标准里就是这么规定的
    const int step = (ver == 32) ? 26
                                 : (ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    for (int i = 0, p = ver * 4 + 10; i < numAlign - 1; ++i, p -= step)
        pos.insert(pos.begin(), p);
    pos.insert(pos.begin(), 6);
    return pos;
}

// ---------------------------------------------------------------- GF(256)

/** GF(2^8) 乘法，本原多项式 0x11D。 */
std::uint8_t rsMultiply(std::uint8_t x, std::uint8_t y)
{
    unsigned z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= unsigned((y >> i) & 1) * x;
    }
    return std::uint8_t(z & 0xFF);
}

/** 生成多项式 (x-α^0)(x-α^1)…，返回除最高次项外的系数，低次在前。 */
std::vector<std::uint8_t> rsDivisor(int degree)
{
    std::vector<std::uint8_t> result(std::size_t(degree), 0);
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[std::size_t(j)] = rsMultiply(result[std::size_t(j)], root);
            if (j + 1 < degree)
                result[std::size_t(j)] ^= result[std::size_t(j) + 1];
        }
        root = rsMultiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> rsRemainder(const std::uint8_t *data, int len,
                                      const std::vector<std::uint8_t> &divisor)
{
    std::vector<std::uint8_t> result(divisor.size(), 0);
    for (int i = 0; i < len; ++i) {
        const std::uint8_t factor = std::uint8_t(data[i] ^ result.front());
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t j = 0; j < divisor.size(); ++j)
            result[j] ^= rsMultiply(divisor[j], factor);
    }
    return result;
}

// ---------------------------------------------------------------- 位流

struct BitBuffer
{
    std::vector<bool> bits;

    void append(unsigned value, int len)
    {
        for (int i = len - 1; i >= 0; --i)
            bits.push_back(((value >> i) & 1) != 0);
    }
    int size() const { return int(bits.size()); }
};

/** 字节模式的字符计数指示符位宽（ISO 表 3）。 */
int charCountBits(int ver) { return ver <= 9 ? 8 : 16; }

int pickVersion(int len, int ecc)
{
    for (int ver = 1; ver <= 40; ++ver) {
        const int capacity = numDataCodewords(ver, ecc) * 8;
        const long long need = 4LL + charCountBits(ver) + 8LL * len;
        if (need <= capacity)
            return ver;
    }
    return 0;
}

/** 模式指示符 + 长度 + 数据 + 终止符 + 补位，凑满该版本的数据码字。 */
std::vector<std::uint8_t> makeDataCodewords(const QByteArray &data, int ver, int ecc)
{
    BitBuffer bb;
    bb.append(0x4, 4); // 字节模式
    bb.append(unsigned(data.size()), charCountBits(ver));
    for (char c : data)
        bb.append(std::uint8_t(c), 8);

    const int capacity = numDataCodewords(ver, ecc) * 8;
    bb.append(0, std::min(4, capacity - bb.size()));
    // 只补到字节边界。已经对齐时补 0 位 —— 多补一整个字节会白白吃掉一个补位码字，
    // 虽然仍能解码，但和其他实现的输出对不上，测试向量就失去意义了。
    bb.append(0, (8 - bb.size() % 8) % 8);
    for (unsigned pad = 0xEC; bb.size() < capacity; pad ^= 0xEC ^ 0x11)
        bb.append(pad, 8);

    std::vector<std::uint8_t> out(std::size_t(bb.size() / 8), 0);
    for (int i = 0; i < bb.size(); ++i) {
        if (bb.bits[std::size_t(i)])
            out[std::size_t(i >> 3)] |= std::uint8_t(1 << (7 - (i & 7)));
    }
    return out;
}

/** 分块算 RS 纠错，再按 ISO §7.6 交错成最终码字序列。 */
std::vector<std::uint8_t> interleave(const std::vector<std::uint8_t> &data, int ver, int ecc)
{
    const int numBlocks = kNumBlocks[ecc][ver];
    const int eccLen = kEccCodewordsPerBlock[ecc][ver];
    const int rawCodewords = numRawDataModules(ver) / 8;
    const int shortLen = rawCodewords / numBlocks - eccLen;
    const int numShort = numBlocks - rawCodewords % numBlocks;

    const std::vector<std::uint8_t> divisor = rsDivisor(eccLen);
    std::vector<std::vector<std::uint8_t>> blockData;
    std::vector<std::vector<std::uint8_t>> blockEcc;
    blockData.reserve(std::size_t(numBlocks));
    blockEcc.reserve(std::size_t(numBlocks));

    for (int i = 0, k = 0; i < numBlocks; ++i) {
        const int n = shortLen + (i < numShort ? 0 : 1);
        std::vector<std::uint8_t> dat(data.begin() + k, data.begin() + k + n);
        k += n;
        blockEcc.push_back(rsRemainder(dat.data(), n, divisor));
        blockData.push_back(std::move(dat));
    }

    std::vector<std::uint8_t> out;
    out.reserve(std::size_t(rawCodewords));
    for (int i = 0; i <= shortLen; ++i) {
        for (int j = 0; j < numBlocks; ++j) {
            // 长块比短块多一个字节，多出来的那个排在最后一轮
            if (i < shortLen || j >= numShort)
                out.push_back(blockData[std::size_t(j)][std::size_t(i)]);
        }
    }
    for (int i = 0; i < eccLen; ++i) {
        for (int j = 0; j < numBlocks; ++j)
            out.push_back(blockEcc[std::size_t(j)][std::size_t(i)]);
    }
    return out;
}

// ---------------------------------------------------------------- 矩阵

class Symbol
{
public:
    Symbol(int ver, int ecc)
        : m_ver(ver)
        , m_ecc(ecc)
        , m_size(ver * 4 + 17)
        , m_modules(std::size_t(m_size) * std::size_t(m_size), false)
        , m_function(std::size_t(m_size) * std::size_t(m_size), false)
    {
    }

    int size() const { return m_size; }
    const std::vector<bool> &modules() const { return m_modules; }

    void build(const std::vector<std::uint8_t> &codewords)
    {
        drawFunctionPatterns();
        drawCodewords(codewords);

        // 八个掩码全试一遍，取罚分最低的那个（ISO §7.8.3）。罚分的意义是
        // 「让扫描器难以定位的图案」—— 大片同色、2×2 同色块、疑似定位图形的
        // 1:1:3:1:1、以及深浅比例偏离 50%。
        int best = 0;
        long long bestPenalty = -1;
        for (int mask = 0; mask < 8; ++mask) {
            applyMask(mask);
            drawFormatBits(mask);
            const long long p = penalty();
            if (bestPenalty < 0 || p < bestPenalty) {
                bestPenalty = p;
                best = mask;
            }
            applyMask(mask); // 掩码是异或，再来一次即还原
        }
        applyMask(best);
        drawFormatBits(best);
    }

private:
    void set(int x, int y, bool dark, bool isFunction)
    {
        const std::size_t i = std::size_t(y) * std::size_t(m_size) + std::size_t(x);
        m_modules[i] = dark;
        if (isFunction)
            m_function[i] = true;
    }
    bool at(int x, int y) const
    {
        return m_modules[std::size_t(y) * std::size_t(m_size) + std::size_t(x)];
    }
    bool isFunction(int x, int y) const
    {
        return m_function[std::size_t(y) * std::size_t(m_size) + std::size_t(x)];
    }

    void drawFunctionPatterns()
    {
        for (int i = 0; i < m_size; ++i) {
            set(6, i, i % 2 == 0, true); // 竖的定时图形
            set(i, 6, i % 2 == 0, true); // 横的
        }
        // 三个定位图形，连同分隔符一起画：d 是切比雪夫距离，
        // d≤1 深、d=2 浅、d=3 深、d=4 是分隔符（浅）
        const int corners[3][2] = { { 3, 3 }, { m_size - 4, 3 }, { 3, m_size - 4 } };
        for (const auto &c : corners) {
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int x = c[0] + dx;
                    const int y = c[1] + dy;
                    if (x < 0 || x >= m_size || y < 0 || y >= m_size)
                        continue;
                    const int d = std::max(std::abs(dx), std::abs(dy));
                    set(x, y, d != 2 && d != 4, true);
                }
            }
        }
        // 校正图形：三个角上和定位图形重叠的那几个不画
        const std::vector<int> pos = alignPositions(m_ver);
        const int n = int(pos.size());
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
                    continue;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx)
                        set(pos[std::size_t(i)] + dx, pos[std::size_t(j)] + dy,
                            std::max(std::abs(dx), std::abs(dy)) != 1, true);
                }
            }
        }
        // 先占位：内容之后会被重画，但这些位置必须现在就标成功能模块，
        // 否则数据会被排进去
        drawFormatBits(0);
        drawVersionBits();
    }

    /** 5 位信息 + BCH(15,5) 校验，再异或 0x5412（ISO §7.9.1）。同一份写两遍。 */
    void drawFormatBits(int mask)
    {
        const unsigned data = unsigned(kFormatEccBits[m_ecc] << 3 | mask);
        unsigned rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const unsigned bits = ((data << 10) | rem) ^ 0x5412;

        for (int i = 0; i <= 5; ++i)
            set(8, i, ((bits >> i) & 1) != 0, true);
        set(8, 7, ((bits >> 6) & 1) != 0, true);
        set(8, 8, ((bits >> 7) & 1) != 0, true);
        set(7, 8, ((bits >> 8) & 1) != 0, true);
        for (int i = 9; i < 15; ++i)
            set(14 - i, 8, ((bits >> i) & 1) != 0, true);

        for (int i = 0; i < 8; ++i)
            set(m_size - 1 - i, 8, ((bits >> i) & 1) != 0, true);
        for (int i = 8; i < 15; ++i)
            set(8, m_size - 15 + i, ((bits >> i) & 1) != 0, true);
        set(8, m_size - 8, true, true); // 固定的那一个深色模块
    }

    /** 版本 ≥ 7 才有：6 位版本号 + BCH(18,6)，左下和右上各一块 6×3。 */
    void drawVersionBits()
    {
        if (m_ver < 7)
            return;
        unsigned rem = unsigned(m_ver);
        for (int i = 0; i < 12; ++i)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const unsigned bits = (unsigned(m_ver) << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            const bool dark = ((bits >> i) & 1) != 0;
            const int a = m_size - 11 + i % 3;
            const int b = i / 3;
            set(a, b, dark, true);
            set(b, a, dark, true);
        }
    }

    /** 之字形放码字：从右下角起，每次两列，右列先、上下方向交替（ISO §7.7.3）。 */
    void drawCodewords(const std::vector<std::uint8_t> &data)
    {
        const std::size_t totalBits = data.size() * 8;
        std::size_t i = 0;
        for (int right = m_size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5; // 第 6 列是定时图形，整列跳过
            for (int vert = 0; vert < m_size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? m_size - 1 - vert : vert;
                    if (!isFunction(x, y) && i < totalBits) {
                        const bool dark = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                        m_modules[std::size_t(y) * std::size_t(m_size) + std::size_t(x)] = dark;
                        ++i;
                    }
                    // 位放完之后剩下的模块保持浅色：只有当版本容量不是 8 的整数倍时
                    // 才会有这么几个（ISO §7.7.3 的"剩余位"）
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < m_size; ++y) {
            for (int x = 0; x < m_size; ++x) {
                if (isFunction(x, y))
                    continue;
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (invert) {
                    const std::size_t i = std::size_t(y) * std::size_t(m_size) + std::size_t(x);
                    m_modules[i] = !m_modules[i];
                }
            }
        }
    }

    /** N3：一行/一列里出现 1:1:3:1:1，且一侧有 4 格浅色（符号边缘算浅色）。 */
    long long lineN3(const std::vector<bool> &line) const
    {
        static const bool pattern[7] = { true, false, true, true, true, false, true };
        long long count = 0;
        int i = 0;
        while (i + 7 <= m_size) {
            bool hit = true;
            for (int k = 0; k < 7 && hit; ++k)
                hit = line[std::size_t(i + k)] == pattern[k];
            if (!hit) {
                ++i;
                continue;
            }
            const int after = i + 7;
            auto anyDark = [&](int from, int to) {
                for (int k = std::max(from, 0); k < std::min(to, m_size); ++k)
                    if (line[std::size_t(k)])
                        return true;
                return false;
            };
            if (i == 0 || i == m_size - 7 || !anyDark(i - 4, i) || !anyDark(after, after + 4)) {
                count += 40;
                i = after;
            } else {
                // 命中了但两侧都没有足够的浅色区：图案里第 4 个模块起可能是
                // 下一个候选的开头，从那里继续找，别退回去重扫
                i += 4;
            }
        }
        return count;
    }

    long long penalty() const
    {
        long long n1 = 0, n2 = 0, n3 = 0;
        long long dark = 0;
        const std::size_t n = std::size_t(m_size);
        std::vector<bool> row(n, false);
        std::vector<bool> col(n, false);

        for (int y = 0; y < m_size; ++y) {
            for (int i = 0; i < m_size; ++i) {
                row[std::size_t(i)] = at(i, y);
                col[std::size_t(i)] = at(y, i);
            }
            int rowRun = 0, colRun = 0;
            int rowPrev = -1, colPrev = -1;
            for (int x = 0; x < m_size; ++x) {
                const int r = row[std::size_t(x)] ? 1 : 0;
                const int c = col[std::size_t(x)] ? 1 : 0;
                dark += r;
                if (r == rowPrev) {
                    ++rowRun;
                } else {
                    if (rowRun >= 5)
                        n1 += rowRun - 2;
                    rowRun = 1;
                }
                if (c == colPrev) {
                    ++colRun;
                } else {
                    if (colRun >= 5)
                        n1 += colRun - 2;
                    colRun = 1;
                }
                if (y > 0 && x > 0 && r == rowPrev && at(x, y - 1) == row[std::size_t(x)]
                    && at(x - 1, y - 1) == row[std::size_t(x)]) {
                    n2 += 3;
                }
                rowPrev = r;
                colPrev = c;
            }
            if (rowRun >= 5)
                n1 += rowRun - 2;
            if (colRun >= 5)
                n1 += colRun - 2;
            n3 += lineN3(row);
            n3 += lineN3(col);
        }

        const double percent = double(dark) * 100.0 / (double(m_size) * double(m_size));
        const long long n4 = 10 * (long long)(std::abs(percent - 50.0) / 5.0);
        return n1 + n2 + n3 + n4;
    }

    int m_ver;
    int m_ecc;
    int m_size;
    std::vector<bool> m_modules;
    std::vector<bool> m_function;
};

} // namespace

QrCode QrCode::encode(const QByteArray &data, Ecc ecc)
{
    QrCode out;
    if (data.isEmpty())
        return out;

    const int e = eccIndex(ecc);
    const int ver = pickVersion(int(data.size()), e);
    if (ver == 0)
        return out; // 超过版本 40 的容量

    Symbol symbol(ver, e);
    symbol.build(interleave(makeDataCodewords(data, ver, e), ver, e));

    out.m_version = ver;
    out.m_size = symbol.size();
    out.m_modules = symbol.modules();
    return out;
}

} // namespace afmu
