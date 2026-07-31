class_name WaypointHudEntry
extends RefCounted

## The waypoint label's resolved entry — the typed cross-object record (ADR 0017)
## behind NovaGameHudPresenter.waypoint_hud_entry() and the HUD info transport edge.
## One instance per frame while the label shows; absence (null) is the hidden
## state (ShowWaypoints off / no track / no current selection).
## [orig: the name+distance pair HUD_DrawWaypointNameAndDistance @0x5947a0 draws]

var text_name := ""   # resolved WPNames display name
var distance_m := 0   # whole meters, the fixed >>16 truncation


func to_info_dict() -> Dictionary:
	# The per-frame HUD info transport shape (the update_info edge).
	return {"name": text_name, "distance_m": distance_m}
