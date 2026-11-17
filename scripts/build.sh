#!/usr/bin/env bash
# 全量构建：先让各语言产出静态库，再由 MinGW 统一链接成 dist/queryforge.exe。
# 用法：build.sh [--allow-missing] [--skip=cpp,rust,go,csharp,kotlin,python,js]
# --allow-missing 时，某阶段构建失败只告警并继续（该阶段由 stubs.c 兜底），
# 便于在语言阶段尚未全部就位时做增量集成。
#
# MSVC 产出的两个阶段（C# NativeAOT 静态库、静态 CPython）也参与默认链接：
#   - 归档成员先由 stages/stubs/coff_repack.py 归一化成 ld 能正确消费的 COFF；
#   - 少量 C++ ABI 入口与 CPU 分派哨兵由 stages/stubs/msvcpp_shim.c 提供。
# 具体原因与实测依据见 docs/adr/0007。
set -euo pipefail

ALLOW_MISSING=0
SKIP=""
for arg in "$@"; do
    case "$arg" in
        --allow-missing) ALLOW_MISSING=1 ;;
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
ARCHIVES=()
LINK_FLAGS=()
USE_CXX=0

if [ -f out/stage-cpp.ok ]; then
    ARCHIVES+=("stages/cpp/libqf_cpp.a"); DEFS+=(-DQF_HAVE_CPP); USE_CXX=1
fi
if [ -f out/stage-rust.ok ]; then
    ARCHIVES+=("stages/rust/libqf_rust.a"); DEFS+=(-DQF_HAVE_RUST)
    LIBS+=(-lsynchronization -luserenv)
fi
if [ -f out/stage-go.ok ]; then
    ARCHIVES+=("stages/go/out/libqf_go.a"); DEFS+=(-DQF_HAVE_GO)
    LIBS+=(-lws2_32 -lwinmm -lntdll)
fi
if [ -f out/stage-kotlin.ok ]; then
    ARCHIVES+=("stages/kotlin/build/libqf_kt.a"); DEFS+=(-DQF_HAVE_KOTLIN)
fi
if [ -f out/stage-js.ok ]; then
    ARCHIVES+=("stages/js/out/libqf_js.a"); DEFS+=(-DQF_HAVE_JAVASCRIPT)
fi

# ---------- MSVC 侧：C# NativeAOT 与静态 CPython ----------
# 归一化后的归档/对象先攒在 P_* 里，全部成功才并入链接；中途失败就整体回退到
# stubs.c 的占位实现，避免"只链接了半个 MSVC 世界"的半成品。
MSVC_DIR=out/msvc
P_OBJS=()
P_LIBS=()
P_SRC=()
P_DEFS=()
P_LIBS_EXTRA=()
P_FLAGS=()

