# 单一求值器：C# 负责全部表达式

## 背景

表达式语义曾经有两份实现：

- **C `eval`**：投影、分组键、聚合参数，以及无 C# 阶段时的 WHERE；
- **C# `Val` + `PredicateCompiler`**：WHERE 谓词，编译成 `Func<..., bool>`。

两份实现必须逐条同步（三值逻辑、int64 比较、整数除法、SUM 类型）。2026-11 的统一提交把语义对齐，但漂移风险仍在：改一处必须记得改另一处，编译器不会提醒。

## 决策

C# 阶段成为**唯一运行时求值器**：

1. 新增 ABI `qf_cs_eval_values(host, ast, node, table, batch, udf, udf_ctx, out, diag)`：
   对整批行求一个非聚合表达式，输出 `batch->rows` 个 `qf_value`。
2. 新增 `qf_udf_call_fn` 回调：C# 遇到 `CALL` 时回到宿主执行 Python；
   文本与值一律用指针传递，避免跨 ABI 按值传 16 字节结构体。
3. C 引擎在进入分组/投影/聚合前，对每个表达式**按整批求值一次**，
   后续只做数组索引；删除 C 的递归解释器与 UDF 参数求值。
4. WHERE 继续使用 `PredicateCompiler`，它属于同一个 C# 求值器家族，
   与 `ValueEvaluator` 共享 `Val` 语义。
5. ABI 版本提升到 3。

## 后果

- **C# 成为必需阶段**：缺少 C# 归档的 `--allow-missing` 构建仍可链接
  （stub 提供符号），但任何查询都会在求值时返回 `QF_ERR_UNSUPPORTED`。
  这是“单一语义”的必然代价。
- **性能**：每列一次跨语言批量调用 + C# 逐行解释，比原 C 解释器多一层边界，
  但避免了逐行跨语言调用；300k 行 group by 仍在 0.4s 量级。
- **测试**：C 阶段只做编译检查与 arena 自测；引擎行为由 C# 阶段自测
  与 e2e（对照 sqlite）覆盖，不再保留表达式求值的测试替身。
- **契约**：`qf_cs_eval_values` 与 WHERE 谓词共用 `Val`，未来修改语义只需
  改一处；`contracts/qf.h` 只声明函数，结构仍由 `schema/qf.ts` 生成。
