# 0010：装配器与阶段清单

## 背景

九种语言各产出一个归档，最终由三套装配器（`scripts/build.sh`、`msvc-all.bat`、
`msvc-static.bat`）装进同一个 exe。装配一个阶段需要知道的事实只有四项：构建脚本、
归档路径、`-D` 宏、额外链接库——但它们在 2026-11 之前抄在四个文件里，新增一个阶段
要改 13~15 处，三份清单已经漂移（`build.sh` 不含 `c`/`typescript`，`test-all.sh` 含）。
cmd 侧还额外写死了 13 处工具绝对路径，换机器或升 VS 就整段失效。

## 决策

1. **阶段清单是唯一事实源**。`scripts/stages.py` 用一张表描述每个阶段，提供三种
   消费格式：`--bash`（gcc 装配器 eval 的数组）、`--bat <profile>`（link.exe 装配器
   `call` 的 set 行）、`--artifacts`（验收遍历的产物清单）。装配器不再列名。

2. **工具位置由 `scripts/env.py` 探测**，输出 `set QF_*` 行供 `.bat` 消费：vswhere 定位
   vcvars，文件系统探测 Strawberry/gcc 版本目录/Python/NativeAOT 运行时/konan libgcc/LLVM。
   `QF_BASH` 显式排除 `C:\Windows\System32\bash.exe`——那是 WSL，接不了 Windows 路径，
   而 cmd 的 PATH 恰好优先命中它。

3. **清单里的每个产物是一个 `(路径, 目标)` 对**，目标名指向 `scripts/produce.sh` 的
   分支：装配器发现缺失就调用 `scripts/produce.py <目标>`（python 负责找到可用的 bash），
   生成后复查，仍缺则退出。生成逻辑集中在一个 module，装配器只负责"缺什么就喊谁"。
   早期版本另有一个自由文本的 `producer` 字段，没有任何消费者，已删除。

4. **「装配器」「producer」不进 `CONTEXT.md`**。判据是这个词是否出现在查询语义里：
   查询、表、计划、算子、载荷是运行时词汇；装配器与 producer 是构建期词汇，只在这里
   与 README 的目录表出现。

## 后果

- 新增一个阶段 = 清单里加一行 + 写 `stages/<lang>/build.sh`。
- 换机器只需 `python scripts/env.py --check` 看缺哪项，不再 grep 绝对路径。
- producer 依赖 bash 与各语言工具链；`produce.sh` 的九个目标都要求幂等，否则
  "缺则生成"会变成每次全量重编。
- 装配器在链接前校验全部输入，失败点从"链接期 400 个未解析符号"提前到"缺 X"。

## 附录：host 前置条件的现状（事实快照）

`contracts/qf.h` 只规定类型，没规定"host 必须提供什么"。各阶段的真实行为：

| 阶段 | 缺 `host->str` 时 |
|---|---|
| C++ / Rust / Go | 入口拒绝，返回错误码 |
| Python | 退回静态缓冲（`qf_py.c` 的 `host_text`） |
| JavaScript | 指向字面量，不复制 |
| Kotlin | `qf_dsl_build` 由 `stages/kotlin/shim.c` 的 `qf_host_gate` 拒绝；`qf_kt_batch` 由 Kotlin 直接导出，`str()` 已改为缺服务返回空串（`alloc` 缺失仍是已知边界） |
| C# | 回退到 `NativeMemory.Alloc` |

统一这些行为需要 host adapter（改动 7 个语言阶段），当前判断收益不抵风险；
`tests/e2e/abi.sh` 守住"入口不漏、导出不多"，Kotlin 的 `str` 路径已不再抛异常。
