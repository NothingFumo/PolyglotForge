#!/usr/bin/env bash
# C 阶段自测：只编译宿主与引擎，不依赖其它语言。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
CC="$(pick "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

cd "$root"
mkdir -p out
"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts -Istages/c \
      stages/c/arena.c stages/c/engine.c tests/c/engine_selftest.c \
      -o out/c_selftest.exe
./out/c_selftest.exe
