class_name MenuWidgetState
extends RefCounted
## MenuDriver's runtime state for one document widget id, replayed onto the
## compiled MenuFrame whenever a screen recompiles (menu_frame_state_replay.gd)
## and read back for cross-screen queries. Every override carries its own
## `has_*` latch: an override the shell never wrote falls back to the
## document's authored flags / text / items, exactly as the absent
## Dictionary key did.

const MenuScrollRange := preload("res://game/menu_scroll_range.gd")

var has_shown := false
var shown := false
var has_disabled := false
var disabled := false
var has_checked := false
var checked := false
var has_text := false
var text := ""
var has_items := false
var items := PackedStringArray()
var has_selected_item := false
var selected_item := 0
var has_scroll_row := false
var scroll_row := 0
## The standalone CScrollWnd range, or null until seeded.
var scroll_range: MenuScrollRange = null
var has_selected_set := false
var selected_set := PackedInt32Array()
## TABLE rows seeded by companions (menu_table_state.gd owns the shapes).
var has_table_rows := false
var table_rows: Array[PackedStringArray] = []
var table_selected := PackedInt32Array()


## True while no override has been written (a replay skips the widget).
func is_empty() -> bool:
	return not (has_shown or has_disabled or has_checked or has_text or has_items \
			or has_selected_item or has_scroll_row or scroll_range != null \
			or has_selected_set or has_table_rows)
