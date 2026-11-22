"""从内嵌 zip 读取标准库源码的内存导入器（在 qf_py.c 里被注入执行）。

约定：
  - C 侧在 exec 本脚本前把 zip 字节放到 builtins.__qf_zip__；
  - 只用内建模块（sys/_struct/zlib），因为此时代码库还没挂上；
  - 只解析中央目录，按需解码单个成员，不做全量解压。
"""
import sys
import zlib
import _struct

_builtins = sys.modules["builtins"]
_data = bytes(getattr(_builtins, "__qf_zip__", b""))
_frozen = sys.modules.get("_frozen_importlib")


def _u16(off):
    return _struct.unpack_from("<H", _data, off)[0]


def _u32(off):
    return _struct.unpack_from("<I", _data, off)[0]


_entries = {}
if _data:
    _eocd = _data.rfind(b"PK\x05\x06")
    if _eocd >= 0:
        _count = _u16(_eocd + 10)
        _pos = _u32(_eocd + 16)
        for _ in range(_count):
            if _data[_pos:_pos + 4] != b"PK\x01\x02":
                break
            _method = _u16(_pos + 10)
            _csize = _u32(_pos + 20)
            _nlen = _u16(_pos + 28)
            _elen = _u16(_pos + 30)
            _clen = _u16(_pos + 32)
            _lho = _u32(_pos + 42)
            _name = _data[_pos + 46:_pos + 46 + _nlen].decode("utf-8")
            _entries[_name] = (_method, _csize, _lho)
            _pos += 46 + _nlen + _elen + _clen


class _Loader:
    def __init__(self, name, info):
        self.name = name
        self.info = info

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        method, csize, lho = self.info
        nlen = _u16(lho + 26)
        elen = _u16(lho + 28)
        start = lho + 30 + nlen + elen
        raw = _data[start:start + csize]
        if method == 8:
            raw = zlib.decompress(raw, -15)
        src = raw.decode("utf-8")
        code = compile(src, "<zip:%s>" % self.name, "exec")
        exec(code, module.__dict__)


class _Finder:
    def find_spec(self, fullname, path=None, target=None):
        base = fullname.replace(".", "/")
        info = _entries.get(base + "/__init__.py")
        is_pkg = info is not None
        if info is None:
            info = _entries.get(base + ".py")
        if info is None:
            return None
        _member = base + ("/__init__.py" if is_pkg else ".py")
        spec = _frozen.ModuleSpec(fullname, _Loader(fullname, info),
                                  origin="<zip>/" + _member)
        if is_pkg:
            spec.submodule_search_locations = []
        return spec


if _entries:
    sys.meta_path.insert(0, _Finder())
