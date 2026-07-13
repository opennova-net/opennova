class_name ReferenceEdge
extends RefCounted
## One edge of the reference graph: `source_path` (of `source_kind`) names
## `target_name` (of `target_kind`) at `site`, annotated with the index's
## resolution probe (status/target_path). Typed record per ADR 0017 — the
## native NovaReferenceIndex returns edge Dictionaries at the GDExtension
## transport edge; from_dict converts them the moment they enter editor code
## (ReferenceServices.from_shell), so consumers never touch the dict shape.

var source_path := ""
var source_kind := ""
var target_name := ""
var target_kind := ""
var site := ""
## Resolution probe: "found" | "missing" | "unprobed" | "" (unannotated).
var status := ""
var target_path := ""


static func from_dict(d: Dictionary) -> ReferenceEdge:
	var edge := ReferenceEdge.new()
	edge.source_path = String(d.get("source_path", ""))
	edge.source_kind = String(d.get("source_kind", ""))
	edge.target_name = String(d.get("target_name", ""))
	edge.target_kind = String(d.get("target_kind", ""))
	edge.site = String(d.get("site", ""))
	edge.status = String(d.get("status", ""))
	edge.target_path = String(d.get("target_path", ""))
	return edge


## Convert a native referrers_of()/references_of() result wholesale.
static func from_dict_rows(rows: Array) -> Array[ReferenceEdge]:
	var out: Array[ReferenceEdge] = []
	for row in rows:
		out.append(from_dict(row as Dictionary))
	return out
