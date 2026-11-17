// Go 阶段：CSV 并行扫描。
//
// 设计要点：
//   - 契约要求所有内存来自 qf_host，所以跨边界只用 C 辅助函数取内存，
//     Go 侧不把带指针的 Go 内存交给宿主保存。
//   - 行按数量均分成若干分片，每片一个 goroutine 解析；合并严格按分片顺序，
//     因此输出与单线程扫描逐单元格一致。
//   - CSV 解析自己写：需要区分「字段为空」与「字段不存在」，并要给出引号未闭合的
//     行列位置，现成库两者都拿不到。
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../gen -I${SRCDIR}/../../contracts
#include <stdint.h>
#include <string.h>

// cgo 的 //export 会把参数上的 const 丢掉，直接包含 qf.h 会与契约原型冲突。
// 这里只在包含头文件期间给那个原型换个名字；导出的符号名仍是 qf_csv_scan。
#define qf_csv_scan qf_csv_scan_header_prototype
#include "qf.h"
#undef qf_csv_scan

static inline int qfgo_host_ok(const qf_host* h) {
    return h != 0 && h->alloc != 0 && h->str != 0;
}

static inline void* qfgo_host_alloc(const qf_host* h, uint64_t size, uint64_t align) {
    return h->alloc(h->ctx, size, align);
}

static inline qf_str qfgo_host_str(const qf_host* h, const char* src, uint32_t len) {
    return h->str(h->ctx, src, len);
}

static inline int qfgo_str_ok(qf_str s) { return s.ptr != 0; }

static inline qf_value qfgo_null(void) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_NULL;
    return v;
}

static inline qf_value qfgo_int(int64_t i) {
    qf_value v = qfgo_null();
    v.type = QF_T_INT;
    v.as.i = i;
    return v;
}

static inline qf_value qfgo_double(double d) {
    qf_value v = qfgo_null();
    v.type = QF_T_DOUBLE;
    v.as.d = d;
    return v;
}

static inline qf_value qfgo_text(qf_str s) {
    qf_value v = qfgo_null();
    v.type = QF_T_TEXT;
    v.as.s = s;
    return v;
}

static inline qf_value qfgo_text_src(const qf_host* h, const char* src, uint32_t len) {
    return qfgo_text(qfgo_host_str(h, src, len));
}

static inline int qfgo_value_ok(qf_value v) {
    return v.type != QF_T_TEXT || v.as.s.ptr != 0;
}
*/
import "C"

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"unsafe"
)

const (
	errOK    C.qf_err = C.qf_err(C.QF_ERR_OK)
	errArg   C.qf_err = C.qf_err(C.QF_ERR_ARG)
	errIO    C.qf_err = C.qf_err(C.QF_ERR_IO)
	errExec  C.qf_err = C.qf_err(C.QF_ERR_EXEC)
	errPanic C.qf_err = C.qf_err(C.QF_ERR_PANIC)

	qfTInt    C.qf_type = C.qf_type(C.QF_T_INT)
	qfTDouble C.qf_type = C.qf_type(C.QF_T_DOUBLE)
	qfTText   C.qf_type = C.qf_type(C.QF_T_TEXT)
)

// colType 是 qf_type 的别名：解析与推断代码不需要直接写 C 类型名。
type colType = C.qf_type

// hostMu 串行化对宿主分配器的调用：arena 是单线程结构，并行分片不能直接共用。
var hostMu sync.Mutex

func hostAlloc(host *C.qf_host, size, align uint64) unsafe.Pointer {
	if size == 0 {
		size = 1
	}
	hostMu.Lock()
	p := C.qfgo_host_alloc(host, C.uint64_t(size), C.uint64_t(align))
	hostMu.Unlock()
	return p
}

func hostStrBytes(host *C.qf_host, b []byte) C.qf_str {
	var p *C.char
	if len(b) > 0 {
		p = (*C.char)(unsafe.Pointer(&b[0]))
	}
	hostMu.Lock()
	s := C.qfgo_host_str(host, p, C.uint32_t(len(b)))
	hostMu.Unlock()
	return s
}

/* ---------- CSV 词法 ---------- */

type csvError struct {
	line uint32
	col  uint32
	msg  string
}

func (e *csvError) Error() string { return e.msg }

type span struct {
	off int
	len int
}

