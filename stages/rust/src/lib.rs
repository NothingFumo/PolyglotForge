//! Rust 阶段：语义分析与计划（`qf_plan_build`）。
//!
//! 职责：
//!   1. 把所有 `QF_N_COL` 节点按 `name` 解析到表列，回填 `a` 与 `flags` 类型位；
//!   2. 做保守类型检查（数值/文本分开，算术仅数值，聚合参数仅数值）；
//!   3. 校验 GROUP BY：非聚合 select 项必须是分组表达式；
//!   4. 生成输出 schema（列名优先级：select_names 别名 → 列名 → expr<N>）；
//!   5. 把 ORDER BY 项解析为**输出列**下标，写回节点 `a` 并改成 `QF_N_COL`。
//!
//! 跨边界约定（见 contracts/qf.h、docs/adr/0002）：
//!   - 任何 panic 都在 FFI 边界内被 `catch_unwind` 拦下并转成 `QF_ERR_PANIC`；
//!   - 所有返回给宿主的字符串都走 `host->str`，不在 Rust 堆上留下所有权。

#![allow(non_camel_case_types)]

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;

/* ---------- 与 gen/qf_layout.h 逐字段对齐的 FFI 结构 ---------- */

pub type qf_err = i32;

pub const QF_ERR_OK: qf_err = 0;
pub const QF_ERR_ARG: qf_err = 1;
pub const QF_ERR_TYPE: qf_err = 4;
pub const QF_ERR_PLAN: qf_err = 5;
pub const QF_ERR_PANIC: qf_err = 8;

pub const QF_T_NULL: i32 = 0;
pub const QF_T_INT: i32 = 1;
pub const QF_T_DOUBLE: i32 = 2;
pub const QF_T_TEXT: i32 = 3;

pub const QF_N_LIT: u8 = 1;
pub const QF_N_COL: u8 = 2;
pub const QF_N_BIN: u8 = 3;
pub const QF_N_UNARY: u8 = 4;
pub const QF_N_CALL: u8 = 5;
pub const QF_N_AGG: u8 = 6;
pub const QF_N_STAR: u8 = 7;

pub const QF_OP_EQ: u8 = 1;
pub const QF_OP_NE: u8 = 2;
pub const QF_OP_LT: u8 = 3;
pub const QF_OP_LE: u8 = 4;
pub const QF_OP_GT: u8 = 5;
pub const QF_OP_GE: u8 = 6;
pub const QF_OP_AND: u8 = 7;
pub const QF_OP_OR: u8 = 8;
pub const QF_OP_NOT: u8 = 9;
pub const QF_OP_ADD: u8 = 10;
pub const QF_OP_SUB: u8 = 11;
pub const QF_OP_MUL: u8 = 12;
pub const QF_OP_DIV: u8 = 13;
pub const QF_OP_MOD: u8 = 14;
pub const QF_OP_NEG: u8 = 15;

pub const QF_AGG_COUNT: u8 = 1;
pub const QF_AGG_SUM: u8 = 2;
pub const QF_AGG_AVG: u8 = 3;
pub const QF_AGG_MIN: u8 = 4;
pub const QF_AGG_MAX: u8 = 5;

