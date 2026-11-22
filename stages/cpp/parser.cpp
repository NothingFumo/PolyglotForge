/*
 * C++ 阶段：SQL 子集的词法与语法分析（手写词法器 + 递归下降）。
 *
 * 支持的文法：
 *   query   := SELECT item (',' item)* FROM table
 *              [WHERE expr]
 *              [GROUP BY expr (',' expr)*]
 *              [ORDER BY expr [ASC|DESC] (',' expr [ASC|DESC])*]
 *              [LIMIT integer]
 *   item    := expr [AS ident]
 *   expr    优先级（低 → 高）：OR < AND < NOT < 比较 < 加减 < 乘除模 < 一元负号 < 原子
 *
 * AST 全部落在宿主 arena 上：解析期间节点暂存在 std::vector，只有整条查询成功
 * 后才一次性拷进 host->alloc 提供的内存；失败时 *out 保持 NULL，不会留下半成品计划。
 *
 * 节点字段语义以 contracts/qf.h 与 stages/c/engine.c 的 eval() 为准：
 *   LIT    lit 为字面量
 *   COL    name 为列名，a 待计划阶段回填列下标
 *   BIN    a = 左子节点，b = 右子节点
 *   UNARY  a = 操作数
 *   CALL   b = 参数起始下标，argc = 参数个数，name = 函数名
 *   AGG    op = 聚合种类，b = 参数下标，argc = 0 表示 count(*)
 *   STAR   SELECT * 的投影项
 */
#include "qf.h"
#include "qf_keywords.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

/* 关键字 token 必须与 gen/qf_keywords.h 的 QF_KEYWORDS 顺序一一对应。 */
enum Tok : uint8_t {
    TokEnd = 0,
    TokBad,
    TokIdent,
    TokInt,
    TokDouble,
    TokString,
    TokComma,
    TokLParen,
    TokRParen,
    TokStar,
    TokPlus,
    TokMinus,
    TokSlash,
    TokPercent,
    TokEq,
    TokNe,
    TokLt,
    TokLe,
    TokGt,
    TokGe,

    TokKwFirst,
    TokSelect = TokKwFirst,
    TokFrom,
    TokWhere,
    TokGroup,
    TokBy,
    TokOrder,
    TokHaving,
    TokLimit,
    TokAs,
    TokDesc,
    TokAsc,
    TokAnd,
    TokOr,
    TokNot
};

const char* const kKeywordText[] = {
#define QF_KW_TEXT(name) name,
    QF_KEYWORDS(QF_KW_TEXT)
#undef QF_KW_TEXT
};

constexpr size_t kKeywordCount = sizeof(kKeywordText) / sizeof(kKeywordText[0]);
static_assert(kKeywordCount == (size_t)(TokNot - TokKwFirst + 1),
              "gen/qf_keywords.h 的关键字顺序必须与 Tok 枚举一致");

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_ident_cont(char c) { return is_ident_start(c) || is_digit(c); }

inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* 大小写不敏感比较：a 有显式长度（文本可能不以 NUL 结尾），b 以 NUL 结尾。 */
bool ci_equal(const char* a, uint32_t len, const char* b) {
    uint32_t i = 0;
    for (; i < len && b[i]; ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return i == len && b[i] == '\0';
}

int agg_code_for(const std::string& text) {
    for (int code = QF_AGG_COUNT; code <= QF_AGG_MAX; ++code) {
        if (ci_equal(text.data(), (uint32_t)text.size(), QF_AGG_NAME[code])) return code;
    }
    return 0;
}

struct Token {
    Tok kind = TokEnd;
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t pos = 0;
    uint32_t len = 0;
    int64_t ival = 0;
    double dval = 0.0;
    std::string text; /* 标识符原文、字符串解码结果 */
};

qf_node blank_node() {
    qf_node n;
    std::memset(&n, 0, sizeof n);
    return n;
}

qf_value int_value(int64_t v) {
    qf_value out;
    std::memset(&out, 0, sizeof out);
    out.type = QF_T_INT;
    out.as.i = v;
    return out;
}

qf_value double_value(double v) {
    qf_value out;
    std::memset(&out, 0, sizeof out);
    out.type = QF_T_DOUBLE;
    out.as.d = v;
    return out;
}

qf_value text_value(qf_str s) {
    qf_value out;
    std::memset(&out, 0, sizeof out);
    out.type = QF_T_TEXT;
    out.as.s = s;
    return out;
}

class Parser {
public:
    Parser(const qf_host* host, const char* base, uint32_t len)
        : host_(host), base_(base), len_(len) {}

    qf_err run(qf_ast** out, qf_diag* diag);

private:
    /* ---------- 词法 ---------- */

    void advance() { cur_ = lex(); }

    Token lex();
    void skip_space();
    Token lex_number(uint32_t start_line, uint32_t start_col, uint32_t start_pos);
    Token lex_string(uint32_t start_line, uint32_t start_col, uint32_t start_pos);
    Token lex_ident(uint32_t start_line, uint32_t start_col, uint32_t start_pos);
    Token bad_token(uint32_t line, uint32_t col);

    /* ---------- 诊断 ---------- */

    bool ok() const { return !failed_; }

    void set_error(uint32_t line, uint32_t col, const std::string& message,
                   const std::string& expected, qf_err code = QF_ERR_PARSE) {
        if (failed_) return;
        failed_ = true;
        err_code_ = code;
        err_line_ = line ? line : 1;
        err_col_ = col ? col : 1;
        err_message_ = message;
        err_hint_ = expected.empty() ? std::string() : ("期望 " + expected);
    }

    std::string describe(const Token& t) const;
    void publish(qf_diag* diag) const;

    /* ---------- 内存与节点 ---------- */

    void* alloc_bytes(uint64_t size, uint64_t align);
    bool intern(const std::string& text, qf_str* out);
    bool intern_or_fail(const std::string& text, qf_str* out, uint32_t line, uint32_t col);

    int32_t add(const qf_node& n);
    int32_t add_bin(uint8_t op, int32_t a, int32_t b);
    int32_t add_unary(uint8_t op, int32_t operand);

    /* ---------- 语法 ---------- */

    bool expect(Tok kind, const char* expected, const char* what);
    void parse_select_list();
    int32_t parse_select_item();
    void parse_table();
    void parse_clauses();
    void parse_group_list();
    void parse_order_list();
    int32_t parse_expr() { return parse_or(); }
    int32_t parse_or();
    int32_t parse_and();
    int32_t parse_not();
    int32_t parse_cmp();
    int32_t parse_add();
    int32_t parse_mul();
    int32_t parse_unary();
    int32_t parse_atom();
    int32_t parse_ident_atom();
    int32_t parse_call(const std::string& name);
    int32_t parse_agg(int code);

    qf_ast* build_ast();

    const qf_host* host_;
    const char* base_;
    uint32_t len_;
    uint32_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t line_start_ = 0;

    Token cur_;
    bool failed_ = false;
    qf_err err_code_ = QF_ERR_PARSE;
    uint32_t err_line_ = 1;
    uint32_t err_col_ = 1;
    std::string err_message_;
    std::string err_hint_;

    std::vector<qf_node> nodes_;
    std::vector<int32_t> select_;
    std::vector<qf_str> select_alias_;
    qf_str pending_alias_ = {nullptr, 0};
    std::vector<int32_t> group_;
    std::vector<int32_t> order_;
    qf_str from_table_ = {nullptr, 0};
    int32_t where_root_ = -1;
    int64_t limit_ = -1;
};

/* ---------- 词法 ---------- */

void Parser::skip_space() {
    while (pos_ < len_) {
        const char c = base_[pos_];
        if (c == '\n') {
            ++pos_;
            ++line_;
            line_start_ = pos_;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            ++pos_;
        } else {
            break;
        }
    }
}

Token Parser::bad_token(uint32_t line, uint32_t col) {
    Token t;
    t.kind = TokBad;
    t.line = line;
    t.col = col;
    t.pos = pos_;
    return t;
}

Token Parser::lex_number(uint32_t start_line, uint32_t start_col, uint32_t start_pos) {
    Token t;
    t.line = start_line;
    t.col = start_col;
    t.pos = start_pos;

    bool is_double = false;
    while (pos_ < len_ && is_digit(base_[pos_])) ++pos_;

    if (pos_ + 1 < len_ && base_[pos_] == '.' && is_digit(base_[pos_ + 1])) {
        is_double = true;
        ++pos_;
        while (pos_ < len_ && is_digit(base_[pos_])) ++pos_;
    }

    if (pos_ < len_ && (base_[pos_] == 'e' || base_[pos_] == 'E')) {
        uint32_t probe = pos_ + 1;
        if (probe < len_ && (base_[probe] == '+' || base_[probe] == '-')) ++probe;
        if (probe < len_ && is_digit(base_[probe])) {
            is_double = true;
            pos_ = probe;
            while (pos_ < len_ && is_digit(base_[pos_])) ++pos_;
        }
    }

    t.len = pos_ - start_pos;
    t.text.assign(base_ + start_pos, t.len);

    errno = 0;
    char* end = nullptr;
    if (is_double) {
        t.dval = std::strtod(t.text.c_str(), &end);
        t.kind = TokDouble;
        if (end != t.text.c_str() + t.text.size() || errno == ERANGE) {
            set_error(start_line, start_col, "浮点字面量超出可表示范围", "合法的浮点数");
            return bad_token(start_line, start_col);
        }
    } else {
        t.ival = (int64_t)std::strtoll(t.text.c_str(), &end, 10);
        t.kind = TokInt;
        if (end != t.text.c_str() + t.text.size() || errno == ERANGE) {
            set_error(start_line, start_col, "整数字面量超出可表示范围", "合法的整数");
            return bad_token(start_line, start_col);
        }
    }
    return t;
}

Token Parser::lex_string(uint32_t start_line, uint32_t start_col, uint32_t start_pos) {
    Token t;
    t.line = start_line;
    t.col = start_col;
    t.pos = start_pos;

    ++pos_; /* 开引号 */
    for (;;) {
        if (pos_ >= len_) {
            set_error(start_line, start_col, "字符串未闭合，缺少结束引号", "结束引号 '");
            return bad_token(start_line, start_col);
        }
        const char c = base_[pos_];
        if (c == '\'') {
            if (pos_ + 1 < len_ && base_[pos_ + 1] == '\'') {
                t.text.push_back('\'');
                pos_ += 2;
                continue;
            }
            ++pos_;
            break;
        }
        if (c == '\n') {
            ++line_;
            line_start_ = pos_ + 1;
        }
        t.text.push_back(c);
        ++pos_;
    }

    t.kind = TokString;
    t.len = pos_ - start_pos;
    return t;
}

Token Parser::lex_ident(uint32_t start_line, uint32_t start_col, uint32_t start_pos) {
    while (pos_ < len_ && is_ident_cont(base_[pos_])) ++pos_;

    Token t;
    t.line = start_line;
    t.col = start_col;
    t.pos = start_pos;
    t.len = pos_ - start_pos;
    t.text.assign(base_ + start_pos, t.len);

    t.kind = TokIdent;
    for (size_t i = 0; i < kKeywordCount; ++i) {
        if (ci_equal(t.text.data(), (uint32_t)t.text.size(), kKeywordText[i])) {
            t.kind = (Tok)(TokKwFirst + i);
            break;
        }
    }
    return t;
}

Token Parser::lex() {
    skip_space();

    Token t;
    t.pos = pos_;
    t.line = line_;
    t.col = pos_ - line_start_ + 1;

    if (pos_ >= len_) {
        t.kind = TokEnd;
        return t;
    }

    const char c = base_[pos_];

    if (is_ident_start(c)) return lex_ident(t.line, t.col, t.pos);
    if (is_digit(c)) return lex_number(t.line, t.col, t.pos);
    if (c == '\'') return lex_string(t.line, t.col, t.pos);

    ++pos_;
    t.len = 1;
    switch (c) {
    case ',': t.kind = TokComma; return t;
    case '(': t.kind = TokLParen; return t;
    case ')': t.kind = TokRParen; return t;
    case '*': t.kind = TokStar; return t;
    case '+': t.kind = TokPlus; return t;
    case '-': t.kind = TokMinus; return t;
    case '/': t.kind = TokSlash; return t;
    case '%': t.kind = TokPercent; return t;
    case '=': t.kind = TokEq; return t;
    case '<':
        if (pos_ < len_ && base_[pos_] == '>') { ++pos_; t.len = 2; t.kind = TokNe; return t; }
        if (pos_ < len_ && base_[pos_] == '=') { ++pos_; t.len = 2; t.kind = TokLe; return t; }
        t.kind = TokLt;
        return t;
    case '>':
        if (pos_ < len_ && base_[pos_] == '=') { ++pos_; t.len = 2; t.kind = TokGe; return t; }
        t.kind = TokGt;
        return t;
    case '!':
        if (pos_ < len_ && base_[pos_] == '=') { ++pos_; t.len = 2; t.kind = TokNe; return t; }
        break;
    default:
        break;
    }

    const unsigned char uc = (unsigned char)c;
    std::string message;
    if (uc >= 0x20 && uc < 0x7f) {
        message = "非法字符 '";
        message.push_back((char)uc);
        message += "'";
    } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "非法字符 0x%02X", (unsigned)uc);
        message = buf;
    }
    set_error(t.line, t.col, message, "标识符、数字、字符串或运算符");
    return bad_token(t.line, t.col);
}

