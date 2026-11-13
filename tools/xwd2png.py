#!/usr/bin/env python3
"""把 24/32 位深、24 或 32bpp 的 XWD 转成 RGB PNG（只用标准库），供 AppStream 截图使用。

XWD 头部是 22 个 CARD32 大端字段（X11 XWDFile.h），本脚本只用其中几个：
  0 header_size、4 version（应为 7）、8 pixmap_format（2 = ZPixmap）、
  12 pixmap_depth、16 width、20 height、32 bits_per_pixel、36 bytes_per_line
像素从 header_size 字节处开始，每像素 BGR(X)；bytes_per_line 为 0 时按
width * bits_per_pixel / 8 计算。
"""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, "rb").read()
header_size, version, pixmap_format, depth, width, height = struct.unpack(">6I", raw[:24])
bpp, = struct.unpack(">I", raw[32:36])
bytes_per_line, = struct.unpack(">I", raw[36:40])

if version != 7 or pixmap_format != 2:
    raise SystemExit("unsupported XWD: version=%d format=%d" % (version, pixmap_format))
if depth not in (24, 32) or bpp not in (24, 32):
    raise SystemExit("unsupported XWD: depth=%d bits_per_pixel=%d" % (depth, bpp))
if bytes_per_line == 0:
    bytes_per_line = width * bpp // 8

pixels = raw[header_size:]
if len(pixels) < bytes_per_line * height:
    raise SystemExit("truncated XWD pixel data")

step = bpp // 8
rows = bytearray()
for y in range(height):
    off = y * bytes_per_line
    rows.append(0)  # PNG 行过滤器：none
    for x in range(width):
        blue, green, red = pixels[off + x * step:off + x * step + 3]
        rows += bytes((red, green, blue))


def chunk(tag, data):
    # PNG chunk = 长度(4) + 类型(4) + 数据 + CRC(4)。顺序写反解码器会直接拒绝文件。
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk(b"IEND", b"")
open(dst, "wb").write(png)
print("%s: %dx%d -> %s (%d bytes)" % (src, width, height, dst, len(png)))
