#!/usr/bin/env bash
# Rust 阶段自测：
#   1. 构建 gnu 目标的 libqf_rust.a；
#   2. cargo test 跑列解析/类型检查/GROUP BY/ORDER BY 的单元测试；
#   3. 用 gcc 链接 C 小测，验证 C ABI 与输出 schema。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
stage="$root/stages/rust"

# 与 build.sh 相同：显式枚举工具路径，避免 MSYS 下 command -v 对 shim 的误判。

CARGO="$(qf_require_glob \
    "/mnt/c/Users/*/.cargo/bin/cargo.exe" \
    "/c/Users/*/.cargo/bin/cargo.exe" \
    "C:/Users/*/.cargo/bin/cargo.exe" \
    "${HOME:-/nonexistent}/.cargo/bin/cargo.exe")" \
    || { echo "找不到 cargo（需要 Rust 工具链）" >&2; exit 1; }
RUSTUP="$(qf_require_glob \
    "/mnt/c/Users/*/.cargo/bin/rustup.exe" \
    "/c/Users/*/.cargo/bin/rustup.exe" \
    "C:/Users/*/.cargo/bin/rustup.exe" \
    "${HOME:-/nonexistent}/.cargo/bin/rustup.exe")" \
    || { echo "找不到 rustup（需要它安装 gnu 目标）" >&2; exit 1; }
# 先取 PATH 里的 gcc：以 /mnt/c/... 形式调用时，MinGW 的 gcc 会算错 exec
# 前缀，找不到自己的 cc1（"cannot execute 'cc1'"）。
CC="$(qf_require_glob \
    "$(command -v gcc 2>/dev/null || true)" \
    "C:/Strawberry/c/bin/gcc.exe" \
    "/mnt/c/Strawberry/c/bin/gcc.exe")" \
    || { echo "找不到 gcc" >&2; exit 1; }

case "$CC" in
    */*) PATH="$(dirname "$CARGO"):$(dirname "$RUSTUP"):$(dirname "$CC"):$PATH" ;;
    *)   PATH="$(dirname "$CARGO"):$(dirname "$RUSTUP"):$PATH" ;;
esac
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
