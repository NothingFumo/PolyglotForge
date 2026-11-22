# PolyglotForge

多语言混合编译学习项目：九种语言各负责查询引擎的一段，最终链接成**一个**可执行文件。
运行时不需要 Python、.NET、JVM 或 Node 环境。

## 产物

| 文件 | 工具链 | CRT | 需要随包的 DLL |
|---|---|---|---|
| `dist/queryforge.exe` | gcc | 静态（libstdc++/libsupc++/libgcc/winpthread） | 无 |
| `dist/queryforge-deps.exe` | gcc | 动态 | `libstdc++-6.dll`、`libwinpthread-1.dll` |
| `dist/queryforge-all.exe` | MSVC `link.exe` | 动态 | `VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` |
| `dist/queryforge-static-msvc.exe` | MSVC `link.exe` | 静态（`/MT`） | 无 |

`ucrtbase.dll`、`api-ms-win-crt-*` 等 Windows 自带组件不计入上表；表格里的 DLL 由构建脚本
自动复制到 `dist/`，运行时与 exe 同目录即可。

## 构建

```bash
bash scripts/build.sh                 # gcc 静态 → dist/queryforge.exe
bash scripts/build.sh --deps           # gcc 动态 → dist/queryforge-deps.exe
```

```bat
cmd /c scripts\msvc-all.bat            :: MSVC 动态 → dist/queryforge-all.exe
cmd /c scripts\msvc-static.bat         :: MSVC 静态 → dist/queryforge-static-msvc.exe
```

前置条件：gcc/g++（MinGW-w64）、Rust、Go、Kotlin/Native、Node 或 Bun、CPython 3.11 源码、
Visual Studio（含 C++ 工具集与 NativeAOT 运行时包）。换机器先跑
`python scripts/env.py --check` 看哪项没探测到。缺失某个语言阶段时可用
`bash scripts/build.sh --allow-missing` 继续，未就位的阶段由 `stages/stubs` 占位；
两个 `.bat` 会在阶段归档缺失时自动调用 `scripts/produce.py` 补齐。

## 运行

```bash
# 分组聚合
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select city as city, count(*) as n from t group by city order by n desc, city asc"

# 执行计划
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select name from t where age > 30" --explain

# JavaScript 沙箱谓词
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select name from t" --where "row.age > 30"

# Python UDF
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select score(age) as s from t limit 2" --plugin stages/python/fixtures/score.py --raw

# Kotlin DSL
./dist/queryforge.exe tests/e2e/data/users.csv \
    --dsl 'from("users"){select("name");limit(2)}'

# 多语句批处理
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select name from t" --batch "select count(*) from t;select name from t limit 1"
```

四个产物接受同一套参数，换文件名即可。

## 验证

```bash
bash tests/e2e/run.sh ./dist/queryforge.exe 10        # 与 sqlite3 逐字节对照
bash tests/e2e/abi.sh                                 # 阶段导出符号是否都在 qf.h 里
bash tests/e2e/portability.sh                         # 干净 PATH 启动 + 随包 DLL + 零落盘
```

`portability.sh` 会检查：动态产物需要的第三方 DLL 是否在 `dist/` 内（含 DLL 自身的依赖），
静态产物的导入表里是否出现 `vcruntime`/`libstdc++`/`winpthread`，四个 exe 在仅有
`C:\Windows\system32` 的 `PATH` 下能否输出结果，以及 Python UDF 运行后临时目录是否为空
（零落盘）。`abi.sh` 用 `nm` 对比阶段归档的导出与 `contracts/qf.h` 的声明，拦"契约外入口"。

## SQL 子集

queryforge 实现的是单表查询子集，不是完整 SQL：

- **支持**：`SELECT` 表达式与 `AS` 别名、单表 `FROM`、`WHERE`、`GROUP BY`、
  `count/sum/avg/min/max`、`ORDER BY`、`LIMIT`；
- **不支持**：`HAVING`、`SELECT *`、`JOIN`、子查询、多表、`DISTINCT`、窗口函数；
- **表**始终是传入的那一个 CSV 文件；SQL 里的表名只用于诊断，不参与解析；
- **ORDER BY** 只能引用输出列：`SELECT` 别名、输出列名或列序号（从 1 开始）；
- **类型**只有整数 / 浮点 / 文本 / 空值四种；整数除法保持整数（与 sqlite 一致），
  除零结果为 `NULL`；
- **空值**遵循 SQL 三值逻辑（`FALSE AND UNKNOWN = FALSE`、`TRUE OR UNKNOWN = TRUE`），
  排序时无论升降序一律排在最后。

## 目录

| 路径 | 内容 |
|---|---|
| `contracts/qf.h` | 跨语言 C ABI（唯一契约） |
| `schema/`、`codegen/` | TypeScript 作为布局与错误码的单一真相源，生成各语言绑定 |
| `gen/` | codegen 产物（入库，保证任意提交都可编译） |
| `stages/<lang>/` | 各语言阶段实现、构建脚本与自测 |
| `tests/e2e/` | 端到端验收与 sqlite3 对照 |
| `scripts/` | `stages.py`（阶段清单）、`env.py`（工具探测）、`produce.py`/`produce.sh`（产物生成）、`build.sh`、`msvc-all.bat`、`msvc-static.bat` |
| `docs/adr/` | 架构决策记录 |
| `dist/` | 构建产物（exe 与随包 DLL，不入库） |

## Python 阶段

标准库作为载荷内嵌，运行期不写临时文件：

- `lib.zip`（`Lib/` 去掉 test/tkinter）用 `ld -r -b binary` 生成对象，再用
  `objcopy --rename-section .data=.rdata,alloc,load,readonly,...` 改成只读段；
- 解释器初始化后把该段字节交给内嵌 zip finder（`_struct` 解析中央目录、`zlib` 解压、
  `compile`）；
- `encodings` 由 `scripts/freeze_encodings.py --install` 在构建 `pythoncore` 之前写入
  `Python/frozen.c` 的 `stdlib_modules[]`（`is_package=1`），这是唯一的冻结机制，
  运行期不再拼表。

```bash
./dist/queryforge.exe tests/e2e/data/users.csv \
    "select stdlib_origin(1) as p from t limit 1" \
    --plugin stages/python/fixtures/probe_path.py --raw
# p
# frozen | <zip>/json/__init__.py
```

## 决策记录

见 `docs/adr/`。
