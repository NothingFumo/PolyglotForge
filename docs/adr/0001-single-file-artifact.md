# 单文件产物：静态链接为主，仅允许 Python 标准库载荷

九种语言必须交付一个 `queryforge.exe`。C / C++ / Rust / Go / C# / Kotlin(Native) / JS(QuickJS) 在 MinGW 主干下真静态链接；Python 以自建静态 `python.lib` 链入，标准库文本以 zip 资源内嵌，运行期由内存 finder 直接读取（不落盘）。这样目标机零依赖，也不需要任何释放目录。

## 考虑的方案

- **全部载荷（自解压）**：最简单，但放弃"混合编译"里最有价值的静态链接与 ABI 知识点。
- **全部静态（含 Python 标准库冻结进二进制）**：需要自写内存 zip importer；作为 v2 目标，v1 不做。
- **Java 走 GraalVM**：native-image 只能把 C 链进 Java，无法把 Java 变成可链进 C 的静态库，故本项目不含 Java。

## 后果

构建机需要 VS 2026 的 MSVC（编译 `pythoncore` 静态库与 C# 兼容 stub）与 MinGW gcc（主干链接）；目标机不需要任何东西。
