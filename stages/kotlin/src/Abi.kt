@file:OptIn(
    kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class,
)

/*
 * ABI 边界：把 DSL 层的表达式树一次性落进宿主 arena 里的 qf_ast。
 *
 * 这里只做搬运与错误码转换，不含任何语法逻辑：
 *   - 所有内存都通过 host->alloc 取得（ADR 0002：谁分配谁释放的约定只属于宿主）；
 *   - 所有字符串都通过 host->str 取得，保证与宿主 arena 生命周期一致；
 *   - 任何 Kotlin 异常都在边界处折成 QF_ERR_PANIC，绝不跨越 C ABI。
 */
package qf.stage

import kotlinx.cinterop.*
import qf.c.*

/*
 * qf_node 布局见 gen/qf_layout.h（C 侧 static_assert 钉死总长 56）：
 *   kind u8@0 | op u8@1 | flags u16@2 | a i32@4 | b i32@8 | argc i32@12 |
 *   lit qf_value@16（type i32@0，union@8）| name qf_str@40（ptr@0, len@8）
 * qf_ast（ABI 2）：nodes@0, node_count@8, select@16, select_count@24,
 *   select_names@32, from_table@40, where_root@56, group_by@64, group_count@72,
 *   order_by@80, order_count@88, limit@96。
 * qf_diag：code@0, line@4, col@8, message@16, hint@32。
 * 只有 C 字符串与匿名 union 用裸偏移写入，其余字段走 cinterop 访问器；
 * selftest.c 对这些 offsetof 有静态断言。
 */
private const val NODE_LIT_OFFSET = 16
private const val NODE_NAME_OFFSET = 40
private const val AST_FROM_TABLE_OFFSET = 40
private const val DIAG_MESSAGE_OFFSET = 16
private const val DIAG_HINT_OFFSET = 32

@OptIn(ExperimentalForeignApi::class)
internal class HostRef(val host: CPointer<qf_host>) {
    private val h = host.pointed

    fun alloc(size: Long, align: Long): COpaquePointer? {
        val fn = h.alloc ?: return null
        return fn(h.ctx, size.convert(), align.convert())
    }

    fun str(value: String): CValue<qf_str> {
        // 缺 str 时返回空串而不是抛异常：异常穿过 C ABI 会变成进程终止。
        // 入口已由 shim.c 的 qf_host_gate 拦截，这里只是最后一道防线。
        val fn = h.str ?: return cValue { ptr = null; len = 0u }
        val bytes = value.encodeToByteArray()
        return memScoped {
            val buf = allocArray<ByteVar>(bytes.size + 1)
            for (i in bytes.indices) buf[i] = bytes[i]
            buf[bytes.size] = 0
            fn(h.ctx, buf, bytes.size.toUInt())
        }
    }
}

private fun CPointer<ByteVar>.offset(by: Int): CPointer<ByteVar> = (this + by)!!

private fun writeStrRaw(at: CPointer<ByteVar>, strPtr: CPointer<ByteVar>?, len: UInt) {
    at.reinterpret<CPointerVar<ByteVar>>()[0] = strPtr
    at.offset(8).reinterpret<UIntVar>()[0] = len
}

private fun writeStr(at: CPointer<ByteVar>, value: CValue<qf_str>) {
    value.useContents { writeStrRaw(at, ptr, len) }
}

internal fun setDiag(
    host: HostRef?,
    diag: CPointer<qf_diag>?,
    code: Int,
    line: Int,
    col: Int,
    message: String,
    hint: String? = null,
) {
    if (diag == null) return
    val d = diag.pointed
    d.code = code
    d.line = line.toUInt()
    d.col = col.toUInt()
    val base: CPointer<ByteVar> = diag.reinterpret()
    if (host == null) {
        writeStrRaw(base.offset(DIAG_MESSAGE_OFFSET), null, 0u)
        writeStrRaw(base.offset(DIAG_HINT_OFFSET), null, 0u)
        return
    }
    writeStr(base.offset(DIAG_MESSAGE_OFFSET), host.str(message))
    writeStr(base.offset(DIAG_HINT_OFFSET), host.str(hint ?: ""))
}

/* ---------- 表达式树扁平化 ---------- */

private class FlatNode(
    val kind: Int,
    var op: Int = 0,
    var flags: Int = 0,
    var a: Int = -1,
    var b: Int = -1,
    var argc: Int = 0,
    var litType: Int = Codes.T_NULL,
    var litInt: Long = 0L,
    var litDouble: Double = 0.0,
    var litText: String? = null,
    var name: String? = null,
)

