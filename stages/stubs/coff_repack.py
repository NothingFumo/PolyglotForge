#!/usr/bin/env python
"""把 MSVC 静态库拆成 MinGW 的 ld 能正确消费的 COFF 目标文件。

为什么需要这一步（实测，见 docs/adr/0007）：

1. ld 从 **MSVC 归档** 里取出成员时，会把成员里 COMDAT 节的定义丢掉一部分：
   同一个目标里既引用又定义 `?SetFree@CObjectHeader@WKS@@QEAAX_K@Z` 时，
   ld 只登记引用，定义在 COMDAT 归并阶段被丢弃，最终链接报 undefined。
   把同样的成员当**普通文件**传给 ld 则一切正常。因此这里解包归档、逐个落盘。
2. 即便落盘，只要节上还带 IMAGE_SCN_LNK_COMDAT，ld 仍可能按“节名”而不是
   “COMDAT 签名符号”去重（MSVC 用 /Gy 时每个函数都叫 .text$mn），导致定义被
   错误丢弃。清掉该标志后由 `ld --allow-multiple-definition` 承担去重语义——
   COMDAT 的重复定义本来就是同码，取任意一份都正确。
3. MSVC 的 TLS 动态初始化列表 `.CRT$XDU` 被 MinGW 的 `__dyn_tls_init` 在
   TLS 数组尚未装配时提前调用（实测 TLS 块指针为脏值 → 访问违例）。把它改名成
   `.CRT$XCU`，改由 CRT 的 `_initterm` 在 TLS 就绪之后执行。

用法：coff_repack.py <in.lib> <out_dir>
输出：out_dir 下每个归档成员一个 .obj（文件名扁平化、去重、稳定）。

注意：不做反汇编、不改符号表，只改节头（Characteristics / Name）与文件布局。
"""

import glob
import os
import re
import struct
import sys

COMDAT = 0x1000
# MSVC 的 TLS 动态初始化列表 → MinGW 的 C 初始化列表（均在 TLS 就绪后执行）
SECTION_RENAMES = {b".CRT$XDU": b".CRT$XCU"}
# 注意：.CRT$XDW 之类带尾部变体的名字同样按前缀匹配
SECTION_PREFIX_RENAMES = [(b".CRT$XD", b".CRT$XC")]


def _long_name(table, offset):
    """MSVC 归档的长名表用 \\0 分隔（不是 POSIX 的 \\n），两种都容忍。"""
    s = table[offset:]
    for term in (b"\x00", b"\n", b"\r"):
        i = s.find(term)
        if i >= 0:
            s = s[:i]
    return s.decode("latin1")


def unpack(path):
    data = open(path, "rb").read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit("%s: 不是 ar 归档" % path)
    off = 8
    longnames = b""
    out = []
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        off += 60
        name = hdr[:16].decode("latin1").rstrip()
        size = int(hdr[48:58].decode("latin1").strip())
        body = data[off:off + size]
        off += size + (size & 1)
        if name == "//":
            longnames = body
            continue
        if name in ("/", "/SYM64/") or size == 0:
            continue  # 索引成员
        if name.startswith("/") and name[1:].isdigit():
            name = _long_name(longnames, int(name[1:]))
        else:
            name = name.rstrip("/")
        out.append((name, body))
    return out


def flatten(name):
    base = re.sub(r"[^A-Za-z0-9._-]", "_", name).strip("_")
    return base[-100:]


def normalize(buf):
    """清 COMDAT 标志、重命名指定节。返回 (新内容, 清标志数, 改名数)。"""
    d = bytearray(buf)
    mach, nsec = struct.unpack_from("<HH", d, 0)
    if mach == 0 or nsec == 0:
        return buf, 0, 0
    soh = struct.unpack_from("<H", d, 16)[0]
    base = 20 + soh
    if base + nsec * 40 > len(d):
        return buf, 0, 0
    cleared = renamed = 0
    for i in range(nsec):
        o = base + i * 40
        chars = struct.unpack_from("<I", d, o + 36)[0]
        if chars & COMDAT:
            struct.pack_into("<I", d, o + 36, chars & ~COMDAT)
            cleared += 1
        raw = bytes(d[o:o + 8]).rstrip(b"\x00")
        new = SECTION_RENAMES.get(raw)
        if new is None:
            for old, rep in SECTION_PREFIX_RENAMES:
                if raw.startswith(old):
                    new = rep + raw[len(old):]
                    break
        if new is not None and new != raw:
            d[o:o + 8] = new + b"\x00" * (8 - len(new))
            renamed += 1
    return bytes(d), cleared, renamed


def main(argv):
    if len(argv) != 3:
        raise SystemExit("用法：coff_repack.py <in.lib> <out_dir>")
    src, outdir = argv[1], argv[2]
    os.makedirs(outdir, exist_ok=True)
    for stale in glob.glob(os.path.join(outdir, "*.obj")):
        os.remove(stale)
    seen = {}
    cleared = renamed = 0
    members = unpack(src)
    for name, body in members:
        base = flatten(name)
        n = seen.get(base, 0)
        seen[base] = n + 1
        fname = base if n == 0 else "%d_%s" % (n, base)
        body, c, r = normalize(body)
        cleared += c
        renamed += r
        with open(os.path.join(outdir, fname + ".obj"), "wb") as f:
            f.write(body)
    print("%s: %d 个成员, 清 COMDAT 标志 %d 处, 节改名 %d 处"
          % (os.path.basename(src), len(members), cleared, renamed))


if __name__ == "__main__":
    main(sys.argv)