/* ---------- 诊断 ---------- */

std::string Parser::describe(const Token& t) const {
    switch (t.kind) {
    case TokEnd: return "输入结束";
    case TokBad: return "非法 token";
    case TokIdent: return "'" + t.text + "'";
    case TokInt: return "整数字面量 " + t.text;
    case TokDouble: return "浮点字面量 " + t.text;
    case TokString: return "字符串字面量";
    case TokComma: return "','";
    case TokLParen: return "'('";
    case TokRParen: return "')'";
    case TokStar: return "'*'";
    case TokPlus: return "'+'";
    case TokMinus: return "'-'";
    case TokSlash: return "'/'";
    case TokPercent: return "'%'";
    case TokEq: return "'='";
    case TokNe: return "'<>'";
    case TokLt: return "'<'";
    case TokLe: return "'<='";
    case TokGt: return "'>'";
    case TokGe: return "'>='";
    default: break;
    }
    if (t.kind >= TokKwFirst && t.kind <= TokNot) {
        std::string upper = kKeywordText[t.kind - TokKwFirst];
        for (char& ch : upper) {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        }
        return "'" + upper + "'";
    }
    return "未知 token";
}

void Parser::publish(qf_diag* diag) const {
    if (!diag) return;
    std::memset(diag, 0, sizeof *diag);
    if (!failed_) {
        diag->code = QF_ERR_OK;
        return;
    }
    diag->code = err_code_;
    diag->line = err_line_;
    diag->col = err_col_;
    if (!err_message_.empty()) {
        diag->message = host_->str(host_->ctx, err_message_.data(), (uint32_t)err_message_.size());
    }
    if (!err_hint_.empty()) {
        diag->hint = host_->str(host_->ctx, err_hint_.data(), (uint32_t)err_hint_.size());
    }
}

