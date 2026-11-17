"""把 CPython 的 Lib/ 打成可被 zipimport 直接使用的标准库载荷。

用法：python scripts/make_payload.py
产物：out/payload/lib.zip

排除只对 CPython 自身开发/测试有用的目录（test、idlelib、tkinter…），
以及 site-packages：本项目只要求 UDF 能 import 标准库，不承诺第三方包。
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "stages", "python", "vendor", "Python-3.11.9", "Lib")
OUT = os.path.join(ROOT, "out", "payload", "lib.zip")

SKIP = {
    "test", "tests", "idlelib", "tkinter", "turtledemo", "lib2to3",
    "ensurepip", "venv", "distutils", "site-packages", "config-3.11",
    "__pycache__",
}


def main():
    if not os.path.isdir(LIB):
        print("缺少 CPython Lib 目录: %s" % LIB, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    count = 0
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(LIB):
            dirs[:] = sorted(d for d in dirs if d not in SKIP)
            for name in sorted(files):
                if name.endswith((".pyc", ".pyo")):
                    continue
                path = os.path.join(root, name)
                arc = os.path.relpath(path, LIB)
                z.write(path, arc)
                count += 1

    print("payload: %d 个文件, %.1f MB" % (count, os.path.getsize(OUT) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
