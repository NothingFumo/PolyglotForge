#!/usr/bin/env bash
# C++ 阶段构建：产出 stages/cpp/libqf_cpp.a（唯一产物）。
# 宿主统一用 MinGW gcc 链接，故这里也用同一套 g++/ar；路径由 pick 解析，
# 兼容 MSYS 的 /mnt/c 与 /c 两种驱动器挂载写法。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CXX="$(pick "$(command -v g++ 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/g++.exe /c/Strawberry/c/bin/g++.exe)" \
    || { echo "找不到 g++" >&2; exit 1; }
AR="$(pick "$(command -v ar 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/ar.exe /c/Strawberry/c/bin/ar.exe)" \
    || { echo "找不到 ar" >&2; exit 1; }

cd "$root"
mkdir -p out

# Windows 版 g++ 不认 MSYS 的 /mnt/... 路径，故用相对路径。
"$CXX" -std=c++17 -O2 -Wall -Wextra -Igen -Icontracts \
       -c stages/cpp/parser.cpp -o out/qf_cpp_parser.o
rm -f stages/cpp/libqf_cpp.a
"$AR" rcs stages/cpp/libqf_cpp.a out/qf_cpp_parser.o
