#!/usr/bin/env python3
"""从 tests/qrcode_test.cpp.in 生成 tests/qrcode_test.cpp。

用法：

    pip install segno
    python tools/gen_qr_vectors.py

向量的权威来源是 **segno**（另一套独立的 QR 实现）。_qrref.py 里那份 Python 参考
实现和 src/QrCode.cpp 是同一套算法的两种写法，它的作用是把「C++ 抄错了」和
「算法本身想错了」分开：脚本会先让参考实现和 segno 逐位对齐，再把结果写成向量，
C++ 测试拿这些向量对照自己。两道关都过了，才说明 C++ 那份是对的。

掩码选择不参与比对：segno 在写入格式信息**之前**算罚分，libqrencode（afmu-linux
用的那个）在**之后**算，两种都合标准，选出来的掩码可能不同，但每个掩码都能扫。
所以这里让 segno 用参考实现选出的掩码，比的是除选择策略之外的全部内容。
"""

import hashlib
import pathlib
import sys

try:
    import segno
    import segno.encoder as senc
except ImportError:  # pragma: no cover
    sys.exit("需要 segno：pip install segno")

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import _qrref as qrref  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "tests" / "qrcode_test.cpp.in"
OUTPUT = ROOT / "tests" / "qrcode_test.cpp"

MASK32 = 0xFFFFFFFF
ECCS = "LMQH"


def _spec_padding(buff, version, length):
    """segno 的 write_padding_bits 在已经对齐时会补一整个零字节（8 - 0 == 8），
    白白吃掉一个补位码字。ISO 18004 §7.4.10 在这种情况下什么都不补。补丁打上，
    两边才能逐字节比 —— 差别只在补位码字上，不影响可解码性。"""
    buff.extend([0] * ((8 - (length % 8)) % 8))


senc.write_padding_bits = _spec_padding


def gen_data(seed, n):
    """和 tests/qrcode_test.cpp 里 genData() 逐位一致的 xorshift32。"""
    s = seed & MASK32
    out = bytearray()
    for _ in range(n):
        s ^= (s << 13) & MASK32
        s ^= s >> 17
        s ^= (s << 5) & MASK32
        out.append(s & 0xFF)
    return bytes(out)


def digest(matrix):
    buf = bytearray()
    for row in matrix:
        buf.extend(0x31 if v else 0x30 for v in row)
    return hashlib.sha256(bytes(buf)).hexdigest()[:32]


def segno_matrix(data, ecc, mask):
    q = segno.make_qr(data, error=ecc, mode="byte", boost_error=False, mask=mask)
    return [[bool(b) for b in row] for row in q.matrix]


def main():
    vectors = []
    for ver in range(1, 41):
        for ei, ecc in enumerate(ECCS):
            # 该版本装得下的最大字节数：数据码字减去模式+长度指示符占的那一两个字节
            cap = qrref.num_data_codewords(ver, ecc) - (1 if ver <= 9 else 2) - 1
            for kind, n in enumerate((cap, max(1, cap - 1))):
                seed = (ver * 131 + ei * 17 + kind * 7) | 1
                data = gen_data(seed, n)
                m = qrref.encode(data, ecc)
                if m is None or m.ver != ver:
                    sys.exit(f"参考实现选错版本：{ver} {ecc} {n}")
                ref = segno_matrix(data, ecc, m.mask)
                if ref != m.mods:
                    sys.exit(f"参考实现和 segno 不一致：{ver} {ecc} {n}")
                vectors.append(
                    '    { %d, %d, %d, %d, "%s" },' % (ver, ei, seed, n, digest(ref)))

    art_data = b"afmu"
    m = qrref.encode(art_data, "M")
    if segno_matrix(art_data, "M", m.mask) != m.mods:
        sys.exit("样例图和 segno 不一致")
    art = ['    "%s",' % "".join("#" if v else "." for v in row) for row in m.mods]

    text = TEMPLATE.read_text(encoding="utf-8")
    text = text.replace("@VECTORS@", "\n".join(vectors))
    text = text.replace("@ART@", "\n".join(art))
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUTPUT.relative_to(ROOT)}：{len(vectors)} 组向量，全部与 segno 逐位一致")


if __name__ == "__main__":
    main()
