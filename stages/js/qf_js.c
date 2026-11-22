/*
 * JavaScript 阶段：QuickJS 驱动的结果表格渲染与 --where 沙箱。
 *
 * 设计要点：
 *   1. 每次调用新建 JSRuntime/JSContext，调用结束即销毁，不跨调用保留状态，
 *      避免用户表达式把状态留在上下文里影响下一次求值。
 *   2. 不做任何 I/O 或模块加载：上下文里只有 ECMAScript 标准内建对象。
 *   3. 跨边界只传 JSON 文本，不把宿主指针暴露给脚本。
 */
#include "qf.h"
#include "quickjs.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 内嵌脚本 ---------- */

static const char qf_js_src[] =
    "'use strict';\n"
    "var __QF_MAX_COL = 32;\n"
    "\n"
    "function __qf_err(e) {\n"
    "  return (e && e.message) ? String(e.message) : String(e);\n"
    "}\n"
    "\n"
    "function __qf_chars(s) {\n"
    "  return Array.from(s);\n"
    "}\n"
    "\n"
    "function __qf_cell(v) {\n"
    "  if (v === null || v === undefined) return 'NULL';\n"
    "  if (typeof v === 'number') {\n"
    "    if (!isFinite(v)) return 'NULL';\n"
    "    return String(v);\n"
    "  }\n"
    "  if (typeof v === 'boolean') return v ? 'true' : 'false';\n"
    "  return String(v);\n"
    "}\n"
    "\n"
    "function __qf_fit(ch) {\n"
    "  if (ch.length <= __QF_MAX_COL) return ch;\n"
    "  return ch.slice(0, __QF_MAX_COL - 1).concat(['\\u2026']);\n"
    "}\n"
    "\n"
    "function __qf_pad(ch, w, right) {\n"
    "  var gap = w - ch.length;\n"
    "  if (gap < 0) gap = 0;\n"
    "  var parts = [];\n"
    "  for (var i = 0; i < gap; i++) parts.push(' ');\n"
    "  return right ? parts.concat(ch).join('') : ch.concat(parts).join('');\n"
    "}\n"
    "\n"
    "function __qf_format(json) {\n"
    "  var res = JSON.parse(json);\n"
    "  var cols = res.cols || [];\n"
    "  var rows = res.rows || [];\n"
    "  var nc = cols.length;\n"
    "  var c, r;\n"
    "  if (nc === 0) return '(' + rows.length + ' rows)\\n';\n"
    "  var header = [], widths = [], numeric = [];\n"
    "  for (c = 0; c < nc; c++) {\n"
    "    var h = __qf_fit(__qf_chars(String(cols[c].name)));\n"
    "    header.push(h);\n"
    "    widths.push(h.length);\n"
    "    numeric.push(cols[c].type === 1 || cols[c].type === 2);\n"
    "  }\n"
    "  var body = [];\n"
    "  for (r = 0; r < rows.length; r++) {\n"
    "    var line = [];\n"
    "    for (c = 0; c < nc; c++) {\n"
    "      var cell = __qf_fit(__qf_chars(__qf_cell(rows[r][c])));\n"
    "      line.push(cell);\n"
    "      if (cell.length > widths[c]) widths[c] = cell.length;\n"
    "    }\n"
    "    body.push(line);\n"
    "  }\n"
    "  var out = [];\n"
    "  var head = [];\n"
    "  for (c = 0; c < nc; c++) head.push(__qf_pad(header[c], widths[c], numeric[c]));\n"
    "  out.push(head.join('  '));\n"
    "  var sep = [];\n"
    "  for (c = 0; c < nc; c++) sep.push('-'.repeat(widths[c]));\n"
    "  out.push(sep.join('  '));\n"
    "  for (r = 0; r < body.length; r++) {\n"
    "    var parts = [];\n"
    "    for (c = 0; c < nc; c++) parts.push(__qf_pad(body[r][c], widths[c], numeric[c]));\n"
    "    out.push(parts.join('  '));\n"
    "  }\n"
    "  out.push('(' + rows.length + ' rows)');\n"
    "  return out.join('\\n') + '\\n';\n"
    "}\n"
    "\n"
    "function __qf_where(exprSrc, json) {\n"
    "  var batch = JSON.parse(json);\n"
    "  var cols = batch.cols || [];\n"
    "  var rows = batch.rows || [];\n"
    "  var fn;\n"
    "  try {\n"
    "    fn = new Function('row', '\"use strict\";return (' + exprSrc + '\\n);');\n"
    "  } catch (e) {\n"
    "    throw new Error('表达式无法编译为 JavaScript：' + __qf_err(e));\n"
    "  }\n"
    "  var out = [];\n"
    "  for (var r = 0; r < rows.length; r++) {\n"
    "    var target = Object.create(null);\n"
    "    for (var c = 0; c < cols.length; c++) target[cols[c]] = rows[r][c];\n"
    "    var row = new Proxy(target, {\n"
    "      get: function(t, key) {\n"
    "        if (typeof key !== 'string') throw new Error('行对象只允许按列名读取');\n"
    "        if (Object.prototype.hasOwnProperty.call(t, key)) return t[key];\n"
    "        throw new Error('未知列：' + String(key));\n"
    "      },\n"
    "      has: function(t, key) {\n"
    "        return typeof key === 'string' &&\n"
    "               Object.prototype.hasOwnProperty.call(t, key);\n"
    "      },\n"
    "      ownKeys: function(t) { return Object.keys(t); },\n"
    "      getOwnPropertyDescriptor: function(t, key) {\n"
    "        if (!Object.prototype.hasOwnProperty.call(t, key)) return undefined;\n"
    "        return { value: t[key], writable: false, enumerable: true, configurable: true };\n"
    "      },\n"
    "      set: function() { throw new Error('行对象是只读的'); },\n"
    "      defineProperty: function() { throw new Error('行对象是只读的'); },\n"
    "      deleteProperty: function() { throw new Error('行对象是只读的'); },\n"
    "      setPrototypeOf: function() { throw new Error('行对象是只读的'); },\n"
    "      getPrototypeOf: function() { return null; }\n"
    "    });\n"
    "    var v;\n"
    "    try {\n"
    "      v = fn(row);\n"
    "    } catch (e) {\n"
    "      throw new Error('第 ' + (r + 1) + ' 行表达式求值失败：' + __qf_err(e));\n"
    "    }\n"
    "    out.push(!!v);\n"
    "  }\n"
    "  return out;\n"
    "}\n";