// lineCol 只在报错时调用，所以允许 O(n)；返回 1 基行列。
func lineCol(data []byte, pos int) (uint32, uint32) {
	line, col := uint32(1), uint32(1)
	if pos > len(data) {
		pos = len(data)
	}
	for i := range pos {
		if data[i] == '\n' {
			line++
			col = 1
		} else {
			col++
		}
	}
	return line, col
}

// scanRecord 从 pos（一条逻辑记录的起点）走到记录结束，返回下一条记录的偏移。
// spans 非空时把每个字段的原始字节追加到 text 并记录 span；两者都为 nil 时只走位。
// CSV 约定：引号只在字段开头生效，"" 表示一个引号，引号内允许换行；行尾支持 CRLF。
func scanRecord(data []byte, pos int, text *[]byte, spans *[]span) (int, *csvError) {
	n := len(data)
	i := pos
	capture := spans != nil
	if capture {
		*spans = (*spans)[:0]
	}
	for {
		start := 0
		if text != nil {
			start = len(*text)
		}
		if i < n && data[i] == '"' {
			qpos := i
			i++
			for {
				if i >= n {
					line, col := lineCol(data, qpos)
					return 0, &csvError{line: line, col: col, msg: "CSV 引号未闭合"}
				}
				if data[i] == '"' {
					if i+1 < n && data[i+1] == '"' {
						if text != nil {
							*text = append(*text, '"')
						}
						i += 2
						continue
					}
					i++ // 闭引号
					break
				}
				if text != nil {
					*text = append(*text, data[i])
				}
				i++
			}
			// 闭引号之后、分隔符之前的杂字符按字面量收下，避免为空格等细节报错。
			for i < n && data[i] != ',' && data[i] != '\n' {
				if data[i] == '\r' && i+1 < n && data[i+1] == '\n' {
					break
				}
				if text != nil {
					*text = append(*text, data[i])
				}
				i++
			}
		} else {
			for i < n && data[i] != ',' && data[i] != '\n' {
				if data[i] == '\r' && i+1 < n && data[i+1] == '\n' {
					break
				}
				if text != nil {
					*text = append(*text, data[i])
				}
				i++
			}
		}
		if capture {
			end := start
			if text != nil {
				end = len(*text)
			}
			*spans = append(*spans, span{off: start, len: end - start})
		}
		if i < n && data[i] == ',' {
			i++
			continue
		}
		if i < n && data[i] == '\r' && i+1 < n && data[i+1] == '\n' {
			i += 2
		} else if i < n && data[i] == '\n' {
			i++
		}
		return i, nil
	}
}

func skipBlankLines(data []byte, pos int) int {
	n := len(data)
	for pos < n {
		if data[pos] == '\n' {
			pos++
			continue
		}
		if data[pos] == '\r' && pos+1 < n && data[pos+1] == '\n' {
			pos += 2
			continue
		}
		break
	}
	return pos
}

// collectRowStarts 返回每条数据记录的起始偏移，空行忽略。
func collectRowStarts(data []byte, pos int) ([]int, *csvError) {
	n := len(data)
	starts := make([]int, 0, 64)
	for pos < n {
		pos = skipBlankLines(data, pos)
		if pos >= n {
			break
		}
		starts = append(starts, pos)
		next, err := scanRecord(data, pos, nil, nil)
		if err != nil {
			return nil, err
		}
		if next <= pos {
			next = pos + 1
		}
		pos = next
	}
	return starts, nil
}

type headerInfo struct {
	text  []byte
	spans []span
	next  int
}

func parseHeader(data []byte) (*headerInfo, *csvError) {
	pos := skipBlankLines(data, 0)
	if pos >= len(data) {
		return nil, &csvError{msg: "CSV 为空，缺少表头"}
	}
	h := &headerInfo{}
	next, err := scanRecord(data, pos, &h.text, &h.spans)
	if err != nil {
		return nil, err
	}
	h.next = next
	return h, nil
}

/* ---------- 类型推断与取值 ---------- */

type cellKind uint8

const (
	cellNull cellKind = iota
	cellInt
	cellDouble
	cellText
)

const (
	seenInt    uint8 = 1 << 0
	seenDouble uint8 = 1 << 1
	seenText   uint8 = 1 << 2
)

