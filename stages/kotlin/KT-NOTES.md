# Kotlin 阶段实现说明

本阶段交付 `stages/kotlin/build/libqf_kt.a`，导出两个 C ABI 入口：

| 符号 | 作用 |
| --- | --- |
| `qf_dsl_build(host, src, out_ast, diag)` | 解析文本 DSL，构造 `qf_ast` |
| `qf_kt_batch(host, run, run_ctx, sqls, n, rows_out, diag)` | 协程并发调用宿主回调，逐条回填行数 |

## 工具链

- **Kotlin/Native 2.4.21，`mingw_x64` 目标，不使用 Gradle**：`build.sh` 直接调用发行包里的
  `cinterop.bat` 与 `konanc.bat`。
- 官方发布物在 2.x 后期改名为 `kotlin-native-prebuilt-windows-x86_64-<版本>.zip`
  （旧名 `kotlin-native-windows-x86_64-<版本>.zip` 已不存在）。`build.sh` 按新名下载，
  解压后若顶层目录名与假设不符，会自动 `find` 出 `bin/konanc.bat` 所在目录。
- 工具链与第三方 klib 放在 `stages/kotlin/vendor/`，由同目录 `.gitignore` 忽略，不随仓库分发；
  `build.sh` 在缺失时自动下载。
- `konanc`/`cinterop` 是 JVM 工具，需要 JDK 17+：优先用 `JAVA_HOME`（必须是 Windows 路径，
  因为批处理把它拼进 Windows `PATH`），未设置时探测 `D:\17`、`jdk-17`、`jdk-21`、`jdk-24`。
- 运行环境可能同时存在 MSYS/WSL bash，且 `bash` 解析到 Windows 自带的 WSL 启动器：
  `build.sh` 一律通过 `cmd.exe /c` 启动 `.bat`，参数保持相对路径（cmd 继承仓库根的 Windows cwd）；
  `cygpath`/`wslpath` 负责 Windows/Unix 路径互转。
- 解压 zip 的降级顺序：`unzip` → `tar`（仅当 `tar -tf` 能识别 zip，即 Windows bsdtar）→
  `python3` 的 `zipfile` 模块 → `powershell Expand-Archive`，避免某些 shell 缺 `unzip`。
- 目标 `mingw_x64` 以本机 MinGW-w64（Strawberry gcc 13.2.0）作为 sysroot，
  `build.sh` 把 `/mnt/c/Strawberry/c/bin` 放进 `PATH`，否则 cinterop 的 clang 找不到 C 运行库头文件。

## cinterop

`stages/kotlin/qf.def` 只暴露 `contracts/qf.h` 及其包含的 `gen/qf_layout.h`：

```
headers = qf.h
compilerOpts = -Icontracts -Igen
package = qf.c
```

`build.sh` 在仓库根执行 `cinterop -def stages/kotlin/qf.def -o stages/kotlin/build/qf_cinterop`，
生成 `qf_cinterop.klib`，konanc 用 `-library` 引入。`qf_host`、`qf_ast`、`qf_node`、`qf_str`、
`qf_value`、`qf_diag` 都是直接映射，没有影子类型。

cinterop 把 C 枚举常量映射成 `UInt`，而 C 侧 `qf_err`、`kind`、`op` 都是 32 位整数语义；
`src/Codes.kt` 在一处完成 `toInt()` 转换，其余代码只用 `Int`。

`qf_value` 的匿名 union、`qf_str` 与 C 字符串字段用裸偏移写入：
`qf_node.lit@16`、`qf_node.name@40`、`qf_ast.select_names@32`、`qf_ast.from_table@40`、
`qf_diag.message@16`、`qf_diag.hint@32`。这些偏移在 `selftest.c` 里有
`_Static_assert(offsetof(...))`，ABI 结构一变测试立刻失败，不会静默错位。

### 为什么有 `shim.c`

`qf.h` 的 `qf_dsl_build` 收 `qf_str`（结构体按值）。Kotlin/Native 导出函数不支持结构体按值参数：
`@CName` 函数写成 `CValue<qf_str>` 时，C 侧按值传、Kotlin 侧按指针读 `CValue`，
运行期直接 `0xC0000005`（已实测，gdb 栈顶在 `CValues.getPointer`）。因此：

- Kotlin 只导出 `qf_dsl_build_impl(host, const char* src, uint32_t len, out, diag)`；
- `stages/kotlin/shim.c` 提供与 `qf.h` 完全一致的 `qf_dsl_build(host, qf_str, out, diag)`，
  做一次按值拆包后转调 impl；
- `build.sh` 用 gcc 编译 `shim.c`，再用 `ar rcs` 把 `qf_shim.o` 并入 `libqf_kt.a`，
  保证 `scripts/build.sh` 只看到单个 `.a`。

`qf_kt_batch` 的参数全是指针/标量（含函数指针），无需 shim，直接由 Kotlin 导出。

## ABI 2：别名写在 select_names

`qf_ast` 有 `qf_str* select_names`（与 `select` 等长）。Kotlin 侧：

- `qf_node.name` 只保存列名 / 函数名（`count` 的 `node.name` 是 `count`，列节点保留原列名）；
- `select("name" as "who", count("*") as "n")` 的别名分别写进
  `select_names[0] = "who"`、`select_names[1] = "n"`；
