package main

import "testing"

func recordFields(t *testing.T, data []byte, pos int) ([]string, int) {
	t.Helper()
	var text []byte
	var spans []span
	next, err := scanRecord(data, pos, &text, &spans)
	if err != nil {
		t.Fatalf("scanRecord: %v", err)
	}
	out := make([]string, len(spans))
	for i, sp := range spans {
		out[i] = string(text[sp.off : sp.off+sp.len])
	}
	return out, next
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestScanRecordQuotedCommaAndCRLF(t *testing.T) {
	data := []byte("1,\"Bob, Jr\",20\r\n")
	got, next := recordFields(t, data, 0)
	want := []string{"1", "Bob, Jr", "20"}
	if !equalStrings(got, want) {
		t.Fatalf("字段 = %q, 期望 %q", got, want)
	}
	if next != len(data) {
		t.Fatalf("next = %d, 期望 %d", next, len(data))
	}
}

func TestScanRecordEscapedQuotes(t *testing.T) {
	data := []byte(`3,"Ci""d",SZ` + "\n")
	got, _ := recordFields(t, data, 0)
	want := []string{"3", `Ci"d`, "SZ"}
	if !equalStrings(got, want) {
		t.Fatalf("字段 = %q, 期望 %q", got, want)
	}
}

func TestScanRecordEmbeddedNewlines(t *testing.T) {
	data := []byte("4,\"Multi\r\nLine\",tail\n")
	got, next := recordFields(t, data, 0)
	want := []string{"4", "Multi\r\nLine", "tail"}
	if !equalStrings(got, want) {
		t.Fatalf("字段 = %q, 期望 %q", got, want)
	}
	if next != len(data) {
		t.Fatalf("next = %d, 期望 %d", next, len(data))
	}
}

func TestScanRecordEmptyQuotedFieldIsEmpty(t *testing.T) {
	data := []byte("a,\"\",c\n")
	got, _ := recordFields(t, data, 0)
	want := []string{"a", "", "c"}
	if !equalStrings(got, want) {
		t.Fatalf("字段 = %q, 期望 %q", got, want)
	}
}

func TestScanRecordUnterminatedQuotePosition(t *testing.T) {
	data := []byte("a,b\n1,\"oops\n2,3\n")
	pos := len("a,b\n") + 2 // 第二行第三个字节
	var text []byte
	var spans []span
	_, err := scanRecord(data, pos, &text, &spans)
	if err == nil {
		t.Fatal("期望引号未闭合错误")
	}
	if err.line != 2 || err.col != 3 {
		t.Fatalf("行列 = %d:%d, 期望 2:3", err.line, err.col)
	}
}

func TestCollectRowStartsSkipsBlankLines(t *testing.T) {
	data := []byte("id,name\r\n\r\n1,A\r\n\n2,\"B\r\nC\"\r\n")
	starts, err := collectRowStarts(data, len("id,name\r\n"))
	if err != nil {
		t.Fatalf("collectRowStarts: %v", err)
	}
	if len(starts) != 2 {
		t.Fatalf("行数 = %d, 期望 2", len(starts))
	}
	wantFirst := len("id,name\r\n\r\n")
	if starts[0] != wantFirst {
		t.Fatalf("首行偏移 = %d, 期望 %d", starts[0], wantFirst)
	}
	var text []byte
	var spans []span
	if _, err := scanRecord(data, starts[1], &text, &spans); err != nil {
		t.Fatalf("第二行解析: %v", err)
	}
	if len(spans) != 2 {
		t.Fatalf("第二行字段数 = %d, 期望 2", len(spans))
	}
}

func TestClassifyField(t *testing.T) {
	cases := []struct {
		in   string
		kind cellKind
		mask uint8
	}{
		{"", cellNull, 0},
		{"42", cellInt, seenInt},
		{"-7", cellInt, seenInt},
		{"+0", cellInt, seenInt},
		{"3.14", cellDouble, seenDouble},
		{".5", cellDouble, seenDouble},
		{"1e3", cellDouble, seenDouble},
		{"-2.5E-2", cellDouble, seenDouble},
		{"abc", cellText, seenText},
		{"NaN", cellText, seenText},
		{"Inf", cellText, seenText},
		{"0x10", cellText, seenText},
		{"1_000", cellText, seenText},
		{"12a", cellText, seenText},
	}
	for _, c := range cases {
		kind, _, _, mask := classifyField([]byte(c.in))
		if kind != c.kind || mask != c.mask {
			t.Fatalf("classifyField(%q) = (%d,%d), 期望 (%d,%d)", c.in, kind, mask, c.kind, c.mask)
		}
	}
}

func TestInferColumnType(t *testing.T) {
	cases := []struct {
		mask uint8
		want colType
	}{
		{seenInt, qfTInt},
		{seenInt | seenDouble, qfTDouble},
		{seenDouble, qfTDouble},
		{seenInt | seenText, qfTText},
		{seenDouble | seenText, qfTText},
		{0, qfTText},
	}
	for _, c := range cases {
		if got := inferColumnType(c.mask); got != c.want {
			t.Fatalf("inferColumnType(%d) = %d, 期望 %d", c.mask, got, c.want)
		}
	}
}

func TestSplitRanges(t *testing.T) {
	cases := []struct {
		rows, shards int
		want         []rowRange
	}{
		{10, 3, []rowRange{{0, 4}, {4, 7}, {7, 10}}},
		{2, 5, []rowRange{{0, 1}, {1, 2}}},
		{0, 4, []rowRange{{0, 0}}},
		{5, 0, []rowRange{{0, 5}}},
	}
	for _, c := range cases {
		got := splitRanges(c.rows, c.shards)
		if len(got) != len(c.want) {
			t.Fatalf("splitRanges(%d,%d) 分片数 = %d, 期望 %d", c.rows, c.shards, len(got), len(c.want))
		}
		for i := range got {
			if got[i] != c.want[i] {
				t.Fatalf("splitRanges(%d,%d)[%d] = %+v, 期望 %+v", c.rows, c.shards, i, got[i], c.want[i])
			}
		}
	}
}

func TestParseShardInfersAndKeepsRawBytes(t *testing.T) {
	data := []byte("id,name,score\r\n1,\"A, B\",1\r\n2,\"line\r\nbreak\",2.5\r\n")
	hdr, err := parseHeader(data)
	if err != nil {
		t.Fatalf("parseHeader: %v", err)
	}
	cols := len(hdr.spans)
	if cols != 3 {
		t.Fatalf("列数 = %d, 期望 3", cols)
	}
	starts, err := collectRowStarts(data, hdr.next)
	if err != nil {
		t.Fatalf("collectRowStarts: %v", err)
	}
	if len(starts) != 2 {
		t.Fatalf("行数 = %d, 期望 2", len(starts))
	}
	sh, err := parseShard(data, starts, 0, len(starts), cols)
	if err != nil {
		t.Fatalf("parseShard: %v", err)
	}
	if sh.rows != 2 || len(sh.cells) != 6 {
		t.Fatalf("rows/cells = %d/%d, 期望 2/6", sh.rows, len(sh.cells))
	}
	if inferColumnType(sh.masks[0]) != qfTInt ||
		inferColumnType(sh.masks[1]) != qfTText ||
		inferColumnType(sh.masks[2]) != qfTDouble {
		t.Fatalf("推断类型 = [%d %d %d]", inferColumnType(sh.masks[0]),
			inferColumnType(sh.masks[1]), inferColumnType(sh.masks[2]))
	}
	text := func(c cell) string { return string(sh.text[c.off : c.off+c.n]) }
	if got := text(sh.cells[1]); got != "A, B" {
		t.Fatalf("单元格 (0,1) = %q", got)
	}
	if got := text(sh.cells[4]); got != "line\r\nbreak" {
		t.Fatalf("单元格 (1,1) = %q", got)
	}
	if c := sh.cells[3]; c.kind != cellInt || c.i != 2 {
		t.Fatalf("单元格 (1,0) = %+v", c)
	}
	if c := sh.cells[5]; c.kind != cellDouble || c.d != 2.5 {
		t.Fatalf("单元格 (1,2) = %+v", c)
	}
}
