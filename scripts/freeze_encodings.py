"""把 encodings 冻结进解释器：生成 .h 与 qf_py_frozen.c。

为什么要冻结 encodings：Py_InitializeFromConfig 在导入机制就绪前就要确定文件系统
编码，此时内存/zip finder 还没装上。encodings 是纯 Python 包，必须变成 `_frozen`
条目才能零落盘启动。

产物：
  stages/python/frozen/encodings_*.h   每个模块一份 marshal 数据
  stages/python/qf_py_frozen.c         汇总表 + 安装函数
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(ROOT, "stages", "python", "vendor", "Python-3.11.9")
FREEZE = os.path.join(VENDOR, "PCbuild", "amd64", "_freeze_module.exe")
SRC = os.path.join(VENDOR, "Lib", "encodings")
OUT_H = os.path.join(ROOT, "stages", "python", "frozen")
OUT_C = os.path.join(ROOT, "stages", "python", "qf_py_frozen.c")


def sym(modname: str) -> str:
    return "_Py_M__" + re.sub(r"[^0-9A-Za-z_]", "_", modname)


def main():
    if not os.path.isfile(FREEZE):
        print("缺少 _freeze_module.exe: %s" % FREEZE, file=sys.stderr)
        return 1
    os.makedirs(OUT_H, exist_ok=True)

    entries = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".py"):
            continue
        mod = "encodings." + name[:-3] if name != "__init__.py" else "encodings"
        hdr = os.path.join(OUT_H, mod + ".h")
        r = subprocess.run(
            [FREEZE, mod, os.path.join(SRC, name), hdr],
            cwd=os.path.join(VENDOR, "PCbuild", "amd64"),
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            print("冻结失败 %s: %s" % (mod, r.stderr.strip()), file=sys.stderr)
            return 1
        entries.append((mod, "frozen/" + os.path.basename(hdr)))

    lines = [
        "/* 由 scripts/freeze_encodings.py 生成，勿手改。 */",
        "#include <Python.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "",
    ]
    for mod, hdr in entries:
        lines.append('#include "%s"' % hdr)
    lines += [
        "",
        "static const struct _frozen qf_frozen_table[] = {",
    ]
    for mod, _ in entries:
        lines.append('    {"%s", %s, (int)sizeof(%s)},' % (mod, sym(mod), sym(mod)))
    lines += [
        "    {0, 0, 0},",
        "};",
        "",
        "/* 把冻结表接到解释器的默认表后面：默认表里是 importlib/io/os 等引导模块，",
        "   丢掉它们解释器起不来。 */",
        "void qf_py_install_frozen(void) {",
        "    const struct _frozen* base = PyImport_FrozenModules;",
        "    size_t n = 0;",
        "    while (base && base[n].name) n++;",
        "    const size_t m = sizeof(qf_frozen_table) / sizeof(qf_frozen_table[0]) - 1;",
        "    struct _frozen* all = (struct _frozen*)malloc((n + m + 1) * sizeof(*all));",
        "    if (!all) return;",
        "    memcpy(all, base, n * sizeof(*all));",
        "    memcpy(all + n, qf_frozen_table, m * sizeof(*all));",
        "    memset(all + n + m, 0, sizeof(*all));",
        "    PyImport_FrozenModules = all;",
        "}",
        "",
    ]
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("frozen: %d 个 encodings 模块 -> %s" % (len(entries), OUT_C))
    return 0


if __name__ == "__main__":
    sys.exit(main())
