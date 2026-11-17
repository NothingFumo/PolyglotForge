@file:OptIn(
    kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class,
)

/*
 * 协程批处理：把 n 条查询同时交给宿主回调 run，回调返回各自行数。
 *
 * 并发点在 Kotlin 侧：runBlocking + coroutineScope + Dispatchers.Default。
 * 每个回调拿自己的栈上 qf_diag，避免并发写同一块诊断；第一条错误按索引胜出后
 * 再拷进调用方的 diag，因此 qf_kt_batch 的错误总是稳定的。
 */
package qf.stage

import kotlinx.cinterop.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.runBlocking
import qf.c.*

private class QueryOutcome(
    val code: Int,
    val rows: ULong,
    val line: Int,
    val col: Int,
    val message: String?,
)

private fun qf_str.readText(): String? {
    val p = ptr ?: return null
    if (len == 0u) return ""
    return p.readBytes(len.toInt()).decodeToString()
}

private fun runOne(
    run: qf_run_fn,
    runCtx: COpaquePointer?,
    sql: CPointer<ByteVar>?,
): QueryOutcome = memScoped {
    val d = alloc<qf_diag>()
    val rows = alloc<ULongVar>()
    rows.value = 0uL
    try {
        val rc = run.invoke(runCtx, sql, rows.ptr, d.ptr)
        QueryOutcome(
            code = rc,
            rows = rows.value,
            line = d.line.toInt(),
            col = d.col.toInt(),
            message = if (rc == Codes.ERR_OK) null else d.message.readText(),
        )
    } catch (e: Throwable) {
        QueryOutcome(Codes.ERR_PANIC, 0uL, 0, 0, "查询回调抛出异常: ${e.message}")
    }
}

@CName("qf_kt_batch")
fun qfKtBatch(
    host: CPointer<qf_host>?,
    run: qf_run_fn?,
    run_ctx: COpaquePointer?,
    sqls: CPointer<CPointerVar<ByteVar>>?,
    n: UInt,
    rows_out: CPointer<ULongVar>?,
    diag: CPointer<qf_diag>?,
): Int {
    if (n == 0u) return Codes.ERR_OK
    if (host == null || run == null || sqls == null || rows_out == null || n > Int.MAX_VALUE.toUInt()) {
        setDiag(null, diag, Codes.ERR_ARG, 0, 0, "qf_kt_batch 参数无效")
        return Codes.ERR_ARG
    }
    val href = HostRef(host)
    val count = n.toInt()

    val outcomes = try {
        runBlocking {
            coroutineScope {
                (0 until count).map { i ->
                    async(Dispatchers.Default) {
                        runOne(run, run_ctx, sqls[i])
                    }
                }.awaitAll()
            }
        }
    } catch (e: Throwable) {
        setDiag(href, diag, Codes.ERR_PANIC, 0, 0, "协程批处理异常: ${e.message}")
        return Codes.ERR_PANIC
    }

    var firstError = -1
    for (i in 0 until count) {
        rows_out[i] = outcomes[i].rows
        if (firstError < 0 && outcomes[i].code != Codes.ERR_OK) firstError = i
    }
    if (firstError >= 0) {
        val o = outcomes[firstError]
        setDiag(href, diag, o.code, o.line, o.col, o.message ?: "查询执行失败")
        return o.code
    }
    return Codes.ERR_OK
}
