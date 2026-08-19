class_name FirstPersonArmsWitness
extends RefCounted

## Typed semantic identity of the actual first-person arms submit. Transport
## owners read these fields only at their serialization boundary (ADR 0017).

var character_id := 0
var arms_graphic := ""
var arms_camo := PackedInt32Array()
var error := ""


func is_valid() -> bool:
	return error.is_empty() and character_id != 0 \
			and not arms_graphic.is_empty() and arms_camo.size() == 3
