#!/usr/bin/env bash
# Python 阶段自测：用 MSVC 把 selftest.c 与阶段产物链接成 exe，加载 fixtures/score.py
# 验证 qf_value ↔ Python 对象互转、契约类型与错误路径。
# 跨 CRT 的最终链接（MinGW 主干 + 本静态库）由集成提交负责，这里只证阶段自身可用。
set -euo pipefail
stage="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$stage"

. "$stage/msvc_env.sh"
qf_msvc_setup || { echo "错误：MSVC 环境准备失败" >&2; exit 1; }

bash "$stage/build.sh"
[ -f out/qf_py.obj ] || { echo "错误：缺少 out/qf_py.obj" >&2; exit 1; }
[ -f out/python311.lib ] || { echo "错误：缺少 out/python311.lib（静态 CPython 未产出）" >&2; exit 1; }

echo "[python] 链接自测程序"
# 静态库没有编译期 home：把源码树自带的 Lib/ 作为标准库目录编进测试程序
# （等价于集成时把 PYTHONHOME 指向载荷解包目录）。
home_fwd="$(to_win "$stage/vendor/Python-3.11.9" | tr '\\' '/')"
"$QF_CL" -nologo -O2 -MD -W4 -D_CRT_SECURE_NO_WARNINGS \
      -DQF_TEST_PYTHONHOME="\"$home_fwd\"" \
      -I../../gen -I../../contracts \
      -Foout/ -Fe:out/python_selftest.exe selftest.c out/qf_py.obj out/python311.lib \
      -link -IMPLIB:out/python_selftest.lib \
      version.lib ws2_32.lib pathcch.lib bcrypt.lib advapi32.lib ole32.lib \
      user32.lib shell32.lib
[ -f out/python_selftest.exe ] || { echo "错误：自测程序没有链接出来" >&2; exit 1; }

./out/python_selftest.exe fixtures/score.py
