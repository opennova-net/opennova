class_name HudHiddenCaptureWitness
extends RefCounted

## Typed semantic state of the reversible HUD-detail-3 capture transaction.
## MainGame adds its CanvasLayer fact; transport owners read the complete
## record only at their serialization boundary (ADR 0017).

var hud_detail_level := 0
var gameplay_hud_visible := true
var player_view_effects_active := false
var ads_active := false
var big_map_active := false
var hud_canvas_layer_active := false
var error := ""


func is_valid() -> bool:
	return error.is_empty()
