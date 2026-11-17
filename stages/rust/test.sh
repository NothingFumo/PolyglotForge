#!/usr/bin/env bash
# Rust 阶段自测：
#   1. 构建 gnu 目标的 libqf_rust.a；
#   2. cargo test 跑列解析/类型检查/GROUP BY/ORDER BY 的单元测试；
#   3. 用 gcc 链接 C 小测，验证 C ABI 与输出 schema。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
stage="$root/stages/rust"

# 与 build.sh 相同：显式枚举工具路径，避免 MSYS 下 command -v 对 shim 的误判。
resolve_tool() {
    local IFS=':'
    for pattern in "$@"; do
        for cand in $pattern; do
            if [ -n "$cand" ] && [ -x "$cand" ] && "$cand" --version >/dev/null 2>&1; then
                printf '%s' "$cand"
                return 0
            fi
        done
    done
    return 1
}

CARGO="$(resolve_tool \
    "/mnt/c/Users/*/.cargo/bin/cargo.exe" \
    "/c/Users/*/.cargo/bin/cargo.exe" \
    "C:/Users/*/.cargo/bin/cargo.exe" \
    "$HOME/.cargo/bin/cargo.exe")" \
    || { echo "找不到 cargo（需要 Rust 工具链）" >&2; exit 1; }
RUSTUP="$(resolve_tool \
    "/mnt/c/Users/*/.cargo/bin/rustup.exe" \
    "/c/Users/*/.cargo/bin/rustup.exe" \
    "C:/Users/*/.cargo/bin/rustup.exe" \
    "$HOME/.cargo/bin/rustup.exe")" \
    || { echo "找不到 rustup（需要它安装 gnu 目标）" >&2; exit 1; }
CC="$(resolve_tool \
    "/mnt/c/Strawberry/c/bin/gcc.exe" \
    "/c/Strawberry/c/bin/gcc.exe")" \
    || { echo "找不到 gcc" >&2; exit 1; }

PATH="$(dirname "$CARGO"):$(dirname "$RUSTUP"):$(dirname "$CC"):$PATH"
export PATH

bash "$stage/build.sh"

cd "$stage"
"$CARGO" test --target x86_64-pc-windows-gnu

cd "$root"
mkdir -p out
"$CC" -std=c11 -Wall -Wextra -Werror -Igen -Icontracts \
      stages/rust/selftest.c stages/rust/libqf_rust.a \
      -o out/rust_selftest.exe \
      -lws2_32 -luserenv -lbcrypt -lntdll -lole32 -loleaut32 \
      -ladvapi32 -lshell32 -lpsapi -lkernel32 -lsynchronization -lgcc_eh
./out/rust_selftest.exe
echo "rust stage OK"