/* ---------- 内存与节点 ---------- */

void* Parser::alloc_bytes(uint64_t size, uint64_t align) {
    if (size == 0) size = 1;
    return host_->alloc(host_->ctx, size, align);
}

bool Parser::intern(const std::string& text, qf_str* out) {
    const uint32_t len = (uint32_t)text.size();
    qf_str s = host_->str(host_->ctx, text.data(), len);
    if (!s.ptr && len != 0) return false;
    *out = s;
    return true;
}

bool Parser::intern_or_fail(const std::string& text, qf_str* out, uint32_t line, uint32_t col) {
    if (intern(text, out)) return true;
    set_error(line, col, "内存不足：无法把文本写入宿主 arena", "更小的查询", QF_ERR_PANIC);
    return false;
}

int32_t Parser::add(const qf_node& n) {
    if (nodes_.size() >= (size_t)INT32_MAX) {
        set_error(cur_.line, cur_.col, "查询过于复杂：节点数超出上限", "更小的查询", QF_ERR_PANIC);
        return -1;
    }
    nodes_.push_back(n);
    return (int32_t)(nodes_.size() - 1);
}

int32_t Parser::add_bin(uint8_t op, int32_t a, int32_t b) {
    qf_node n = blank_node();
    n.kind = QF_N_BIN;
    n.op = op;
    n.a = a;
    n.b = b;
    return add(n);
}

int32_t Parser::add_unary(uint8_t op, int32_t operand) {
    qf_node n = blank_node();
    n.kind = QF_N_UNARY;
    n.op = op;
    n.a = operand;
    return add(n);
}

/* ---------- 语法 ---------- */

bool Parser::expect(Tok kind, const char* expected, const char* what) {
    if (!ok()) return false;
    if (cur_.kind != kind) {
        set_error(cur_.line, cur_.col, std::string(what) + "（实际遇到 " + describe(cur_) + "）",
                  expected);
        return false;
    }
    advance();
    return ok();
}

void Parser::parse_select_list() {
    for (;;) {
        pending_alias_ = {nullptr, 0};
        const int32_t item = parse_select_item();
        if (item < 0) return;
        select_.push_back(item);
        select_alias_.push_back(pending_alias_);
        if (cur_.kind != TokComma) return;
        advance();
        if (!ok()) return;
    }
}

