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
| C# | ✅ NativeAOT 静态库 | ✅（`coff_repack` + C++ ABI shim，待复核） |
| Python | ✅ 静态 CPython | 接入中 |
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

## 构建

```bash
bash scripts/build.sh --allow-missing   # 构建可得阶段 → dist/queryforge.exe
bash tests/e2e/run.sh                   # 端到端验收（sqlite3 对照）
bash scripts/test-all.sh                # 各语言自测 + 端到端
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
