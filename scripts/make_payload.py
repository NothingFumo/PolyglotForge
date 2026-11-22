"""把 CPython 的 Lib/ 打成可被 zipimport 直接使用的标准库载荷。

用法：
  python scripts/make_payload.py            # 只打 zip → out/payload/lib.zip
  python scripts/make_payload.py --object   # 再生成可链入的只读对象 → out/payload_blob.obj

两件事放在同一个 module 里：zip 的成员策略与"链入时必须是只读段"是同一条
载荷契约的两半，分开写在两个脚本里时容易只改一半。

排除只对 CPython 自身开发/测试有用的目录（test、idlelib、tkinter…），
以及 site-packages：本项目只要求 UDF 能 import 标准库，不承诺第三方包。
"""
import os
import shutil
import subprocess
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


def _pick(*cands):
    for c in cands:
        if c and (shutil.which(c) or os.path.isfile(c)):
            return c
    return None


def build_object():
    """把 lib.zip 变成可被 gcc / link.exe 直接消费的只读对象。

    ld 的 PE 目标不支持 --rename-section，所以先用 ld -r -b binary 生成
    .data 段，再用 objcopy 改名成 .rdata 并标记只读。
    """
    out_dir = os.path.join(ROOT, "out")
    obj = os.path.join(out_dir, "payload_blob.obj")
    ld = _pick(os.environ.get("LD"), "ld",
               r"C:\Strawberry\c\bin\ld.exe") or "ld"
    objcopy = _pick(os.environ.get("OBJCOPY"), "objcopy",
                    r"C:\Strawberry\c\bin\objcopy.exe") or "objcopy"
    subprocess.run([ld, "-r", "-b", "binary", "payload/lib.zip", "-o", "payload_blob.obj"],
                   cwd=out_dir, check=True)
    subprocess.run([objcopy,
                    "--rename-section", ".data=.rdata,alloc,load,readonly,data,contents",
                    "payload_blob.obj"], cwd=out_dir, check=True)
    if not os.path.isfile(obj):
        raise SystemExit("载荷对象没有生成: %s" % obj)
    print("payload object: %s (只读 .rdata)" % obj)


def main(argv):
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
    if "--object" in argv:
        build_object()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
