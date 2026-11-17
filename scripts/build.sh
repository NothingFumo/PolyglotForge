#!/usr/bin/env bash
# 全量构建：先让各语言产出静态库，再由 MinGW 统一链接成 dist/queryforge.exe。
# 每个阶段的存在性用产物文件判断，缺阶段时启用 stubs.c 的占位实现。
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CC="$(pick "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }
CXX="$(pick "$(command -v g++ 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/g++.exe /c/Strawberry/c/bin/g++.exe)" \
    || { echo "找不到 g++" >&2; exit 1; }
RUN="$(pick "$(command -v bun 2>/dev/null || true)" "$(command -v node 2>/dev/null || true)" \
            /mnt/c/Users/Lenovo/.bun/bin/bun.exe /c/Users/Lenovo/.bun/bin/bun.exe \
            /mnt/d/nodejs/node.exe /d/nodejs/node.exe)" || { echo "找不到 bun 或 node" >&2; exit 1; }

mkdir -p out dist
"$RUN" codegen/gen.ts > /dev/null

# 各阶段先自建
for s in cpp rust go csharp kotlin python js; do
    if [ -x "stages/$s/build.sh" ]; then
        echo "[build] $s"
        bash "stages/$s/build.sh"
    fi
done

SRC=(stages/c/main.c stages/c/arena.c stages/c/host.c stages/c/engine.c)
DEFS=()
LIBS=()
USE_CXX=0

[ -f stages/cpp/libqf_cpp.a ]      && { SRC+=("stages/cpp/libqf_cpp.a");      DEFS+=(-DQF_HAVE_CPP); }
[ -f stages/rust/libqf_rust.a ]    && { SRC+=("stages/rust/libqf_rust.a");    DEFS+=(-DQF_HAVE_RUST); }
[ -f stages/go/out/libqf_go.a ]    && { SRC+=("stages/go/out/libqf_go.a");    DEFS+=(-DQF_HAVE_GO); LIBS+=(-lws2_32 -lwinmm -lntdll); }
[ -f stages/csharp/out/qf_cs.lib ] && { SRC+=("stages/csharp/out/qf_cs.lib"); DEFS+=(-DQF_HAVE_CSHARP); LIBS+=(-lole32 -loleaut32 -luuid -lbcrypt); }
[ -f stages/kotlin/build/libqf_kt.a ] && { SRC+=("stages/kotlin/build/libqf_kt.a"); DEFS+=(-DQF_HAVE_KOTLIN); }
[ -f stages/python/out/qf_py.obj ] && { SRC+=("stages/python/out/qf_py.obj"); DEFS+=(-DQF_HAVE_PYTHON); LIBS+=("stages/python/out/python311.lib"); }
[ -f stages/js/out/libqf_js.a ]    && { SRC+=("stages/js/out/libqf_js.a");    DEFS+=(-DQF_HAVE_JAVASCRIPT); }

SRC+=(stages/stubs/stubs.c)

echo "[link] ${#SRC[@]} 个目标"
"$CC" -std=c11 -O2 -Wall -Wextra -Igen -Icontracts -Istages/c \
      "${DEFS[@]}" "${SRC[@]}" \
      -o dist/queryforge.exe \
      "${LIBS[@]}" -lstdc++ -lm

echo "[done] dist/queryforge.exe"
"$root/dist/queryforge.exe" --version
