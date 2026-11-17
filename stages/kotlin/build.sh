#!/usr/bin/env bash
# Kotlin 阶段构建：konanc 直接产出静态库 build/libqf_kt.a（不使用 Gradle）。
#
# 依赖（首次运行自动下载到 stages/kotlin/vendor/）：
#   - Kotlin/Native 预编译工具链，Windows x86_64，版本见 KVER
#   - kotlinx-coroutines-core 的 mingw_x64 klib
# konanc/cinterop 是 Windows 批处理（JVM 工具），需要 JDK 17+；
# 本机可能同时存在 MSYS bash 与 WSL bash，所以批处理一律经 cmd.exe 启动。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
here="$root/stages/kotlin"
cd "$root"

KVER="${QF_KONAN_VERSION:-2.4.21}"
COROUTINES_VER="${QF_COROUTINES_VERSION:-1.11.0}"
ATOMICFU_VER="${QF_ATOMICFU_VERSION:-0.32.1}"
KONAN="${KONAN_HOME:-$here/vendor/kotlin-native-prebuilt-windows-x86_64-$KVER}"
COROUTINES_KLIB="$here/vendor/kotlinx-coroutines-core-mingwx64-$COROUTINES_VER.klib"
ATOMICFU_KLIB="$here/vendor/atomicfu-mingwx64-$ATOMICFU_VER.klib"

to_win() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"
    elif command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"
    else printf '%s' "$1"
    fi
}

to_unix() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"
    elif command -v wslpath >/dev/null 2>&1; then wslpath -u "$1"
    else printf '%s' "$1"
    fi
}

fetch() { # url dest
    if command -v curl >/dev/null 2>&1; then
        curl -L --retry 3 --fail -o "$2" "$1"
    else
        curl.exe -L --retry 3 --fail -o "$2" "$1"
    fi
}

# 目标 mingw_x64 以本机 MinGW-w64 作为 sysroot；把 gcc 目录放进 PATH，
# 让 cinterop 的 clang 能找到 C 运行库头文件。
for d in /mnt/c/Strawberry/c/bin /c/Strawberry/c/bin; do
    [ -d "$d" ] && export PATH="$d:$PATH"
done

mkdir -p "$here/vendor" "$here/build"

if [ ! -f "$KONAN/bin/konanc.bat" ]; then
    zip="$here/vendor/kotlin-native-prebuilt-windows-x86_64-$KVER.zip"
    if [ ! -f "$zip" ]; then
        echo "[kotlin] 下载 Kotlin/Native $KVER"
        fetch "https://github.com/JetBrains/kotlin/releases/download/v$KVER/kotlin-native-prebuilt-windows-x86_64-$KVER.zip" "$zip"
    fi
    echo "[kotlin] 解压 Kotlin/Native"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$zip" -d "$here/vendor"
    elif command -v tar >/dev/null 2>&1 && tar -tf "$zip" >/dev/null 2>&1; then
        # Windows 自带 bsdtar 支持 zip；GNU tar 会在 -tf 探测时失败，自动跳过。
        tar -xf "$zip" -C "$here/vendor"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
            "$zip" "$here/vendor"
    elif command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -Command "Expand-Archive -Force -Path '$(to_win "$zip")' -DestinationPath '$(to_win "$here/vendor")'"
    else
        echo "没有可用的解压工具（unzip/tar/python3/powershell）" >&2
        exit 1
    fi
    if [ ! -f "$KONAN/bin/konanc.bat" ]; then
        found="$(find "$here/vendor" -maxdepth 3 -name konanc.bat -printf '%h\n' | head -1)"
        [ -n "$found" ] || { echo "解压后找不到 konanc.bat" >&2; exit 1; }
        KONAN="$(dirname "$found")"
    fi
fi
KONANC="$KONAN/bin/konanc.bat"
CINTEROP="$KONAN/bin/cinterop.bat"
[ -f "$KONANC" ] || { echo "konanc 不可用: $KONANC" >&2; exit 1; }
[ -f "$CINTEROP" ] || { echo "cinterop 不可用: $CINTEROP" >&2; exit 1; }