int32_t Parser::parse_select_item() {
    if (!ok()) return -1;

    if (cur_.kind == TokStar) {
        qf_node n = blank_node();
        n.kind = QF_N_STAR;
        n.a = -1;
        n.b = -1;
        const int32_t idx = add(n);
        if (idx < 0) return -1;
        advance();
        return idx;
    }

    const int32_t idx = parse_expr();
    if (idx < 0) return -1;

    if (cur_.kind == TokAs) {
        advance();
        if (!ok()) return -1;
        if (cur_.kind != TokIdent) {
            set_error(cur_.line, cur_.col,
                      "AS 之后需要别名标识符（实际遇到 " + describe(cur_) + "）", "别名标识符");
            return -1;
        }
        qf_str alias;
        if (!intern_or_fail(cur_.text, &alias, cur_.line, cur_.col)) return -1;
        pending_alias_ = alias;
        advance();
    }
    return idx;
}

void Parser::parse_table() {
    if (!ok()) return;
    if (cur_.kind != TokIdent) {
        set_error(cur_.line, cur_.col,
                  "FROM 之后需要表名（实际遇到 " + describe(cur_) + "）", "表名标识符");
        return;
    }
    if (!intern_or_fail(cur_.text, &from_table_, cur_.line, cur_.col)) return;
    advance();
}

void Parser::parse_clauses() {
    if (!ok()) return;

    if (cur_.kind == TokWhere) {
        advance();
        if (!ok()) return;
        where_root_ = parse_expr();
        if (where_root_ < 0) return;
    }

    if (cur_.kind == TokGroup) {
        advance();
        if (!ok()) return;
        if (!expect(TokBy, "BY", "GROUP 之后需要 BY")) return;
        parse_group_list();
        if (!ok()) return;
    }

    if (cur_.kind == TokOrder) {
        advance();
        if (!ok()) return;
        if (!expect(TokBy, "BY", "ORDER 之后需要 BY")) return;
        parse_order_list();
        if (!ok()) return;
    }

    if (cur_.kind == TokLimit) {
        advance();
        if (!ok()) return;
        if (cur_.kind != TokInt) {
            set_error(cur_.line, cur_.col,
                      "LIMIT 之后需要非负整数（实际遇到 " + describe(cur_) + "）", "整数");
            return;
        }
        limit_ = cur_.ival;
        advance();
    }
}

void Parser::parse_group_list() {
    for (;;) {
        const int32_t item = parse_expr();
        if (item < 0) return;
        group_.push_back(item);
        if (cur_.kind != TokComma) return;
        advance();
        if (!ok()) return;
    }
}

void Parser::parse_order_list() {
    for (;;) {
        const int32_t item = parse_expr();
        if (item < 0) return;
        if (cur_.kind == TokDesc) {
            nodes_[(size_t)item].flags = (uint16_t)(nodes_[(size_t)item].flags | QF_F_DESC);
            advance();
            if (!ok()) return;
        } else if (cur_.kind == TokAsc) {
            advance();
            if (!ok()) return;
        }
        order_.push_back(item);
        if (cur_.kind != TokComma) return;
        advance();
        if (!ok()) return;
    }
}

int32_t Parser::parse_or() {
    int32_t left = parse_and();
    while (left >= 0 && ok() && cur_.kind == TokOr) {
        advance();
        if (!ok()) return -1;
        const int32_t right = parse_and();
        if (right < 0) return -1;
        left = add_bin(QF_OP_OR, left, right);
        if (left < 0) return -1;
    }
    return left;
}

int32_t Parser::parse_and() {
    int32_t left = parse_not();
    while (left >= 0 && ok() && cur_.kind == TokAnd) {
        advance();
        if (!ok()) return -1;
        const int32_t right = parse_not();
        if (right < 0) return -1;
        left = add_bin(QF_OP_AND, left, right);
        if (left < 0) return -1;
    }
    return left;
}

int32_t Parser::parse_not() {
    if (ok() && cur_.kind == TokNot) {
        advance();
        if (!ok()) return -1;
        const int32_t operand = parse_not();
        if (operand < 0) return -1;
        return add_unary(QF_OP_NOT, operand);
    }
    return parse_cmp();
}

int32_t Parser::parse_cmp() {
    int32_t left = parse_add();
    while (left >= 0 && ok()) {
        uint8_t op = QF_OP_NONE;
        switch (cur_.kind) {
        case TokEq: op = QF_OP_EQ; break;
        case TokNe: op = QF_OP_NE; break;
        case TokLt: op = QF_OP_LT; break;
        case TokLe: op = QF_OP_LE; break;
        case TokGt: op = QF_OP_GT; break;
        case TokGe: op = QF_OP_GE; break;
        default: return left;
        }
        advance();
        if (!ok()) return -1;
        const int32_t right = parse_add();
        if (right < 0) return -1;
        left = add_bin(op, left, right);
        if (left < 0) return -1;
    }
    return left;
}

