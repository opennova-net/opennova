class_name DocumentTabRow
extends RefCounted
## One row of a workspace's document-tab strip: the label/badge the shell
## renders plus the path identity behind it. Produced by DocumentTabSet.tabs()
## (and any get_document_tabs() override), consumed by the shell strip and the
## MCP state tools. Typed record per ADR 0017 — the row degrades to a
## Dictionary only at the MCP wire via to_dict().

var label := ""
var dirty := false
var path := ""
var tooltip := ""


static func make(p_label: String, p_dirty: bool, p_path: String, p_tooltip: String) -> DocumentTabRow:
	var row := DocumentTabRow.new()
	row.label = p_label
	row.dirty = p_dirty
	row.path = p_path
	row.tooltip = p_tooltip
	return row


## MCP wire encoding of one row (transport edge only).
func to_dict() -> Dictionary:
	return {"label": label, "dirty": dirty, "path": path, "tooltip": tooltip}


## MCP wire encoding of a whole strip (transport edge only).
static func to_dict_rows(rows: Array[DocumentTabRow]) -> Array:
	var out := []
	for row in rows:
		out.append(row.to_dict())
	return out
