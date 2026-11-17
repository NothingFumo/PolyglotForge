# PolyglotForge

多语言混合编译学习项目：九种语言各自实现查询引擎的一部分，最终静态链接为**一个** `queryforge.exe`。

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
bash tests/e2e/run.sh          # 端到端验收
```

## 决策记录

见 `docs/adr/`。