int32_t Parser::parse_add() {
    int32_t left = parse_mul();
    while (left >= 0 && ok()) {
        uint8_t op = QF_OP_NONE;
        if (cur_.kind == TokPlus) {
            op = QF_OP_ADD;
        } else if (cur_.kind == TokMinus) {
            op = QF_OP_SUB;
        } else {
            return left;
        }
        advance();
        if (!ok()) return -1;
        const int32_t right = parse_mul();
        if (right < 0) return -1;
        left = add_bin(op, left, right);
        if (left < 0) return -1;
    }
    return left;
}

int32_t Parser::parse_mul() {
    int32_t left = parse_unary();
    while (left >= 0 && ok()) {
        uint8_t op = QF_OP_NONE;
        if (cur_.kind == TokStar) {
            op = QF_OP_MUL;
        } else if (cur_.kind == TokSlash) {
            op = QF_OP_DIV;
        } else if (cur_.kind == TokPercent) {
            op = QF_OP_MOD;
        } else {
            return left;
        }
        advance();
        if (!ok()) return -1;
        const int32_t right = parse_unary();
        if (right < 0) return -1;
        left = add_bin(op, left, right);
        if (left < 0) return -1;
    }
    return left;
}

int32_t Parser::parse_unary() {
    if (ok() && cur_.kind == TokMinus) {
        advance();
        if (!ok()) return -1;
        const int32_t operand = parse_unary();
        if (operand < 0) return -1;
        return add_unary(QF_OP_NEG, operand);
    }
    return parse_atom();
}

int32_t Parser::parse_atom() {
    if (!ok()) return -1;

    switch (cur_.kind) {
    case TokInt: {
        qf_node n = blank_node();
        n.kind = QF_N_LIT;
        n.lit = int_value(cur_.ival);
        const int32_t idx = add(n);
        if (idx < 0) return -1;
        advance();
        return idx;
    }
    case TokDouble: {
        qf_node n = blank_node();
        n.kind = QF_N_LIT;
        n.lit = double_value(cur_.dval);
        const int32_t idx = add(n);
        if (idx < 0) return -1;
        advance();
        return idx;
    }
    case TokString: {
        const uint32_t line = cur_.line;
        const uint32_t col = cur_.col;
        qf_str text;
        if (!intern_or_fail(cur_.text, &text, line, col)) return -1;
        qf_node n = blank_node();
        n.kind = QF_N_LIT;
        n.lit = text_value(text);
        const int32_t idx = add(n);
        if (idx < 0) return -1;
        advance();
        return idx;
    }
    case TokLParen: {
        advance();
        if (!ok()) return -1;
        const int32_t inner = parse_expr();
        if (inner < 0) return -1;
        if (!expect(TokRParen, ")", "括号没有闭合")) return -1;
        return inner;
    }
    case TokIdent:
        return parse_ident_atom();
    default:
        break;
    }

    set_error(cur_.line, cur_.col,
              "此处需要表达式（实际遇到 " + describe(cur_) + "）",
              "整数、浮点数、字符串、列名、函数调用或 (");
    return -1;
}

int32_t Parser::parse_ident_atom() {
    const std::string name = cur_.text;
    const uint32_t line = cur_.line;
    const uint32_t col = cur_.col;
    advance();
    if (!ok()) return -1;

    if (cur_.kind == TokLParen) {
        const int code = agg_code_for(name);
        if (code != 0) return parse_agg(code);
        return parse_call(name);
    }

    qf_str column;
    if (!intern_or_fail(name, &column, line, col)) return -1;
    qf_node n = blank_node();
    n.kind = QF_N_COL;
    n.name = column;
    n.a = 0; /* 列下标由计划阶段回填 */
    return add(n);
}

