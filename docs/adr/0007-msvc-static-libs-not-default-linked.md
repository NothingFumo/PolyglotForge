# C# 与 CPython 的静态库暂不参与默认链接

两者的产物都能独立构建并通过各自自测，但把 MSVC 编译的静态库交给 MinGW 主干链接时，缺的不再是几个兼容符号，而是整套 CRT/C++ 运行时边界。默认构建因此只链接 MinGW 可消费的阶段（C / C++ / Rust / Go / Kotlin / JS），C# 与 Python 保留为「已验证可独立运行」的阶段，`--link-msvc` 作为实验开关保留。

## 实测证据

- **C#（NativeAOT 静态库）**：`qf_cs.lib` 18 MB，`dumpbin` 可见三个导出符号；用 MSVC `link.exe` + `bootstrapperdll.obj` 链接后自测通过（`WHERE age > 30` → keep 1/0/1）。但 gcc 链接需要约 3000 处 `Rhp*` 运行时助手、MSVC 的 C++ operator new/delete（`??_U@...`）与 `std::nothrow` 常量；补齐 `Runtime.WorkstationGC.lib` 与 `aotminipal.lib` 后仍有 C++ 运算符与版本化 WinAPI 导入（`WaitOnAddress`）。C++ 名字修饰与 ABI 无法由 MinGW 提供。
- **CPython 静态库**：`python311.lib` 33 MB（真静态，含 `Py_Initialize`），MSVC 链接自测通过（UDF `score(30)==60`）。gcc 链接剩余缺口集中在 UCRT 导入（`fprintf/sprintf/snprintf`）、`__chkstk`、`__isa_available`、`_Py_atomic_load_64bit_impl` 与 `PathCchSkipRoot`。

## 后续路线（任一即可恢复 9/9）

1. **第二装配器**：用 MSVC `link.exe` 做最终链接，改为验证 Go `c-archive` 与 Kotlin/Native 归档能否被 MSVC 消费。
2. **换实现**：Python 改用 RustPython（`freeze-stdlib` + `capi`），彻底绕开 MSVC CRT；C# 则必须走第二装配器，没有等价替代。

在路线确定前，不把这两个阶段塞进默认链接——让构建保持绿色，比让链接"看起来全绿"更重要。

## 追加（2026-11）：第二装配器落地，Go 与 Kotlin 也进了 link.exe

`scripts/msvc-all.bat` 现在用 `link.exe` 装出 `dist/queryforge-all.exe`，九个阶段全部在内
（C / C++ / Rust / C# / Kotlin / Go / CPython / JS），`tests/e2e/run.sh` 6/6 通过。
这条路线实际撞到三个 link.exe 契约问题，全部由 `stages/stubs/` 承担，连同实测依据记录如下。

### 1. Go `c-archive` 在 link.exe 下首次调用永久挂起

**症状**：`--version` 正常；任何要走扫描的查询没有任何输出、也不退出（`timeout 8` → rc=124）。
去掉 Go 后立即返回「缺少 Go 扫描阶段」，说明挂起只由 Go 引起，且发生在第一次调用。

**根因（源码级）**：

- `-buildmode=c-archive` 在 windows/amd64 上不自动启动运行时。`go.o` 把
  `_rt0_amd64_windows_lib` 的地址写进 **`.ctors`** 段（`objdump -t` 可见符号位于 `.text+0x73700`，
  `.ctors` 里就是它的指针），指望 C 运行时在 `main` 之前调用它；
- 该函数只做一件事：`_cgo_sys_thread_create` 起一个线程跑 `runtime·rt0_go`，自己立即返回；
- 每个导出函数入口都先调 `_cgo_wait_runtime_init_done()`，其实现是
  `while (!_cgo_is_runtime_initialized()) WaitForSingleObject(runtime_init_wait, INFINITE);`
  （`runtime/cgo/gcc_libinit_windows.c`），信号由初始化线程跑完后的
  `x_cgo_notify_runtime_init_done()` → `SetEvent` 给出。

**`.ctors` 是 MinGW/gcc 的约定**：gcc 的 CRT 在 `main` 前遍历 `__CTOR_LIST__ .. __CTOR_LIST_END__`；
MSVC 的 CRT 只遍历 `__xc_a .. __xc_z`，也就是 **`.CRT$XCA` … `.CRT$XCZ`** 这一串按名排序的
子节。link.exe 不认识 `.ctors`，把它原样当成普通数据搬进镜像——初始化线程从未启动，
于是 `_cgo_wait_runtime_init_done` 永远等不到 `SetEvent`，表现为**无输出的死锁**（不是崩溃）。

