/**
 * 单一真相源：ABI 版本、错误码、值类型、AST 节点与编码。
 * codegen/gen.ts 从这里生成 C / C# / Python 的绑定，禁止在别处手写这些常量。
 */

export const ABI_VERSION = 1;

export const ERR = {
  OK: 0,
  ARG: 1,
  IO: 2,
  PARSE: 3,
  TYPE: 4,
  PLAN: 5,
  EXEC: 6,
  UDF: 7,
  PANIC: 8,
  UNSUPPORTED: 9,
} as const;

export const TYPE = { NULL: 0, INT: 1, DOUBLE: 2, TEXT: 3 } as const;

export const NODE_KIND = {
  LIT: 1,
  COL: 2,
  BIN: 3,
  UNARY: 4,
  CALL: 5,
  AGG: 6,
  STAR: 7,
} as const;

export const OP = {
  NONE: 0,
  EQ: 1,
  NE: 2,
  LT: 3,
  LE: 4,
  GT: 5,
  GE: 6,
  AND: 7,
  OR: 8,
  NOT: 9,
  ADD: 10,
  SUB: 11,
  MUL: 12,
  DIV: 13,
  MOD: 14,
  NEG: 15,
} as const;

export const AGG = { COUNT: 1, SUM: 2, AVG: 3, MIN: 4, MAX: 5 } as const;

export const FLAGS = { TYPE_SHIFT: 0, TYPE_MASK: 0xf, DESC: 1 << 4, RESOLVED: 1 << 5 } as const;

/** 结构体字段布局：offset/size 由契约约定，生成端与消费端共用同一张表。 */
export type Field = { name: string; c: string; cs: string; off: number; size: number };

export const LAYOUT = {
  qf_str: {
    size: 16,
    fields: [
      { name: 'ptr', c: 'const char*', cs: 'byte*', off: 0, size: 8 },
      { name: 'len', c: 'uint32_t', cs: 'uint', off: 8, size: 4 },
    ] as Field[],
  },
  qf_value: {
    size: 24,
    fields: [
      { name: 'type', c: 'qf_type', cs: 'int', off: 0, size: 4 },
      { name: 'as', c: 'union { int64_t i; double d; qf_str s; }', cs: 'qf_value_union', off: 8, size: 16 },
    ] as Field[],
  },
  qf_node: {
    size: 56,
    fields: [
      { name: 'kind', c: 'uint8_t', cs: 'byte', off: 0, size: 1 },
      { name: 'op', c: 'uint8_t', cs: 'byte', off: 1, size: 1 },
      { name: 'flags', c: 'uint16_t', cs: 'ushort', off: 2, size: 2 },
      { name: 'a', c: 'int32_t', cs: 'int', off: 4, size: 4 },
      { name: 'b', c: 'int32_t', cs: 'int', off: 8, size: 4 },
      { name: 'argc', c: 'int32_t', cs: 'int', off: 12, size: 4 },
      { name: 'lit', c: 'qf_value', cs: 'qf_value', off: 16, size: 24 },
      { name: 'name', c: 'qf_str', cs: 'qf_str', off: 40, size: 16 },
    ] as Field[],
  },
} as const;

/** SQL 关键字与算子展示名，供 C++ 词法表与 --explain 输出共用。 */
export const KEYWORDS = [
  'select', 'from', 'where', 'group', 'by', 'order', 'having', 'limit', 'as', 'desc', 'asc', 'and', 'or', 'not',
] as const;

export const OP_NAMES: Record<number, string> = {
  [OP.EQ]: '=',
  [OP.NE]: '<>',
  [OP.LT]: '<',
  [OP.LE]: '<=',
  [OP.GT]: '>',
  [OP.GE]: '>=',
  [OP.AND]: 'and',
  [OP.OR]: 'or',
  [OP.NOT]: 'not',
  [OP.ADD]: '+',
  [OP.SUB]: '-',
  [OP.MUL]: '*',
  [OP.DIV]: '/',
  [OP.MOD]: '%',
  [OP.NEG]: '-',
};

export const AGG_NAMES: Record<number, string> = {
  [AGG.COUNT]: 'count',
  [AGG.SUM]: 'sum',
  [AGG.AVG]: 'avg',
  [AGG.MIN]: 'min',
  [AGG.MAX]: 'max',
};
