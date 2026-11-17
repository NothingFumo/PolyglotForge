#!/usr/bin/env bash
# Python 阶段构建：产出 out/qf_py.obj 与 out/python311.lib。
#
# qf_py.c 用 MSVC 编译：静态 CPython 的头文件通过 PC/pyconfig.h 暴露，
# 必须定义 Py_NO_ENABLE_SHARED，否则 PyAPI_FUNC 会退化成 dllimport。
# 静态库本身由 build_static_python.sh 从 CPython 源码产出（首次会跑十几分钟）。
set -euo pipefail
stage="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$stage"
mkdir -p out

die() { echo "错误：$*" >&2; exit 1; }

. "$stage/msvc_env.sh"
qf_msvc_setup || die "MSVC 环境准备失败"

vendor="$stage/vendor/Python-3.11.9"
[ -d "$vendor/Include" ] || die "缺少 CPython 源码：先跑 bash stages/python/build_static_python.sh"
[ -f "$vendor/PC/pyconfig.h" ] || die "缺少 PC/pyconfig.h（检查 vendor 是否完整）"

echo "[python] 编译 qf_py.c"
"$QF_CL" -nologo -c -O2 -MD -W4 -D_CRT_SECURE_NO_WARNINGS -DPy_NO_ENABLE_SHARED -DQF_HAVE_PY_PAYLOAD \
      -Ivendor/Python-3.11.9/Include -Ivendor/Python-3.11.9/PC \
      -I../../gen -I../../contracts \
      -Foout/qf_py.obj qf_py.c
[ -f out/qf_py.obj ] || die "cl 没有产出 out/qf_py.obj"

lib_src="$vendor/PCbuild/amd64/python311.lib"
if [ ! -f "$lib_src" ]; then
    echo "[python] 静态 CPython 库还不存在，开始构建（耗时较长）"
    bash "$stage/build_static_python.sh"
fi
[ -f "$lib_src" ] || die "静态库缺失：$lib_src（见 stages/python/STATIC-NOTES.md）"

if [ ! -f out/python311.lib ] || [ "$lib_src" -nt out/python311.lib ]; then
    cp -f "$lib_src" out/python311.lib
fi
echo "[python] out/qf_py.obj + out/python311.lib 就绪"
