/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

#ifndef QF_KEYWORDS_H
#define QF_KEYWORDS_H

#define QF_KEYWORDS(X) X("select") X("from") X("where") X("group") X("by") X("order") X("having") X("limit") X("as") X("desc") X("asc") X("and") X("or") X("not")

static const char* const QF_OP_NAME[] = {
    "?",
    "=",
    "<>",
    "<",
    "<=",
    ">",
    ">=",
    "and",
    "or",
    "not",
    "+",
    "-",
    "*",
    "/",
    "%",
    "-"
};

static const char* const QF_AGG_NAME[] = {
    "?", "count", "sum", "avg", "min", "max"
};

#endif /* QF_KEYWORDS_H */
