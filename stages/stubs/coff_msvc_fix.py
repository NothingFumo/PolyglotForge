#!/usr/bin/env python3
"""把 GNU 工具链产出的 COFF 改造成 MSVC link.exe 能消费的形状。

link.exe 只接受“MSVC 形状”的 COFF。MinGW 的 gcc / llvm（Kotlin-Native）产出的
对象在三处违约，每一处都实测为一个硬错误：

1. **静态初始化表**：gcc/LLVM 把全局构造函数放进 `.ctors`，link.exe 不认这个约定，
   不会调用其中任何一个函数。Kotlin/Native 的运行时全局（SafePoint 互斥量、
   GlobalData、ThreadSuspension）就靠它初始化，缺了以后 `qf_dsl_build` 在
   `initRuntime` 里对空互斥量 `pthread_mutex_lock(NULL)` 直接访问违例。
   处理：改名成 `.CRT$XCU`（与 `.ctors` 等长 8 字节，可原地改写），并把特性位里的
   可写标志去掉 —— link.exe 只在特性与 MSVC 自己的初始化表一致时，才把这些贡献
   折进 `.rdata` 的 `__xc_a..__xc_z` 区间；带可写标志会另起一个独立的 `.CRT` 节，
   同样一个都不会被调用（这一条是实测出来的：改名前 `__xc_a/__xc_z` 区间只有
   0x20 字节，改名并清标志后区间里出现了 0x28 字节的 `.CRT$XCU`）。

2. **COMDAT**：`.xdata$X` / `.pdata$X` 是 `SELECT_ANY` 但没有代表符号，link.exe 报

       fatal error LNK1143: 无效或损坏的文件: 没有 COMDAT 0xD 节的符号

   `dumpbin /headers` 把同一判定写作 `COMDAT (no symbol)`。正确形状是 MSVC 目标上
   LLVM 自己会发出的“关联 COMDAT”：改成 `SELECT_ASSOCIATIVE` 并把 Number 指向
   父节 `.text$X`（关联节由父节的符号代表）。这一步对 `libstdc++.a` 同样必要 ——
   清理完 Kotlin 对象后 `string-inst.o` 就是下一个 LNK1143。

3. **节数**：gcc 用 `.text.unlikely.<sym>` / `.xdata.unlikely.<sym>` 这类点号后缀
   给每个函数开节，link.exe 按完整节名合并，于是每个贡献都变成独立的 PE 节。
   PE 规格上限是 96 节，超过时 link.exe 给出 `LNK4072: 节计数 212 超过最大值(96)`。
   处理：把点号后缀折回基名（`.text` / `.xdata` / `.pdata`），实测 212 → 17。

所有改写都是等长原地改写，文件布局、节偏移、符号表索引都不变，因此也可以直接
作用在 `ar` 归档上（归档成员逐个处理，长度不变所以成员索引不失效）。

用法：python coff_msvc_fix.py <in(obj|a)> <out>
退出码：0 表示已写出；1 表示参数或对象格式不对。
"""
import struct
import sys

AR_MAGIC = b"!<arch>\n"
AR_HDR = 60

IMAGE_SCN_LNK_COMDAT = 0x00001000
IMAGE_SCN_MEM_WRITE = 0x80000000
IMAGE_SYM_CLASS_STATIC = 3
SELECT_ASSOCIATIVE = 5


