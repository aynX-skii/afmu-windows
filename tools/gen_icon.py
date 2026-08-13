#!/usr/bin/env python3
"""生成 resources/afmu.ico 和 resources/afmu.png（纯标准库，不依赖 Pillow）。

    python tools/gen_icon.py

图标是画出来的而不是提交一个二进制：这样它和 qml/Theme.qml 里的配色跟着同一组数值走，
改主题色时改这里一行就行，也不用为了一张图往仓库里塞一个没人能 diff 的文件。

形状：圆角方块底 + 一上一下两个反向箭头 —— "两台设备之间来回传文件"。16×16 下
细节全糊，所以箭头画得很粗，小尺寸时也还认得出方向。
"""

import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "resources"

# 和 qml/Theme.qml 对齐
BG = (0x12, 0x16, 0x1D)
BORDER = (0x2A, 0x32, 0x3F)
ACCENT = (0x4C, 0x8D, 0xFF)

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # 每个像素边长上取 4 个样本，共 16 个 —— 够把圆角和斜边磨平


def rounded_rect_sdf(x, y, half, radius):
    """以原点为中心、半边长 half、圆角 radius 的方块的有符号距离。"""
    dx = abs(x) - (half - radius)
    dy = abs(y) - (half - radius)
    ax, ay = max(dx, 0.0), max(dy, 0.0)
    return (ax * ax + ay * ay) ** 0.5 + min(max(dx, dy), 0.0) - radius


def segment_sdf(px, py, ax, ay, bx, by):
    """点到线段 ab 的距离。"""
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    denom = vx * vx + vy * vy
    t = 0.0 if denom == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / denom))
    cx, cy = ax + t * vx, ay + t * vy
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def arrow_sdf(x, y, cy, direction, span, head, thick):
    """一条水平箭头：横杆 + 箭头两撇。direction 为 +1 向右、-1 向左。"""
    x0, x1 = -span * direction, span * direction
    d = segment_sdf(x, y, x0, cy, x1, cy)
    d = min(d, segment_sdf(x, y, x1, cy, x1 - head * direction, cy - head))
    d = min(d, segment_sdf(x, y, x1, cy, x1 - head * direction, cy + head))
    return d - thick


def blend(dst, src, alpha):
    return tuple(int(round(d + (s - d) * alpha)) for d, s in zip(dst, src))


def render(size):
    """返回 RGBA 字节，行优先。坐标系归一到 [-1, 1]。"""
    px = bytearray(size * size * 4)
    step = 2.0 / (size * SS)
    # 小尺寸下线要相对更粗，否则 16×16 时箭头细成一根丝
    thick = 0.085 if size >= 48 else 0.11
    radius = 0.30

    for py in range(size):
        for pxi in range(size):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    x = -1.0 + ((pxi * SS + sx) + 0.5) * step
                    y = -1.0 + ((py * SS + sy) + 0.5) * step

                    d_bg = rounded_rect_sdf(x, y, 0.94, radius)
                    if d_bg > 0:
                        continue  # 方块之外，完全透明
                    color = BG
                    # 边框：贴着边缘的一圈
                    if d_bg > -0.045:
                        color = BORDER
                    d_up = arrow_sdf(x, y, -0.30, 1, 0.46, 0.20, thick)
                    d_dn = arrow_sdf(x, y, 0.30, -1, 0.46, 0.20, thick)
                    if min(d_up, d_dn) <= 0:
                        color = ACCENT
                    r += color[0]
                    g += color[1]
                    b += color[2]
                    a += 255.0

            n = SS * SS
            alpha = a / n
            if alpha <= 0:
                continue
            # 颜色按已覆盖的样本求平均，再配上覆盖率作为 alpha —— 直接除以 n 会让
            # 边缘像素往黑里拉（把透明样本当成黑色算了进去）
            cov = a / 255.0
            i = (py * size + pxi) * 4
            px[i + 0] = int(round(r / cov))
            px[i + 1] = int(round(g / cov))
            px[i + 2] = int(round(b / cov))
            px[i + 3] = int(round(alpha))
    return bytes(px)


def png_bytes(size, rgba):
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)  # 每行的过滤器类型：None
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def ico_bytes(images):
    """images: [(size, png_bytes)]。ICO 允许直接内嵌 PNG（Vista 之后都认）。"""
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    blobs = b""
    for size, data in images:
        entries += struct.pack("<BBBBHHII",
                               0 if size >= 256 else size,  # 0 表示 256
                               0 if size >= 256 else size,
                               0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return header + entries + blobs


def main():
    OUT_DIR.mkdir(exist_ok=True)
    images = []
    for size in SIZES:
        data = png_bytes(size, render(size))
        images.append((size, data))
        print(f"  {size}×{size} → {len(data)} 字节")
    (OUT_DIR / "afmu.ico").write_bytes(ico_bytes(images))
    (OUT_DIR / "afmu.png").write_bytes(dict(images)[256])
    print(f"resources/afmu.ico（{len(SIZES)} 个尺寸）、resources/afmu.png 已生成")


if __name__ == "__main__":
    main()
