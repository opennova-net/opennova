class_name WaypointHudEntry
extends RefCounted

## The waypoint label's resolved entry — the typed cross-object record (ADR 0017)
## behind GameHudPresenter.waypoint_hud_entry() and the overlay's typed
## set_waypoint feed. One instance per frame while the label shows; absence
## (null) is the hidden state (ShowWaypoints off / no track / no current
## selection).
## [orig: the name+distance pair HUD_DrawWaypointNameAndDistance @0x5947a0 draws]

var text_name := ""   # resolved WPNames display name
var distance_m := 0   # whole meters, the fixed >>16 truncation
var mission_position := Vector2.ZERO  # mission ground plane (x, y)
var altitude_wu := 0.0  # world-unit altitude; the spinmap tricolor input
