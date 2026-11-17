#!/usr/bin/env bash
# Go 阶段构建：cgo 需要 gcc，所以先把它的目录放回 PATH。
# 头文件路径由 qf.go 的 #cgo CFLAGS（基于 ${SRCDIR}）给出，不受调用目录影响。
set -euo pipefail
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}
GO="$(pick "$(command -v go 2>/dev/null || true)" /c/Go/bin/go.exe /mnt/c/Go/bin/go.exe \
           /d/go/bin/go.exe /mnt/d/go/bin/go.exe)" \
    || { echo "找不到 go" >&2; exit 1; }
GCC="$(pick "$(command -v gcc 2>/dev/null || true)" \
            /mnt/c/Strawberry/c/bin/gcc.exe /c/Strawberry/c/bin/gcc.exe)" \
    || { echo "找不到 gcc（cgo 需要）" >&2; exit 1; }

export PATH="$(dirname "$GCC"):$PATH"
export CGO_ENABLED=1

mkdir -p "$dir/out"
cd "$dir"
"$GO" build -buildmode=c-archive -o out/libqf_go.a qf.go
