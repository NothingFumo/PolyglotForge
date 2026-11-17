#!/usr/bin/env bash
# Kotlin 阶段自测：编译 C 测试并链接 build/libqf_kt.a，验证 DSL 与协程批处理。
# 说明：Windows 版 konanc/gcc 不认 MSYS 的 /mnt/* 路径，统一先 cd 到仓库根再用相对路径。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
here="$root/stages/kotlin"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CC="$(pick "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }
# gcc 需要同目录的 cc1/collect2，单独导出绝对路径时也要把目录放进 PATH。
PATH="$(dirname "$CC"):$PATH"
export PATH

bash "$here/build.sh"

cd "$root"
mkdir -p out
"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts \
      stages/kotlin/selftest.c stages/kotlin/build/libqf_kt.a \
      -o out/kotlin_selftest.exe \
      -lstdc++ -lm
./out/kotlin_selftest.exe

echo "kotlin stage OK"
