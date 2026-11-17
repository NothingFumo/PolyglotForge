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
