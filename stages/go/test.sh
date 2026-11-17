#!/usr/bin/env bash
# Go 阶段自测：先构建 c-archive，再跑 Go 单元测试，最后用 C 程序走一遍真实 ABI。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dir="$root/stages/go"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
GO="$(pick "$(command -v go 2>/dev/null || true)" /c/Go/bin/go.exe /mnt/c/Go/bin/go.exe \
           /d/go/bin/go.exe /mnt/d/go/bin/go.exe)" \
    || { echo "找不到 go" >&2; exit 1; }
CC="$(pick "$(command -v gcc 2>/dev/null || true)" \
            /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc" >&2; exit 1; }

export PATH="$(dirname "$CC"):$PATH"
export CGO_ENABLED=1

bash "$dir/build.sh"

cd "$dir"
"$GO" test -count=1 ./...

cd "$root"
mkdir -p stages/go/out
"$CC" -std=c11 -Wall -Wextra -Werror -DQF_SELFTEST_BUILD -Igen -Icontracts \
      stages/go/selftest.c stages/go/out/libqf_go.a \
      -o stages/go/out/selftest.exe \
      -lws2_32 -lwinmm -lntdll -lstdc++ -lm
./stages/go/out/selftest.exe stages/go/out/selftest.csv

echo "go stage OK"