private fun opCode(op: DslOp): Int = when (op) {
    DslOp.EQ -> Codes.OP_EQ
    DslOp.NE -> Codes.OP_NE
    DslOp.LT -> Codes.OP_LT
    DslOp.LE -> Codes.OP_LE
    DslOp.GT -> Codes.OP_GT
    DslOp.GE -> Codes.OP_GE
    DslOp.AND -> Codes.OP_AND
    DslOp.OR -> Codes.OP_OR
    DslOp.NOT -> Codes.OP_NOT
    DslOp.ADD -> Codes.OP_ADD
    DslOp.SUB -> Codes.OP_SUB
    DslOp.MUL -> Codes.OP_MUL
    DslOp.DIV -> Codes.OP_DIV
    DslOp.MOD -> Codes.OP_MOD
    DslOp.NEG -> Codes.OP_NEG
}

private fun aggCode(agg: DslAgg): Int = when (agg) {
    DslAgg.COUNT -> Codes.AGG_COUNT
    DslAgg.SUM -> Codes.AGG_SUM
    DslAgg.AVG -> Codes.AGG_AVG
    DslAgg.MIN -> Codes.AGG_MIN
    DslAgg.MAX -> Codes.AGG_MAX
}

private fun aggName(agg: DslAgg): String = when (agg) {
    DslAgg.COUNT -> "count"
    DslAgg.SUM -> "sum"
    DslAgg.AVG -> "avg"
    DslAgg.MIN -> "min"
    DslAgg.MAX -> "max"
}

/** 后序扁平化：子节点先入表，父节点拿到的下标总是合法的。 */
private class Flattener {
    val nodes = ArrayList<FlatNode>()

    fun push(n: FlatNode): Int {
        nodes += n
        return nodes.size - 1
    }

    fun pushColumn(name: String, flags: Int = 0): Int =
        push(FlatNode(Codes.N_COL, flags = flags, name = name))

    fun expr(e: KExpr): Int = when (e) {
        is KLitInt -> push(FlatNode(Codes.N_LIT, litType = Codes.T_INT, litInt = e.value))
        is KLitText -> push(FlatNode(Codes.N_LIT, litType = Codes.T_TEXT, litText = e.value))
        is KCol -> pushColumn(e.column)
        is KStar -> push(FlatNode(Codes.N_STAR))
        is KUnary -> {
            val child = expr(e.operand)
            push(FlatNode(Codes.N_UNARY, op = opCode(e.op), a = child, b = child, argc = 1))
        }
        is KBin -> {
            val left = expr(e.left)
            val right = expr(e.right)
            push(FlatNode(Codes.N_BIN, op = opCode(e.op), a = left, b = right, argc = 2))
        }
        is KAgg -> {
            /* count(*) 用 argc=0 表达：引擎对 argc=0 的计数按整行计。 */
            val arg = e.arg?.let { expr(it) } ?: -1
            push(
                FlatNode(
                    Codes.N_AGG,
                    op = aggCode(e.agg),
                    a = arg,
                    b = arg,
                    argc = if (arg >= 0) 1 else 0,
                    name = aggName(e.agg),
                )
            )
        }
    }
}

private fun writeNode(nodes: CPointer<qf_node>, idx: Int, f: FlatNode, host: HostRef) {
    val node = nodes[idx]
    node.kind = f.kind.toUByte()
    node.op = f.op.toUByte()
    node.flags = f.flags.toUShort()
    node.a = f.a
    node.b = f.b
    node.argc = f.argc

    val base: CPointer<ByteVar> = interpretCPointer(node.rawPtr)!!
    if (f.name != null) writeStr(base.offset(NODE_NAME_OFFSET), host.str(f.name!!))
    if (f.litType != Codes.T_NULL) {
        val lit = base.offset(NODE_LIT_OFFSET)
        lit.reinterpret<IntVar>()[0] = f.litType
        val union = lit.offset(8)
        when (f.litType) {
            Codes.T_INT -> union.reinterpret<LongVar>()[0] = f.litInt
            Codes.T_DOUBLE -> union.reinterpret<DoubleVar>()[0] = f.litDouble
            Codes.T_TEXT -> writeStr(union, host.str(f.litText ?: ""))
        }
    }
}

private fun writeIndices(host: HostRef, values: List<Int>): CPointer<IntVar>? {
    if (values.isEmpty()) return null
    val mem = host.alloc(values.size.toLong() * 4L, 4L)
        ?: throw IllegalStateException("下标数组分配失败")
    val arr = mem.reinterpret<IntVar>()
    for (i in values.indices) arr[i] = values[i]
    return arr
}