type cell struct {
	kind cellKind
	i    int64
	d    float64
	off  int
	n    int
}

func isInteger(b []byte) bool {
	if len(b) == 0 {
		return false
	}
	i := 0
	if b[0] == '+' || b[0] == '-' {
		i = 1
		if len(b) == 1 {
			return false
		}
	}
	for ; i < len(b); i++ {
		if b[i] < '0' || b[i] > '9' {
			return false
		}
	}
	return true
}

// isDecimalFloat 只认十进制写法，挡掉 ParseFloat 也接受的 NaN/Inf/0x1p3/下划线。
func isDecimalFloat(b []byte) bool {
	if len(b) == 0 {
		return false
	}
	i := 0
	if b[0] == '+' || b[0] == '-' {
		i++
		if i == len(b) {
			return false
		}
	}
	digits := 0
	for i < len(b) && b[i] >= '0' && b[i] <= '9' {
		i++
		digits++
	}
	if i < len(b) && b[i] == '.' {
		i++
		for i < len(b) && b[i] >= '0' && b[i] <= '9' {
			i++
			digits++
		}
	}
	if digits == 0 {
		return false
	}
	if i < len(b) && (b[i] == 'e' || b[i] == 'E') {
		i++
		if i < len(b) && (b[i] == '+' || b[i] == '-') {
			i++
		}
		exp := 0
		for i < len(b) && b[i] >= '0' && b[i] <= '9' {
			i++
			exp++
		}
		if exp == 0 {
			return false
		}
	}
	return i == len(b)
}

func classifyField(b []byte) (cellKind, int64, float64, uint8) {
	if len(b) == 0 {
		return cellNull, 0, 0, 0
	}
	if isInteger(b) {
		if v, err := strconv.ParseInt(string(b), 10, 64); err == nil {
			return cellInt, v, 0, seenInt
		}
	}
	if isDecimalFloat(b) {
		if v, err := strconv.ParseFloat(string(b), 64); err == nil {
			return cellDouble, 0, v, seenDouble
		}
	}
	return cellText, 0, 0, seenText
}

func inferColumnType(mask uint8) C.qf_type {
	switch {
	case mask&seenText != 0:
		return qfTText
	case mask&seenDouble != 0:
		return qfTDouble
	case mask&seenInt != 0:
		return qfTInt
	default:
		// 整列都是空字段：没有可推断的类型，按文本列处理。
		return qfTText
	}
}

/* ---------- 分片 ---------- */

type rowRange struct {
	from int
	to   int
}

func splitRanges(rows, shards int) []rowRange {
	if shards < 1 {
		shards = 1
	}
	if rows <= 0 {
		return []rowRange{{0, 0}}
	}
	if shards > rows {
		shards = rows
	}
	out := make([]rowRange, shards)
	base, rem := rows/shards, rows%shards
	cur := 0
	for i := range out {
		n := base
		if i < rem {
			n++
		}
		out[i] = rowRange{from: cur, to: cur + n}
		cur += n
	}
	return out
}

type shardScan struct {
	rows   int
	cells  []cell
	text   []byte
	spans  []span
	masks  []uint8
	values []C.qf_value
	err    *csvError
	panicT string
}

func parseShard(data []byte, starts []int, from, to, cols int) (*shardScan, *csvError) {
	rows := to - from
	sh := &shardScan{
		rows:  rows,
		cells: make([]cell, rows*cols),
		masks: make([]uint8, cols),
	}
	for r := range rows {
		if _, err := scanRecord(data, starts[from+r], &sh.text, &sh.spans); err != nil {
			return nil, err
		}
		base := r * cols
		for c := range cols {
			if c >= len(sh.spans) {
				continue // 缺列按空值补
			}
			sp := sh.spans[c]
			raw := sh.text[sp.off : sp.off+sp.len]
			kind, iv, dv, mask := classifyField(raw)
			sh.cells[base+c] = cell{kind: kind, i: iv, d: dv, off: sp.off, n: sp.len}
			sh.masks[c] |= mask
		}
	}
	return sh, nil
}