class Coff:
    def __init__(self, data):
        self.data = data
        machine, nsec, _ts, symptr, nsym, optsz, _chars = struct.unpack_from("<HHIIIHH", data, 0)
        if machine != 0x8664:
            raise SystemExit("不是 x86-64 COFF：machine=0x%04x" % machine)
        if optsz != 0:
            raise SystemExit("对象文件不应带可选头")
        self.nsec = nsec
        self.symptr = symptr
        self.nsym = nsym
        self.strtab = symptr + nsym * 18
        self.sec_off = 20
        self.secs = {}
        for i in range(1, nsec + 1):
            off = self.sec_off + (i - 1) * 40
            raw = struct.unpack_from("<8sIIIIIIHHI", data, off)
            self.secs[i] = {"off": off, "name": self._sec_name(raw[0]), "chars": raw[9]}
        self.section_syms = {}
        for off, name, value, sec, _typ, cls, naux in self._symbols():
            if cls == IMAGE_SYM_CLASS_STATIC and naux >= 1 and value == 0 and sec > 0:
                self.section_syms.setdefault(sec, (off, naux))

    def _sec_name(self, raw):
        name = raw.rstrip(b"\0").decode("latin1")
        if name.startswith("/") and name[1:].isdigit():
            off = self.strtab + int(name[1:])
            end = self.data.index(b"\0", off)
            return self.data[off:end].decode("latin1")
        return name

    def _symbols(self):
        i = 0
        while i < self.nsym:
            off = self.symptr + i * 18
            name, value, sec, typ, cls, naux = struct.unpack_from("<8sIhHBB", self.data, off)
            yield off, name, value, sec, typ, cls, naux
            i += 1 + naux

    def patch(self):
        by_name = {}
        for idx, info in self.secs.items():
            by_name.setdefault(info["name"], idx)
        # .ctors 是 gcc 的静态初始化表，link.exe 只认 .CRT$XCU。两者都是
        # “函数指针数组、CRT 在 main 之前逐个调用”，且 ".CRT$XCU" 恰好 8 字节，
        # 与 PE 节名域等长，可以直接原地改名（不动文件布局）。
        renamed_ctors = 0
        for _idx, info in self.secs.items():
            if info["name"] == ".ctors":
                self.data[info["off"]:info["off"] + 8] = b".CRT$XCU"
                # 与 MSVC 自己的初始化表保持同一特性：link.exe 只在特性一致时
                # 把 .CRT$ 贡献折进 .rdata 里的 __xc_a..__xc_z 区间；
                # 可写标志 (0x80000000) 会让它另起一个独立的 .CRT 输出节，
                # 于是一个初始化函数都不会被调用。
                ch_off = info["off"] + 36
                ch = struct.unpack_from("<I", self.data, ch_off)[0]
                struct.pack_into("<I", self.data, ch_off, ch & ~IMAGE_SCN_MEM_WRITE)
                renamed_ctors += 1
        # GCC 用 .text.unlikely.<sym> / .xdata.unlikely.<sym> / .pdata.unlikely.<sym>
        # 给每个函数单独开节。link.exe 只按节名合并，这些名字各不相同的贡献会
        # 变成一个个独立的 PE 节——Windows 的 PE 规格上限是 96 节，超过时
        # link.exe 给出 LNK4072 且映像“可能无法运行”。把点号后缀折回基名即可。
        collapsed = 0
        for _idx, info in self.secs.items():
            for prefix in (".text", ".xdata", ".pdata"):
                if info["name"].startswith(prefix + "."):
                    self.data[info["off"]:info["off"] + 8] = prefix.encode().ljust(8, b"\0")
                    collapsed += 1
                    break
        made_assoc = made_plain = 0
        for idx, info in sorted(self.secs.items()):
            if not info["chars"] & IMAGE_SCN_LNK_COMDAT:
                continue
            name = info["name"]
            if not (name.startswith(".xdata$") or name.startswith(".pdata$")):
                continue
            sym = self.section_syms.get(idx)
            if sym is None:
                continue
            parent = by_name.get(".text$" + name.split("$", 1)[1])
            off, _naux = sym
            aux = off + 18
            if parent is not None:
                struct.pack_into("<H", self.data, aux + 12, parent)  # Number = 父节号
                self.data[aux + 14] = SELECT_ASSOCIATIVE
                made_assoc += 1
            else:
                self.data[aux + 14] = 0
                chars_off = info["off"] + 36
                chars = struct.unpack_from("<I", self.data, chars_off)[0]
                struct.pack_into("<I", self.data, chars_off, chars & ~IMAGE_SCN_LNK_COMDAT)
                made_plain += 1
        return renamed_ctors, collapsed, made_assoc, made_plain


def patch_archive(data):
    """就地清理 ar 归档里每个 COFF 成员的 COMDAT，返回 (成员数, 关联数, 退化数)。"""
    pos = len(AR_MAGIC)
    members = ctors = collapsed = assoc = plain = 0
    while pos + AR_HDR <= len(data):
        hdr = data[pos:pos + AR_HDR]
        size = int(hdr[48:58].decode("ascii").strip() or 0)
        body = pos + AR_HDR
        raw = data[body:body + size]
        if len(raw) >= 2 and struct.unpack_from("<H", raw, 0)[0] == 0x8664:
            chunk = bytearray(raw)
            c, col, a, p = Coff(chunk).patch()
            data[body:body + size] = chunk
            ctors += c
            collapsed += col
            assoc += a
            plain += p
            members += 1
        pos = body + size + (size & 1)
    return members, ctors, collapsed, assoc, plain


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 1
    src, dst = argv[1], argv[2]
    with open(src, "rb") as f:
        data = bytearray(f.read())
    if data[:8] == AR_MAGIC:
        members, ctors, collapsed, assoc, plain = patch_archive(data)
        note = "%d 个 COFF 成员，%d 个 .ctors 改名，%d 个长节名折叠" % (members, ctors, collapsed)
    else:
        ctors, collapsed, assoc, plain = Coff(data).patch()
        note = "单对象，%d 个 .ctors 改名，%d 个长节名折叠" % (ctors, collapsed)
    with open(dst, "wb") as f:
        f.write(data)
    print("[coff] %s -> %s（%s）：%d 个 .xdata/.pdata 转为关联 COMDAT，%d 个退化为普通节"
          % (src, dst, note, assoc, plain))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
