# PolyglotForge

多语言混合编译学习项目：九种语言各自实现查询引擎的一部分，最终静态链接为**一个** `queryforge.exe`。

## 当前状态

**`dist/queryforge-all.exe`（MSVC `link.exe` 装配）已包含全部九个阶段**，端到端 6/6 与 sqlite3 逐字节一致：

| 阶段 | MSVC 装配体 | GCC 装配体 |
|---|---|---|
| C / C++ / Rust | ✅ | ✅ |
| Go | ✅（`.CRT$XCU` 补运行时初始化） | ✅ |
| Kotlin | ✅（COFF 归一化后） | ✅ |
| JavaScript | ✅ QuickJS | ✅ QuickJS |
| C# | ✅ NativeAOT 静态库 | ✅（`coff_repack` + C++ ABI shim） |
| Python | ✅ 静态 CPython | ✅（同一套归一化） |
| TypeScript | 构建期 schema/codegen | 同 |

```console
$ ./dist/queryforge-all.exe tests/e2e/data/users.csv \
    "select city as city, count(*) as n from t group by city order by n desc, city asc"
city  n
----  -
BJ    2
SH    2
SZ    2
(3 rows)

$ ./dist/queryforge-all.exe tests/e2e/data/users.csv --dsl 'from("users"){select("name");limit(2)}'
$ ./dist/queryforge-all.exe tests/e2e/data/users.csv \
    "select name as name, age as age from t" --where "row.age > 35"      # JS 沙箱
$ ./dist/queryforge-all.exe tests/e2e/data/users.csv \
    "select name as name from t where age > 35"                          # C# 编译谓词
$ ./dist/queryforge-all.exe tests/e2e/data/users.csv \
    "select score(age) as s from t" --plugin stages/python/fixtures/score.py   # CPython
```

## 目录

| 路径 | 内容 |
|---|---|
| `contracts/qf.h` | 跨语言 C ABI（唯一契约） |
| `schema/` + `codegen/` | TypeScript 作为布局与错误码的单一真相源，生成各语言绑定 |
| `gen/` | codegen 产物（入库，便于每个提交自证可编译） |
| `stages/<lang>/` | 每个语言的阶段实现与独立自测 |
| `tests/e2e/` | 端到端验收（含 sqlite3 对照） |
| `scripts/build.sh` | 构建与装配脚本 |

## 四产物的依赖边界

| 产物 | 第三方 DLL | 其余动态依赖 | 打包方式 |
|---|---|---|---|
| `queryforge.exe`（gcc 静态） | 无 | `ucrtbase.dll` + `api-ms-win-crt-*`（Windows 自带） | libstdc++/libsupc++/libgcc/winpthread 全静态 |
| `queryforge-deps.exe`（gcc 动态） | `libstdc++-6.dll`、`libwinpthread-1.dll` | 同上 | 第三方 DLL 不内嵌，由 `build.sh --deps` 复制到 `dist/` |
| `queryforge-all.exe`（MSVC 动态） | `VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` | 同上 | 由 `msvc-all.bat` 复制到 `dist/` |
| `queryforge-static-msvc.exe`（MSVC 静态） | 无 | 10 个系统 DLL、11 个导入符号 | `/MT` + `libcmt/libvcruntime/libucrt`，导入表零非系统 DLL |

规则与 hello world 一致：**动态格不内嵌第三方运行库，但必须随包；静态格内嵌，且导入表不得出现非系统 DLL。**
`bash tests/e2e/cells.sh` 会清空 `PATH`（只留 `C:\Windows\system32`）逐个启动四产物来验证这条规则。

Python 标准库作为载荷内嵌，且**不落盘**：构建期把 `Lib/` 压成 `lib.zip`（排除 test/tkinter），
以 `ld -r -b binary` 链入 `.rdata`；解释器初始化后把这段字节交给内嵌的 zip finder
（`_struct` 解中央目录 + `zlib` 解压 + `compile`），encodings 则在构建期冻结进
`Python/frozen.c` 的 `stdlib_modules[]`。运行期不写任何临时文件：

```console
$ ./dist/queryforge.exe tests/e2e/data/users.csv \
    "select stdlib_origin(1) as p from t limit 1" --plugin stages/python/fixtures/probe_path.py --raw
p
frozen | <builtin>
```

因此产物的外部依赖只剩上表的 DLL；本机是否安装 Python 不再影响运行。

## 构建

```bash
bash scripts/build.sh --allow-missing   # gcc 主干 → dist/queryforge.exe
cmd /c 'scripts\msvc-all.bat'           # MSVC 主干 → dist/queryforge-all.exe
bash tests/e2e/run.sh ./dist/queryforge.exe 10
bash tests/e2e/run.sh ./dist/queryforge-all.exe 10
```

## 使用

```bash
./dist/queryforge.exe data/users.csv "select name, age from t where age > 30 order by age desc"
./dist/queryforge.exe data/users.csv "select city, count(*) as n from t group by city order by n desc" --explain
./dist/queryforge.exe data/users.csv "select name from t" --where "row.age > 30"   # JS 沙箱过滤
./dist/queryforge.exe data/users.csv "select score(name) from t" --plugin score.py # Python UDF
./dist/queryforge.exe data/users.csv --dsl 'from(users) { select("name"); limit(2) }'  # Kotlin DSL
./dist/queryforge.exe data/users.csv "select name from t" --batch "select count(*) from t;select name from t limit 1"
```

## 决策记录

见 `docs/adr/`。
