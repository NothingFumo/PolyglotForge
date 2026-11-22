#!/usr/bin/env bash
# Rust 阶段构建：产出 stages/rust/libqf_rust.a。
#
# 目标固定为 x86_64-pc-windows-gnu：宿主是 C（gcc 链接），MSVC 目标的 .lib
# 无法被 gcc 直接消费。rustup target add 是幂等的。
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/env.sh"
stage="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="x86_64-pc-windows-gnu"
lib="$stage/target/$target/release/libqf_rust.a"
out="$stage/libqf_rust.a"

# 本机 MSYS 里 `command -v cargo` 对 rustup 的 shim 不可靠，因此按显式路径
# 枚举候选，并用 --version 验证可用性（glob 用于匹配任意用户名）。

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
GCC="$(qf_require_glob \
    "$(command -v gcc 2>/dev/null || true)" \
    "C:/Strawberry/c/bin/gcc.exe" \
    "/mnt/c/Strawberry/c/bin/gcc.exe")" \
    || { echo "找不到 gcc" >&2; exit 1; }

case "$GCC" in
    */*) PATH="$(dirname "$CARGO"):$(dirname "$RUSTUP"):$(dirname "$GCC"):$PATH" ;;
    *)   PATH="$(dirname "$CARGO"):$(dirname "$RUSTUP"):$PATH" ;;
esac
export PATH

# 幂等：目标已安装时 rustup 直接成功；网络不可用而目标缺失才会失败。
"$RUSTUP" target add "$target"

cd "$stage"
"$CARGO" build --release --target "$target"
cp -f "$lib" "$out"
echo "built $out"
