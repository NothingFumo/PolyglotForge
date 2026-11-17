/*
 * 文本 DSL → builder 调用。
 *
 * 语法（示例）：
 *   from(users) {
 *     select("name", count("*") as "n")
 *     where { col("age") gt 30 }
 *     groupBy("name")
 *     orderBy("n" desc)
 *     limit(3)
 *   }
 *
 * 解析器不构造任何 AST，只按顺序调用 QueryBuilder 的成员函数；
 * 这样 DSL 的语义只有一份，文本只是它的一个前端。
 */
package qf.stage

private enum class TokKind { IDENT, STRING, NUMBER, LPAREN, RPAREN, LBRACE, RBRACE, COMMA, SEMI, STAR, MINUS, EOF }

private val COMPARISONS = setOf("eq", "ne", "lt", "le", "gt", "ge")

private class Tok(val kind: TokKind, val text: String, val line: Int, val col: Int)

private fun tokenize(text: String): List<Tok> {
    val out = ArrayList<Tok>()
    var i = 0
    var line = 1
    var col = 1
    fun advance(n: Int, l: Int) {
        i += n
        if (l > 0) {
            line += l
            col = 1
        } else {
            col += n
        }
    }
    while (i < text.length) {
        val c = text[i]
        when {
            c == '\n' -> advance(1, 1)
            c == ' ' || c == '\t' || c == '\r' -> advance(1, 0)
            c == '(' -> { out += Tok(TokKind.LPAREN, "(", line, col); advance(1, 0) }
            c == ')' -> { out += Tok(TokKind.RPAREN, ")", line, col); advance(1, 0) }
            c == '{' -> { out += Tok(TokKind.LBRACE, "{", line, col); advance(1, 0) }
            c == '}' -> { out += Tok(TokKind.RBRACE, "}", line, col); advance(1, 0) }
            c == ',' -> { out += Tok(TokKind.COMMA, ",", line, col); advance(1, 0) }
            c == ';' -> { out += Tok(TokKind.SEMI, ";", line, col); advance(1, 0) }
            c == '*' -> { out += Tok(TokKind.STAR, "*", line, col); advance(1, 0) }
            c == '-' -> { out += Tok(TokKind.MINUS, "-", line, col); advance(1, 0) }
            c == '"' -> {
                val startLine = line
                val startCol = col
                val sb = StringBuilder()
                advance(1, 0)
                var closed = false
                while (i < text.length) {
                    val d = text[i]
                    when {
                        d == '"' -> { advance(1, 0); closed = true }
                        d == '\\' -> {
                            if (i + 1 >= text.length) break
                            val esc = text[i + 1]
                            sb.append(
                                when (esc) {
                                    'n' -> '\n'
                                    't' -> '\t'
                                    'r' -> '\r'
                                    '"' -> '"'
                                    '\\' -> '\\'
                                    else -> esc
                                }
                            )
                            advance(2, 0)
                        }
                        d == '\n' -> break
                        else -> { sb.append(d); advance(1, 0) }
                    }
                    if (closed) break
                }
                if (!closed) throw DslParseError("字符串没有闭合的引号", startLine, startCol)
                out += Tok(TokKind.STRING, sb.toString(), startLine, startCol)
            }
            c.isDigit() -> {
                val start = i
                while (i < text.length && text[i].isDigit()) advance(1, 0)
                out += Tok(TokKind.NUMBER, text.substring(start, i), line, col - (i - start))
            }
            c.isLetter() || c == '_' -> {
                val start = i
                while (i < text.length && (text[i].isLetterOrDigit() || text[i] == '_')) advance(1, 0)
                out += Tok(TokKind.IDENT, text.substring(start, i), line, col - (i - start))
            }
            else -> throw DslParseError("无法识别的字符 '$c'", line, col)
        }
    }
    out += Tok(TokKind.EOF, "", line, col)
    return out
}

private class Parser(private val toks: List<Tok>) {
    private var pos = 0
    private val predicates = PredicateBuilder()

