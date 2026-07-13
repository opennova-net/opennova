class_name MissionParamSpec
extends RefCounted
## The designer-facing meaning of one trigger/action type's param1..4: the
## description template ({p1}..{p4} placeholders), whether the type is a known
## (RE-mapped) row, whether its slot usage varies by sub-type, and always four
## MissionParamSlotSpec slots. Typed record per ADR 0017 over the semantic
## table in libs/mission (NovaMissionData returns its Dictionary encoding at
## the GDExtension transport edge; from_dict converts it here).

var desc := ""
var known := false
var variable := false
var params: Array[MissionParamSlotSpec] = []


static func from_dict(d: Dictionary) -> MissionParamSpec:
	var spec := MissionParamSpec.new()
	spec.desc = String(d.get("desc", ""))
	spec.known = bool(d.get("known", false))
	spec.variable = bool(d.get("variable", false))
	for slot in d.get("params", []):
		spec.params.append(MissionParamSlotSpec.from_dict(slot as Dictionary))
	return spec