func materializeShard(host *C.qf_host, sh *shardScan, types []C.qf_type) bool {
	if len(sh.cells) == 0 {
		sh.values = nil
		return true
	}
	size := uint64(len(sh.cells)) * uint64(unsafe.Sizeof(C.qf_value{}))
	raw := hostAlloc(host, size, 8)
	if raw == nil {
		return false
	}
	vals := unsafe.Slice((*C.qf_value)(raw), len(sh.cells))
	cols := len(types)
	// 整个分片只锁一次：文本拷贝才是分配热点，逐格加锁没有意义。
	hostMu.Lock()
	defer hostMu.Unlock()
	for i := range sh.cells {
		c := &sh.cells[i]
		t := types[i%cols]
		switch {
		case c.kind == cellNull:
			vals[i] = C.qfgo_null()
		case t == qfTInt:
			if c.kind == cellInt {
				vals[i] = C.qfgo_int(C.int64_t(c.i))
			} else {
				vals[i] = C.qfgo_null()
			}
		case t == qfTDouble:
			switch c.kind {
			case cellInt:
				vals[i] = C.qfgo_double(C.double(float64(c.i)))
			case cellDouble:
				vals[i] = C.qfgo_double(C.double(c.d))
			default:
				vals[i] = C.qfgo_null()
			}
		default:
			b := sh.text[c.off : c.off+c.n]
			var p *C.char
			if len(b) > 0 {
				p = (*C.char)(unsafe.Pointer(&b[0]))
			}
			v := C.qfgo_text_src(host, p, C.uint32_t(len(b)))
			if C.qfgo_value_ok(v) == 0 {
				return false
			}
			vals[i] = v
		}
	}
	sh.values = vals
	return true
}

func writeSchema(host *C.qf_host, table *C.qf_schema, hdr *headerInfo, types []C.qf_type) bool {
	n := len(types)
	size := uint64(n) * uint64(unsafe.Sizeof(C.qf_col{}))
	raw := hostAlloc(host, size, 8)
	if raw == nil {
		return false
	}
	cols := unsafe.Slice((*C.qf_col)(raw), n)
	for i := range n {
		sp := hdr.spans[i]
		name := hostStrBytes(host, hdr.text[sp.off:sp.off+sp.len])
		if C.qfgo_str_ok(name) == 0 {
			return false
		}
		cols[i].name = name
		cols[i]._type = types[i]
	}
	table.count = C.uint32_t(n)
	table.cols = (*C.qf_col)(raw)
	return true
}

/* ---------- 诊断 ---------- */

func fail(host *C.qf_host, diag *C.qf_diag, code C.qf_err, line, col uint32, msg string) C.qf_err {
	if diag != nil {
		diag.code = code
		diag.line = C.uint32_t(line)
		diag.col = C.uint32_t(col)
		if C.qfgo_host_ok(host) != 0 {
			diag.message = hostStrBytes(host, []byte(msg))
		}
	}
	return code
}

func csvFail(host *C.qf_host, diag *C.qf_diag, e *csvError) C.qf_err {
	return fail(host, diag, errIO, e.line, e.col, e.msg)
}

/* ---------- 导出入口 ---------- */