prepare_msvc_side() {
    local PY AR OBJCOPY pair chkstk vs_root base aot aot_msys aot_win winstyle=0
    PY="$(pick "${QF_PYTHON:-}" \
              /mnt/d/Python/Python311/python.exe /d/Python/Python311/python.exe \
              "$(command -v python 2>/dev/null || true)" "$(command -v python3 2>/dev/null || true)")" \
        || { echo "找不到 python（coff_repack.py 需要）" >&2; return 1; }
    # Windows 版 python 只认 C:/...，WSL/Debian 版只认 /mnt/c/...；按实际解释器选路径风格。
    if "$PY" -c "import sys; raise SystemExit(0 if sys.platform == 'win32' else 1)" 2>/dev/null; then
        winstyle=1
    fi
    tpath() { if [ "$winstyle" = "1" ]; then winpath "$1"; else printf '%s' "$1"; fi; }
    # ar/objcopy 必须和 gcc 同一套 binutils：PATH 里可能先撞上 WSL 的 ELF 版本。
    local cc_dir
    cc_dir="$(dirname "$CC")"
    AR="$(pick "$cc_dir/ar.exe" "$cc_dir/ar" \
              /mnt/c/Strawberry/c/bin/ar.exe /c/Strawberry/c/bin/ar.exe)" \
        || { echo "找不到 ar" >&2; return 1; }
    OBJCOPY="$(pick "$cc_dir/objcopy.exe" "$cc_dir/objcopy" \
                   /mnt/c/Strawberry/c/bin/objcopy.exe /c/Strawberry/c/bin/objcopy.exe)" \
        || { echo "找不到 objcopy" >&2; return 1; }

    rm -rf "$MSVC_DIR"
    mkdir -p "$MSVC_DIR/libs" "$MSVC_DIR/objs"

    # repack <名字> <MSVC .lib（msys 路径）>：拆成员 + 归一化 + 重新打包成 GNU 归档
    repack() {
        local name="$1" lib="$2"
        "$PY" stages/stubs/coff_repack.py "$(tpath "$lib")" "$MSVC_DIR/objs/$name" || return 1
        rm -f "$MSVC_DIR/libs/$name.a"
        "$AR" rcs "$MSVC_DIR/libs/$name.a" "$MSVC_DIR/objs/$name"/*.obj || return 1
    }

    # C++ ABI：把实现名换成 MSVC 修饰名。--defsym 不接受 `?`/`@`（实测 syntax error），
    # objcopy 的 --redefine-sym（"旧名=新名"）可以，且不需要 .def 文件。
    local shim_pairs=(
        "qf_msvc_delete_array=??_V@YAXPEAX@Z"
        "qf_msvc_delete_array_sized=??_V@YAXPEAX_K@Z"
        "qf_msvc_new_array_nothrow=??_U@YAPEAX_KAEBUnothrow_t@std@@@Z"
        "qf_msvc_new_array=??_U@YAPEAX_K@Z"
        "qf_msvc_new_nothrow=??2@YAPEAX_KAEBUnothrow_t@std@@@Z"
        "qf_msvc_new=??2@YAPEAX_K@Z"
        "qf_msvc_delete_sized=??3@YAXPEAX_K@Z"
        "qf_msvc_delete=??3@YAXPEAX@Z"
        "qf_msvc_nothrow_tag=?nothrow@std@@3Unothrow_t@1@B"
    )
    local renames=()
    for pair in "${shim_pairs[@]}"; do renames+=(--redefine-sym "$pair"); done
    "$CC" -std=c11 -O2 -c stages/stubs/msvcpp_shim.c -o "$MSVC_DIR/shim_raw.o" || return 1
    "$OBJCOPY" "${renames[@]}" "$MSVC_DIR/shim_raw.o" "$MSVC_DIR/shim.o" || return 1
    P_OBJS+=("$MSVC_DIR/shim.o")

    # __chkstk：优先用 MSVC 自己的探测函数；找不到就别名到 mingw 的 ___chkstk_ms
    # （两者都是 ABI 规定的“RAX=帧大小、逐页触碰保护页”实现）。
    chkstk=""
    for vs_root in "/mnt/d/Microsoft Visual Studio" "/d/Microsoft Visual Studio" \
                   "D:/Microsoft Visual Studio" "/mnt/c/Program Files/Microsoft Visual Studio"; do
        [ -d "$vs_root" ] || continue
        chkstk="$(ls -d "$vs_root"/*/*/VC/Tools/MSVC/*/lib/x64/chkstk.obj 2>/dev/null | tail -1 || true)"
        [ -n "$chkstk" ] && break
    done
    if [ -n "$chkstk" ]; then
        P_OBJS+=("$(winpath "$chkstk")")
    else
        P_FLAGS+=(-Wl,--defsym,__chkstk=___chkstk_ms)
    fi

    if [ -f out/stage-csharp.ok ]; then
        aot_msys=""
        for base in "$HOME/.nuget/packages" /mnt/c/Users/Lenovo/.nuget/packages \
                    /c/Users/Lenovo/.nuget/packages; do
            [ -d "$base" ] || continue
            aot_msys="$(ls -d "$base"/microsoft.netcore.app.runtime.nativeaot.win-x64/*/runtimes/win-x64/native 2>/dev/null | tail -1 || true)"
            [ -n "$aot_msys" ] && break
        done
        [ -n "$aot_msys" ] || { echo "找不到 NativeAOT win-x64 运行时库" >&2; return 1; }
        aot_win="$(winpath "$aot_msys")"

        "$PY" stages/stubs/coff_repack.py "$(tpath stages/csharp/out/qf_cs.lib)" \
              "$MSVC_DIR/objs/qf_cs" || return 1
        P_OBJS+=("$MSVC_DIR/objs/qf_cs"/*.obj)
        # NativeAOT 的运行时初始化挂在 bootstrapperdll.obj 的 .CRT$XCU 项上；
        # exe 场景的 bootstrapper.obj 会引入 __managed__Main，静态库使用者不能用。
        P_OBJS+=("$aot_win/bootstrapperdll.obj")

        for aot in Runtime.WorkstationGC Runtime.VxsortEnabled aotminipal eventpipe-disabled \
                   standalonegc-disabled System.Globalization.Native.Aot \
                   System.IO.Compression.Native.Aot zlibstatic brotlienc brotlidec brotlicommon; do
            [ -f "$aot_msys/$aot.lib" ] || continue
            repack "$aot" "$aot_msys/$aot.lib" || return 1
            P_LIBS+=("$MSVC_DIR/libs/$aot.a")
        done
        P_DEFS+=(-DQF_HAVE_CSHARP)
        P_LIBS_EXTRA+=(-lsynchronization -luserenv -lole32 -loleaut32 -luuid -lbcrypt)
    fi

    if [ -f out/stage-python.ok ]; then
        P_SRC+=("stages/python/out/qf_py.obj")
        P_DEFS+=(-DQF_HAVE_PYTHON)
        repack python311 stages/python/out/python311.lib || return 1
        P_LIBS+=("$MSVC_DIR/libs/python311.a")
        P_LIBS_EXTRA+=(-lucrtbase -lpathcch -lversion)
    fi

    # MSVC 归档里有大量 COMDAT 定义的重复副本（/Gy 让每个函数一节），
    # 去 COMDAT 后同样的代码会多份，取先出现者即可。
    P_FLAGS+=(-Wl,--allow-multiple-definition)
    # ld 把 .tls 的 SECREL 立即数也登记成基址重定位，加载时被加上镜像基址偏移，
    # MSVC 的 TLS 访问会拿到野指针（实测）。关掉动态基址即可；本
    # 可执行文件固定在 0x140000000，不需要重定位表。
    P_FLAGS+=(-Wl,--disable-dynamicbase)
    return 0
}

if [ -f out/stage-csharp.ok ] || [ -f out/stage-python.ok ]; then
    echo "[prep] 归一化 MSVC 静态库"
    if prepare_msvc_side; then
        SRC+=("${P_SRC[@]}")
        DEFS+=("${P_DEFS[@]}")
        LIBS+=("${P_LIBS_EXTRA[@]}")
        LINK_FLAGS+=("${P_FLAGS[@]}")
        SRC+=(stages/stubs/msvcrt_compat.c)
    else
        echo "[warn] MSVC 侧准备失败，C#/Python 阶段降级为占位实现" >&2
        [ "$ALLOW_MISSING" = "1" ] || exit 1
        P_OBJS=(); P_LIBS=()
    fi
fi

SRC+=(stages/stubs/stubs.c)

echo "[link] ${#SRC[@]} 个源/对象, ${#ARCHIVES[@]} 个归档 + ${#P_LIBS[@]} 个 MSVC 归档"
# Kotlin/Native 的归档引用 libstdc++ 的 RTTI vtable（__class_type_info），
# 只有共享 libstdc++ 才提供，静态化会断链：实测 `-static-libstdc++` 报
# `vtable for __cxxabiv1::__class_type_info` undefined。因此 libstdc++ 保持动态，
# 仅把 winpthread（QuickJS 需要）与 libgcc 静态化。
if [ "$USE_CXX" = "1" ]; then LIBS+=(-lstdc++); fi
LIBS+=(-static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)

ARCH_ARGS=()
if [ "${#ARCHIVES[@]}" -gt 0 ] || [ "${#P_LIBS[@]}" -gt 0 ]; then
    ARCH_ARGS+=(-Wl,--start-group "${ARCHIVES[@]}" "${P_LIBS[@]}" -Wl,--end-group)
fi

"$CC" -std=c11 -O2 -Wall -Wextra -Igen -Icontracts -Istages/c \
      "${LINK_FLAGS[@]}" "${DEFS[@]}" "${SRC[@]}" "${P_OBJS[@]}" \
      -o dist/queryforge.exe \
      "${ARCH_ARGS[@]}" \
      "${LIBS[@]}" -lm

echo "[done] dist/queryforge.exe"
"$root/dist/queryforge.exe" --version
