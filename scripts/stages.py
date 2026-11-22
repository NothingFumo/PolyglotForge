#!/usr/bin/env python3
"""阶段清单：装配器唯一的事实来源。

一个阶段（CONTEXT.md）由一门语言实现、通过 C ABI 暴露函数。它在装配期需要被
知道的事实只有几项：构建脚本、gcc 侧归档与 -D 宏、link.exe 侧的归档、以及这个
归档由谁产出。以前这些事实抄在 scripts/build.sh、scripts/msvc-all.bat、
scripts/msvc-static.bat、scripts/test-all.sh 四处，加一个阶段要改 13~15 处。

用法：
  python scripts/stages.py --list               阶段名，一行一个
  python scripts/stages.py --bash               供 bash eval 的数组（gcc 装配器）
  python scripts/stages.py --bat msvc-all       供 call 的 set 行（link.exe 装配器）
  python scripts/stages.py --bat msvc-static
  python scripts/stages.py --artifacts          交付产物清单（验收模块用）

字段说明：
  gcc.archive    gcc 链接时直接吃进去的归档；None 表示该阶段由 MSVC 侧产出
  gcc.define     编译宿主时启用的宏；None 表示宿主无需知道
  gcc.libs       额外链接库（gcc 语法）
  gcc.cxx        链接时需要用 g++ 驱动
  msvc.all       动态 link.exe 装配时追加的归档/对象
  msvc.static    静态 link.exe 装配时追加的归档/对象
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STAGES = [
    {
        "name": "cpp",
        "build": "stages/cpp/build.sh",
        "gcc": {"archive": "stages/cpp/libqf_cpp.a", "define": "QF_HAVE_CPP",
                "libs": [], "cxx": True},
        "msvc": {"all": [], "static": []},  # parser.cpp 由装配器自己编译
    },
    {
        "name": "rust",
        "build": "stages/rust/build.sh",
        "gcc": {"archive": "stages/rust/libqf_rust.a", "define": "QF_HAVE_RUST",
                "libs": ["synchronization", "userenv"], "cxx": False},
        "msvc": {"all": [("stages/rust/target/x86_64-pc-windows-msvc/release/qf_rust.lib", "rust-msvc")],
                 "static": [("out/msvc/rust-crt-static.lib", "rust-msvc-static")]},
    },
    {
        "name": "go",
        "build": "stages/go/build.sh",
        "gcc": {"archive": "stages/go/out/libqf_go.a", "define": "QF_HAVE_GO",
                "libs": ["ws2_32", "winmm", "ntdll"], "cxx": False},
        "msvc": {"all": [("stages/go/out/libqf_go.a", "go")],
                 "static": [("stages/go/out/libqf_go.a", "go")]},
    },
    {
        "name": "kotlin",
        "build": "stages/kotlin/build.sh",
        "gcc": {"archive": "stages/kotlin/build/libqf_kt.a", "define": "QF_HAVE_KOTLIN",
                "libs": [], "cxx": False},
        "msvc": {"all": [("out/msvc/mingw/libqf_kt.a", "kotlin-msvc")],
                 "static": [("out/msvc/mingw/libqf_kt.a", "kotlin-msvc")]},
    },
    {
        "name": "js",
        "build": "stages/js/build.sh",
        "gcc": {"archive": "stages/js/out/libqf_js.a", "define": "QF_HAVE_JAVASCRIPT",
                "libs": [], "cxx": False},
        "msvc": {"all": [("stages/js/out/libqf_js.a", "js-gcc")],
                 "static": [("stages/js/out/qf_js_msvc.lib", "js-msvc")]},
    },
    {
        "name": "csharp",
        "build": "stages/csharp/build.sh",
        "gcc": {"archive": None, "define": "QF_HAVE_CSHARP",
                "libs": ["synchronization", "userenv", "ole32", "oleaut32", "uuid", "bcrypt"],
                "cxx": False},
        "msvc": {"all": [("stages/csharp/out/qf_cs.lib", "csharp")],
                 "static": [("stages/csharp/out/qf_cs.lib", "csharp")]},
    },
    {
        "name": "python",
        "build": "stages/python/build.sh",
        "gcc": {"archive": None, "define": "QF_HAVE_PYTHON",
                "libs": ["ucrtbase", "pathcch", "version"], "cxx": False},
        "msvc": {"all": [("stages/python/out/qf_py.obj", "python-obj"),
                          ("stages/python/out/qf_py_diag.obj", "python-obj"),
                          ("stages/python/out/qf_py_value.obj", "python-obj"),
                          ("stages/python/out/python311.lib", "python-obj"),
                          ("out/payload_blob.obj", "payload")],
                 "static": [("stages/python/out/qf_py.obj", "python-obj"),
                            ("stages/python/out/qf_py_diag.obj", "python-obj"),
                            ("stages/python/out/qf_py_value.obj", "python-obj"),
                            ("stages/python/out/python311.lib", "python-obj"),
                            ("out/payload_blob.obj", "payload")]},
    },
]

# 交付产物：验收模块按这份清单遍历，新增产物不必再改测试脚本。
ARTIFACTS = [
    {"path": "dist/queryforge.exe", "kind": "static"},
    {"path": "dist/queryforge-deps.exe", "kind": "dynamic"},
    {"path": "dist/queryforge-all.exe", "kind": "dynamic"},
    {"path": "dist/queryforge-static-msvc.exe", "kind": "static"},
]

# 动态产物必须随包的第三方 DLL（含 DLL 自身的依赖）。
RUNTIME_DLLS = [
    "libstdc++-6.dll", "libwinpthread-1.dll", "libgcc_s_seh-1.dll",
    "vcruntime140.dll", "vcruntime140_1.dll",
]


def q(s):
    return "'" + s.replace("'", "'\\''") + "'"


def emit_bash(out):
    names = " ".join(s["name"] for s in STAGES)
    out.write("QF_STAGES=(%s)\n" % names)
    for s in STAGES:
        g = s["gcc"]
        out.write("QF_STAGE_BUILD[%s]=%s\n" % (s["name"], q(s["build"])))
        out.write("QF_STAGE_DEFINE[%s]=%s\n" % (s["name"], q(g["define"] or "")))
        out.write("QF_STAGE_ARCHIVE[%s]=%s\n" % (s["name"], q(g["archive"] or "")))
        out.write("QF_STAGE_LIBS[%s]=%s\n" % (s["name"], q(" ".join(g["libs"]))))
        out.write("QF_STAGE_CXX[%s]=%s\n" % (s["name"], "1" if g["cxx"] else "0"))
    out.write("QF_ARTIFACTS=(%s)\n" % " ".join(q(a["path"]) for a in ARTIFACTS))


def win(path):
    return path.replace("/", "\\")


def emit_bat(out, profile):
    """输出三段：宏、产物清单、缺失产物的生成规则。

    每个 msvc 条目是 (路径, 目标)，目标名交给 scripts/produce.py 生成；
    装配器不再自己知道 rust/csharp/kotlin 怎么建，只负责"缺了就喊"。
    每一步之后都复查，失败立即返回，绝不带着半成品链接。
    """
    key = "all" if profile == "msvc-all" else "static"
    defines = " ".join("/D" + s["gcc"]["define"] for s in STAGES if s["gcc"]["define"])
    pairs = []
    for s in STAGES:
        pairs.extend(s["msvc"][key])
    out.write("set QF_DEFS=%s\r\n" % defines)
    out.write("set QF_STAGE_ARCHS=%s\r\n" % " ".join(win(p) for p, _ in pairs))
    for path, target in pairs:
        out.write('if not exist "%s" "%%QF_PYEXE%%" scripts\\produce.py %s\r\n'
                  % (win(path), target))
    for path, _ in pairs:
        out.write('if not exist "%s" (echo [stage] 无法生成 %s & exit /b 1)\r\n'
                  % (win(path), win(path)))


def main(argv):
    # 输出被 bash eval / cmd call 直接消费：必须固定 LF，否则 Windows 文本模式
    # 会把 \n 变成 \r\n，数组元素带上 \r，路径判断全部失败。
    sys.stdout.reconfigure(newline="\n")
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bash", action="store_true")
    ap.add_argument("--bat")
    ap.add_argument("--artifacts", action="store_true")
    ap.add_argument("--kind")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    if a.list:
        print("\n".join(s["name"] for s in STAGES))
    elif a.bash:
        emit_bash(sys.stdout)
    elif a.bat:
        # cmd 只认 CRLF：关掉换行翻译，由 emit_bat 自己写 \r\n。
        sys.stdout.reconfigure(newline="")
        if a.bat not in ("msvc-all", "msvc-static"):
            raise SystemExit("未知 profile: %s" % a.bat)
        emit_bat(sys.stdout, a.bat)
    elif a.kind:
        for art in ARTIFACTS:
            if art["path"] == a.kind:
                print(art["kind"])
                break
        else:
            print("unknown")
    elif a.artifacts:
        print("\n".join(x["path"] for x in ARTIFACTS))
    elif a.json:
        import json
        json.dump({"stages": STAGES, "artifacts": ARTIFACTS, "dlls": RUNTIME_DLLS},
                  sys.stdout, ensure_ascii=False, indent=2)
    else:
        ap.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