//export qf_csv_scan
func qf_csv_scan(host *C.qf_host, path C.qf_str, table *C.qf_schema, shards C.uint32_t,
	out *C.qf_batch, diag *C.qf_diag) (rc C.qf_err) {

	if diag != nil {
		*diag = C.qf_diag{}
	}
	if out != nil {
		*out = C.qf_batch{}
	}
	if out == nil || path.ptr == nil || path.len == 0 || path.len > 0x7FFFFFFF ||
		C.qfgo_host_ok(host) == 0 {
		return fail(nil, diag, errArg, 0, 0, "参数无效")
	}
	defer func() {
		if r := recover(); r != nil {
			rc = fail(host, diag, errPanic, 0, 0, fmt.Sprintf("Go 扫描阶段崩溃: %v", r))
		}
	}()

	data, rerr := os.ReadFile(C.GoStringN(path.ptr, C.int(path.len)))
	if rerr != nil {
		return fail(host, diag, errIO, 0, 0, "无法打开表文件: "+rerr.Error())
	}

	hdr, cerr := parseHeader(data)
	if cerr != nil {
		return csvFail(host, diag, cerr)
	}
	starts, cerr := collectRowStarts(data, hdr.next)
	if cerr != nil {
		return csvFail(host, diag, cerr)
	}

	cols := len(hdr.spans)
	var types []C.qf_type
	explicit := false
	// 显式表：列定义就是输出列序，按位置读取 CSV（表头只用于跳过），
	// 与「表头先于数据」的宿主约定一致；count==0 时才回填推断结果。
	if table != nil && table.count > 0 {
		n := int(table.count)
		if table.cols == nil {
			return fail(host, diag, errArg, 0, 0, "显式表缺少列定义")
		}
		types = make([]C.qf_type, n)
		cs := unsafe.Slice(table.cols, n)
		for i := range cs {
			types[i] = cs[i]._type
		}
		cols = n
		explicit = true
	}

	totalRows := len(starts)
	if uint64(totalRows) > 0xFFFFFFFF || uint64(cols) > 0xFFFFFFFF {
		return fail(host, diag, errExec, 0, 0, "行数或列数超出契约范围")
	}

	ranges := splitRanges(totalRows, int(shards))
	shardScans := make([]*shardScan, len(ranges))

	// 解析阶段：每个分片一个 goroutine，只读 data、各写各的结果槽位。
	var wg sync.WaitGroup
	for i, rg := range ranges {
		wg.Add(1)
		go func(i int, rg rowRange) {
			defer wg.Done()
			defer func() {
				if r := recover(); r != nil {
					shardScans[i] = &shardScan{panicT: fmt.Sprint(r)}
				}
			}()
			sh, err := parseShard(data, starts, rg.from, rg.to, cols)
			if err != nil {
				shardScans[i] = &shardScan{err: err}
				return
			}
			shardScans[i] = sh
		}(i, rg)
	}
	wg.Wait()

	// 按分片顺序检查错误与合并，保证错误诊断与单线程一致。
	for _, sh := range shardScans {
		if sh == nil {
			return fail(host, diag, errPanic, 0, 0, "Go 扫描分片缺失")
		}
		if sh.err != nil {
			return csvFail(host, diag, sh.err)
		}
		if sh.panicT != "" {
			return fail(host, diag, errPanic, 0, 0, "Go 扫描分片崩溃: "+sh.panicT)
		}
	}

	if !explicit {
		types = make([]C.qf_type, cols)
		for c := range cols {
			var mask uint8
			for _, sh := range shardScans {
				mask |= sh.masks[c]
			}
			types[c] = inferColumnType(mask)
		}
		if table != nil {
			if !writeSchema(host, table, hdr, types) {
				return fail(host, diag, errExec, 0, 0, "宿主分配失败（列定义）")
			}
		}
	}

	// 取值阶段：类型已确定，再次并行把每个分片落成 []C.qf_value。
	oks := make([]bool, len(shardScans))
	var wg2 sync.WaitGroup
	for i := range shardScans {
		wg2.Add(1)
		go func(i int) {
			defer wg2.Done()
			defer func() {
				if r := recover(); r != nil {
					shardScans[i].panicT = fmt.Sprint(r)
				}
			}()
			oks[i] = materializeShard(host, shardScans[i], types)
		}(i)
	}
	wg2.Wait()

	for i, sh := range shardScans {
		if sh.panicT != "" {
			return fail(host, diag, errPanic, 0, 0, "Go 取值分片崩溃: "+sh.panicT)
		}
		if !oks[i] {
			return fail(host, diag, errExec, 0, 0, "宿主分配失败（列值）")
		}
	}

	var merged *C.qf_value
	total := uint64(totalRows) * uint64(cols)
	if total > 0 {
		if total > uint64(^uint(0)>>1)/uint64(unsafe.Sizeof(C.qf_value{})) {
			return fail(host, diag, errExec, 0, 0, "结果集过大")
		}
		raw := hostAlloc(host, total*uint64(unsafe.Sizeof(C.qf_value{})), 8)
		if raw == nil {
			return fail(host, diag, errExec, 0, 0, "宿主分配失败（结果集）")
		}
		dst := unsafe.Slice((*C.qf_value)(raw), int(total))
		off := 0
		for _, sh := range shardScans {
			off += copy(dst[off:], sh.values)
		}
		if off != int(total) {
			return fail(host, diag, errExec, 0, 0, "分片合并长度不一致")
		}
		merged = (*C.qf_value)(raw)
	}

	out.rows = C.uint32_t(totalRows)
	out.cols = C.uint32_t(cols)
	out.values = merged
	return errOK
}

func main() {}