    private fun cur(): Tok = toks[pos]

    private fun at(kind: TokKind): Boolean = cur().kind == kind

    private fun atIdent(name: String): Boolean = cur().kind == TokKind.IDENT && cur().text == name

    private fun fail(message: String): Nothing {
        val t = cur()
        val got = if (t.kind == TokKind.EOF) "输入结束" else "'${t.text}'"
        throw DslParseError("$message，实际遇到 $got", t.line, t.col)
    }

    private fun take(kind: TokKind, what: String): Tok {
        if (!at(kind)) fail("期望 $what")
        return toks[pos++]
    }

    private fun takeIdent(name: String) {
        if (!atIdent(name)) fail("期望 '$name'")
        pos++
    }

    private fun name(): String {
        val t = cur()
        return when (t.kind) {
            TokKind.STRING -> { pos++; t.text }
            TokKind.IDENT -> { pos++; t.text }
            else -> fail("期望表名或列名")
        }
    }

    private fun number(): Long {
        val negative = at(TokKind.MINUS)
        if (negative) pos++
        val t = take(TokKind.NUMBER, "整数")
        val v = t.text.toLongOrNull() ?: throw DslParseError("整数超出范围 '${t.text}'", t.line, t.col)
        return if (negative) -v else v
    }

    fun parseQuery(): QueryBuilder {
        takeIdent("from")
        take(TokKind.LPAREN, "'('")
        val qb = QueryBuilder()
        qb.from(name())
        take(TokKind.RPAREN, "')'")
        take(TokKind.LBRACE, "'{'")

        while (!at(TokKind.RBRACE)) {
            if (at(TokKind.EOF)) fail("from 块没有闭合")
            if (at(TokKind.SEMI)) { pos++; continue }
            val stmt = take(TokKind.IDENT, "语句名").text
            when (stmt) {
                "select" -> parseSelect(qb)
                "where" -> parseWhere(qb)
                "groupBy" -> parseGroupBy(qb)
                "orderBy" -> parseOrderBy(qb)
                "limit" -> parseLimit(qb)
                else -> fail("未知语句 '$stmt'（可用：select/where/groupBy/orderBy/limit）")
            }
        }
        take(TokKind.RBRACE, "'}'")
        while (at(TokKind.SEMI)) pos++
        if (!at(TokKind.EOF)) fail("from 块结束后还有多余内容")
        return qb
    }

    private fun parseSelect(qb: QueryBuilder) {
        take(TokKind.LPAREN, "'('")
        val items = ArrayList<Any>()
        while (true) {
            val expr = parseSelectExpr()
            var alias: String? = null
            if (atIdent("as")) {
                pos++
                alias = take(TokKind.STRING, "别名字符串").text
            }
            items += if (alias == null) expr else Aliased(expr, alias)
            if (!at(TokKind.COMMA)) break
            pos++
        }
        take(TokKind.RPAREN, "')'")
        qb.select(*items.toTypedArray())
    }

    private fun parseSelectExpr(): KExpr {
        if (at(TokKind.STRING)) return KCol(name())
        if (at(TokKind.MINUS) || at(TokKind.NUMBER)) return KLitInt(number())
        if (at(TokKind.STAR)) fail("select(*) 不支持；请写 count(\"*\")")
        val fn = take(TokKind.IDENT, "列名或函数名").text
        if (!at(TokKind.LPAREN)) return KCol(fn)
        pos++
        val agg = when (fn) {
            "count" -> DslAgg.COUNT
            "sum" -> DslAgg.SUM
            "avg" -> DslAgg.AVG
            "min" -> DslAgg.MIN
            "max" -> DslAgg.MAX
            else -> fail("未知函数 '$fn'")
        }
        val arg: KExpr?
        if (at(TokKind.STAR)) {
            pos++
            arg = null
        } else {
            arg = KCol(name())
        }
        take(TokKind.RPAREN, "')'")
        return KAgg(agg, arg)
    }

