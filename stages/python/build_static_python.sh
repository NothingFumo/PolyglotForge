#!/usr/bin/env bash
# 从 CPython 源码产出可静态链入的 python311.lib。
#
# 流程沿用 CPython 官方 PCbuild，只在 vendor 内做了四处改造（全部由本脚本落实）：
#   1. PCbuild/pythoncore.vcxproj：ConfigurationType 改成 StaticLibrary；
#   2. 预处理器宏去掉 _USRDLL/Py_ENABLE_SHARED，改用 Py_NO_ENABLE_SHARED，
#      否则 PyAPI_FUNC 会退化成 __declspec(dllimport)；
#   3. PCbuild/pyproject.props 关掉 WholeProgramOptimization：/GL 产物是 IL，
#      binutils(ld) 读不了，会挡住 MinGW 主干链接；
#   4. 新增 PC/qf_static_support.c：静态构建时 Py_ENABLE_SHARED 关闭导致
#      PyWin_DLLhModule 没有定义，而 getpath/dynload_win/sysmodule 仍然引用它。
# 细节与踩坑记录见同目录 STATIC-NOTES.md。
set -euo pipefail
stage="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$stage"
mkdir -p vendor out

die() { echo "错误：$*" >&2; exit 1; }

. "$stage/msvc_env.sh"
qf_msvc_setup || die "MSVC 环境准备失败"

VER="3.11.9"
TGZ="vendor/Python-$VER.tgz"
SRC="vendor/Python-$VER"
URL="https://www.python.org/ftp/python/$VER/Python-$VER.tgz"

pick_python() {
    local c
    for c in "$(command -v python 2>/dev/null || true)" \
             /mnt/d/Python/Python311/python.exe /mnt/c/Python311/python.exe; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        if "$c" -c 'import sys; sys.exit(0 if sys.version_info[:2] == (3, 11) else 1)' 2>/dev/null; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}
PY="$(pick_python)" || die "需要 Python 3.11 驱动 deepfreeze（字节码版本必须与 3.11 一致）"
PY_WIN="$(to_win "$PY")"

if [ ! -f "$TGZ" ]; then
    echo "[cpython] 下载源码 $URL"
    curl -L --fail --retry 3 -o "$TGZ" "$URL"
fi
if [ ! -d "$SRC" ]; then
    echo "[cpython] 解压 $TGZ"
    tar -xzf "$TGZ" -C vendor
fi
[ -f "$SRC/PCbuild/pythoncore.vcxproj" ] || die "源码解压不完整：$SRC"

# 外部依赖：pythoncore 只用 zlib。官方脚本走 urllib，某些机器取不到证书链，
# 于是失败后回落到 curl 抓同一个 cpython-source-deps 压缩包。
if [ ! -f "$SRC/externals/zlib-1.3.1/zlib.h" ]; then
    echo "[cpython] 抓 externals"
    ( cd "$SRC/PCbuild" && MSYS_NO_PATHCONV=1 "$(command -v cmd.exe)" /c \
        'get_externals.bat --no-tkinter --no-openssl --no-libffi --python '"$(to_win "$PY")" ) || true
fi
if [ ! -f "$SRC/externals/zlib-1.3.1/zlib.h" ]; then
    echo "[cpython] get_externals 未成功（证书链），改用 curl 抓 zlib"
    mkdir -p "$SRC/externals/zips"
    curl -L --fail --retry 3 -o "$SRC/externals/zips/zlib-1.3.1.zip" \
        https://github.com/python/cpython-source-deps/archive/zlib-1.3.1.zip
    "$PY" - "$SRC/externals/zips/zlib-1.3.1.zip" "$SRC/externals" <<'PYEOF'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PYEOF
    rm -rf "$SRC/externals/zlib-1.3.1"
    mv "$SRC/externals/cpython-source-deps-zlib-1.3.1" "$SRC/externals/zlib-1.3.1"
fi
[ -f "$SRC/externals/zlib-1.3.1/zlib.h" ] || die "zlib 外部依赖缺失"

# 静态库补丁文件（见文件头第 4 条）。
cat > "$SRC/PC/qf_static_support.c" <<'CEOF'
/*
 * 静态 pythoncore 的 Windows 补丁源文件。
 *
 * 上游 PC/dl_nt.c 把 PyWin_DLLhModule 的定义（以及 DllMain）整个包在
 * #ifdef Py_ENABLE_SHARED 里；静态链接时 Py_ENABLE_SHARED 关闭，于是
 * Modules/getpath.c、Python/dynload_win.c、Python/sysmodule.c 的引用悬空。
 * 静态构建没有 DLL 句柄可言，给一个空句柄即可。
 */
#include <windows.h>

HMODULE PyWin_DLLhModule = NULL;
CEOF

# vendor 内的项目改造。重复执行是安全的。
"$PY" - "$SRC" <<'PYEOF'
import io, sys
root = sys.argv[1]

def patch(path, old, new):
    with io.open(path, encoding='utf-8-sig') as f:
        text = f.read()
    if new in text:
        return
    if old not in text:
        raise SystemExit('patch anchor missing in %s' % path)
    with io.open(path, 'w', encoding='utf-8-sig', newline='') as f:
        f.write(text.replace(old, new, 1))

core = root + '/PCbuild/pythoncore.vcxproj'
patch(core,
      '<ConfigurationType>DynamicLibrary</ConfigurationType>',
      '<ConfigurationType>StaticLibrary</ConfigurationType>')
patch(core,
      '<PreprocessorDefinitions>_USRDLL;Py_BUILD_CORE;Py_BUILD_CORE_BUILTIN;Py_ENABLE_SHARED;',
      '<PreprocessorDefinitions>Py_NO_ENABLE_SHARED;Py_BUILD_CORE;Py_BUILD_CORE_BUILTIN;')
patch(core,
      '<ClCompile Include="..\\PC\\dl_nt.c" />',
      '<ClCompile Include="..\\PC\\dl_nt.c" />\n    <ClCompile Include="..\\PC\\qf_static_support.c" />')
patch(root + '/PCbuild/pyproject.props',
      '<WholeProgramOptimization>true</WholeProgramOptimization>',
      '<WholeProgramOptimization>false</WholeProgramOptimization>')
PYEOF

build() {
    echo "[cpython] msbuild $1"
    # MSBuild 是 Windows 程序：在 PCbuild 里用相对项目名，路径才不会撞上 /mnt/d。
    # GIT 指到一个不存在的命令，避免构建过程去碰仓库的 git 元数据。
    ( cd "$SRC/PCbuild" && "$QF_MSBUILD" "$1" /nologo /v:m /m \
        /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset="$QF_TOOLSET" \
        /p:GIT=__no_git__ "/p:PythonForBuild=$PY_WIN" )
}

# 1) _freeze_module 自己的构建会在 AfterBuild 里生成 Python/frozen_modules/*.h
#    与 Python/deepfreeze/deepfreeze.c；pythoncore 缺了这些文件就链接不了。
build _freeze_module.vcxproj
[ -f "$SRC/Python/deepfreeze/deepfreeze.c" ] || die "deepfreeze.c 没有生成"
[ -f "$SRC/Python/frozen_modules/importlib._bootstrap.h" ] || die "frozen 模块头没有生成"

# 2) 静态 pythoncore → PCbuild/amd64/python311.lib
build pythoncore.vcxproj
[ -f "$SRC/PCbuild/amd64/python311.lib" ] || die "pythoncore 没有产出 python311.lib"

cp -f "$SRC/PCbuild/amd64/python311.lib" out/python311.lib
echo "[cpython] out/python311.lib ($(stat -c %s out/python311.lib) 字节)"
