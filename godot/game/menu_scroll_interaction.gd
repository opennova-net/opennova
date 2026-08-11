extends RefCounted

const MenuScrollRange := preload("res://game/menu_scroll_range.gd")

# The witnessed standalone-scroll (CScrollWnd) interaction, owned by
# MenuDriver: arrows step -/+1 (the ctor default), a track press pages toward
# the click, a shuttle press captures an anchor and drags through the travel
# ratio; every change clamps and reports the new value
# [orig: CScrollWnd_HandleEvent @ 0x64d050 — arrows @ 0x64d2d9/0x64d31a,
#  track @ 0x64d0f0/0x64d10e, anchor @ 0x64d1cb..0x64d217, drag
#  @ 0x64d231..0x64d2aa; ctor defaults @ 0x64c4cf/0x64c4d9].

var _drag_id := -1
var _drag_anchor := 0


func active() -> bool:
	return _drag_id >= 0


func end_drag() -> void:
	_drag_id = -1


## No-value sentinel: the press started a thumb drag or hit nothing.
const NO_VALUE := -2147483648


## A press over the scroll widget: returns the new value, or NO_VALUE when the
## press started a thumb drag (or hit nothing actionable).
func press(frame: MenuFrame, id: int, index: int, scroll: MenuScrollRange,
		position: Vector2) -> int:
	match frame.scroll_hit_at(index, position):
		1:
			return scroll.value - 1
		2:
			return scroll.value + 1
		3:
			_drag_id = id
			_drag_anchor = frame.scroll_drag_anchor(index, position)
			return NO_VALUE
		4:
			return scroll.value - scroll.page
		5:
			return scroll.value + scroll.page
	return NO_VALUE


## The captured drag's value for the current mouse position.
func drag_value(frame: MenuFrame, index: int, position: Vector2) -> int:
	return frame.scroll_drag_value(index, position, _drag_anchor)


func drag_id() -> int:
	return _drag_id


## A slider, or a table/list whose embedded scrollbar strip is under the
## point (drives MenuDriver's press routing).
func widget_at(driver: MenuDriver, frame: MenuFrame, position: Vector2) -> int:
	# Scrollbar parts claim directly — shipped menus author scrollbar rects
	# OUTSIDE the owner widget (options.mnu CONTROL_MAPPING), so the plain
	# rect hit misses them (the D-MNU-16 outside-arrow class).
	var index: int = frame.scroll_owner_at(position)
	if index < 0:
		var hit: int = frame.hit_test(position)
		if hit < 0:
			return -1
		var hit_id := driver._id_at_index(hit)
		if hit_id >= 0 and driver.widget_kind_of(hit_id) == MnuDocument.TYPE_SCROLL:
			return hit_id
		return -1
	return driver._id_at_index(index)


## A slider's authored range, or the rows model (0..row_limit, page =
## visible-1, value = scroll_row) [orig: the table SCROLLBAR delegate
## @ 0x643b22].
func model_of(driver: MenuDriver, frame: MenuFrame, id: int,
		index: int, stored_range: MenuScrollRange, scroll_row: int) -> MenuScrollRange:
	if driver.widget_kind_of(id) == MnuDocument.TYPE_SCROLL:
		return stored_range
	var limit: int = frame.scroll_row_limit(index)
	if limit <= 0:
		return null
	return MenuScrollRange.new(0, limit, frame.scroll_page_rows(index),
			scroll_row)
