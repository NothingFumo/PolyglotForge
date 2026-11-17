# C# 阶段：MinGW 链接 NativeAOT 静态库的限制

## 结论

`bash stages/csharp/build.sh` 稳定产出 `stages/csharp/out/qf_cs.lib`，三个导出符号
`qf_cs_compile` / `qf_cs_eval` / `qf_cs_free` 都在（`dumpbin -symbols` 可见）。

但 **MinGW 的 gcc/ld 无法把这个库链接进自测程序**：NativeAOT 静态库依赖 MSVC CRT 与
AOT 运行时库，两者的启动代码互斥。按分工约定，这项实验到此为止，
`stages/csharp/test.sh` 改为「库存在即通过，链接失败输出明确降级提示并返回 0」。

本阶段对应的验收断言（`WHERE age > 30` 三行批次 → keep = 1/0/1）已经在 MSVC 链路上
跑通，记录见文末「已验证的可行链路」，供以后切换工具链时复用。

## 原因

`NativeLib=Static` 的产物只有 `qf_cs.obj` 一个成员：

```
$ cat obj/Release/net10.0/win-x64/native/lib.rsp
/OUT:"bin\Release\net10.0\win-x64\native\qf_cs.lib"
"obj\Release\net10.0\win-x64\native\qf_cs.obj"
```

托管代码编译出的机器码引用了一大批 AOT 运行时助手（`RhpNewFast`、`RhpAssignRef`、
`RhpThrowEx` …），这些助手、GC 运行时（`Runtime.WorkstationGC.lib`）、
`bootstrapperdll.obj` 以及 MSVC CRT（`vcruntime.lib` / `ucrt.lib` / `libcpmt.lib`）
都要由最终链接方提供。

MSVC 的 CRT 启动代码（`crt2` 里的 `atexit`、`tlsdyn` 里的 `__dyn_tls_init`）与
MinGW 的 `crt2.o` / `libmingw32.a` 定义同名符号，链接期直接互斥，无法通过追加库解决。

## 尝试 1：直接 gcc 链接

```
gcc -std=c11 -O2 -Igen -Icontracts stages/csharp/selftest.c \
    stages/csharp/out/qf_cs.lib -o out/csharp_selftest.exe
```

`ld` 报告 121 个未定义符号（合计 3062 处引用），按出现次数取前几名：

| 符号 | 引用次数 |
|---|---|
| `RhpAssignRef` | 3062 |
| `RhpNewFast` | 2804 |
| `RhpThrowEx` | 1068 |
| `RhpNewPtrArrayFast` | 359 |
| `__security_cookie` | 264 |
| `RhpCheckedAssignRef` | 262 |
| `RhpNewArrayFast` | 197 |
| `RhpFallbackFailFast` | 170 |

报错原文（节选）：

```
ld.exe: stages/csharp/out/qf_cs.lib(obj/Release/net10.0/win-x64/native/qf_cs.obj):(.managedcode$I+0xe7a): undefined reference to `RhpNewPtrArrayFast'
ld.exe: stages/csharp/out/qf_cs.lib(obj/Release/net10.0/win-x64/native/qf_cs.obj):(.managedcode$I+0xe89): undefined reference to `RhpAssignRef'
```

清单里预期会出现、但这一步还没轮到的 CRT 符号（`__security_check_cookie`、
`__GSHandlerCheck`、`_guard_check_icall_nop`、`_CallMemberFunction0`、`??_7type_info`）
属于 CRT 侧实现，MinGW 不提供；本库只直接引用到其中的 `__security_cookie`。

## 尝试 2：追加 AOT 运行时库 + MSVC CRT

链接时补上 `~/.nuget/packages/microsoft.netcore.app.runtime.nativeaot.win-x64/<ver>/runtimes/win-x64/native/`
下的 `bootstrapperdll.obj`、`Runtime.WorkstationGC.lib`、`eventpipe-disabled.lib`、
`Runtime.VxsortDisabled.lib`、`standalonegc-disabled.lib`、`aotminipal.lib`、
`zlibstatic.lib`、`brotli*.lib`、`System.Globalization.Native.Aot.lib`、
`System.IO.Compression.Native.Aot.lib`，并用 `-L` 指向 MSVC 与 Windows SDK 的 lib 目录，
追加 `-lvcruntime -lucrt -llibcpmt -lmsvcprt -loldnames` 与 `kernel32`/`advapi32`/`ws2_32` 等。

