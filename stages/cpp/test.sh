#!/usr/bin/env bash
# C++ 阶段自测：构建 libqf_cpp.a，再用 C 编译 selftest.c 链接它。
# selftest.c 用 C 写，直接验证 extern "C" 边界；链接走 g++ 以带上 C++ 运行时。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CC="$(qf_require "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }
CXX="$(qf_require "$(command -v g++ 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/g++.exe /c/Strawberry/c/bin/g++.exe)" \
    || { echo "找不到 g++" >&2; exit 1; }

cd "$root"
bash stages/cpp/build.sh
mkdir -p out

"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts \
      -c stages/cpp/selftest.c -o out/qf_cpp_selftest.o
"$CXX" out/qf_cpp_selftest.o stages/cpp/libqf_cpp.a -o out/qf_cpp_selftest.exe
./out/qf_cpp_selftest.exe
echo "cpp stage OK"
