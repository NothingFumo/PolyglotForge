# 统一补齐工具链路径。Windows 上 MSYS 的 PATH 与原生 exe 的 PATH 不一致，
# 每个构建/测试脚本 source 这个文件，避免各语言各自猜路径。
for d in \
  "/mnt/c/Users/Lenovo/.cargo/bin" "/c/Users/Lenovo/.cargo/bin" \
  "/mnt/d/go/bin" "/d/go/bin" \
  "/mnt/c/Program Files/dotnet" "/c/Program Files/dotnet" \
  "/mnt/c/Strawberry/c/bin" "/c/Strawberry/c/bin" \
  "/mnt/c/Users/Lenovo/.bun/bin" "/c/Users/Lenovo/.bun/bin" \
  "/mnt/d/nodejs" "/d/nodejs" \
  "/mnt/d/Python/Python311" "/d/Python/Python311"; do
  if [ -d "$d" ]; then
    PATH="$d:$PATH"
  fi
done
export PATH

# 把 MSYS 路径转成 Windows 盘符路径：Windows 版链接器/编译器只认后者。
winpath() {
    case "$1" in
        /mnt/[a-z]/*) printf '%s:/%s' "$(echo "${1:5:1}" | tr 'a-z' 'A-Z')" "${1:7}" ;;
        /[a-z]/*)     printf '%s:/%s' "$(echo "${1:1:1}" | tr 'a-z' 'A-Z')" "${1:3}" ;;
        *)            printf '%s' "$1" ;;
    esac
}


# 统一的工具发现接口。以前每个脚本自带一份 pick()（15 份）与 resolve_tool()
# （2 份），规则差异只有踩到才知道。候选路径由调用方给出，这里只回答"第一个可用
# 的是谁"——找不到就返回非零，由调用点决定报错文案。
qf_require() {
    local c
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

# 同上，但要求 --version 能跑通（给 rustup/cargo 这类 shim 用）。
qf_require_cmd() {
    local c
    for c in "$@"; do
        if [ -n "$c" ] && [ -x "$c" ] && "$c" --version >/dev/null 2>&1; then
            printf '%s' "$c"; return 0
        fi
    done
    return 1
}

# Python 解释器：调用方可要求最低版本（如 3.11，_freeze_module 的字节码版本）。
qf_require_python() {
    local want="${1:-}" c
    for c in "${QF_PYTHON:-}" "$(command -v python 2>/dev/null || true)" \
             "$(command -v python3 2>/dev/null || true)" \
             "D:/Python/Python311/python.exe" /d/Python/Python311/python.exe \
             /mnt/d/Python/Python311/python.exe; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        if [ -z "$want" ]; then printf '%s' "$c"; return 0; fi
        if "$c" -c "import sys; raise SystemExit(0 if sys.version_info[:2] >= tuple(int(x) for x in '$want'.split('.')) else 1)" 2>/dev/null; then
            printf '%s' "$c"; return 0
        fi
    done
    return 1
}

# 候选路径可能带通配（rustup 的 shim 按用户目录展开），这里保留 glob 语义。
qf_require_glob() {
    local pat cand
    for pat in "$@"; do
        for cand in $pat; do
            if [ -n "$cand" ] && [ -x "$cand" ] && "$cand" --version >/dev/null 2>&1; then
                printf '%s' "$cand"; return 0
            fi
        done
    done
    return 1
}
