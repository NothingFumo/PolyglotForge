#!/usr/bin/env bash
# JS 阶段自测：编译 selftest.c 并链接 out/libqf_js.a。
# 说明：Windows 版 gcc 不认 MSYS 的 /mnt 路径，统一先 cd 到仓库根再用相对路径。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"


CC="$(qf_require "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

cd "$root"

LIB="stages/js/out/libqf_js.a"
need_build=0
[ -f "$LIB" ] || need_build=1
if [ "$need_build" -eq 0 ]; then
    stale="$(find stages/js/qf_js.c stages/js/qf_js_ext.h stages/js/vendor/quickjs \
                  -newer "$LIB" -print -quit 2>/dev/null || true)"
    if [ -n "$stale" ]; then need_build=1; fi
fi
if [ "$need_build" -eq 1 ]; then
    bash stages/js/build.sh
fi

"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts -Istages/js \
      stages/js/selftest.c "$LIB" -o stages/js/out/js_selftest.exe -lm

./stages/js/out/js_selftest.exe

echo "js stage OK"