/* ABI 2：输出别名单独放在 select_names，node.name 只保留列名 / 函数名。 */
private fun writeSelectNames(host: HostRef, aliases: List<String>): CPointer<qf_str>? {
    if (aliases.isEmpty()) return null
    val stride = sizeOf<qf_str>().toInt()
    val mem = host.alloc(aliases.size.toLong() * stride.toLong(), 8L)
        ?: throw IllegalStateException("select_names 分配失败")
    val base: CPointer<ByteVar> = mem.reinterpret()
    for (i in aliases.indices) {
        writeStr(base.offset(i * stride), host.str(aliases[i]))
    }
    return mem.reinterpret<qf_str>()
}

private fun materialize(qb: QueryBuilder, host: HostRef): CPointer<qf_ast> {
    val table = qb.table ?: throw DslParseError("缺少 from(表名)", 1, 1)
    if (qb.selections.isEmpty()) throw DslParseError("缺少 select(...)", 1, 1)

    val flat = Flattener()
    val selectIdx = ArrayList<Int>()
    val selectNames = ArrayList<String>()
    for (sel in qb.selections) {
        selectIdx += flat.expr(sel.expr)
        selectNames += sel.alias ?: ""
    }

    val whereIdx = qb.predicate?.let { flat.expr(it) } ?: -1
    val groupIdx = qb.groups.map { flat.pushColumn(it) }
    val orderIdx = qb.orders.map {
        flat.pushColumn(it.column, if (it.descending) Codes.F_DESC else 0)
    }

    val nodeCount = flat.nodes.size
    val nodesMem = host.alloc(nodeCount.toLong() * sizeOf<qf_node>(), 8L)
        ?: throw IllegalStateException("qf_node 数组分配失败")
    val nodes = nodesMem.reinterpret<qf_node>()
    for (i in 0 until nodeCount) writeNode(nodes, i, flat.nodes[i], host)

    val astMem = host.alloc(sizeOf<qf_ast>(), 8L)
        ?: throw IllegalStateException("qf_ast 分配失败")
    val astPtr = astMem.reinterpret<qf_ast>()
    val ast = astPtr.pointed
    ast.nodes = nodes
    ast.node_count = nodeCount.toUInt()
    ast.select = writeIndices(host, selectIdx)
    ast.select_count = selectIdx.size.toUInt()
    ast.select_names = writeSelectNames(host, selectNames)
    writeStr(astMem.reinterpret<ByteVar>().offset(AST_FROM_TABLE_OFFSET), host.str(table))
    ast.where_root = whereIdx
    ast.group_by = writeIndices(host, groupIdx)
    ast.group_count = groupIdx.size.toUInt()
    ast.order_by = writeIndices(host, orderIdx)
    ast.order_count = orderIdx.size.toUInt()
    ast.limit = qb.limitCount
    return astPtr
}

/* ---------- 导出：DSL 文本 → qf_ast ---------- */

/*
 * qf.h 的 qf_dsl_build 收 qf_str（按值）。Kotlin/Native 导出函数不支持结构体按值参数
 * （CValue 会按指针传，C 侧按值传，运行时直接崩），所以导出指针版本，由 shim.c 里的
 * 同名 C 函数做一次按值拆包。qf_str 的布局由 contracts/qf.h 保证。
 */
@CName("qf_dsl_build_impl")
fun qfDslBuildImpl(
    host: CPointer<qf_host>?,
    src: CPointer<ByteVar>?,
    srcLen: UInt,
    out: CPointer<CPointerVar<qf_ast>>?,
    diag: CPointer<qf_diag>?,
): Int {
    if (host == null || out == null) return Codes.ERR_ARG
    out.pointed.value = null
    val href = HostRef(host)
    return try {
        val text = if (src == null) "" else src.readBytes(srcLen.toInt()).decodeToString()
        val ast = materialize(parseDsl(text), href)
        out.pointed.value = ast
        Codes.ERR_OK
    } catch (e: DslParseError) {
        setDiag(href, diag, Codes.ERR_PARSE, e.line, e.col, e.message ?: "DSL 语法错误")
        Codes.ERR_PARSE
    } catch (e: IllegalArgumentException) {
        setDiag(href, diag, Codes.ERR_PARSE, 0, 0, e.message ?: "DSL 参数非法")
        Codes.ERR_PARSE
    } catch (e: Throwable) {
        setDiag(href, diag, Codes.ERR_PANIC, 0, 0, "Kotlin DSL 阶段异常: ${e.message}")
        Codes.ERR_PANIC
    }
}