int32_t Parser::parse_call(const std::string& name) {
    /* 当前 token 是 '(' */
    const uint32_t line = cur_.line;
    const uint32_t col = cur_.col;
    advance();
    if (!ok()) return -1;

    std::vector<int32_t> args;
    if (cur_.kind != TokRParen) {
        for (;;) {
            const int32_t arg = parse_expr();
            if (arg < 0) return -1;
            args.push_back(arg);
            if (cur_.kind != TokComma) break;
            advance();
            if (!ok()) return -1;
        }
    }
    if (!expect(TokRParen, ")", "函数调用缺少右括号")) return -1;

    qf_str fn;
    if (!intern_or_fail(name, &fn, line, col)) return -1;

    qf_node n = blank_node();
    n.kind = QF_N_CALL;
    n.name = fn;
    n.argc = (int32_t)args.size();

    if (args.empty()) {
        n.b = -1;
    } else if (args.size() == 1) {
        n.b = args[0];
    } else {
        /*
         * engine.c 用 n->b + i 取第 i 个参数，参数根节点必须连续。
         * 各实参子树之间会夹着子节点，因此这里把每个实参根节点原样复制一份
         * 到节点尾部（只复制根，子节点引用保持有效），保证 b..b+argc-1 连续。
         */
        n.b = (int32_t)nodes_.size();
        for (const int32_t root : args) {
            const qf_node copy = nodes_[(size_t)root];
            nodes_.push_back(copy);
        }
    }
    return add(n);
}

int32_t Parser::parse_agg(int code) {
    /* 当前 token 是 '(' */
    advance();
    if (!ok()) return -1;

    qf_node n = blank_node();
    n.kind = QF_N_AGG;
    n.op = (uint8_t)code;

    if (code == QF_AGG_COUNT && cur_.kind == TokStar) {
        advance();
        if (!ok()) return -1;
        if (!expect(TokRParen, ")", "count(*) 缺少右括号")) return -1;
        n.argc = 0;
        n.b = -1;
        return add(n);
    }

    if (cur_.kind == TokStar) {
        set_error(cur_.line, cur_.col, "只有 count 支持 * 参数，其它聚合需要表达式", "表达式");
        return -1;
    }

    const int32_t arg = parse_expr();
    if (arg < 0) return -1;
    if (cur_.kind == TokComma) {
        set_error(cur_.line, cur_.col, "聚合函数只接受一个参数（实际遇到 ','）", ")");
        return -1;
    }
    if (!expect(TokRParen, ")", "聚合函数缺少右括号")) return -1;

    n.argc = 1;
    n.b = arg;
    return add(n);
}

qf_ast* Parser::build_ast() {
    qf_ast* ast = (qf_ast*)alloc_bytes(sizeof(qf_ast), alignof(qf_ast));
    if (!ast) {
        set_error(1, 1, "内存不足：无法分配 AST 根", "更大的 arena", QF_ERR_PANIC);
        return nullptr;
    }
    std::memset(ast, 0, sizeof *ast);

    if (!nodes_.empty()) {
        const uint64_t bytes = (uint64_t)nodes_.size() * sizeof(qf_node);
        qf_node* nodes = (qf_node*)alloc_bytes(bytes, alignof(qf_node));
        if (!nodes) {
            set_error(1, 1, "内存不足：无法分配 AST 节点", "更大的 arena", QF_ERR_PANIC);
            return nullptr;
        }
        std::memcpy(nodes, nodes_.data(), (size_t)bytes);
        ast->nodes = nodes;
        ast->node_count = (uint32_t)nodes_.size();
    }

    {
        const uint64_t bytes = (uint64_t)select_.size() * sizeof(int32_t);
        int32_t* select = (int32_t*)alloc_bytes(bytes, alignof(int32_t));
        if (!select) {
            set_error(1, 1, "内存不足：无法分配 select 索引", "更大的 arena", QF_ERR_PANIC);
            return nullptr;
        }
        std::memcpy(select, select_.data(), (size_t)bytes);
        ast->select = select;
        ast->select_count = (uint32_t)select_.size();
    }

    {
        const uint64_t bytes = (uint64_t)select_alias_.size() * sizeof(qf_str);
        qf_str* names = (qf_str*)alloc_bytes(bytes, alignof(qf_str));
        if (!names) {
            set_error(1, 1, "内存不足：无法分配输出列名", "更大的 arena", QF_ERR_PANIC);
            return nullptr;
        }
        std::memcpy(names, select_alias_.data(), (size_t)bytes);
        ast->select_names = names;
    }

    if (!group_.empty()) {
        const uint64_t bytes = (uint64_t)group_.size() * sizeof(int32_t);
        int32_t* group = (int32_t*)alloc_bytes(bytes, alignof(int32_t));
        if (!group) {
            set_error(1, 1, "内存不足：无法分配 group_by 索引", "更大的 arena", QF_ERR_PANIC);
            return nullptr;
        }
        std::memcpy(group, group_.data(), (size_t)bytes);
        ast->group_by = group;
        ast->group_count = (uint32_t)group_.size();
    }

    if (!order_.empty()) {
        const uint64_t bytes = (uint64_t)order_.size() * sizeof(int32_t);
        int32_t* order = (int32_t*)alloc_bytes(bytes, alignof(int32_t));
        if (!order) {
            set_error(1, 1, "内存不足：无法分配 order_by 索引", "更大的 arena", QF_ERR_PANIC);
            return nullptr;
        }
        std::memcpy(order, order_.data(), (size_t)bytes);
        ast->order_by = order;
        ast->order_count = (uint32_t)order_.size();
    }

    ast->from_table = from_table_;
    ast->where_root = where_root_;
    ast->limit = limit_;
    return ast;
}

