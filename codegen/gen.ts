/**
 * 从 schema/qf.ts 生成各语言绑定。
 * 运行：bun codegen/gen.ts
 */
import { writeFileSync } from 'node:fs';
import {
  ABI_VERSION, AGG, ERR, FLAGS, KEYWORDS, LAYOUT, NODE_KIND, OP, OP_NAMES, AGG_NAMES, TYPE,
} from '../schema/qf.ts';

const HEADER = '/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */\n';
const write = (path: string, body: string) => writeFileSync(path, HEADER + body, 'utf8');

const enumC = (prefix: string, entries: Record<string, number>, type = '') => {
  const body = Object.entries(entries)
    .map(([k, v]) => `    ${prefix}_${k} = ${v}`)
    .join(',\n');
  return type
    ? `typedef enum {\n${body}\n} ${type};`
    : `enum {\n${body}\n};`;
};

const cStructs = Object.entries(LAYOUT)
  .map(([name, l]) => `typedef struct ${name} {\n${l.fields.map((f) => `    ${f.c} ${f.name};`).join('\n')}\n} ${name};`)
  .join('\n\n');

const cAsserts = Object.entries(LAYOUT)
  .map(([name, l]) => [
    `QF_STATIC_ASSERT(sizeof(${name}) == ${l.size}, "${name} 尺寸漂移");`,
    ...l.fields.map((f) => `QF_STATIC_ASSERT(offsetof(${name}, ${f.name}) == ${f.off}, "${name}.${f.name} 偏移漂移");`),
  ].join('\n'))
  .join('\n');

// ---------------- C ----------------
const cLayout = `
#ifndef QF_LAYOUT_H
#define QF_LAYOUT_H
#include <stdint.h>
#include <stddef.h>

#define QF_ABI_VERSION ${ABI_VERSION}

typedef int32_t qf_err;
${enumC('QF_ERR', ERR)}

typedef int32_t qf_type;
${Object.entries(TYPE).map(([k, v]) => `enum { QF_T_${k} = ${v} };`).join('\n')}

${enumC('QF_N', NODE_KIND, 'qf_node_kind')}

${enumC('QF_OP', OP, 'qf_op')}

${enumC('QF_AGG', AGG, 'qf_agg')}

enum {
    QF_F_TYPE_SHIFT = ${FLAGS.TYPE_SHIFT},
    QF_F_TYPE_MASK = ${FLAGS.TYPE_MASK},
    QF_F_DESC = ${FLAGS.DESC},
    QF_F_RESOLVED = ${FLAGS.RESOLVED}
};

${cStructs}

#if defined(__cplusplus)
#define QF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define QF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define QF_STATIC_ASSERT(cond, msg)
#endif

${cAsserts}

#endif /* QF_LAYOUT_H */
`;

// ---------------- C# ----------------
const csStructs = Object.entries(LAYOUT)
  .map(([name, l]) => {
    const fields: { name: string; cs: string; off: number }[] =
      'csFields' in l ? l.csFields : l.fields;
    const structName =
      'Qf' + name.replace(/^qf_/, '').replace(/(^|_)([a-z])/g, (_m, _p, c) => c.toUpperCase());
    const lines = fields.map((f) => {
      const fieldName = f.name.replace(/(^|_)([a-z])/g, (_m, _p, c) => c.toUpperCase());
      return `    [FieldOffset(${f.off})] public ${f.cs} ${fieldName};`;
    });
    return `[StructLayout(LayoutKind.Explicit, Size = ${l.size})]
internal unsafe struct ${structName}
{
${lines.join('\n')}
}`;
  })
  .join('\n\n');

const csLayout = `
// 由 schema/qf.ts + codegen/gen.ts 生成，勿手改
using System.Runtime.InteropServices;

namespace QfCs;

${csStructs}
`;

