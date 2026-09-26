# -*- coding: utf-8 -*-
"""把 ImageGen 生成的 1024px PNG 打包成多尺寸 ICO + 顶栏 logo PNG。

流程：手写 PNG 解码（zlib + 反滤波）→ 裁掉边缘 4%（去生成图角落的杂质）
→ 双线性缩放到 256/128/64/48/32/24/16 → 每档重编码 PNG → 打包 PNG-in-ICO。
Vista+ 的 ICO 支持 PNG 压缩存储，explorer/任务栏都能读。
"""
import struct, zlib, sys, os

SRC = sys.argv[1]
OUT_DIR = sys.argv[2]

# ---------------- PNG 解码 ----------------
def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    i = 8; idat = b''; w = h = nch = bitd = ctype = None; plte = None; trns = None
    while i < len(d):
        ln = struct.unpack_from('>I', d, i)[0]; typ = d[i+4:i+8]
        body = d[i+8:i+8+ln]
        if typ == b'IHDR':
            w, h, bitd, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        i += 12 + ln
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * nch
    prev = bytearray(stride); rows = []; pos = 0
    for y in range(h):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos+stride]); pos += stride
        if ft == 1:
            for j in range(nch, stride): line[j] = (line[j] + line[j-nch]) & 0xFF
        elif ft == 2:
            for j in range(stride): line[j] = (line[j] + prev[j]) & 0xFF
        elif ft == 3:
            for j in range(stride):
                a = line[j-nch] if j >= nch else 0
                line[j] = (line[j] + ((a + prev[j]) >> 1)) & 0xFF
        elif ft == 4:
            for j in range(stride):
                a = line[j-nch] if j >= nch else 0
                b = prev[j]; c = prev[j-nch] if j >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                line[j] = (line[j] + (a if (pa <= pb and pa <= pc)
                                      else (b if pb <= pc else c))) & 0xFF
        rows.append(bytes(line)); prev = line

    # 归一到 RGBA（注意 rows[y] 是**单行**字节，行内偏移不带 stride）
    out = bytearray(w * h * 4)
    if ctype == 6:
        for j in range(w * h):
            o = (j % w) * 4
            out[j*4:j*4+4] = rows[j//w][o:o+4]
    elif ctype == 2:
        for j in range(w * h):
            o = (j % w) * 3
            r = rows[j//w]
            out[j*4] = r[o]; out[j*4+1] = r[o+1]; out[j*4+2] = r[o+2]
            out[j*4+3] = 255
    elif ctype == 3:
        for j in range(w * h):
            idx = rows[j//w][j % w]
            out[j*4] = plte[idx*3]; out[j*4+1] = plte[idx*3+1]; out[j*4+2] = plte[idx*3+2]
            out[j*4+3] = trns[idx] if (trns and idx < len(trns)) else 255
    else:
        raise SystemExit('unsupported color type %d' % ctype)
    return w, h, bytes(out)

def bilinear(buf, sw, sh, tw, th):
    out = bytearray(tw * th * 4)
    sx = sw / tw; sy = sh / th
    for ty in range(th):
        fy = min(sh - 1.0, (ty + 0.5) * sy - 0.5)
        y0 = max(0, int(fy)); y1 = min(sh - 1, y0 + 1); wy = fy - y0
        for tx in range(tw):
            fx = min(sw - 1.0, (tx + 0.5) * sx - 0.5)
            x0 = max(0, int(fx)); x1 = min(sw - 1, x0 + 1); wx = fx - x0
            o = (ty * tw + tx) * 4
            for c in range(4):
                p00 = buf[(y0*sw+x0)*4+c]; p10 = buf[(y0*sw+x1)*4+c]
                p01 = buf[(y1*sw+x0)*4+c]; p11 = buf[(y1*sw+x1)*4+c]
                top = p00 + (p10 - p00) * wx
                bot = p01 + (p11 - p01) * wx
                out[o+c] = int(top + (bot - top) * wy + 0.5)
    return bytes(out)

def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

# ---------------- 主流程 ----------------
w, h, rgba = read_png(SRC)
print('源图 %dx%d' % (w, h))

# 裁掉边缘 4%：生成图四角偶有杂质/水印痕迹，图标缩到 16px 时会被放大
crop = int(w * 0.04)
cw = w - crop * 2
cropped = bytearray()
for y in range(crop, crop + cw):
    o = (y * w + crop) * 4
    cropped += rgba[o:o + cw * 4]
print('裁边后 %dx%d' % (cw, cw))

sizes = [256, 128, 64, 48, 32, 24, 16]
images = {}
for s in sizes:
    images[s] = bilinear(cropped, cw, cw, s, s)
    print('缩放 %dx%d ok' % (s, s))

os.makedirs(OUT_DIR, exist_ok=True)

# 顶栏 logo 用 256 那档
write_png(os.path.join(OUT_DIR, 'app_logo.png'), 256, 256, images[256])

# 打包 PNG-in-ICO
ico = struct.pack('<HHH', 0, 1, len(sizes))
blobs = []
for s in sizes:
    tmp = os.path.join(OUT_DIR, '_tmp.png')
    write_png(tmp, s, s, images[s])
    blobs.append(open(tmp, 'rb').read())
    os.remove(tmp)
offset = 6 + 16 * len(sizes)
entries = b''
for s, blob in zip(sizes, blobs):
    b = 0 if s >= 256 else s
    entries += struct.pack('<BBBBHHII', b, b, 0, 0, 1, 32, len(blob), offset)
    offset += len(blob)
open(os.path.join(OUT_DIR, 'app.ico'), 'wb').write(ico + entries + b''.join(blobs))
print('ICO 打包完成:', os.path.join(OUT_DIR, 'app.ico'),
      '共', len(sizes), '档,', os.path.getsize(os.path.join(OUT_DIR, 'app.ico')), '字节')