# konanc 是 JVM 工具：JAVA_HOME 必须是 Windows 形式（批处理里当 Windows 路径用）。
if [ -z "${JAVA_HOME:-}" ]; then
    for j in 'D:\17' 'C:\Program Files\Java\jdk-17' 'C:\Program Files\Java\jdk-21' 'C:\Program Files\Java\jdk-24'; do
        if [ -x "$(to_unix "$j")/bin/java.exe" ]; then export JAVA_HOME="$j"; break; fi
    done
else
    case "$JAVA_HOME" in
        /*) export JAVA_HOME="$(to_win "$JAVA_HOME")" ;;
    esac
fi
[ -n "${JAVA_HOME:-}" ] || echo "[kotlin] 警告：JAVA_HOME 未设置，konanc 可能找不到 JDK 17+" >&2

if [ ! -f "$COROUTINES_KLIB" ]; then
    echo "[kotlin] 下载 kotlinx-coroutines-core $COROUTINES_VER (mingw_x64)"
    fetch "https://repo1.maven.org/maven2/org/jetbrains/kotlinx/kotlinx-coroutines-core-mingwx64/$COROUTINES_VER/kotlinx-coroutines-core-mingwx64-$COROUTINES_VER.klib" "$COROUTINES_KLIB"
fi
# coroutines 的 native 目标依赖 atomicfu（SynchronizedObject 等），必须一起给 konanc。
if [ ! -f "$ATOMICFU_KLIB" ]; then
    echo "[kotlin] 下载 atomicfu $ATOMICFU_VER (mingw_x64)"
    fetch "https://repo1.maven.org/maven2/org/jetbrains/kotlinx/atomicfu-mingwx64/$ATOMICFU_VER/atomicfu-mingwx64-$ATOMICFU_VER.klib" "$ATOMICFU_KLIB"
fi

# Windows 批处理必须由 cmd.exe 解释；参数保持相对路径（cmd 继承仓库根的 Windows cwd）。
run_bat() {
    local bat="$1"
    shift
    if command -v cmd.exe >/dev/null 2>&1; then
        cmd.exe /c "$(to_win "$bat")" "$@"
    else
        "$bat" "$@"
    fi
}

echo "[kotlin] cinterop: contracts/qf.h + gen/qf_layout.h"
run_bat "$CINTEROP" -def "stages/kotlin/qf.def" -o "stages/kotlin/build/qf_cinterop"

echo "[kotlin] konanc: $KVER -> mingw_x64"
rm -f "$here/build/libqf_kt.a"
run_bat "$KONANC" -target mingw_x64 -produce static -opt \
    -o "stages/kotlin/build/libqf_kt" \
    -library "stages/kotlin/build/qf_cinterop.klib" \
    -library "stages/kotlin/vendor/kotlinx-coroutines-core-mingwx64-$COROUTINES_VER.klib" \
    -library "stages/kotlin/vendor/atomicfu-mingwx64-$ATOMICFU_VER.klib" \
    stages/kotlin/src/*.kt

if [ ! -f "$here/build/libqf_kt.a" ]; then
    produced="$(find "$here/build" -maxdepth 1 -name 'libqf_kt*.a' -printf '%f\n' | head -1)"
    [ -n "$produced" ] || { echo "konanc 没有产出静态库" >&2; exit 1; }
    mv "$here/build/$produced" "$here/build/libqf_kt.a"
fi

# Kotlin/Native 导出函数不支持结构体按值参数：shim.c 提供 qf_dsl_build 的 C 适配层。
pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CC="$(pick "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc（编译 C ABI 适配层需要）" >&2; exit 1; }
AR="$(pick "$(command -v ar 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/ar.exe /c/Strawberry/c/bin/ar.exe)" \
    || { echo "找不到 ar（合并 C ABI 适配层需要）" >&2; exit 1; }

"$CC" -std=c11 -O2 -Igen -Icontracts -c stages/kotlin/shim.c -o stages/kotlin/build/qf_shim.o
"$AR" rcs stages/kotlin/build/libqf_kt.a stages/kotlin/build/qf_shim.o

echo "[kotlin] $(ls -l "$here/build/libqf_kt.a" | awk '{print $5}') bytes -> stages/kotlin/build/libqf_kt.a"
