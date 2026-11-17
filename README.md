# PolyglotForge

多语言混合编译学习项目：九种语言各自实现查询引擎的一部分，最终静态链接为**一个** `queryforge.exe`。

## 当前状态

| 阶段 | 状态 |
|---|---|
| TypeScript | 已接入（构建期 schema/codegen） |
| C / C++ / Rust / Go / JavaScript | **已静态链接进 `dist/queryforge.exe`**，端到端 6/6 与 sqlite3 逐字节一致 |
| Python | 静态 CPython（33 MB）与 UDF 自测通过；因 MSVC/MinGW CRT 边界未入默认链接（见 ADR 0007） |
| C# | NativeAOT 静态库与谓词自测通过；同上 |
| Kotlin | 阶段开发中（Kotlin/Native 工具链获取受阻） |

```console
$ ./dist/queryforge.exe tests/e2e/data/users.csv \
    "select city as city, count(*) as n from t group by city order by n desc, city asc"
city  n
----  -
BJ    2
SH    2
SZ    2
(3 rows)
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

## 构建

```bash
bash scripts/build.sh          # 全量构建 → dist/queryforge.exe
bash tests/e2e/run.sh          # 端到端验收（sqlite3 对照）
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
