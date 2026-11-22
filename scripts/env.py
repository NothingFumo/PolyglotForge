#!/usr/bin/env python3
"""构建期环境探测：给 cmd 侧装配器提供唯一一份"工具在哪里"的答案。

以前 msvc-all.bat / msvc-static.bat / py-rebuild.bat 各自写死
  D:\\Microsoft Visual Studio\\18\\...\\vcvars64.bat
  C:\\Strawberry\\c\\...
  C:\\Users\\Lenovo\\.nuget\\...
  C:\\Users\\Lenovo\\.konan\\...
换机器或升 VS 就全断。这里用 vswhere 与文件系统探测，输出一段可在 .bat 里
`call` 的 set 行；cmd 侧只消费变量，不再知道任何绝对路径。

用法：
  python scripts/env.py --bat      # set 行（CRLF，供 cmd call）
  python scripts/env.py --check    # 人读的键值清单，缺项返回非零
"""
import glob
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.environ.get("USERPROFILE") or os.path.expanduser("~")


def _first(paths):
    for p in paths:
        if p and os.path.exists(p):
            return p
    return None


def _latest(pattern):
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


def find_vcvars():
    vswhere = _first([
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
        r"C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe",
    ])
    if not vswhere:
        return None
    import subprocess
    try:
        out = subprocess.run(
            [vswhere, "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception:
        return None
    if not out:
        return None
    bat = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat")
    return bat if os.path.exists(bat) else None


def find_strawberry():
    for c in (r"C:\Strawberry\c", r"D:\Strawberry\c", r"C:\msys64\mingw64"):
        if os.path.isdir(os.path.join(c, "bin")):
            return c
    return None


def find_gccver(straw):
    if not straw:
        return None
    return _latest(os.path.join(straw, "lib", "gcc", "x86_64-w64-mingw32", "*"))


def _python_works(path):
    """WindowsApps 里的 python 是 0 字节别名，存在但执行失败；必须真跑一次。"""
    import subprocess
    try:
        return subprocess.run([path, "-c", "pass"], capture_output=True,
                              timeout=20).returncode == 0
    except Exception:
        return False


def find_python():
    cands = [os.environ.get("QF_PYTHON"), shutil.which("python"),
             shutil.which("python3"), r"D:\Python\Python311\python.exe",
             r"C:\Python311\python.exe",
             os.path.join(HOME, "AppData", "Local", "Programs", "Python", "Python311", "python.exe")]
    for c in cands:
        if c and os.path.exists(c) and _python_works(c):
            return c
    return None


def find_aot():
    base = os.path.join(HOME, ".nuget", "packages",
                        "microsoft.netcore.app.runtime.nativeaot.win-x64")
    ver = _latest(os.path.join(base, "*"))
    if not ver:
        return None
    native = os.path.join(ver, "runtimes", "win-x64", "native")
    return native if os.path.isdir(native) else None


def find_konan_libgcc():
    return _latest(os.path.join(HOME, ".konan", "dependencies", "msys2-mingw-w64-*",
                                "lib", "gcc", "x86_64-w64-mingw32", "*", "libgcc.a"))


def find_bash():
    """cmd 的 PATH 里 C:\\Windows\\System32\\bash.exe 是 WSL，不能用于 Windows 路径；
    这里显式排除它，优先 Git for Windows 自带的 bash。"""
    cands = []
    w = shutil.which("bash")
    if w and "system32" not in w.lower():
        cands.append(w)
    cands += [r"D:\Git\bin\bash.exe", r"C:\Program Files\Git\bin\bash.exe",
              r"C:\Program Files\Git\usr\bin\bash.exe"]
    return _first(cands)


def find_llvm():
    for d in (r"C:\Program Files\LLVM\bin",
              os.path.join(HOME, "scoop", "apps", "llvm", "current", "bin")):
        if os.path.isdir(d):
            return d
    return None


def detect():
    straw = find_strawberry()
    gccver = find_gccver(straw)
    llvm = find_llvm()
    return {
        "QF_VCVARS": find_vcvars(),
        "QF_STRAWBERRY": straw,
        "QF_GCCLIB": gccver,
        "QF_PYEXE": find_python(),
        "QF_AOT": find_aot(),
        "QF_KONAN_GCC": find_konan_libgcc(),
        "QF_LLVM": llvm,
        "QF_BASH": find_bash(),
    }


def main(argv):
    env = detect()
    if "--check" in argv:
        bad = 0
        for k, v in env.items():
            print("%-16s %s" % (k, v or "<缺失>"))
            if not v:
                bad = 1
        return bad

    lines = ["rem 由 scripts/env.py 生成，勿手改。"]
    for k, v in env.items():
        if v:
            lines.append("set %s=%s" % (k, v.replace("/", "\\")))
        else:
            lines.append("rem %s 未探测到" % k)
    # cmd 只认 CRLF；直接写字节，避免文本模式把 \n 再翻成 \r\n。
    sys.stdout.buffer.write(("\r\n".join(lines) + "\r\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