pub const QF_F_TYPE_MASK: u16 = 0x000f;
pub const QF_F_DESC: u16 = 0x0010;
pub const QF_F_RESOLVED: u16 = 0x0020;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_str {
    pub ptr: *const u8,
    pub len: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union qf_value_union {
    pub i: i64,
    pub d: f64,
    pub s: qf_str,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_value {
    pub type_: i32,
    pub as_: qf_value_union,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_node {
    pub kind: u8,
    pub op: u8,
    pub flags: u16,
    pub a: i32,
    pub b: i32,
    pub argc: i32,
    pub lit: qf_value,
    pub name: qf_str,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_col {
    pub name: qf_str,
    pub type_: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_schema {
    pub count: u32,
    pub cols: *mut qf_col,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_diag {
    pub code: i32,
    pub line: u32,
    pub col: u32,
    pub message: qf_str,
    pub hint: qf_str,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_ast {
    pub nodes: *mut qf_node,
    pub node_count: u32,

    pub select: *mut i32,
    pub select_count: u32,

    /// 与 select 等长：第 i 项的输出列名（AS 别名），无别名时为空串。
    pub select_names: *mut qf_str,

    pub from_table: qf_str,
    pub where_root: i32,

    pub group_by: *mut i32,
    pub group_count: u32,

    pub order_by: *mut i32,
    pub order_count: u32,

    pub limit: i64,
}

pub type qf_alloc_fn =
    unsafe extern "C" fn(ctx: *mut std::ffi::c_void, size: u64, align: u64) -> *mut std::ffi::c_void;
pub type qf_str_fn = unsafe extern "C" fn(
    ctx: *mut std::ffi::c_void,
    src: *const u8,
    len: u32,
) -> qf_str;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct qf_host {
    pub alloc: Option<qf_alloc_fn>,
    pub str: Option<qf_str_fn>,
    pub ctx: *mut std::ffi::c_void,
}

const EMPTY_STR: qf_str = qf_str {
    ptr: std::ptr::null(),
    len: 0,
};

/* ABI 布局防漂移：与 gen/qf_layout.h 的 QF_STATIC_ASSERT 对应。 */
const _: () = assert!(std::mem::size_of::<qf_str>() == 16);
const _: () = assert!(std::mem::size_of::<qf_value>() == 24);
const _: () = assert!(std::mem::size_of::<qf_node>() == 56);
const _: () = assert!(std::mem::size_of::<qf_col>() == 24);
const _: () = assert!(std::mem::size_of::<qf_diag>() == 48);
const _: () = assert!(std::mem::size_of::<qf_ast>() == 104);

// 布局由 schema/qf.ts 生成，防止手写结构漂移。
include!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../gen/qf_layout_assert.rs"));

/* ---------- 诊断 ---------- */

struct PlanError {
    code: qf_err,
    message: String,
    hint: String,
}

fn terr(message: impl Into<String>, hint: impl Into<String>) -> PlanError {
    PlanError {
        code: QF_ERR_TYPE,
        message: message.into(),
        hint: hint.into(),
    }
}

fn perr(message: impl Into<String>, hint: impl Into<String>) -> PlanError {
    PlanError {
        code: QF_ERR_PLAN,
        message: message.into(),
        hint: hint.into(),
    }
}

type PResult<T> = Result<T, PlanError>;

fn type_name(t: i32) -> &'static str {
    match t {
        QF_T_NULL => "空值",
        QF_T_INT => "整数",
        QF_T_DOUBLE => "浮点",
        QF_T_TEXT => "文本",
        _ => "未知类型",
    }
}

unsafe fn as_bytes<'a>(s: &qf_str) -> &'a [u8] {
    if s.ptr.is_null() || s.len == 0 {
        &[]
    } else {
        slice::from_raw_parts(s.ptr, s.len as usize)
    }
}

fn display(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// 列名匹配：先精确，再 ASCII 大小写不敏感（SQL 标识符大小写不敏感）。
/// 解析阶段若已归一化大小写，第一步就会命中。
fn name_matches(a: &[u8], b: &[u8]) -> bool {
    a == b || a.eq_ignore_ascii_case(b)
}

/* ---------- 宿主服务 ---------- */

unsafe fn make_str(host: *const qf_host, bytes: &[u8]) -> qf_str {
    if host.is_null() {
        return EMPTY_STR;
    }
    match (*host).str {
        Some(f) => f((*host).ctx, bytes.as_ptr(), bytes.len() as u32),
        None => EMPTY_STR,
    }
}

unsafe fn set_diag(
    host: *const qf_host,
    diag: *mut qf_diag,
    code: qf_err,
    message: &str,
    hint: &str,
) {
    if diag.is_null() {
        return;
    }
    let d = &mut *diag;
    d.code = code;
    d.line = 0;
    d.col = 0;
    d.message = make_str(host, message.as_bytes());
    d.hint = make_str(host, hint.as_bytes());
}

unsafe fn clear_diag(diag: *mut qf_diag) {
    if diag.is_null() {
        return;
    }
    let d = &mut *diag;
    d.code = QF_ERR_OK;
    d.line = 0;
    d.col = 0;
    d.message = EMPTY_STR;
    d.hint = EMPTY_STR;
}

/* ---------- 计划器 ---------- */

struct Planner {
    host: *const qf_host,
    ast: *mut qf_ast,
    table: *const qf_schema,
    node_count: usize,
    /// ORDER BY 顶层节点下标：这些节点最后才改写成“输出列引用”，
    /// 不能提前按表列名解析。
    order_top: Vec<usize>,
    order_is_top: Vec<bool>,
    /// 递归类型推导时的环检测。
    in_progress: Vec<bool>,
}

impl Planner {
    unsafe fn new(
        host: *const qf_host,
        ast: *mut qf_ast,
        table: *const qf_schema,
    ) -> PResult<Self> {
        let a = &*ast;
        let node_count = a.node_count as usize;
        if node_count > 0 && a.nodes.is_null() {
            return Err(perr(
                "AST 节点数组为空但节点数不为零",
                "语法阶段应保证 nodes 与 node_count 一致",
            ));
        }
        if a.select_count > 0 && a.select.is_null() {
            return Err(perr(
                "AST select 数组为空但列数不为零",
                "语法阶段应保证 select 与 select_count 一致",
            ));
        }
        if a.select_count == 0 {
            return Err(perr("查询缺少 select 项", "至少选择一列或一个聚合"));
        }
        if a.select_names.is_null() {
            return Err(perr(
                "select_names 数组缺失",
                "语法阶段应分配与 select 等长的别名数组（无别名为空串）",
            ));
        }
        if a.group_count > 0 && a.group_by.is_null() {
            return Err(perr(
                "group_by 数组为空但分组数不为零",
                "语法阶段应保证 group_by 与 group_count 一致",
            ));
        }
        if a.order_count > 0 && a.order_by.is_null() {
            return Err(perr(
                "order_by 数组为空但排序项不为零",
                "语法阶段应保证 order_by 与 order_count 一致",
            ));
        }
        let t = &*table;
        if t.count > 0 && t.cols.is_null() {
            return Err(perr(
                "表结构缺少列数组",
                "宿主应传入带 cols 的 qf_schema",
            ));
        }
        Ok(Planner {
            host,
            ast,
            table,
            node_count,
            order_top: Vec::new(),
            order_is_top: vec![false; node_count],
            in_progress: vec![false; node_count],
        })
    }

    unsafe fn node_ptr(&self, idx: i32) -> Option<*mut qf_node> {
        if idx < 0 || idx as usize >= self.node_count {
            return None;
        }
        let base = (*self.ast).nodes;
        if base.is_null() {
            return None;
        }
        Some(base.add(idx as usize))
    }

    unsafe fn required_node(&self, idx: i32) -> PResult<*mut qf_node> {
        self.node_ptr(idx)
            .ok_or_else(|| perr("节点下标越界", "AST 节点数组与下标不一致"))
    }

    unsafe fn host_alloc(&self, size: usize, align: usize) -> *mut u8 {
        if self.host.is_null() {
            return std::ptr::null_mut();
        }
        match (*self.host).alloc {
            Some(f) => f((*self.host).ctx, size as u64, align as u64) as *mut u8,
            None => std::ptr::null_mut(),
        }
    }

    unsafe fn find_column(&self, name: &[u8]) -> Option<usize> {
        let t = &*self.table;
        if t.cols.is_null() {
            return None;
        }
        let cols = slice::from_raw_parts(t.cols, t.count as usize);
        if let Some(i) = cols.iter().position(|c| as_bytes(&c.name) == name) {
            return Some(i);
        }
        cols.iter()
            .position(|c| as_bytes(&c.name).eq_ignore_ascii_case(name))
    }

    unsafe fn col_type(&self, idx: usize) -> i32 {
        let t = &*self.table;
        let cols = slice::from_raw_parts(t.cols, t.count as usize);
        cols[idx].type_
    }

    unsafe fn table_name(&self) -> String {
        display(as_bytes(&(*self.ast).from_table))
    }

    /// 解析单个列节点：`a` 写列下标，`flags` 低位写 `QF_T_*`（保留 DESC），并置 RESOLVED。
    unsafe fn resolve_col(&self, p: *mut qf_node) -> PResult<i32> {
        let n = *p;
        let name = as_bytes(&n.name);
        if name.is_empty() {
            return Err(terr(
                "列节点缺少列名",
                "语法阶段应为列节点填入 name",
            ));
        }
        let table = self.table_name();
        let idx = self.find_column(name).ok_or_else(|| {
            terr(
                format!("表 {} 中不存在列：{}", table, display(name)),
                "确认 FROM 表的列名拼写是否正确",
            )
        })?;
        let ty = self.col_type(idx);
        if !(0..=3).contains(&ty) {
            return Err(perr(
                format!("列 {} 的类型编码非法：{}", display(name), ty),
                "表结构的 qf_type 只允许 0..3",
            ));
        }
        let node = &mut *p;
        node.a = idx as i32;
        node.flags =
            (node.flags & QF_F_DESC) | ((ty as u16) & QF_F_TYPE_MASK) | QF_F_RESOLVED;
        Ok(ty)
    }

    unsafe fn set_type(&self, p: *mut qf_node, ty: i32) {
        let node = &mut *p;
        node.flags =
            (node.flags & QF_F_DESC) | ((ty as u16) & QF_F_TYPE_MASK) | QF_F_RESOLVED;
    }

    /// 标记从 root 可达的所有节点（用于发现 ORDER BY 节点被其它子句复用）。
    unsafe fn mark_reachable(&self, root: i32, mark: &mut [bool]) {
        let mut stack = vec![root];
        while let Some(idx) = stack.pop() {
            if idx < 0 || idx as usize >= self.node_count {
                continue;
            }
            let i = idx as usize;
            if mark[i] {
                continue;
            }
            mark[i] = true;
            let n = *match self.node_ptr(idx) {
                Some(p) => p,
                None => continue,
            };
            match n.kind {
                QF_N_BIN => {
                    stack.push(n.a);
                    stack.push(n.b);
                }
                QF_N_UNARY => stack.push(n.a),
                QF_N_CALL | QF_N_AGG => {
                    if n.argc > 0 {
                        for k in 0..n.argc {
                            stack.push(n.b + k);
                        }
                    }
                }
                _ => {}
            }
        }
    }

    fn is_numeric(t: i32) -> bool {
        t == QF_T_INT || t == QF_T_DOUBLE
    }

    fn is_logical(t: i32) -> bool {
        t == QF_T_INT || t == QF_T_NULL
    }

    fn comparable(l: i32, r: i32) -> bool {
        if l == QF_T_NULL || r == QF_T_NULL {
            return true;
        }
        (Self::is_numeric(l) && Self::is_numeric(r))
            || (l == QF_T_TEXT && r == QF_T_TEXT)
    }

    /// 递归类型推导。返回 `QF_T_*`；同时把类型写回节点 flags。
    unsafe fn infer(&mut self, idx: i32, depth: u32, allow_agg: bool) -> PResult<i32> {
        if depth > 256 {
            return Err(perr(
                "表达式嵌套过深",
                "检查 AST 是否成环或异常深",
            ));
        }
        let p = self.required_node(idx)?;
        let i = idx as usize;
        if self.in_progress[i] {
            return Err(perr(
                "AST 存在环",
                "语法阶段应生成树状 AST，而不是共享/循环节点",
            ));
        }
        self.in_progress[i] = true;
        let n = *p;
        let ty = match n.kind {
            QF_N_LIT => {
                if !(0..=3).contains(&n.lit.type_) {
                    return Err(terr(
                        format!("字面量类型编码非法：{}", n.lit.type_),
                        "字面量类型只允许 0..3",
                    ));
                }
                n.lit.type_
            }
            QF_N_COL => {
                if n.flags & QF_F_RESOLVED != 0 {
                    (n.flags & QF_F_TYPE_MASK) as i32
                } else {
                    self.resolve_col(p)?
                }
            }
            QF_N_STAR => {
                return Err(perr(
                    "暂不支持 SELECT *",
                    "请显式列出需要的列名（本阶段不改变节点数组规模）",
                ));
            }
            QF_N_BIN => {
                let lt = self.infer(n.a, depth + 1, false)?;
                let rt = self.infer(n.b, depth + 1, false)?;
                match n.op {
                    QF_OP_EQ | QF_OP_NE | QF_OP_LT | QF_OP_LE | QF_OP_GT | QF_OP_GE => {
                        if !Self::comparable(lt, rt) {
                            return Err(terr(
                                format!(
                                    "比较两侧类型不兼容：{}({}) 与 {}({})",
                                    self.describe(n.a),
                                    type_name(lt),
                                    self.describe(n.b),
                                    type_name(rt)
                                ),
                                "数值只能与数值比较，文本只能与文本比较",
                            ));
                        }
                        QF_T_INT
                    }
                    QF_OP_AND | QF_OP_OR => {
                        if !Self::is_logical(lt) || !Self::is_logical(rt) {
                            return Err(terr(
                                format!(
                                    "逻辑运算两侧必须是布尔（整数）：{}({}) 与 {}({})",
                                    self.describe(n.a),
                                    type_name(lt),
                                    self.describe(n.b),
                                    type_name(rt)
                                ),
                                "AND/OR 只接受比较结果或整数列",
                            ));
                        }
                        QF_T_INT
                    }
                    QF_OP_ADD | QF_OP_SUB | QF_OP_MUL | QF_OP_DIV | QF_OP_MOD => {
                        if !(Self::is_numeric(lt) || lt == QF_T_NULL)
                            || !(Self::is_numeric(rt) || rt == QF_T_NULL)
                        {
                            return Err(terr(
                                format!(
                                    "算术运算两侧必须是数值：{}({}) 与 {}({})",
                                    self.describe(n.a),
                                    type_name(lt),
                                    self.describe(n.b),
                                    type_name(rt)
                                ),
                                "文本列不能参与 +、-、*、/、%",
                            ));
                        }
                        if lt == QF_T_NULL || rt == QF_T_NULL {
                            QF_T_NULL
                        } else if lt == QF_T_INT && rt == QF_T_INT {
                            QF_T_INT
                        } else {
                            QF_T_DOUBLE
                        }
                    }
                    _ => {
                        return Err(perr(
                            format!("未知二元运算符：{}", n.op),
                            "检查语法阶段的算子编码",
                        ));
                    }
                }
            }
            QF_N_UNARY => {
                let vt = self.infer(n.a, depth + 1, false)?;
                match n.op {
                    QF_OP_NOT => {
                        if !Self::is_logical(vt) {
                            return Err(terr(
                                format!(
                                    "NOT 的操作数必须是布尔（整数）：{}({})",
                                    self.describe(n.a),
                                    type_name(vt)
                                ),
                                "NOT 只接受比较结果或整数列",
                            ));
                        }
                        QF_T_INT
                    }
                    QF_OP_NEG => {
                        if !(Self::is_numeric(vt) || vt == QF_T_NULL) {
                            return Err(terr(
                                format!(
                                    "负号只能作用于数值：{}({})",
                                    self.describe(n.a),
                                    type_name(vt)
                                ),
                                "文本列不能取负",
                            ));
                        }
                        vt
                    }
                    _ => {
                        return Err(perr(
                            format!("未知一元运算符：{}", n.op),
                            "检查语法阶段的算子编码",
                        ));
                    }
                }
            }
            QF_N_CALL => {
                if n.argc < 0 || (n.argc > 0 && n.b < 0) {
                    return Err(perr(
                        "函数调用的参数区间非法",
                        "检查 CALL 节点的 b/argc",
                    ));
                }
                for k in 0..n.argc {
                    self.infer(n.b + k, depth + 1, false)?;
                }
                // UDF 的返回类型在运行期才知道，这里只能标记为“未知/空值”。
                QF_T_NULL
            }
            QF_N_AGG => {
                if !allow_agg {
                    return Err(perr(
                        "聚合函数只能作为 select 项出现",
                        "GROUP BY / WHERE / 表达式内部不支持嵌套聚合",
                    ));
                }
                if n.argc < 0 || (n.argc > 0 && n.b < 0) {
                    return Err(perr(
                        "聚合函数的参数区间非法",
                        "检查 AGG 节点的 b/argc",
                    ));
                }
                match n.op {
                    QF_AGG_COUNT => {
                        if n.argc > 1 {
                            return Err(perr(
                                "count 只接受零个或一个参数",
                                "count(*) 用 argc=0 表示",
                            ));
                        }
                        if n.argc == 1 {
                            self.infer(n.b, depth + 1, false)?;
                        }
                        QF_T_INT
                    }
                    QF_AGG_SUM | QF_AGG_AVG => {
                        if n.argc != 1 {
                            return Err(perr(
                                "sum/avg 必须恰好有一个参数",
                                "检查聚合节点的 argc",
                            ));
                        }
                        let at = self.infer(n.b, depth + 1, false)?;
                        if !(Self::is_numeric(at) || at == QF_T_NULL) {
                            return Err(terr(
                                format!(
                                    "聚合 {} 的参数必须是数值：{}",
                                    if n.op == QF_AGG_SUM { "sum" } else { "avg" },
                                    type_name(at)
                                ),
                                "先确认参数列是整数或浮点列",
                            ));
                        }
                        // sum 保持参数类型：整数输入输出整数，浮点输入输出浮点，
                        // 与 C 引擎的 agg_finish 及 sqlite 一致；avg 恒为 double。
                        if n.op == QF_AGG_AVG { QF_T_DOUBLE } else { at }
                    }
                    QF_AGG_MIN | QF_AGG_MAX => {
                        if n.argc != 1 {
                            return Err(perr(
                                "min/max 必须恰好有一个参数",
                                "检查聚合节点的 argc",
                            ));
                        }
                        let at = self.infer(n.b, depth + 1, false)?;
                        if at != QF_T_NULL
                            && !Self::is_numeric(at)
                            && at != QF_T_TEXT
                        {
                            return Err(terr(
                                "min/max 的参数类型不可比较",
                                "只支持整数、浮点、文本列",
                            ));
                        }
                        at
                    }
                    _ => {
                        return Err(perr(
                            format!("未知聚合算子：{}", n.op),
                            "检查语法阶段的聚合编码",
                        ));
                    }
                }
            }
            other => {
                return Err(perr(
                    format!("未知节点类型：{}", other),
                    "检查语法阶段是否写坏了 qf_node.kind",
                ));
            }
        };
        self.in_progress[i] = false;
        self.set_type(p, ty);
        Ok(ty)
    }

    /// 结构相等：用于把 ORDER BY 表达式对应到 select 表达式。
    /// 列节点优先比较列名（ORDER BY 顶层节点尚未按表解析），其余按算子与子节点递归。
    unsafe fn expr_equal(&self, l: i32, r: i32, depth: u32) -> bool {
        if depth > 256 {
            return false;
        }
        let lp = match self.node_ptr(l) {
            Some(p) => *p,
            None => return false,
        };
        let rp = match self.node_ptr(r) {
            Some(p) => *p,
            None => return false,
        };
        if lp.kind != rp.kind {
            return false;
        }
        match lp.kind {
            QF_N_COL => {
                if (lp.flags & QF_F_RESOLVED) != 0 && (rp.flags & QF_F_RESOLVED) != 0 {
                    // 双方都已解析时直接比列下标，避免大小写不同的列名产生歧义。
                    lp.a == rp.a
                } else {
                    let ln = as_bytes(&lp.name);
                    let rn = as_bytes(&rp.name);
                    if !ln.is_empty() && !rn.is_empty() {
                        name_matches(ln, rn)
                    } else {
                        lp.a == rp.a
                    }
                }
            }
            QF_N_LIT => {
                if lp.lit.type_ != rp.lit.type_ {
                    return false;
                }
                match lp.lit.type_ {
                    QF_T_INT => lp.lit.as_.i == rp.lit.as_.i,
                    QF_T_DOUBLE => lp.lit.as_.d.to_bits() == rp.lit.as_.d.to_bits(),
                    QF_T_TEXT => {
                        as_bytes(&lp.lit.as_.s) == as_bytes(&rp.lit.as_.s)
                    }
                    _ => true,
                }
            }
            QF_N_STAR => true,
            QF_N_UNARY => lp.op == rp.op && self.expr_equal(lp.a, rp.a, depth + 1),
            QF_N_BIN => {
                lp.op == rp.op
                    && self.expr_equal(lp.a, rp.a, depth + 1)
                    && self.expr_equal(lp.b, rp.b, depth + 1)
            }
            QF_N_CALL | QF_N_AGG => {
                if lp.op != rp.op || lp.argc != rp.argc {
                    return false;
                }
                if lp.kind == QF_N_CALL && !name_matches(as_bytes(&lp.name), as_bytes(&rp.name)) {
                    return false;
                }
                for k in 0..lp.argc {
                    if !self.expr_equal(lp.b + k, rp.b + k, depth + 1) {
                        return false;
                    }
                }
                true
            }
            _ => lp.op == rp.op && lp.a == rp.a && lp.b == rp.b,
        }
    }

    unsafe fn describe(&self, idx: i32) -> String {
        if let Some(p) = self.node_ptr(idx) {
            let n = *p;
            let name = as_bytes(&n.name);
            if !name.is_empty() {
                return display(name);
            }
            if n.kind == QF_N_COL && n.a >= 0 {
                if let Some(cols) = self.table_cols() {
                    if let Some(c) = cols.get(n.a as usize) {
                        return display(as_bytes(&c.name));
                    }
                }
            }
        }
        format!("表达式#{}", idx)
    }

    unsafe fn table_cols(&self) -> Option<&'static [qf_col]> {
        let t = &*self.table;
        if t.cols.is_null() {
            return None;
        }
        Some(slice::from_raw_parts(t.cols, t.count as usize))
    }

    /// 主流程。
    unsafe fn execute(&mut self, out_schema: *mut qf_schema) -> PResult<()> {
        let ast = *self.ast;
        let select = slice::from_raw_parts(ast.select, ast.select_count as usize);
        let select_names: &[qf_str] =
            slice::from_raw_parts(ast.select_names, ast.select_count as usize);
        let group: &[i32] = if ast.group_count > 0 {
            slice::from_raw_parts(ast.group_by, ast.group_count as usize)
        } else {
            &[]
        };
        let order: &[i32] = if ast.order_count > 0 {
            slice::from_raw_parts(ast.order_by, ast.order_count as usize)
        } else {
            &[]
        };

        /* 1. 收集 ORDER BY 顶层节点并检查边界/重复。 */
        for &oi in order {
            if self.node_ptr(oi).is_none() {
                return Err(perr(
                    format!("ORDER BY 节点下标越界：{}", oi),
                    "AST 的 order_by 必须指向有效节点",
                ));
            }
            let i = oi as usize;
            if self.order_is_top[i] {
                return Err(perr(
                    "ORDER BY 出现重复/共享节点",
                    "每个排序项应使用独立节点，语义阶段要回写下标",
                ));
            }
            self.order_is_top[i] = true;
            self.order_top.push(i);
        }

        /* 2. select/where/group 用到的节点不允许被 ORDER BY 复用：
              改写 ORDER BY 节点的 a 会破坏这些表达式的列引用。 */
        let mut used = vec![false; self.node_count];
        for &s in select {
            self.mark_reachable(s, &mut used);
        }
        if ast.where_root >= 0 {
            self.mark_reachable(ast.where_root, &mut used);
        }
        for &g in group {
            self.mark_reachable(g, &mut used);
        }
        for &oi in &self.order_top {
            if used[oi] {
                return Err(perr(
                    "ORDER BY 项复用了 select/WHERE/GROUP BY 的节点",
                    "语法阶段应为每个 ORDER BY 项建立独立节点，否则回写输出列下标会污染其它子句",
                ));
            }
        }

        /* 3. 解析所有列节点。ORDER BY 子树整体跳过：它们不是表列引用，
              而是“输出列”引用；子节点只在结构比较时用到名字，无需解析。 */
        let mut order_subtree = vec![false; self.node_count];
        for &oi in &self.order_top {
            self.mark_reachable(oi as i32, &mut order_subtree);
        }
        {
            let base = ast.nodes;
            for i in 0..self.node_count {
                if order_subtree[i] {
                    continue;
                }
                let p = base.add(i);
                if (*p).kind == QF_N_COL {
                    self.resolve_col(p)?;
                }
            }
        }

        /* 4. 类型推导 + 子句校验。 */
        let mut select_types = Vec::with_capacity(select.len());
        let mut select_has_agg = Vec::with_capacity(select.len());
        for &s in select {
            let ty = self.infer(s, 0, true)?;
            let is_agg = match self.node_ptr(s) {
                Some(p) => (*p).kind == QF_N_AGG,
                None => false,
            };
            select_types.push(ty);
            select_has_agg.push(is_agg);
        }

        if ast.where_root != -1 {
            if ast.where_root < 0 {
                return Err(perr(
                    format!("where_root 非法：{}", ast.where_root),
                    "-1 表示没有 WHERE",
                ));
            }
            let wt = self.infer(ast.where_root, 0, false)?;
            if wt != QF_T_INT && wt != QF_T_NULL {
                return Err(terr(
                    format!("WHERE 谓词必须是布尔：{}", type_name(wt)),
                    "WHERE 只接受比较/逻辑表达式",
                ));
            }
        }

        for &g in group {
            self.infer(g, 0, false)?;
        }

        /* 5. GROUP BY 校验：非聚合 select 项必须等于某个分组表达式。 */
        let any_agg_select = select_has_agg.iter().any(|&b| b);
        if group.is_empty() {
            if any_agg_select {
                for (i, &is_agg) in select_has_agg.iter().enumerate() {
                    if !is_agg {
                        return Err(perr(
                            format!(
                                "select 第 {} 项是非聚合表达式，但查询没有 GROUP BY",
                                i + 1
                            ),
                            "混用聚合与非聚合时必须显式 GROUP BY，或把该项也改成聚合",
                        ));
                    }
                }
            }
        } else {
            for (i, &s) in select.iter().enumerate() {
                if select_has_agg[i] {
                    continue;
                }
                let grouped = group.iter().any(|&g| self.expr_equal(s, g, 0));
                if !grouped {
                    return Err(perr(
                        format!(
                            "非聚合 select 项必须出现在 GROUP BY 中：{}",
                            self.describe(s)
                        ),
                        "把该表达式加入 GROUP BY，或把它改成聚合函数",
                    ));
                }
            }
        }

        /* 6. 生成输出 schema。 */
        let count = select.len();
        let col_size = std::mem::size_of::<qf_col>();
        let col_align = std::mem::align_of::<qf_col>();
        let bytes = count.checked_mul(col_size).ok_or_else(|| {
            perr("输出列数过大", "select 项数量超出可分配范围")
        })?;
        let cols_mem = self.host_alloc(bytes, col_align);
        if cols_mem.is_null() {
            return Err(perr(
                "宿主分配输出 schema 失败",
                "检查 qf_host.alloc 是否可用或 arena 是否耗尽",
            ));
        }
        let cols = cols_mem as *mut qf_col;

        for (i, &s) in select.iter().enumerate() {
            let p = self.required_node(s)?;
            let n = *p;
            let alias = as_bytes(&select_names[i]);
            let name_bytes: Vec<u8> = if !alias.is_empty() {
                alias.to_vec()
            } else if n.kind == QF_N_COL {
                let cols_ref = self.table_cols().ok_or_else(|| {
                    perr("表结构缺少列数组", "宿主应传入带 cols 的 qf_schema")
                })?;
                let idx = n.a;
                if idx < 0 || idx as usize >= cols_ref.len() {
                    return Err(perr(
                        "列节点下标越界",
                        "检查列解析是否成功",
                    ));
                }
                as_bytes(&cols_ref[idx as usize].name).to_vec()
            } else {
                format!("expr{}", i + 1).into_bytes()
            };
            let out_name = make_str(self.host, &name_bytes);
            if out_name.ptr.is_null() && !name_bytes.is_empty() {
                return Err(perr(
                    "宿主分配输出列名失败",
                    "检查 qf_host.str 是否可用或 arena 是否耗尽",
                ));
            }
            let out_col = &mut *cols.add(i);
            out_col.name = out_name;
            out_col.type_ = select_types[i];
        }

        /* 7. ORDER BY → 输出列下标。 */
        for &oi in order {
            let p = self.required_node(oi)?;
            let n = *p;
            let mut target: Option<usize> = None;

            if n.kind == QF_N_LIT && n.lit.type_ == QF_T_INT {
                let v = n.lit.as_.i;
                if v >= 1 && v <= count as i64 {
                    target = Some((v - 1) as usize);
                } else {
                    return Err(terr(
                        format!(
                            "ORDER BY 序号 {} 超出输出列范围（共 {} 列）",
                            v, count
                        ),
                        "序号从 1 开始，且不超过 select 列数",
                    ));
                }
            } else {
                let want = as_bytes(&n.name);
                if !want.is_empty() {
                    // 输出列名已按 select_names → 列名 → expr<N> 生成，
                    // 因此这里天然覆盖 AS 别名与列名两种写法。
                    for i in 0..count {
                        if as_bytes(&(*cols.add(i)).name) == want {
                            target = Some(i);
                            break;
                        }
                    }
                    if target.is_none() {
                        for i in 0..count {
                            if as_bytes(&(*cols.add(i)).name).eq_ignore_ascii_case(want) {
                                target = Some(i);
                                break;
                            }
                        }
                    }
                }
                if target.is_none() {
                    for (i, &s) in select.iter().enumerate() {
                        if self.expr_equal(oi, s, 0) {
                            target = Some(i);
                            break;
                        }
                    }
                }
                if target.is_none() {
                    let label = if want.is_empty() {
                        format!("表达式#{}", oi)
                    } else {
                        display(want)
                    };
                    return Err(terr(
                        format!("ORDER BY 无法对应到输出列：{}", label),
                        "按 select 别名、输出列名或列序号排序",
                    ));
                }
            }

            let idx = target.unwrap();
            let out_ty = select_types[idx];
            let node = &mut *p;
            node.kind = QF_N_COL;
            node.a = idx as i32;
            node.b = 0;
            node.argc = 0;
            node.flags =
                (node.flags & QF_F_DESC) | ((out_ty as u16) & QF_F_TYPE_MASK) | QF_F_RESOLVED;
        }

        (*out_schema).count = count as u32;
        (*out_schema).cols = cols;
        Ok(())
    }
}

/* ---------- 导出入口 ---------- */

#[no_mangle]
pub extern "C" fn qf_plan_build(
    host: *const qf_host,
    ast: *mut qf_ast,
    table: *const qf_schema,
    out_schema: *mut qf_schema,
    diag: *mut qf_diag,
) -> qf_err {
    let outcome = catch_unwind(AssertUnwindSafe(|| unsafe {
        run(host, ast, table, out_schema, diag)
    }));
    match outcome {
        Ok(code) => code,
        Err(_) => unsafe {
            // 宿主指针可能无效，set_diag 内部会做空指针检查。
            set_diag(
                host,
                diag,
                QF_ERR_PANIC,
                "Rust 语义阶段内部错误（panic 已被拦截）",
                "检查 AST 节点数组、下标与宿主分配器是否一致",
            );
            QF_ERR_PANIC
        },
    }
}

unsafe fn run(
    host: *const qf_host,
    ast: *mut qf_ast,
    table: *const qf_schema,
    out_schema: *mut qf_schema,
    diag: *mut qf_diag,
) -> qf_err {
    if host.is_null() {
        if !diag.is_null() {
            let msg: &[u8] = "qf_host 为空".as_bytes();
            let d = &mut *diag;
            d.code = QF_ERR_ARG;
            d.line = 0;
            d.col = 0;
            d.message = qf_str { ptr: msg.as_ptr(), len: msg.len() as u32 };
            d.hint = EMPTY_STR;
        }
        return QF_ERR_ARG;
    }
    let h = &*host;
    if h.alloc.is_none() || h.str.is_none() {
        set_diag(
            host,
            diag,
            QF_ERR_ARG,
            "qf_host 缺少 alloc/str 服务",
            "宿主必须提供内存分配与字符串驻留函数",
        );
        return QF_ERR_ARG;
    }
    if ast.is_null() || table.is_null() || out_schema.is_null() {
        set_diag(
            host,
            diag,
            QF_ERR_ARG,
            "qf_plan_build 收到空指针参数",
            "ast/table/out_schema 都不能为空",
        );
        return QF_ERR_ARG;
    }

    let result = Planner::new(host, ast, table).and_then(|mut p| p.execute(out_schema));
    match result {
        Ok(()) => {
            clear_diag(diag);
            QF_ERR_OK
        }
        Err(e) => {
            set_diag(host, diag, e.code, &e.message, &e.hint);
            e.code
        }
    }
}

/* ---------- 单元测试（cargo test） ---------- */

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{alloc_zeroed, Layout};
    use std::ffi::c_void;

    unsafe extern "C" fn test_alloc(_ctx: *mut c_void, size: u64, align: u64) -> *mut c_void {
        let layout = Layout::from_size_align(size.max(1) as usize, align.max(1) as usize).unwrap();
        alloc_zeroed(layout) as *mut c_void
    }

    unsafe extern "C" fn test_str(_ctx: *mut c_void, src: *const u8, len: u32) -> qf_str {
        if len == 0 {
            return EMPTY_STR;
        }
        let layout = Layout::from_size_align(len as usize, 1).unwrap();
        let p = alloc_zeroed(layout);
        std::ptr::copy_nonoverlapping(src, p, len as usize);
        qf_str {
            ptr: p as *const u8,
            len,
        }
    }

    fn host() -> qf_host {
        qf_host {
            alloc: Some(test_alloc),
            str: Some(test_str),
            ctx: std::ptr::null_mut(),
        }
    }

    fn s(bytes: &'static [u8]) -> qf_str {
        qf_str {
            ptr: bytes.as_ptr(),
            len: bytes.len() as u32,
        }
    }

    fn node(kind: u8) -> qf_node {
        let mut n: qf_node = unsafe { std::mem::zeroed() };
        n.kind = kind;
        n
    }

    fn lit_int(v: i64) -> qf_node {
        let mut n = node(QF_N_LIT);
        n.lit.type_ = QF_T_INT;
        n.lit.as_.i = v;
        n
    }

    fn lit_text(v: &'static [u8]) -> qf_node {
        let mut n = node(QF_N_LIT);
        n.lit.type_ = QF_T_TEXT;
        n.lit.as_.s = s(v);
        n
    }

    fn col(name: &'static [u8]) -> qf_node {
        let mut n = node(QF_N_COL);
        n.name = s(name);
        n.a = -1;
        n
    }

    fn bin(op: u8, a: i32, b: i32) -> qf_node {
        let mut n = node(QF_N_BIN);
        n.op = op;
        n.a = a;
        n.b = b;
        n
    }

    fn agg(op: u8, arg: i32, argc: i32) -> qf_node {
        let mut n = node(QF_N_AGG);
        n.op = op;
        n.b = arg;
        n.argc = argc;
        n
    }

    fn table() -> Vec<qf_col> {
        vec![
            qf_col {
                name: s(b"name"),
                type_: QF_T_TEXT,
            },
            qf_col {
                name: s(b"age"),
                type_: QF_T_INT,
            },
            qf_col {
                name: s(b"score"),
                type_: QF_T_DOUBLE,
            },
        ]
    }

    struct Fixture {
        nodes: Vec<qf_node>,
        select: Vec<i32>,
        select_names: Vec<qf_str>,
        group: Vec<i32>,
        order: Vec<i32>,
        cols: Vec<qf_col>,
        ast: qf_ast,
        out_schema: qf_schema,
        diag: qf_diag,
        h: qf_host,
    }

    impl Fixture {
        fn new(nodes: Vec<qf_node>, select: Vec<i32>) -> Self {
            let select_names = vec![EMPTY_STR; select.len()];
            Fixture {
                nodes,
                select,
                select_names,
                group: Vec::new(),
                order: Vec::new(),
                cols: table(),
                ast: qf_ast {
                    nodes: std::ptr::null_mut(),
                    node_count: 0,
                    select: std::ptr::null_mut(),
                    select_count: 0,
                    select_names: std::ptr::null_mut(),
                    from_table: s(b"people"),
                    where_root: -1,
                    group_by: std::ptr::null_mut(),
                    group_count: 0,
                    order_by: std::ptr::null_mut(),
                    order_count: 0,
                    limit: -1,
                },
                out_schema: qf_schema {
                    count: 0,
                    cols: std::ptr::null_mut(),
                },
                diag: unsafe { std::mem::zeroed() },
                h: host(),
            }
        }

        fn alias(mut self, i: usize, name: &'static [u8]) -> Self {
            self.select_names[i] = s(name);
            self
        }

        fn group(mut self, g: Vec<i32>) -> Self {
            self.group = g;
            self
        }

        fn order(mut self, o: Vec<i32>) -> Self {
            self.order = o;
            self
        }

        fn run(&mut self) -> qf_err {
            self.ast.nodes = self.nodes.as_mut_ptr();
            self.ast.node_count = self.nodes.len() as u32;
            self.ast.select = self.select.as_mut_ptr();
            self.ast.select_count = self.select.len() as u32;
            self.ast.select_names = self.select_names.as_mut_ptr();
            self.ast.group_by = if self.group.is_empty() {
                std::ptr::null_mut()
            } else {
                self.group.as_mut_ptr()
            };
            self.ast.group_count = self.group.len() as u32;
            self.ast.order_by = if self.order.is_empty() {
                std::ptr::null_mut()
            } else {
                self.order.as_mut_ptr()
            };
            self.ast.order_count = self.order.len() as u32;
            qf_plan_build(
                &self.h,
                &mut self.ast,
                &qf_schema {
                    count: self.cols.len() as u32,
                    cols: self.cols.as_mut_ptr(),
                },
                &mut self.out_schema,
                &mut self.diag,
            )
        }

        fn msg(&self) -> String {
            unsafe { String::from_utf8_lossy(as_bytes(&self.diag.message)).into_owned() }
        }

        fn hint(&self) -> String {
            unsafe { String::from_utf8_lossy(as_bytes(&self.diag.hint)).into_owned() }
        }

        fn out_names(&self) -> Vec<String> {
            unsafe {
                let cols = slice::from_raw_parts(self.out_schema.cols, self.out_schema.count as usize);
                cols.iter().map(|c| display(as_bytes(&c.name))).collect()
            }
        }

        fn out_types(&self) -> Vec<i32> {
            unsafe {
                let cols = slice::from_raw_parts(self.out_schema.cols, self.out_schema.count as usize);
                cols.iter().map(|c| c.type_).collect()
            }
        }
    }

    #[test]
    fn resolves_column_indices_and_types() {
        // SELECT age, name FROM people WHERE age > 30
        let nodes = vec![
            col(b"age"),          // 0
            col(b"name"),         // 1
            bin(QF_OP_GT, 0, 3),  // 2: age > 30
            lit_int(30),          // 3
        ];
        let mut f = Fixture::new(nodes, vec![0, 1]);
        f.ast.where_root = 2;
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.nodes[0].a, 1);
        assert_eq!(f.nodes[1].a, 0);
        assert_eq!(f.nodes[0].flags & QF_F_TYPE_MASK, QF_T_INT as u16);
        assert_eq!(f.nodes[1].flags & QF_F_TYPE_MASK, QF_T_TEXT as u16);
        assert_ne!(f.nodes[0].flags & QF_F_RESOLVED, 0);
        assert_eq!(f.out_names(), vec!["age".to_string(), "name".to_string()]);
        assert_eq!(f.out_types(), vec![QF_T_INT, QF_T_TEXT]);
        assert_eq!(f.diag.code, QF_ERR_OK);
    }

    #[test]
    fn unknown_column_reports_name() {
        let nodes = vec![col(b"nope")];
        let mut f = Fixture::new(nodes, vec![0]);
        assert_eq!(f.run(), QF_ERR_TYPE);
        assert!(f.msg().contains("nope"), "message = {}", f.msg());
        assert!(!f.hint().is_empty());
    }

    #[test]
    fn type_mismatch_is_rejected() {
        // SELECT name + 1
        let nodes = vec![bin(QF_OP_ADD, 1, 2), col(b"name"), lit_int(1)];
        let mut f = Fixture::new(nodes, vec![0]);
        assert_eq!(f.run(), QF_ERR_TYPE);
        assert!(f.msg().contains("文本"), "message = {}", f.msg());
        assert!(f.msg().contains("name"), "类型错误应带列名，message = {}", f.msg());

        // WHERE name > 1
        let nodes = vec![bin(QF_OP_GT, 1, 2), col(b"name"), lit_int(1)];
        let mut f = Fixture::new(nodes, vec![1]);
        f.ast.where_root = 0;
        assert_eq!(f.run(), QF_ERR_TYPE);
        assert!(!f.msg().is_empty());

        // SELECT count(name) —— 聚合参数可以是文本
        let nodes = vec![agg(QF_AGG_COUNT, 1, 1), col(b"name")];
        let mut f = Fixture::new(nodes, vec![0]);
        assert_eq!(f.run(), QF_ERR_OK);

        // SELECT sum(name) —— 聚合参数必须数值
        let nodes = vec![agg(QF_AGG_SUM, 1, 1), col(b"name")];
        let mut f = Fixture::new(nodes, vec![0]);
        assert_eq!(f.run(), QF_ERR_TYPE);
    }

    #[test]
    fn aggregate_requires_group_by_consistency() {
        // SELECT name, count(*) FROM people GROUP BY name
        let nodes = vec![
            col(b"name"),                      // 0
            agg(QF_AGG_COUNT, 0, 0),           // 1
            col(b"name"),                      // 2: group by 独立节点
        ];
        let mut f = Fixture::new(nodes, vec![0, 1]).group(vec![2]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.out_names(), vec!["name".to_string(), "expr2".to_string()]);
        assert_eq!(f.out_types(), vec![QF_T_TEXT, QF_T_INT]);

        // SELECT age, count(*) ... GROUP BY name  —— age 不是分组列
        let nodes = vec![
            col(b"age"),
            agg(QF_AGG_COUNT, 0, 0),
            col(b"name"),
        ];
        let mut f = Fixture::new(nodes, vec![0, 1]).group(vec![2]);
        assert_eq!(f.run(), QF_ERR_PLAN);
        assert!(f.msg().contains("GROUP BY"), "message = {}", f.msg());

        // 没有 GROUP BY 却混用聚合与非聚合
        let nodes = vec![col(b"age"), agg(QF_AGG_COUNT, 0, 0)];
        let mut f = Fixture::new(nodes, vec![0, 1]);
        assert_eq!(f.run(), QF_ERR_PLAN);
    }

    #[test]
    fn aggregate_result_types() {
        // SELECT count(*), avg(age), sum(age), min(age), max(score), avg(score)
        let nodes = vec![
            agg(QF_AGG_COUNT, 0, 0), // 0
            agg(QF_AGG_AVG, 6, 1),   // 1
            agg(QF_AGG_SUM, 6, 1),   // 2
            agg(QF_AGG_MIN, 6, 1),   // 3
            agg(QF_AGG_MAX, 7, 1),   // 4
            agg(QF_AGG_AVG, 7, 1),   // 5
            col(b"age"),             // 6
            col(b"score"),           // 7
        ];
        let mut f = Fixture::new(nodes, vec![0, 1, 2, 3, 4, 5]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(
            f.out_types(),
            vec![
                QF_T_INT,    // count
                QF_T_DOUBLE, // avg(age)
                QF_T_INT,    // sum(age)：整数输入保持整数
                QF_T_INT,    // min(age)
                QF_T_DOUBLE, // max(score)
                QF_T_DOUBLE  // avg(score)
            ]
        );
    }

    #[test]
    fn order_by_maps_to_output_columns() {
        // SELECT age + 1 AS next, age FROM people ORDER BY next DESC, 2
        let nodes = vec![
            bin(QF_OP_ADD, 1, 2), // 0: select 表达式，别名 next
            col(b"age"),          // 1
            lit_int(1),           // 2
            col(b"age"),          // 3: 第二个 select 项
            col(b"next"),         // 4: ORDER BY next DESC
            lit_int(2),           // 5: ORDER BY 2
        ];
        let mut nodes = nodes;
        nodes[4].flags |= QF_F_DESC;
        let mut f = Fixture::new(nodes, vec![0, 3])
            .alias(0, b"next")
            .order(vec![4, 5]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.out_names(), vec!["next".to_string(), "age".to_string()]);
        assert_eq!(f.out_types(), vec![QF_T_INT, QF_T_INT]);
        assert_eq!(f.order, vec![4, 5]);
        assert_eq!(f.nodes[4].kind, QF_N_COL);
        assert_eq!(f.nodes[4].a, 0);
        assert_ne!(f.nodes[4].flags & QF_F_DESC, 0);
        assert_eq!(f.nodes[4].flags & QF_F_TYPE_MASK, QF_T_INT as u16);
        assert_eq!(f.nodes[5].kind, QF_N_COL);
        assert_eq!(f.nodes[5].a, 1);
    }

    #[test]
    fn order_by_unknown_target_is_type_error() {
        // SELECT age FROM people ORDER BY nope
        let nodes = vec![col(b"age"), col(b"nope")];
        let mut f = Fixture::new(nodes, vec![0]).order(vec![1]);
        assert_eq!(f.run(), QF_ERR_TYPE);
        assert!(f.msg().contains("nope"), "message = {}", f.msg());

        // ORDER BY 越界序号
        let nodes = vec![col(b"age"), lit_int(5)];
        let mut f = Fixture::new(nodes, vec![0]).order(vec![1]);
        assert_eq!(f.run(), QF_ERR_TYPE);
        assert!(f.msg().contains("5"));
    }

    #[test]
    fn order_by_expression_matches_select_expression() {
        // SELECT age + 1 AS next FROM people ORDER BY age + 1
        let nodes = vec![
            bin(QF_OP_ADD, 1, 2), // 0
            col(b"age"),          // 1
            lit_int(1),           // 2
            bin(QF_OP_ADD, 4, 5), // 3: ORDER BY 表达式
            col(b"age"),          // 4
            lit_int(1),           // 5
        ];
        let mut f = Fixture::new(nodes, vec![0])
            .alias(0, b"next")
            .order(vec![3]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.nodes[3].kind, QF_N_COL);
        assert_eq!(f.nodes[3].a, 0);
    }

    #[test]
    fn argument_errors_do_not_allocate() {
        let h = host();
        assert_eq!(
            qf_plan_build(
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut()
            ),
            QF_ERR_ARG
        );
        let mut out = qf_schema {
            count: 0,
            cols: std::ptr::null_mut(),
        };
        assert_eq!(
            qf_plan_build(
                &h,
                std::ptr::null_mut(),
                std::ptr::null(),
                &mut out,
                std::ptr::null_mut()
            ),
            QF_ERR_ARG
        );
    }

    #[test]
    fn text_comparison_and_literal_equality() {
        // SELECT name FROM people WHERE name = 'Ann'
        let nodes = vec![
            col(b"name"),               // 0
            bin(QF_OP_EQ, 0, 2),        // 1
            lit_text(b"Ann"),           // 2
        ];
        let mut f = Fixture::new(nodes, vec![0]);
        f.ast.where_root = 1;
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.diag.code, QF_ERR_OK);
    }

    #[test]
    fn select_names_alias_wins_over_column_name() {
        // SELECT name AS who FROM people ORDER BY who
        // node.name 保持列名 name，别名只存在于 select_names。
        let nodes = vec![col(b"name"), col(b"who")];
        let mut f = Fixture::new(nodes, vec![0])
            .alias(0, b"who")
            .order(vec![1]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.out_names(), vec!["who".to_string()]);
        assert_eq!(f.nodes[0].a, 0);
        assert_eq!(f.nodes[1].a, 0);
    }

    #[test]
    fn missing_select_names_is_plan_error() {
        let mut nodes = vec![col(b"name")];
        let mut sel = vec![0i32];
        let mut cols = table();
        let table_schema = qf_schema {
            count: cols.len() as u32,
            cols: cols.as_mut_ptr(),
        };
        let mut out = qf_schema {
            count: 0,
            cols: std::ptr::null_mut(),
        };
        let mut diag: qf_diag = unsafe { std::mem::zeroed() };
        let mut ast = qf_ast {
            nodes: nodes.as_mut_ptr(),
            node_count: 1,
            select: sel.as_mut_ptr(),
            select_count: 1,
            select_names: std::ptr::null_mut(),
            from_table: s(b"people"),
            where_root: -1,
            group_by: std::ptr::null_mut(),
            group_count: 0,
            order_by: std::ptr::null_mut(),
            order_count: 0,
            limit: -1,
        };
        let h = host();
        assert_eq!(
            qf_plan_build(&h, &mut ast, &table_schema, &mut out, &mut diag),
            QF_ERR_PLAN
        );
    }

    #[test]
    fn case_insensitive_column_lookup() {
        // SELECT NAME FROM people（列名大小写不敏感）
        let nodes = vec![col(b"NAME")];
        let mut f = Fixture::new(nodes, vec![0]);
        assert_eq!(f.run(), QF_ERR_OK);
        assert_eq!(f.nodes[0].a, 0);
    }
}