**修法**：`stages/stubs/go_crt_init.c` 用 MSVC 的初始化段补上同一个调用：

```c
void _rt0_amd64_windows_lib(void);
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
static void (*qf_go_runtime_init_hook)(void) = _rt0_amd64_windows_lib;
```

`_rt0_amd64_windows_lib` 的签名就是 `void(void)`，与 gcc 走 `.ctors` 时调用的是同一个函数，
两条装配路径的启动顺序一致。修好后 SQL 查询在毫秒级返回，e2e 6/6。

顺带实测到的一条细节：link.exe **只有在特性一致时**才把 `.CRT$*` 贡献折进 `.rdata` 里的
`__xc_a..__xc_z` 区间。带可写标志（`IMAGE_SCN_MEM_WRITE`）的贡献会另起一个独立的 `.CRT`
PE 节，落在区间之外，同样一个都不会被调用——这一点在 Kotlin 侧表现为静默不初始化。

### 2. Kotlin/Native 归档：LNK1143、LNK4072 与静态初始化

三种报错，全部由 `stages/stubs/coff_msvc_fix.py` 在链接前改写 COFF（等长原地改写，
可以直接作用在 `ar` 归档上）：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `fatal error LNK1143: 没有 COMDAT 0xD 节的符号`（`dumpbin /headers` 同一判定写作 `COMDAT (no symbol)`，单是 Kotlin 对象就有 3282 处） | `.xdata$X` / `.pdata$X` 是 `SELECT_ANY` 却没有代表符号；MSVC 要求 COMDAT 必须由一个外部符号命名 | 改成 `SELECT_ASSOCIATIVE`，`Number` 指向父节 `.text$X`（关联节由父节符号代表） |
| 清完 Kotlin 后 `libstdc++.a(string-inst.o)` 报同一个 LNK1143 | 这是 GNU 对象的共性，不是 Kotlin 特有 | 同一工具直接处理 `libstdc++` / `libmingwex` / `libwinpthread` / `libgcc` / `libgcc_eh` |
| `LNK4072: 节计数 212 超过最大值(96)` | link.exe 按完整节名合并，gcc 的 `.text.unlikely.<sym>`、`.xdata.unlikely.<sym>` 各成一节；PE 规格上限 96 节 | 把点号后缀折回基名（`.text` / `.xdata` / `.pdata`）：测试 exe 212 → 17 节，完整装配体 26 节 |
| `qf_dsl_build` 访问违例（`0xC0000005`） | `.ctors` 里的 `_GLOBAL__sub_I_{GlobalData,SafePoint,ThreadSuspension}.cpp` 从未执行，`initRuntime` 里对未初始化的 `pthread_mutex_lock` 传了 NULL | 同上：`.ctors` 改名成 `.CRT$XCU` 并清掉可写标志（`__xc_a..__xc_z` 区间从 0x20 字节变为 0x28 字节，初始化才真正进入 `_initterm` 的遍历范围） |

崩溃定位用的是 `/MAP` + gdb 回溯：`initRuntime+0x99a → pthread_mutex_lock → mutex_impl → mov (%rax),%rax`，
`rax` 为 0，正好对应未执行 `pthread_mutex_init(&safePointActionMutex, NULL)`。

### 3. 混装两套 CRT 的剩余代价

`/FORCE:MULTIPLE` 仍然必须：`libmingwex` 与 `ucrt` 都提供 `ldexp / modf / round / hypot / cbrt`
等 C99 数学函数与 `__guard_check_icall_fptr` 这类 CFG 桩，语义相同（ucrt 侧为转发），
实测只影响 warning。除此之外不需要任何 C++ 运算符或 `nothrow` 常量替身——Kotlin/Native 的
C++ 依赖全部由 MinGW 的 `libstdc++.a` + `libgcc_eh.a`（同样过一遍 `coff_msvc_fix.py`）满足，
没有出现 ADR 0007 正文里为 C# 记录的 `??_V@YAXPEAX@Z` 一类缺口。

### 4. 结论与继续保留的分工

- **默认构建仍是 MinGW 主干**（本 ADR 原判不变）：`scripts/build.sh` 的九个阶段在 gcc 侧继续演进；
- **MSVC 装配器是第二条可运行的验收路径**，也是唯一同时含 C# 与 CPython 的装配体；
- Go 不需要换实现（TinyGo 等替代方案未采用）：挂起是初始化表约定不同，不是 Go 运行时不兼容；
- Kotlin/Native 归档可以被 link.exe 消费，代价是链接前必须过一遍 COFF 归一化。
