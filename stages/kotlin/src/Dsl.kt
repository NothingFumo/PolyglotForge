/*
 * 纯 Kotlin DSL 层。这里只描述查询形状，完全不认识 C 结构：
 * 文本解析器把源码翻成一串 builder 调用，builder 产出一棵与语言无关的表达式树，
 * 最后由 Abi.kt 一次性落进 qf_ast。
 *
 * 用法（真 Kotlin DSL，不是字符串）：
 *   from("users") {
 *       select("name", count("*") as "n")
 *       where { col("age") gt 30L }
 *       groupBy("name")
 *       orderBy(desc("n"))
 *       limit(3)
 *   }
 */
package qf.stage

/** 与 qf_op 对应的语义算子；具体整数码在 Abi.kt 里映射。 */
enum class DslOp { EQ, NE, LT, LE, GT, GE, AND, OR, NOT, ADD, SUB, MUL, DIV, MOD, NEG }

/** 与 qf_agg 对应的聚合函数。 */
enum class DslAgg { COUNT, SUM, AVG, MIN, MAX }

/** builder 内部的表达式树。 */
sealed class KExpr

class KLitInt(val value: Long) : KExpr()

class KLitText(val value: String) : KExpr()

class KCol(val column: String) : KExpr()

class KBin(val op: DslOp, val left: KExpr, val right: KExpr) : KExpr()

class KUnary(val op: DslOp, val operand: KExpr) : KExpr()

object KStar : KExpr()

/** arg 为 null 表示 count(*)：不依赖任何列。 */
class KAgg(val agg: DslAgg, val arg: KExpr?) : KExpr()

/** `expr as "alias"` 的结果，只有 select 接受它。 */
class Aliased(val expr: KExpr, val alias: String)

/** orderBy 的一个键。有序方向是键的一部分，而不是表达式属性。 */
class KOrder(val column: String, val descending: Boolean)

/** 文本解析器的错误；只在 Kotlin 内部流转，ABI 边界会转成 qf_diag。 */
class DslParseError(message: String, val line: Int, val col: Int) : Exception(message)

/** 带接收者 lambda 的查询 builder：DSL 与解析器之间的唯一接口。 */
class QueryBuilder {
    var table: String? = null
        private set

    val selections: List<KSelection>
        get() = _selections

    val predicate: KExpr?
        get() = _predicate

    val groups: List<String>
        get() = _groups

    val orders: List<KOrder>
        get() = _orders

    var limitCount: Long = -1L
        private set

    private val _selections = ArrayList<KSelection>()
    private val _groups = ArrayList<String>()
    private val _orders = ArrayList<KOrder>()
    private var _predicate: KExpr? = null

    fun from(name: String) {
        require(name.isNotEmpty()) { "表名不能为空" }
        table = name
    }

    /** 接受列名字符串、表达式以及 `expr as "别名"`。 */
    fun select(vararg items: Any?) {
        require(items.isNotEmpty()) { "select 至少需要一个输出列" }
        for (item in items) {
            _selections += when (item) {
                is String -> KSelection(KCol(item), null)
                is Aliased -> KSelection(item.expr, item.alias)
                is KExpr -> KSelection(item, null)
                else -> throw IllegalArgumentException("select 只接受列名字符串或表达式")
            }
        }
    }

    fun where(expr: KExpr) {
        _predicate = expr
    }

    fun where(block: PredicateBuilder.() -> KExpr) {
        _predicate = PredicateBuilder().block()
    }

    fun groupBy(vararg columns: String) {
        require(columns.isNotEmpty()) { "groupBy 至少需要一列" }
        _groups += columns
    }

    fun orderBy(vararg keys: KOrder) {
        require(keys.isNotEmpty()) { "orderBy 至少需要一个键" }
        _orders += keys
    }

    fun limit(n: Long) {
        require(n >= 0L) { "limit 不能为负" }
        limitCount = n
    }
}

class KSelection(val expr: KExpr, val alias: String?)

/** where { ... } 的接收者：谓词由 KExpr 组合而成。 */
class PredicateBuilder {
    fun col(column: String): KExpr = KCol(column)

    fun lit(value: Long): KExpr = KLitInt(value)

    fun lit(value: String): KExpr = KLitText(value)

    infix fun KExpr.eq(other: Long): KExpr = KBin(DslOp.EQ, this, KLitInt(other))
    infix fun KExpr.eq(other: String): KExpr = KBin(DslOp.EQ, this, KLitText(other))
    infix fun KExpr.ne(other: Long): KExpr = KBin(DslOp.NE, this, KLitInt(other))
    infix fun KExpr.ne(other: String): KExpr = KBin(DslOp.NE, this, KLitText(other))
    infix fun KExpr.lt(other: Long): KExpr = KBin(DslOp.LT, this, KLitInt(other))
    infix fun KExpr.lt(other: String): KExpr = KBin(DslOp.LT, this, KLitText(other))
    infix fun KExpr.le(other: Long): KExpr = KBin(DslOp.LE, this, KLitInt(other))
    infix fun KExpr.le(other: String): KExpr = KBin(DslOp.LE, this, KLitText(other))
    infix fun KExpr.gt(other: Long): KExpr = KBin(DslOp.GT, this, KLitInt(other))
    infix fun KExpr.gt(other: String): KExpr = KBin(DslOp.GT, this, KLitText(other))
    infix fun KExpr.ge(other: Long): KExpr = KBin(DslOp.GE, this, KLitInt(other))
    infix fun KExpr.ge(other: String): KExpr = KBin(DslOp.GE, this, KLitText(other))
    infix fun KExpr.and(other: KExpr): KExpr = KBin(DslOp.AND, this, other)
    infix fun KExpr.or(other: KExpr): KExpr = KBin(DslOp.OR, this, other)

    fun not(operand: KExpr): KExpr = KUnary(DslOp.NOT, operand)
}

/** DSL 入口：`from("users") { ... }`。 */
fun from(table: String, block: QueryBuilder.() -> Unit): QueryBuilder =
    QueryBuilder().apply {
        from(table)
        block()
    }

fun query(block: QueryBuilder.() -> Unit): QueryBuilder = QueryBuilder().apply(block)

fun count(column: String = "*"): KExpr =
    if (column == "*") KAgg(DslAgg.COUNT, null) else KAgg(DslAgg.COUNT, KCol(column))

fun sum(column: String): KExpr = KAgg(DslAgg.SUM, KCol(column))

fun avg(column: String): KExpr = KAgg(DslAgg.AVG, KCol(column))

fun min(column: String): KExpr = KAgg(DslAgg.MIN, KCol(column))

fun max(column: String): KExpr = KAgg(DslAgg.MAX, KCol(column))

fun desc(column: String): KOrder = KOrder(column, true)

fun asc(column: String): KOrder = KOrder(column, false)

/** `count("*") as "n"`。`as` 是 Kotlin 关键字，用反引号声明成普通 infix 函数。 */
infix fun KExpr.`as`(alias: String): Aliased = Aliased(this, alias)