qf_err Parser::run(qf_ast** out, qf_diag* diag) {
    advance();

    if (!failed_ && cur_.kind != TokSelect) {
        set_error(cur_.line, cur_.col,
                  "查询必须以 SELECT 开始（实际遇到 " + describe(cur_) + "）", "SELECT");
    }

    if (!failed_) {
        advance();
        if (!ok()) {
            publish(diag);
            return err_code_;
        }
        parse_select_list();
        if (ok()) expect(TokFrom, "FROM", "SELECT 列表之后缺少 FROM");
        if (ok()) parse_table();
        if (ok()) parse_clauses();
        if (ok() && cur_.kind == TokHaving) {
            set_error(cur_.line, cur_.col, "暂不支持 HAVING 子句，请改用 WHERE", "输入结束");
        }
        if (ok()) expect(TokEnd, "输入结束", "查询已经完整，后面不应再有内容");
    }

    if (failed_) {
        publish(diag);
        return err_code_;
    }

    qf_ast* ast = build_ast();
    if (!ast) {
        publish(diag);
        return err_code_;
    }

    *out = ast;
    publish(diag);
    return QF_ERR_OK;
}

} /* namespace */

extern "C" qf_err qf_sql_parse(const qf_host* host, qf_str sql, qf_ast** out, qf_diag* diag) {
    if (!out) {
        if (diag) {
            std::memset(diag, 0, sizeof *diag);
            diag->code = QF_ERR_ARG;
            diag->line = 1;
            diag->col = 1;
            static const char kMsg[] = "输出指针为空";
            diag->message.ptr = kMsg;
            diag->message.len = (uint32_t)(sizeof kMsg - 1);
        }
        return QF_ERR_ARG;
    }
    *out = nullptr;

    if (!host || !host->alloc || !host->str) {
        if (diag) {
            std::memset(diag, 0, sizeof *diag);
            diag->code = QF_ERR_ARG;
            diag->line = 1;
            diag->col = 1;
            if (host && host->str) {
                static const char kMsg[] = "宿主缺少 alloc/str 服务";
                diag->message = host->str(host->ctx, kMsg, (uint32_t)(sizeof kMsg - 1));
            }
        }
        return QF_ERR_ARG;
    }
    if (!sql.ptr && sql.len != 0) {
        if (diag) {
            std::memset(diag, 0, sizeof *diag);
            diag->code = QF_ERR_ARG;
            diag->line = 1;
            diag->col = 1;
            static const char kMsg[] = "SQL 文本指针为空";
            diag->message = host->str(host->ctx, kMsg, (uint32_t)(sizeof kMsg - 1));
        }
        return QF_ERR_ARG;
    }

    try {
        Parser parser(host, sql.ptr ? sql.ptr : "", sql.len);
        return parser.run(out, diag);
    } catch (const std::bad_alloc&) {
        if (diag) {
            std::memset(diag, 0, sizeof *diag);
            diag->code = QF_ERR_PANIC;
            diag->line = 1;
            diag->col = 1;
            static const char kMsg[] = "解析器内存不足";
            diag->message = host->str(host->ctx, kMsg, (uint32_t)(sizeof kMsg - 1));
        }
        return QF_ERR_PANIC;
    } catch (...) {
        if (diag) {
            std::memset(diag, 0, sizeof *diag);
            diag->code = QF_ERR_PANIC;
            diag->line = 1;
            diag->col = 1;
            static const char kMsg[] = "解析器内部错误";
            diag->message = host->str(host->ctx, kMsg, (uint32_t)(sizeof kMsg - 1));
        }
        return QF_ERR_PANIC;
    }
}
