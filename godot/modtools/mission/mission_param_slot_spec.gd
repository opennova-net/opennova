class_name MissionParamSlotSpec
extends RefCounted
## One trigger/action parameter slot of the RE-derived schema: the designer
## label/tooltip, the MissionParamSchema.Kind that picks the editor widget,
## whether the type uses the slot at all, and the fixed picker rows for ENUM
## kinds. Typed record per ADR 0017 — the native NovaMissionData returns slot
## Dictionaries at the GDExtension transport edge; from_dict converts them in
## MissionParamSchema.

var label := ""
var kind := MissionParamSchema.Kind.RAW
var tip := ""
## False only for described types past their param count; unknown / variable
## types keep every slot editable.
var used := true
## ENUM picker rows in the shared [{value: int, label: String}] items shape
## MissionParamSlot renders; [] for every other kind.
var enum_items: Array = []


static func from_dict(d: Dictionary) -> MissionParamSlotSpec:
	var slot := MissionParamSlotSpec.new()
	slot.label = String(d.get("label", ""))
	slot.kind = int(d.get("kind", MissionParamSchema.Kind.RAW))
	slot.tip = String(d.get("tip", ""))
	slot.used = bool(d.get("used", true))
	slot.enum_items = d.get("enum", [])
	return slot
