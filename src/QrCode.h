#pragma once

#include <QByteArray>
#include <vector>

namespace afmu {

/**
 * 按 ISO/IEC 18004 自己实现的二维码编码器（字节模式，版本自动选）。
 *
 * afmu-linux 那边用的是 libqrencode —— Debian/Ubuntu 的 libqrencode-dev、Fedora 的
 * qrencode-devel、Arch 的 qrencode，一条命令就装上了。Windows 上没有这个东西：
 * 要么让每个想编译的人先装一套 vcpkg 再 build 一遍 libqrencode，要么把它作为
 * 子模块拉进来现编。为了一张二维码，两条路都比自带一份实现贵。
 *
 * 代价是这份实现必须自证正确 —— 编错了不会报错，只会「有些二维码扫不出来」。
 * 所以 tests/qrcode_test.cpp 用另一个独立实现（Python 的 segno）生成的向量，把
 * 40 个版本 × 4 个纠错级别在容量边界上逐位钉死；掩码选择用的是 libqrencode 的口径
 * （罚分在写入格式信息之后算），两边生成的图案因此是同一张。
 */
class QrCode
{
public:
    enum class Ecc { Low = 0, Medium = 1, Quartile = 2, High = 3 };

    QrCode() = default;

    /** 编码失败（空数据 / 超出版本 40 容量）时返回 isValid() == false 的对象。 */
    static QrCode encode(const QByteArray &data, Ecc ecc = Ecc::Medium);

    bool isValid() const { return m_size > 0; }
    int size() const { return m_size; }
    int version() const { return m_version; }

    /** 越界一律当成浅色，方便调用方直接带静区循环。 */
    bool module(int x, int y) const
    {
        return x >= 0 && x < m_size && y >= 0 && y < m_size
            && m_modules[std::size_t(y) * std::size_t(m_size) + std::size_t(x)];
    }

private:
    int m_version = 0;
    int m_size = 0;
    std::vector<bool> m_modules;
};

} // namespace afmu