结果：**3 个重复定义 + 157 个未定义**，仍然失败。

重复定义（CRT 启动代码互斥，无法通过库顺序解决）：

```
ld.exe: D:/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/lib/x64/msvcrt.lib(...utility.obj):(.text$mn+0x0): multiple definition of `atexit'; .../crt2.o:crtexe.c:(.text+0x583): first defined here
ld.exe: .../msvcrt.lib(...tlsdyn.obj):(.text$mn+0x0): multiple definition of `__dyn_tls_init'; .../libmingw32.a(lib64_libmingw32_a-tlssup.o):tlssup.c:(.text+0x0): first defined here
ld.exe: .../msvcrt.lib(...tlsdyn.obj):(.rdata+0x0): multiple definition of `__dyn_tls_init_callback'; .../libmingw32.a(lib64_libmingw32_a-tlssup.o):tlssup.c:(.rdata+0x28): first defined here
```

未定义符号集中在 GC 的 C++ 实现与 MSVC CRT 内部（共 157 个），示例：

```
ld.exe: ... crt2.o:crtexe.c:(.text+0x3d): undefined reference to `__set_app_type'
ld.exe: ... msvcrt.lib(...): undefined reference to `??_R4type_info@@6B@'
ld.exe: ... Runtime.WorkstationGC.lib(...): undefined reference to `?GetCurrentThread@ThreadStore@@SAPEAVThread@@XZ'
ld.exe: ... Runtime.WorkstationGC.lib(...): undefined reference to `??0bad_alloc@std@@QEAA@XZ'
ld.exe: ... Runtime.WorkstationGC.lib(...): undefined reference to `_register_onexit_function'
```

`--allow-multiple-definition` 之类的开关可以让链接“通过”，但那是把两套 CRT 拼在一起，
运行期行为不可预期；本阶段不做这种拼装。

## 已验证的可行链路（MSVC，供未来切换）

同一份 `selftest.c` 与同一份 `out/qf_cs.lib`，用 MSVC 工具链链接即可跑通：

```
cl -nologo -O2 -W3 -Igen -Icontracts -c stages/csharp/selftest.c -Fo:selftest.obj
link -NOLOGO -SUBSYSTEM:CONSOLE -OUT:csharp_selftest.exe selftest.obj stages/csharp/out/qf_cs.lib \
     <AOT>/bootstrapperdll.obj <AOT>/Runtime.WorkstationGC.lib <AOT>/eventpipe-disabled.lib \
     <AOT>/Runtime.VxsortDisabled.lib <AOT>/standalonegc-disabled.lib <AOT>/aotminipal.lib \
     <AOT>/zlibstatic.lib <AOT>/brotlicommon.lib <AOT>/brotlienc.lib <AOT>/brotlidec.lib \
     <AOT>/System.Globalization.Native.Aot.lib <AOT>/System.IO.Compression.Native.Aot.lib \
     advapi32.lib bcrypt.lib crypt32.lib iphlpapi.lib kernel32.lib mswsock.lib ncrypt.lib \
     normaliz.lib ntdll.lib ole32.lib oleaut32.lib secur32.lib user32.lib version.lib ws2_32.lib
```

输出：

```
[case] WHERE age > 30
  keep = 1/0/1 (期望 1/0/1)
[case] 空值参与比较
  keep = 1/0/1/0 (期望 1/0/1/0)
[case] 文本比较按 ordinal
  keep = 1/0/0 (期望 1/0/0)
[case] 不支持的节点返回 QF_ERR_TYPE
  diag: 谓词不支持调用 UDF
csharp stage OK
```

注意 `<AOT>/bootstrapperdll.obj` 是 DLL 场景的引导对象；exe 场景的 `bootstrapper.obj`
会引入 `__managed__Main`，静态库使用者不能链接它。
