#!/usr/bin/env bash
# TypeScript 阶段自测：codegen 产物必须能让 C 编译并通过布局断言。
# 说明：本机 Windows 版 node/gcc 不认 MSYS 的 /mnt/d 路径，故统一先 cd 再用相对路径。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"


RUN="$(qf_require "$(command -v bun 2>/dev/null || true)" "$(command -v node 2>/dev/null || true)" \
            "$HOME/.bun/bin/bun.exe" /d/nodejs/node.exe)" || { echo "找不到 bun 或 node" >&2; exit 1; }
CC="$(qf_require "$(command -v gcc 2>/dev/null || true)" /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

cd "$root"
mkdir -p out
"$RUN" codegen/gen.ts > /dev/null
PY="$(qf_require_python)" || { echo "找不到 python" >&2; exit 1; }
"$PY" -c 'import importlib.util; spec = importlib.util.spec_from_file_location("qf_layout", "gen/qf_layout.py"); mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); print("python layout OK")'
"$CC" -std=c11 -Wall -Wextra -Werror -Igen tests/typescript/layout_check.c -o out/layout_check.exe
./out/layout_check.exe
