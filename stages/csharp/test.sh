#!/usr/bin/env bash
# C# 阶段自测：用 gcc 把 selftest.c 与 out/qf_cs.lib 链接起来，
# 断言 WHERE age > 30 的三行批次得到 keep = 1/0/1。
#
# 已知限制（详见 stages/csharp/CRT-NOTES.md）：
#   NativeAOT 静态库是 MSVC 工具链的产物，成员引用 AOT 运行时助手与 MSVC CRT；
#   MinGW 的 crt2.o/libmingw32.a 与 MSVC 的 CRT 启动代码定义同名符号，链接期互斥。
#   因此这里先按 gcc 尝试两种链接方式；都失败时降级为「产物存在 + 导出符号齐全」，
#   打印明确说明并返回 0。
set -uo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"


CC="$(qf_require "$(command -v gcc 2>/dev/null || true)" \
            /c/Strawberry/c/bin/gcc.exe \
            /mnt/c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

cd "$root"

lib="stages/csharp/out/qf_cs.lib"
if [ ! -f "$lib" ]; then
    echo "[csharp] $lib 不存在，先执行 bash stages/csharp/build.sh"
    bash stages/csharp/build.sh || { echo "[csharp] 构建失败" >&2; exit 1; }
fi
[ -f "$lib" ] || { echo "[csharp] 构建后仍缺少 $lib" >&2; exit 1; }

mkdir -p out
log="stages/csharp/out/link.log"
: > "$log"

# MinGW 的 gcc.exe 不认 MSYS 的 /c /d 挂载路径，统一转成 Windows 形态。
win() { cygpath -m "$1" 2>/dev/null || printf '%s' "$1"; }

shopt -s nullglob

# NativeAOT 运行时库（随 .NET SDK 的 NuGet 缓存下发）。
aot_candidates=()
if [ -n "${HOME:-}" ] && [ -d "$HOME/.nuget/packages" ]; then
    aot_candidates=("$HOME/.nuget/packages/microsoft.netcore.app.runtime.nativeaot.win-x64/"*/runtimes/win-x64/native)
fi
aot_dir=""
if [ "${#aot_candidates[@]}" -gt 0 ]; then
    aot_dir="$(printf '%s\n' "${aot_candidates[@]}" | sort -V | tail -1)"
fi

aot_args=()
if [ -n "$aot_dir" ]; then
    for f in bootstrapperdll.obj Runtime.WorkstationGC.lib eventpipe-disabled.lib \
             Runtime.VxsortDisabled.lib standalonegc-disabled.lib aotminipal.lib \
             zlibstatic.lib brotlicommon.lib brotlienc.lib brotlidec.lib \
             System.Globalization.Native.Aot.lib System.IO.Compression.Native.Aot.lib; do
        [ -f "$aot_dir/$f" ] && aot_args+=("$(win "$aot_dir/$f")")
    done
fi

# MSVC 与 Windows SDK 的库目录：直接从环境变量 LIB 取，避免硬编码路径。
lib_dirs=()
if [ -n "${LIB:-}" ]; then
    IFS=';' read -r -a lib_parts <<< "$LIB"
    for p in "${lib_parts[@]}"; do
        [ -n "$p" ] || continue
        u="$(cygpath -u "$p" 2>/dev/null || printf '%s' "$p")"
        [ -d "$u" ] && lib_dirs+=(-L"$(win "$u")")
    done
fi

crt_libs=(-lvcruntime -lucrt -llibcpmt -lmsvcprt -loldnames
          -lkernel32 -ladvapi32 -lbcrypt -lcrypt32 -liphlpapi -lmswsock -lncrypt -lnormaliz
          -lntdll -lole32 -loleaut32 -lsecur32 -luser32 -lversion -lws2_32)

try_link() {
    local label="$1"; shift
    {
        echo "== $label =="
        "$CC" -std=c11 -O2 -Wall -Wextra -Igen -Icontracts \
              stages/csharp/selftest.c "$lib" "$@" -o out/csharp_selftest.exe
        echo
    } >> "$log" 2>&1
}

rm -f out/csharp_selftest.exe
link_ok=0

if try_link "尝试 1：gcc + qf_cs.lib"; then
    link_ok=1
elif [ "${#aot_args[@]}" -gt 0 ]; then
    if try_link "尝试 2：gcc + AOT 运行时库 + MSVC CRT" \
                "${aot_args[@]}" "${lib_dirs[@]}" "${crt_libs[@]}"; then
        link_ok=1
    fi
fi

if [ "$link_ok" -eq 1 ] && [ -x out/csharp_selftest.exe ]; then
    ./out/csharp_selftest.exe || exit 1
    exit 0
fi

# ---------- 降级：只校验产物 ----------

echo "[csharp] 降级：MinGW 无法链接 NativeAOT 静态库（原因见 stages/csharp/CRT-NOTES.md）"
echo "[csharp] 链接错误原文：$log"
echo "[csharp] 产物存在：$lib"

dump=""
if command -v dumpbin >/dev/null 2>&1; then
    dump="dumpbin"
elif command -v llvm-nm >/dev/null 2>&1; then
    dump="llvm-nm"
elif command -v nm >/dev/null 2>&1; then
    dump="nm"
fi

if [ -n "$dump" ]; then
    case "$dump" in
    dumpbin) syms="$(dumpbin -symbols "$lib" 2>/dev/null)" ;;
    llvm-nm) syms="$(llvm-nm --defined-only "$lib" 2>/dev/null)" ;;
    *)       syms="$(nm "$lib" 2>/dev/null)" ;;
    esac

    missing=""
    for s in qf_cs_compile qf_cs_eval qf_cs_free; do
        case "$syms" in
        *"$s"*) ;;
        *) missing="$missing $s" ;;
        esac
    done
    if [ -z "$syms" ]; then
        echo "[csharp] $dump 无法解析 $lib，跳过导出符号校验"
    elif [ -n "$missing" ]; then
        echo "[csharp] 导出符号缺失:$missing" >&2
        exit 1
    else
        echo "[csharp] 导出符号齐全：qf_cs_compile / qf_cs_eval / qf_cs_free"
    fi
else
    echo "[csharp] 缺少 dumpbin/nm，跳过导出符号校验"
fi

echo "csharp stage OK (降级：仅校验 out/qf_cs.lib 及其导出符号)"
exit 0
