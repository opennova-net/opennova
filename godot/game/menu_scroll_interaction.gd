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
	var index := frame.hit_test(position)
	if index < 0:
		return -1
	var id := driver._id_at_index(index)
	if id < 0:
		return -1
	var kind := driver.widget_kind_of(id)
	if kind == MnuDocument.TYPE_SCROLL:
		return id
	if kind in [MnuDocument.TYPE_TABLE, MnuDocument.TYPE_LIST,
			MnuDocument.TYPE_MULTI, MnuDocument.TYPE_LAN_LIST] \
			and frame.scroll_hit_at(index, position) != 0:
		return id
	return -1


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
