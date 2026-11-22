#!/usr/bin/env bash
# C 阶段自测：只编译宿主与引擎，不依赖其它语言。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CC="$(qf_require "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

cd "$root"
mkdir -p out
"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts -Istages/c \
      -fsyntax-only stages/c/main.c stages/c/engine.c stages/c/arena.c stages/c/host.c
"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts -Istages/c \
      stages/c/arena.c tests/c/arena_selftest.c -o out/c_arena_selftest.exe
./out/c_arena_selftest.exe
