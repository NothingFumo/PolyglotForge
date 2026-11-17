# 0009：MSVC 全静态产物（MSVC 全静态）的依赖边界

## 背景

四产物里"MSVC + 全静态 CRT"最难：C# 的 NativeAOT、Rust 的 `+crt-static`、
Kotlin/Native 的 MinGW 归档、QuickJS 的 cl 端口都要在 `link.exe` 下共存，
且不允许出现 `VCRUNTIME140.dll` / `ucrtbase.dll` / `libstdc++-6.dll`。

## 决策

1. 我们自己的代码（C/C++/qf_py）：`cl /MT`，链接 `libcmt + libvcruntime + libucrt` 静态版，
   并 `/NODEFAULTLIB:msvcrt;ucrt;vcruntime`。
2. Kotlin/Native 归档需要 C++ 异常与 `std::thread` 的实现，`konanc` 没有
   "关闭异常"选项（`konanc -X` 只有 worker 异常处理模式），因此链接 MinGW 的
   `libstdc++.a`、`libgcc_eh.a`、`libwinpthread.a`（均经 `coff_msvc_fix.py` 归一化）。
3. 上述静态库引用的少量 CRT 入口（`__cxa_*` 所需的长跳转、`__mingw_vsnprintf`、
   `StringCchCopyNA`、`_aligned_malloc` 等）由 `stages/stubs/mingw_crt_shim.c` 提供；
   Itanium RTTI 虚表由 `stages/stubs/cxxabi_shim.cpp` 以 mingw 目标编译后归一化提供。
4. 验收标准不是"零 import"，而是"零第三方 DLL"：最终导入表只允许
   `KERNEL32/ADVAPI32/USER32/VERSION/WS2_32/bcrypt/ntdll/ole32/oleaut32/api-ms-win-*`。

## 结果

`dist/queryforge-static-msvc.exe`（18.3 MB）：导入 10 个系统 DLL，
`grep -icE "vcruntime|ucrtbase|msvcrt|libstdc|winpthread"` 为 0，e2e 6/6，Python 阶段零落盘。

## 代价

- `pthread` 语义由 `libwinpthread.a` 承担；Kotlin 的 GC 并发标记确实会调用
  `pthread_rwlock_*`，不能用空实现糊弄。
- 因此这一格**不是**"完全无外部符号"，而是把运行时依赖全部静态化，
  仅保留 Windows 系统 DLL。