- 无别名时写空串（`len = 0`），不是空指针。

## DSL：builder 是 Kotlin，文本只是前端

`src/Dsl.kt` 是真正的带接收者 lambda DSL：

```kotlin
from("users") {
    select("name", count("*") as "n")
    where { col("age") gt 30L }
    groupBy("name")
    orderBy(desc("n"))
    limit(3)
}
```

`src/Parse.kt` 只做词法与递归下降，把文本翻译成对 `QueryBuilder` 的调用序列
（`select` / `where` / `groupBy` / `orderBy` / `limit`），不直接构造 AST。
`src/Abi.kt` 才把 `KExpr` 后序扁平化成 `qf_node` 数组并写入宿主 arena。

文法要点：

- `from(表名) { ... }`，表名可带引号；
- `select(expr, ...)`：列名字符串、`col("x")`、`count/sum/avg/min/max("x"|"*")`、整数字面量，
  任意表达式后可跟 `as "别名"`；
- `where { ... }`：`col("x")` + `eq|ne|lt|le|gt|ge` + 字符串/整数字面量，
  支持 `and` / `or` / `not` 与括号；
- `groupBy("a", "b")`、`orderBy("a" desc, "b" asc)`、`limit(n)`；
- 语法/语义错误返回 `QF_ERR_PARSE`，`qf_diag` 带行、列与消息；builder 的参数校验
  （空表名、空 select 等）同样折成 `QF_ERR_PARSE`；
- 任何其它 Kotlin 异常（含宿主分配失败）折成 `QF_ERR_PANIC`，绝不跨越 C ABI。

聚合节点语义与 C 引擎一致：`count(*)` 用 `argc = 0`（引擎按整行计数），
其余聚合 `argc = 1`、`b` 指向参数节点，`op` 取 `qf_agg` 值。

## 协程批处理

- 依赖 `kotlinx-coroutines-core` 的 native klib。KMP 按目标单独发布，
  坐标是 `org.jetbrains.kotlinx:kotlinx-coroutines-core-mingwx64:1.11.0`；
  它又依赖 `org.jetbrains.kotlinx:atomicfu-mingwx64:0.32.1`。
  两个 klib 都必须用 `-library` 传给 konanc：只给 coroutines 会编译通过，
  但运行期报 `Variable uses unlinked class symbol 'kotlinx.atomicfu.locks/SynchronizedObject'`。
  （主坐标 `kotlinx-coroutines-core-1.11.0.jar` 只有 metadata，不能直接给 konanc。）
- 实现：`runBlocking { coroutineScope { (0 until n).map { async(Dispatchers.Default) { ... } }.awaitAll() } }`。
- 每个回调使用自己栈上的 `qf_diag`，避免并发写同一块诊断；全部结束后按索引回填
  `rows_out[i]`。第一条 `code != QF_ERR_OK` 的结果胜出，其错误码/消息/位置拷进调用方 `diag`。
- `run` 会在不同线程上并发执行，`run` 与 `run_ctx` 必须是线程安全的；
  Kotlin 2.x 使用新内存管理器，没有旧的 freezing 限制。
- **未降级**：coroutines klib 可用，实际使用 `Dispatchers.Default`，没有退到
  `Worker`/`AtomicInt` 实现。

## 构建与链接

`build.sh`：

1. 缺失时下载/解压 Kotlin/Native、coroutines 与 atomicfu klib；
2. `cinterop -def stages/kotlin/qf.def -o stages/kotlin/build/qf_cinterop`；
3. `konanc -target mingw_x64 -produce static -opt -o stages/kotlin/build/libqf_kt ...`；
4. 必要时把产物重命名为 `build/libqf_kt.a`；
5. 编译 `shim.c` 并将 `qf_shim.o` 并入 `libqf_kt.a`。

`test.sh` 用 MinGW gcc（Strawberry 13.2.0）编译 `selftest.c`，链接 `build/libqf_kt.a`。
实测只需 `-lstdc++ -lm`（Kotlin/Native 运行时的 C++ 依赖由 `libstdc++` 提供，
不需要额外 Winsock/加密库），与根 `scripts/build.sh` 的链接选项一致。断言覆盖：

- DSL 的 select/groupBy/orderBy 数量、`QF_F_DESC`、`select_names` 别名、
  `node.name` 保留语义名、where 根节点形状与字面量、limit；
- 非法 DSL → `QF_ERR_PARSE`，且不产出 AST，诊断带行号；
- 3 条 SQL 的并发回调：逐条行数（2/3/5）与总和 10，回调次数为 3。

## 已知限制

- 只支持单表 `from(...)`；DSL 是查询构建器前端，不是完整 SQL 方言。
- 文本解析器支持整数与字符串字面量，不开放浮点字面量（builder 与 `qf_value` 都能表达）。
- `where` 只解析谓词表达式，暂不支持在 `where` 中调用 UDF。
- Kotlin/Native 静态库必须由 C++ 运行库消费（`-lstdc++`）；这是 K/N 运行时本身的要求，
  不是本阶段额外引入的依赖。
