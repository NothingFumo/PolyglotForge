# Kotlin 用 Kotlin/Native 直接产出静态库，DSL 与解析分离

Kotlin 阶段必须是可被 C 链接的静态库，故用 Kotlin/Native 的 `mingw_x64` 目标（与 MinGW 主干同 ABI），`konanc -produce static` 直接出 `.a`，不引入 Gradle。对外暴露文本 DSL，内部先落一套带接收者 lambda 的 builder，解析器只把文本翻译成 builder 调用——DSL 是 Kotlin 语言特性本身，而不是字符串模板。

## 考虑的方案

- **Kotlin/JVM + 内嵌 JVM**：语义最完整，但需要载荷化的 JVM 运行时，违背单文件与"尽量静态"。
- **Gradle 构建**：依赖分发更标准，但引入一套需要联网的构建系统，只为产出一次静态库不划算。

## 后果

构建机需要下载 konan 工具链（约 1 GB）；协程依赖 `kotlinx-coroutines-core` 的 native 产物，属于本项目对"仅标准库"的唯一破例，理由见提交记录。