/* ---------- 可增长文本缓冲（仅用于临时 JSON 与诊断文本） ---------- */

typedef struct sbuf {
    char*  p;
    size_t len;
    size_t cap;
    int    oom;
} sbuf;

static void sb_init(sbuf* b) {
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static void sb_free(sbuf* b) {
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static void sb_putn(sbuf* b, const char* s, size_t n) {
    if (b->oom) return;
    if (n == 0) {
        if (b->p) b->p[b->len] = '\0';
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char*  q;
        while (cap < b->len + n + 1) {
            if (cap > (size_t)-1 / 2) { b->oom = 1; return; }
            cap *= 2;
        }
        q = (char*)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(sbuf* b, const char* s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(sbuf* b, char c) { sb_putn(b, &c, 1); }

static void sb_put_i64(sbuf* b, int64_t v) {
    char t[32];
    int  n = snprintf(t, sizeof t, "%lld", (long long)v);
    if (n > 0) sb_putn(b, t, (size_t)n);
}

static void sb_json_str(sbuf* b, const char* s, uint32_t len) {
    uint32_t i;
    sb_putc(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_putn(b, "\\\"", 2); break;
        case '\\': sb_putn(b, "\\\\", 2); break;
        case '\b': sb_putn(b, "\\b", 2); break;
        case '\f': sb_putn(b, "\\f", 2); break;
        case '\n': sb_putn(b, "\\n", 2); break;
        case '\r': sb_putn(b, "\\r", 2); break;
        case '\t': sb_putn(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char t[8];
                int  n = snprintf(t, sizeof t, "\\u%04x", (unsigned)c);
                if (n > 0) sb_putn(b, t, (size_t)n);
            } else {
                sb_putc(b, (char)c);
            }
        }
    }
    sb_putc(b, '"');
}

static void sb_json_value(sbuf* b, const qf_value* v) {
    switch (v->type) {
    case QF_T_INT:
        sb_put_i64(b, v->as.i);
        break;
    case QF_T_DOUBLE:
        if (isfinite(v->as.d)) {
            char t[48];
            int  n = snprintf(t, sizeof t, "%.17g", v->as.d);
            if (n > 0) sb_putn(b, t, (size_t)n);
        } else {
            sb_puts(b, "null"); /* JSON 没有 NaN/Infinity */
        }
        break;
    case QF_T_TEXT:
        sb_json_str(b, v->as.s.ptr ? v->as.s.ptr : "", v->as.s.len);
        break;
    default:
        sb_puts(b, "null");
        break;
    }
}

/* ---------- 诊断 ---------- */

static void diag_set(const qf_host* host, qf_diag* diag, qf_err code,
                     const char* msg, size_t len) {
    if (!diag) return;
    memset(diag, 0, sizeof *diag);
    diag->code = code;
    if (host && host->str) {
        diag->message = host->str(host->ctx, msg, (uint32_t)len);
    }
    if (!diag->message.ptr) {
        diag->message.ptr = msg;
        diag->message.len = (uint32_t)len;
    }
}

static void diag_lit(const qf_host* host, qf_diag* diag, qf_err code, const char* lit) {
    diag_set(host, diag, code, lit, strlen(lit));
}

/* 取出当前上下文里的异常，拼成一条中文诊断。 */
static void diag_js_error(const qf_host* host, JSContext* ctx, qf_diag* diag,
                          const char* prefix) {
    JSValue     exc = JS_GetException(ctx);
    JSValue     m = JS_GetPropertyStr(ctx, exc, "message");
    const char* msg = NULL;
    size_t      mlen = 0;
    sbuf        b;

    if (JS_IsString(m)) msg = JS_ToCStringLen(ctx, &mlen, m);
    if (!msg) {
        msg = JS_ToCString(ctx, exc);
        if (msg) mlen = strlen(msg);
    }

    sb_init(&b);
    sb_puts(&b, prefix);
    if (msg && mlen) {
        sb_puts(&b, ": ");
        sb_putn(&b, msg, mlen);
    } else {
        sb_puts(&b, ": 未知的 JavaScript 异常");
    }

    if (!b.oom && b.p && host && host->str) {
        qf_str copied = host->str(host->ctx, b.p, (uint32_t)b.len);
        if (copied.ptr) {
            if (diag) {
                memset(diag, 0, sizeof *diag);
                diag->code = QF_ERR_EXEC;
                diag->message = copied;
            }
        } else {
            diag_lit(host, diag, QF_ERR_EXEC, "JavaScript 求值失败");
        }
    } else {
        diag_lit(host, diag, QF_ERR_EXEC, "JavaScript 求值失败");
    }

    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, exc);
    sb_free(&b);
}

/* ---------- 每次调用独立的运行时 ---------- */

typedef struct js_env {
    JSRuntime* rt;
    JSContext* ctx;
} js_env;

static void js_env_close(js_env* env) {
    if (env->ctx) {
        JS_FreeContext(env->ctx);
        env->ctx = NULL;
    }
    if (env->rt) {
        JS_FreeRuntime(env->rt);
        env->rt = NULL;
    }
}

static qf_err js_env_open(js_env* env) {
    JSValue r;

    env->rt = NULL;
    env->ctx = NULL;

    env->rt = JS_NewRuntime();
    if (!env->rt) return QF_ERR_PANIC;
    JS_SetMemoryLimit(env->rt, (size_t)32 * 1024 * 1024);
    JS_SetMaxStackSize(env->rt, (size_t)1024 * 1024);
    JS_SetCanBlock(env->rt, 0);

    env->ctx = JS_NewContext(env->rt);
    if (!env->ctx) {
        js_env_close(env);
        return QF_ERR_PANIC;
    }

    r = JS_Eval(env->ctx, qf_js_src, sizeof(qf_js_src) - 1, "qf_js_embed.js",
                JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JS_FreeValue(env->ctx, r);
        js_env_close(env);
        return QF_ERR_PANIC;
    }
    JS_FreeValue(env->ctx, r);
    return QF_ERR_OK;
}

/* 把脚本函数的结果字符串复制进宿主内存，末尾补 NUL。 */
static qf_err take_string(const qf_host* host, JSContext* ctx, JSValue val,
                          qf_str* out) {
    const char* s;
    size_t      len = 0;
    char*       buf;

    out->ptr = NULL;
    out->len = 0;
    if (!JS_IsString(val)) return QF_ERR_PANIC;

    s = JS_ToCStringLen(ctx, &len, val);
    if (!s) return QF_ERR_PANIC;
    if (len > (size_t)UINT32_MAX - 1) {
        JS_FreeCString(ctx, s);
        return QF_ERR_PANIC;
    }

    buf = (char*)host->alloc(host->ctx, (uint64_t)len + 1, 1);
    if (!buf) {
        JS_FreeCString(ctx, s);
        return QF_ERR_PANIC;
    }
    if (len) memcpy(buf, s, len);
    buf[len] = '\0';
    out->ptr = buf;
    out->len = (uint32_t)len;
    JS_FreeCString(ctx, s);
    return QF_ERR_OK;
}

/* ---------- 契约实现 ---------- */

qf_err qf_js_init(void) {
    js_env env;
    qf_err rc = js_env_open(&env);
    if (rc == QF_ERR_OK) js_env_close(&env);
    return rc;
}

qf_err qf_js_format(const qf_host* host, const qf_result* result, qf_str* out,
                    qf_diag* diag) {
    js_env   env;
    sbuf     json;
    qf_err   rc;
    uint32_t c;
    uint64_t i;
    JSValue  global, fn, arg, ret;

    if (!host || !host->alloc || !result || !out) {
        diag_lit(host, diag, QF_ERR_ARG, "qf_js_format 参数为空");
        return QF_ERR_ARG;
    }
    if (result->schema.count > 0 && !result->schema.cols) {
        diag_lit(host, diag, QF_ERR_ARG, "结果集的列定义缺失");
        return QF_ERR_ARG;
    }
    if (result->row_count > 0 && !result->values) {
        diag_lit(host, diag, QF_ERR_ARG, "结果集的行数据缺失");
        return QF_ERR_ARG;
    }
    if (result->row_count > 0xFFFFFFFFull) {
        diag_lit(host, diag, QF_ERR_ARG, "结果集行数超出可渲染范围");
        return QF_ERR_ARG;
    }

    sb_init(&json);
    sb_puts(&json, "{\"cols\":[");
    for (c = 0; c < result->schema.count; c++) {
        qf_col col = result->schema.cols[c];
        if (c) sb_putc(&json, ',');
        sb_puts(&json, "{\"name\":");
        sb_json_str(&json, col.name.ptr ? col.name.ptr : "", col.name.len);
        sb_puts(&json, ",\"type\":");
        sb_put_i64(&json, col.type);
        sb_putc(&json, '}');
    }
    sb_puts(&json, "],\"rows\":[");
    for (i = 0; i < result->row_count; i++) {
        if (i) sb_putc(&json, ',');
        sb_putc(&json, '[');
        for (c = 0; c < result->schema.count; c++) {
            if (c) sb_putc(&json, ',');
            sb_json_value(&json, &result->values[i * (uint64_t)result->schema.count + c]);
        }
        sb_putc(&json, ']');
    }
    sb_puts(&json, "]}");

    if (json.oom || !json.p) {
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "无法序列化结果集");
        return QF_ERR_PANIC;
    }

    rc = js_env_open(&env);
    if (rc != QF_ERR_OK) {
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "无法创建 JavaScript 运行时");
        return rc;
    }

    global = JS_GetGlobalObject(env.ctx);
    fn = JS_GetPropertyStr(env.ctx, global, "__qf_format");
    JS_FreeValue(env.ctx, global);
    if (!JS_IsFunction(env.ctx, fn)) {
        JS_FreeValue(env.ctx, fn);
        js_env_close(&env);
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "内嵌脚本缺少格式化函数");
        return QF_ERR_PANIC;
    }

    arg = JS_NewStringLen(env.ctx, json.p, json.len);
    sb_free(&json);
    if (JS_IsException(arg)) {
        JS_FreeValue(env.ctx, fn);
        js_env_close(&env);
        diag_lit(host, diag, QF_ERR_PANIC, "无法构造表格输入");
        return QF_ERR_PANIC;
    }

    ret = JS_Call(env.ctx, fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(env.ctx, arg);
    JS_FreeValue(env.ctx, fn);
    if (JS_IsException(ret)) {
        diag_js_error(host, env.ctx, diag, "表格渲染失败");
        JS_FreeValue(env.ctx, ret);
        js_env_close(&env);
        return QF_ERR_EXEC;
    }

    rc = take_string(host, env.ctx, ret, out);
    JS_FreeValue(env.ctx, ret);
    js_env_close(&env);
    if (rc != QF_ERR_OK) {
        diag_lit(host, diag, QF_ERR_PANIC, "表格输出无法复制到宿主内存");
        return rc;
    }
    if (diag) memset(diag, 0, sizeof *diag);
    return QF_ERR_OK;
}

qf_err qf_js_where_named(const qf_host* host, qf_str expr, const qf_schema* cols,
                         const qf_batch* batch, uint8_t* keep, qf_diag* diag) {
    js_env   env;
    sbuf     json;
    qf_err   rc;
    uint32_t c;
    uint32_t r;
    JSValue  global, fn, argv[2], ret, lenv;
    uint32_t got = 0;

    if (!host || !host->alloc || !batch || !keep) {
        diag_lit(host, diag, QF_ERR_ARG, "qf_js_where 参数为空");
        return QF_ERR_ARG;
    }
    if (batch->rows > 0 && !batch->values) {
        diag_lit(host, diag, QF_ERR_ARG, "批次的行数据缺失");
        return QF_ERR_ARG;
    }
    if (batch->cols > 0 && cols && cols->count != batch->cols) {
        diag_lit(host, diag, QF_ERR_ARG, "列定义与批次的列数不一致");
        return QF_ERR_ARG;
    }
    if (expr.len > 0 && !expr.ptr) {
        diag_lit(host, diag, QF_ERR_ARG, "表达式文本缺失");
        return QF_ERR_ARG;
    }

    memset(keep, 0, batch->rows);

    sb_init(&json);
    sb_puts(&json, "{\"cols\":[");
    for (c = 0; c < batch->cols; c++) {
        char      namebuf[16];
        qf_str    name;
        if (c) sb_putc(&json, ',');
        if (cols) {
            name = cols->cols[c].name;
        } else {
            int n = snprintf(namebuf, sizeof namebuf, "c%u", (unsigned)c);
            name.ptr = namebuf;
            name.len = (uint32_t)(n > 0 ? n : 0);
        }
        sb_json_str(&json, name.ptr ? name.ptr : "", name.len);
    }
    sb_puts(&json, "],\"rows\":[");
    for (r = 0; r < batch->rows; r++) {
        if (r) sb_putc(&json, ',');
        sb_putc(&json, '[');
        for (c = 0; c < batch->cols; c++) {
            if (c) sb_putc(&json, ',');
            sb_json_value(&json, &batch->values[(uint64_t)r * batch->cols + c]);
        }
        sb_putc(&json, ']');
    }
    sb_puts(&json, "]}");

    if (json.oom || !json.p) {
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "无法序列化批次");
        return QF_ERR_PANIC;
    }

    rc = js_env_open(&env);
    if (rc != QF_ERR_OK) {
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "无法创建 JavaScript 运行时");
        return rc;
    }

    global = JS_GetGlobalObject(env.ctx);
    fn = JS_GetPropertyStr(env.ctx, global, "__qf_where");
    JS_FreeValue(env.ctx, global);
    if (!JS_IsFunction(env.ctx, fn)) {
        JS_FreeValue(env.ctx, fn);
        js_env_close(&env);
        sb_free(&json);
        diag_lit(host, diag, QF_ERR_PANIC, "内嵌脚本缺少 where 函数");
        return QF_ERR_PANIC;
    }

    argv[0] = JS_NewStringLen(env.ctx, expr.ptr ? expr.ptr : "", expr.len);
    argv[1] = JS_NewStringLen(env.ctx, json.p, json.len);
    sb_free(&json);
    if (JS_IsException(argv[0]) || JS_IsException(argv[1])) {
        JS_FreeValue(env.ctx, argv[0]);
        JS_FreeValue(env.ctx, argv[1]);
        JS_FreeValue(env.ctx, fn);
        js_env_close(&env);
        diag_lit(host, diag, QF_ERR_PANIC, "无法构造 where 输入");
        return QF_ERR_PANIC;
    }

    ret = JS_Call(env.ctx, fn, JS_UNDEFINED, 2, argv);
    JS_FreeValue(env.ctx, argv[0]);
    JS_FreeValue(env.ctx, argv[1]);
    JS_FreeValue(env.ctx, fn);
    if (JS_IsException(ret)) {
        diag_js_error(host, env.ctx, diag, "where 表达式求值失败");
        JS_FreeValue(env.ctx, ret);
        js_env_close(&env);
        return QF_ERR_EXEC;
    }

    if (!JS_IsArray(env.ctx, ret)) {
        JS_FreeValue(env.ctx, ret);
        js_env_close(&env);
        diag_lit(host, diag, QF_ERR_PANIC, "where 未返回布尔数组");
        return QF_ERR_PANIC;
    }

    lenv = JS_GetPropertyStr(env.ctx, ret, "length");
    if (JS_IsException(lenv) || JS_ToUint32(env.ctx, &got, lenv) < 0) {
        JS_FreeValue(env.ctx, lenv);
        JS_FreeValue(env.ctx, ret);
        js_env_close(&env);
        diag_lit(host, diag, QF_ERR_PANIC, "where 结果长度无法读取");
        return QF_ERR_PANIC;
    }
    JS_FreeValue(env.ctx, lenv);
    if (got != batch->rows) {
        JS_FreeValue(env.ctx, ret);
        js_env_close(&env);
        diag_lit(host, diag, QF_ERR_PANIC, "where 结果行数与批次不一致");
        return QF_ERR_PANIC;
    }

    for (r = 0; r < batch->rows; r++) {
        JSValue v = JS_GetPropertyUint32(env.ctx, ret, r);
        int     t;
        if (JS_IsException(v)) {
            JS_FreeValue(env.ctx, v);
            JS_FreeValue(env.ctx, ret);
            diag_js_error(host, env.ctx, diag, "where 结果读取失败");
            js_env_close(&env);
            return QF_ERR_EXEC;
        }
        t = JS_ToBool(env.ctx, v);
        JS_FreeValue(env.ctx, v);
        if (t < 0) {
            JS_FreeValue(env.ctx, ret);
            diag_js_error(host, env.ctx, diag, "where 结果不是布尔值");
            js_env_close(&env);
            return QF_ERR_EXEC;
        }
        keep[r] = (uint8_t)(t ? 1 : 0);
    }

    JS_FreeValue(env.ctx, ret);
    js_env_close(&env);
    if (diag) memset(diag, 0, sizeof *diag);
    return QF_ERR_OK;
}

qf_err qf_js_where(const qf_host* host, qf_str expr, const qf_batch* batch,
                   uint8_t* keep, qf_diag* diag) {
    return qf_js_where_named(host, expr, NULL, batch, keep, diag);
}
