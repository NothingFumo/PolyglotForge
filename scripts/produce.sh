#!/usr/bin/env bash
# 产物生成器：装配器只声明"我需要哪个产物"，怎么构建集中在这里。
# 每个目标都必须幂等——装配器在产物缺失时才会调用它。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
# shellcheck source=/dev/null
. scripts/env.sh

target="${1:?用法: produce.sh <target>}"
shift || true

# 在 cmd 派生的环境里 `python` 可能是应用商店别名；用统一解析拿真实解释器。
PY="$(qf_require_python)" || { echo "找不到 python" >&2; exit 1; }

case "$target" in
    go)          bash stages/go/build.sh ;;
    kotlin)      bash stages/kotlin/build.sh ;;
    kotlin-msvc)
                 bash stages/kotlin/build.sh
                 "$PY" stages/stubs/coff_msvc_fix.py \
                     stages/kotlin/build/libqf_kt.a out/msvc/mingw/libqf_kt.a ;;
    js-gcc)      bash stages/js/build.sh ;;
    js-msvc)     bash stages/js/build-msvc.sh ;;
    csharp)      bash stages/csharp/build.sh ;;
    python-obj)  bash stages/python/build.sh ;;
    python-lib)  bash stages/python/build_static_python.sh ;;
    payload)     "$PY" scripts/make_payload.py --object ;;
    rust-msvc)   ( cd stages/rust && cargo build --release --target x86_64-pc-windows-msvc ) ;;
    rust-msvc-static)
                 ( cd stages/rust && RUSTFLAGS="-C target-feature=+crt-static" \
                       cargo build --release --target x86_64-pc-windows-msvc )
                 mkdir -p out/msvc
                 cp -f stages/rust/target/x86_64-pc-windows-msvc/release/qf_rust.lib \
                       out/msvc/rust-crt-static.lib ;;
    *) echo "未知产物目标: $target" >&2; exit 2 ;;
esac
