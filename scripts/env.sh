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