    private fun parseWhere(qb: QueryBuilder) {
        take(TokKind.LBRACE, "'{'")
        val expr = parseOr()
        take(TokKind.RBRACE, "'}'")
        qb.where(expr)
    }

    private fun parseOr(): KExpr {
        var left = parseAnd()
        while (atIdent("or")) {
            pos++
            val right = parseAnd()
            left = predicates.run { left or right }
        }
        return left
    }

    private fun parseAnd(): KExpr {
        var left = parseAtom()
        while (atIdent("and")) {
            pos++
            val right = parseAtom()
            left = predicates.run { left and right }
        }
        return left
    }

    private fun parseAtom(): KExpr {
        if (atIdent("not")) {
            pos++
            return predicates.not(parseAtom())
        }
        if (at(TokKind.LPAREN)) {
            pos++
            val e = parseOr()
            take(TokKind.RPAREN, "')'")
            return e
        }
        takeIdent("col")
        take(TokKind.LPAREN, "'('")
        val column = name()
        take(TokKind.RPAREN, "')'")
        val col = predicates.col(column)

        val opTok = cur()
        if (opTok.kind != TokKind.IDENT) fail("谓词缺少比较运算（eq/ne/lt/le/gt/ge）")
        pos++
        if (opTok.text !in COMPARISONS) {
            throw DslParseError("未知比较运算 '${opTok.text}'", opTok.line, opTok.col)
        }
        return compare(opTok.text, col, predicateLiteral())
    }

    private fun predicateLiteral(): Any =
        when {
            at(TokKind.STRING) -> name()
            at(TokKind.MINUS) || at(TokKind.NUMBER) -> number()
            else -> fail("期望比较值（字符串或整数）")
        }

    /** 比较值来自文本，类型要到运行时才知道；两种重载在这里汇合。 */
    private fun compare(op: String, col: KExpr, value: Any): KExpr {
        if (value is Long) {
            return predicates.run {
                when (op) {
                    "eq" -> col eq value
                    "ne" -> col ne value
                    "lt" -> col lt value
                    "le" -> col le value
                    "gt" -> col gt value
                    "ge" -> col ge value
                    else -> fail("未知比较运算 '$op'")
                }
            }
        }
        if (value is String) {
            return predicates.run {
                when (op) {
                    "eq" -> col eq value
                    "ne" -> col ne value
                    "lt" -> col lt value
                    "le" -> col le value
                    "gt" -> col gt value
                    "ge" -> col ge value
                    else -> fail("未知比较运算 '$op'")
                }
            }
        }
        return fail("比较值类型不支持")
    }

    private fun parseGroupBy(qb: QueryBuilder) {
        take(TokKind.LPAREN, "'('")
        val columns = ArrayList<String>()
        while (true) {
            columns += name()
            if (!at(TokKind.COMMA)) break
            pos++
        }
        take(TokKind.RPAREN, "')'")
        qb.groupBy(*columns.toTypedArray())
    }

    private fun parseOrderBy(qb: QueryBuilder) {
        take(TokKind.LPAREN, "'('")
        val keys = ArrayList<KOrder>()
        while (true) {
            val column = name()
            var desc = false
            if (atIdent("desc")) { pos++; desc = true }
            else if (atIdent("asc")) { pos++ }
            keys += KOrder(column, desc)
            if (!at(TokKind.COMMA)) break
            pos++
        }
        take(TokKind.RPAREN, "')'")
        qb.orderBy(*keys.toTypedArray())
    }

    private fun parseLimit(qb: QueryBuilder) {
        take(TokKind.LPAREN, "'('")
        val n = number()
        take(TokKind.RPAREN, "')'")
        if (n < 0) fail("limit 不能为负")
        qb.limit(n)
    }
}

/** 解析一段 DSL 文本，返回已填好的 builder。语法错误抛 DslParseError。 */
internal fun parseDsl(text: String): QueryBuilder {
    if (text.isBlank()) throw DslParseError("DSL 文本为空", 1, 1)
    return Parser(tokenize(text)).parseQuery()
}