// ---------------- Python ----------------
const pyLayout = `
import ctypes


class QfStr(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_char_p), ("len", ctypes.c_uint32)]


class QfValueUnion(ctypes.Union):
    _fields_ = [("i", ctypes.c_int64), ("d", ctypes.c_double), ("s", QfStr)]


class QfValue(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int32), ("as_", QfValueUnion)]


class QfNode(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("op", ctypes.c_uint8),
        ("flags", ctypes.c_uint16),
        ("a", ctypes.c_int32),
        ("b", ctypes.c_int32),
        ("argc", ctypes.c_int32),
        ("lit", QfValue),
        ("name", QfStr),
    ]


class QfCol(ctypes.Structure):
    _fields_ = [("name", QfStr), ("type", ctypes.c_int32)]


class QfSchema(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("cols", ctypes.POINTER(QfCol))]


class QfBatch(ctypes.Structure):
    _fields_ = [
        ("rows", ctypes.c_uint32),
        ("cols", ctypes.c_uint32),
        ("values", ctypes.POINTER(QfValue)),
    ]


class QfResult(ctypes.Structure):
    _fields_ = [
        ("schema", QfSchema),
        ("row_count", ctypes.c_uint64),
        ("values", ctypes.POINTER(QfValue)),
    ]


class QfDiag(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_int32),
        ("line", ctypes.c_uint32),
        ("col", ctypes.c_uint32),
        ("message", QfStr),
        ("hint", QfStr),
    ]


class QfAst(ctypes.Structure):
    _fields_ = [
        ("nodes", ctypes.POINTER(QfNode)),
        ("node_count", ctypes.c_uint32),
        ("select", ctypes.POINTER(ctypes.c_int32)),
        ("select_count", ctypes.c_uint32),
        ("select_names", ctypes.POINTER(QfStr)),
        ("from_table", QfStr),
        ("where_root", ctypes.c_int32),
        ("group_by", ctypes.POINTER(ctypes.c_int32)),
        ("group_count", ctypes.c_uint32),
        ("order_by", ctypes.POINTER(ctypes.c_int32)),
        ("order_count", ctypes.c_uint32),
        ("limit", ctypes.c_int64),
    ]


ABI_VERSION = ${ABI_VERSION}
QF_OK = ${ERR.OK}
QF_ERR_PARSE = ${ERR.PARSE}
QF_ERR_TYPE = ${ERR.TYPE}
QF_ERR_UDF = ${ERR.UDF}

QF_T_NULL, QF_T_INT, QF_T_DOUBLE, QF_T_TEXT = ${TYPE.NULL}, ${TYPE.INT}, ${TYPE.DOUBLE}, ${TYPE.TEXT}

QF_NODE_KIND = ${JSON.stringify(NODE_KIND)}
QF_OP = ${JSON.stringify(OP)}
QF_AGG = ${JSON.stringify(AGG)}

assert ctypes.sizeof(QfStr) == 16
assert ctypes.sizeof(QfValue) == 24
assert ctypes.sizeof(QfNode) == 56
assert ctypes.sizeof(QfCol) == 24
assert ctypes.sizeof(QfSchema) == 16
assert ctypes.sizeof(QfBatch) == 16
assert ctypes.sizeof(QfResult) == 32
assert ctypes.sizeof(QfDiag) == 48
assert ctypes.sizeof(QfAst) == 104
assert QfAst.select_names.offset == 32
assert QfAst.limit.offset == 96
`;

// ---------------- Rust: 布局断言 ----------------
const rustTypes: Record<string, true> = {
  qf_str: true,
  qf_value: true,
  qf_node: true,
  qf_col: true,
  qf_schema: true,
  qf_diag: true,
  qf_ast: true,
};
const rustAsserts = Object.entries(LAYOUT)
  .filter(([name]) => rustTypes[name])
  .map(([name, l]) => {
    const lines = [`const _: () = assert!(std::mem::size_of::<${name}>() == ${l.size});`];
    for (const f of l.fields) {
      const fieldName = f.name === 'type' ? 'type_' : f.name === 'as' ? 'as_' : f.name;
      lines.push(`const _: () = assert!(std::mem::offset_of!(${name}, ${fieldName}) == ${f.off});`);
    }
    return lines.join('\n');
  })
  .join('\n');

const rustLayout = `// 由 schema/qf.ts + codegen/gen.ts 生成，勿手改
${rustAsserts}
`;

// ---------------- C: 关键字与展示名 ----------------
const cKeywords = `
#ifndef QF_KEYWORDS_H
#define QF_KEYWORDS_H

#define QF_KEYWORDS(X) ${KEYWORDS.map((k) => `X("${k}")`).join(' ')}

static const char* const QF_OP_NAME[] = {
${Array.from({ length: 16 }, (_, i) => `    ${JSON.stringify(OP_NAMES[i] ?? '?')}`).join(',\n')}
};

static const char* const QF_AGG_NAME[] = {
    "?", "count", "sum", "avg", "min", "max"
};

#endif /* QF_KEYWORDS_H */
`;

write('gen/qf_layout.h', cLayout);
write('gen/qf_layout.cs', csLayout);
writeFileSync('gen/qf_layout.py', '# 由 schema/qf.ts + codegen/gen.ts 生成，勿手改\n' + pyLayout, 'utf8');
write('gen/qf_layout_assert.rs', rustLayout);
write('gen/qf_keywords.h', cKeywords);
console.log('generated:', ['gen/qf_layout.h', 'gen/qf_layout.cs', 'gen/qf_layout.py', 'gen/qf_layout_assert.rs', 'gen/qf_keywords.h'].join(', '));
console.log('agg names:', Object.values(AGG_NAMES).join(','));
