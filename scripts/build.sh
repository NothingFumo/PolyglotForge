#!/usr/bin/env bash
# 全量构建：先让各语言产出静态库，再由 MinGW 统一链接成 dist/queryforge.exe。
# 用法：build.sh [--allow-missing]
# --allow-missing 时，某阶段构建失败只告警并继续（该阶段由 stubs.c 兜底），
# 便于在语言阶段尚未全部就位时做增量集成。
set -euo pipefail

ALLOW_MISSING=0
LINK_MSVC_LIBS=0
SKIP=""
for arg in "$@"; do
    case "$arg" in
        --allow-missing) ALLOW_MISSING=1 ;;
        --link-msvc)     LINK_MSVC_LIBS=1 ;;
        --skip=*)        SKIP="${arg#--skip=}" ;;
    esac
done

cd "$(dirname "$0")/.."
root="$PWD"
# shellcheck source=/dev/null
. scripts/env.sh

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
rm -f out/stage-*.ok
"$RUN" codegen/gen.ts > /dev/null

# 各阶段自建；成功的打标记，标记决定链接时是否纳入该阶段。
for s in cpp rust go csharp kotlin python js; do
    case ",$SKIP," in *",$s,"*) echo "[skip] $s"; continue ;; esac
    [ -x "stages/$s/build.sh" ] || continue
    echo "[build] $s"
    if bash "stages/$s/build.sh"; then
        touch "out/stage-$s.ok"
    else
        echo "[warn] $s 阶段构建失败，使用占位实现"
        [ "$ALLOW_MISSING" = "1" ] || exit 1
    fi
done

SRC=(stages/c/main.c stages/c/arena.c stages/c/host.c stages/c/engine.c)
DEFS=()
LIBS=()
USE_CXX=0

if [ -f out/stage-cpp.ok ]; then
    SRC+=("stages/cpp/libqf_cpp.a"); DEFS+=(-DQF_HAVE_CPP); USE_CXX=1
fi
if [ -f out/stage-rust.ok ]; then
    SRC+=("stages/rust/libqf_rust.a"); DEFS+=(-DQF_HAVE_RUST)
    LIBS+=(-lsynchronization -luserenv)
fi
if [ -f out/stage-go.ok ]; then
    SRC+=("stages/go/out/libqf_go.a"); DEFS+=(-DQF_HAVE_GO)
    LIBS+=(-lws2_32 -lwinmm -lntdll)
fi
if [ -f out/stage-csharp.ok ] && [ "$LINK_MSVC_LIBS" = "1" ]; then
    SRC+=("stages/csharp/out/qf_cs.lib"); DEFS+=(-DQF_HAVE_CSHARP)
    LIBS+=(-lole32 -loleaut32 -luuid -lbcrypt)
    AOT=""
    for base in /mnt/c/Users/Lenovo/.nuget/packages /c/Users/Lenovo/.nuget/packages; do
        if [ -d "$base" ]; then AOT="$base"; break; fi
    done
    if [ -n "$AOT" ]; then
        NATIVE_MSYS="$(ls -d "$AOT"/microsoft.netcore.app.runtime.nativeaot.win-x64/*/runtimes/win-x64/native 2>/dev/null | tail -1 || true)"
        if [ -n "$NATIVE_MSYS" ]; then
            NATIVE="$(winpath "$NATIVE_MSYS")"
            SRC+=("$NATIVE/Runtime.WorkstationGC.lib" "$NATIVE/aotminipal.lib")
        fi
    fi
fi
if [ -f out/stage-kotlin.ok ]; then
    SRC+=("stages/kotlin/build/libqf_kt.a"); DEFS+=(-DQF_HAVE_KOTLIN)
fi
if [ -f out/stage-python.ok ] && [ "$LINK_MSVC_LIBS" = "1" ]; then
    SRC+=("stages/python/out/qf_py.obj" "stages/python/out/python311.lib")
    DEFS+=(-DQF_HAVE_PYTHON)
    LIBS+=(-lucrtbase)
fi
if [ -f out/stage-js.ok ]; then
    SRC+=("stages/js/out/libqf_js.a"); DEFS+=(-DQF_HAVE_JAVASCRIPT)
fi

SRC+=(stages/stubs/stubs.c)
if [ "$LINK_MSVC_LIBS" = "1" ] && { [ -f out/stage-csharp.ok ] || [ -f out/stage-python.ok ]; }; then
    SRC+=(stages/stubs/msvcrt_compat.c)
fi

echo "[link] ${#SRC[@]} 个目标"
LINK_LIBS=()
if [ "$USE_CXX" = "1" ]; then LINK_LIBS+=(-lstdc++); fi
"$CC" -std=c11 -O2 -Wall -Wextra -Igen -Icontracts -Istages/c \
      "${DEFS[@]}" "${SRC[@]}" \
      -o dist/queryforge.exe \
      "${LIBS[@]}" "${LINK_LIBS[@]}" -lm

echo "[done] dist/queryforge.exe"
"$root/dist/queryforge.exe" --version
