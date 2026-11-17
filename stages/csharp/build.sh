#!/usr/bin/env bash
# C# 阶段构建：.NET 10 + NativeAOT 静态库 → stages/csharp/out/qf_cs.lib
# 产物是纯原生静态库，宿主（MinGW/MSVC）负责最终链接，目标机不需要 .NET 运行时。
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pick() {
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

DOTNET="$(pick "$(command -v dotnet 2>/dev/null || true)" \
                "/c/Program Files/dotnet/dotnet.exe" \
                "/mnt/c/Program Files/dotnet/dotnet.exe")" \
    || { echo "找不到 dotnet（需要 .NET 10 SDK）" >&2; exit 1; }

cd "$root/stages/csharp"
mkdir -p out
rm -f out/qf_cs.lib

"$DOTNET" publish qf_cs.csproj \
    -c Release \
    -r win-x64 \
    -p:NativeLib=Static \
    -p:PublishAot=true \
    -o out \
    --nologo

if [ ! -f out/qf_cs.lib ]; then
    echo "[csharp] 构建失败：out/qf_cs.lib 未生成" >&2
    ls -l out >&2 || true
    exit 1
fi

echo "[csharp] out/qf_cs.lib 已生成"
