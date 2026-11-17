# 工具链边界：MinGW 主干，MSVC 只用于产出可链入的静态库

最终链接统一用 MinGW gcc，因为它能消费 Go `c-archive`、Rust `windows-gnu`、Kotlin/Native `mingw_x64` 与 QuickJS 的产物；MSVC 只负责产出 C# NativeAOT 静态库与 CPython 静态库。二者共用一份 CRT 兼容 stub（`/DEFAULTLIB` 缺失的 `__security_check_cookie`、`__GSHandlerCheck`、`_guard_check_icall_nop`、`_CallMemberFunction0`、`??_7type_info`），由 `cl.exe` 编译成 `.obj` 参与链接。

## 考虑的方案

- **MSVC 单一主干**：C# 与 Python 零摩擦，但 Kotlin/Native 的 Windows 目标只有 `mingw_x64`，且 Go `c-archive` 官方要求 gcc，会丢掉两门语言。
- **MinGW 单一主干且 C#/Python 也改用 MinGW 可链实现**：C# 无替代，Python 只能退到 RustPython（子集语义），违背"语言惯用法优先"。

## 后果

Kotlin/Native 需要约 1 GB 的 konan 工具链；CPython 需从源码用 `PCbuild` 构建静态 `python.lib`；兼容 stub 若在某版本失效，C# 阶段降级为 DLL 载荷并在 ADR 记录。
