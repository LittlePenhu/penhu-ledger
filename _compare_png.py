# 对比两组 PNG 截图是否逐像素一致（布局回归断言用）。
# 纯 stdlib：zlib 解 IDAT 得 raw 扫描线数据；相同 raw 且 IHDR 相同 = 像素完全一致。
# raw 不同时反 filter（PNG 标准 5 种 filter）统计差异像素数。
import struct
import sys
import zlib
from pathlib import Path


def read_chunks(p: Path):
    data = p.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"不是 PNG: {p}"
    pos = 8
    idat = b""
    ihdr = None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)  # w h depth color comp filter interlace
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + ln
    assert ihdr is not None
    w, h, depth, color, comp, filt, interlace = ihdr
    assert depth == 8 and interlace == 0, f"不支持的 PNG 形态 {ihdr}"
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(idat)
    return (w, h, color, channels), raw


def unfilter(raw, w, h, channels):
    stride = w * channels
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _y in range(h):
        f = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        bpp = channels
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line
    return bytes(out)


def compare(pa: Path, pb: Path):
    (wa, ha, ca, cha), rawa = read_chunks(pa)
    (wb, hb, cb, chb), rawb = read_chunks(pb)
    if (wa, ha, ca) != (wb, hb, cb):
        return f"尺寸/色彩不同 {wa}x{ha}c{ca} vs {wb}x{hb}c{cb}"
    if rawa == rawb:
        return "像素完全一致"
    pixa = unfilter(rawa, wa, ha, cha)
    pixb = unfilter(rawb, wb, hb, chb)
    if pixa == pixb:
        return "像素一致（仅编码 filter 策略不同）"
    diff = 0
    maxd = 0
    for i in range(0, len(pixa), cha):
        d = max(abs(pixa[i + k] - pixb[i + k]) for k in range(cha))
        if d:
            diff += 1
            maxd = max(maxd, d)
    total = wa * ha
    return f"差异像素 {diff}/{total}（{100.0 * diff / total:.3f}%）最大通道差 {maxd}"


if __name__ == "__main__":
    a_dir = Path(sys.argv[1])
    b_dir = Path(sys.argv[2])
    names = sorted({p.name for p in a_dir.glob("*.png")} & {p.name for p in b_dir.glob("*.png")})
    only_a = sorted({p.name for p in a_dir.glob("*.png")} - set(names))
    only_b = sorted({p.name for p in b_dir.glob("*.png")} - set(names))
    print(f"共同文件 {len(names)} 仅前 {only_a} 仅后 {only_b}")
    bad = 0
    for n in names:
        r = compare(a_dir / n, b_dir / n)
        ok = "一致" in r
        if not ok:
            bad += 1
        print(f"  [{'OK ' if ok else 'DIF'}] {n}: {r}")
    print(f"结论: {len(names) - bad} 一致 / {bad} 有差异")
    sys.exit(1 if bad else 0)
