class_name HudScopeCircleMask
extends Control

## The scoped-view circle mask: the near-black annulus that masks everything
## outside the scope circle, plus the reticle cross and cardinal grid ticks it
## chains when the equipped weapon authored no SIGHTS row.
##
## retail: the scene frame's overlay fork runs binoculars -> Sighted -> Scoped,
## and the Scoped arm draws the SIGHTS card and THEN this mask unconditionally;
## the card's authored-row count gates only the inner cross and grid, never the
## annulus. The geometry (centre, the two radii off five eighths of the viewport
## height, the aspect scales, the 65-stop / 130-vertex strip, the four tapered
## spokes and the sixteen tick diamonds) and the exact colours are the engine's
## -- runtime/hud/scope_circle_mask.h through HudPos.scope_mask_*, which carries
## every addressed witness. See docs/interface/hud-re.md.
##
## The mask sits behind the HudOverlay draw list and in front of the SIGHTS
## card rows, mirroring the original's submit order (card, then mask, then the
## HUD overlays later in the frame).

## Retail's submit order for the three batches (the values are 0/1/2, so a
## batch id doubles as its slot in the cached arrays below).
static var BATCHES: Array[int] = [
	HudPos.SCOPE_MASK_RING,
	HudPos.SCOPE_MASK_CROSS,
	HudPos.SCOPE_MASK_GRID,
]

var _mask_up := false
var _draw_crosshair := false
var _nvg_lens := false
var _points: Array[PackedVector2Array] = []
var _colors: Array[PackedColorArray] = []
var _indices: Array[PackedInt32Array] = []
var _cached_size := Vector2.ZERO
var _cached_crosshair := false
var _cached_lens := false
var _cached := false


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)


## The Scoped-arm verdict plus the card's row count.
## `up` is true only on the scoped branch (never binocular, never the Sighted
## card); `draw_crosshair` is retail's single argument to the mask drawer -- the
## SIGHTS card drew no authored row. `nvg_lens` is the NVG composite's Scoped
## arm: the lens (FrameFX) draws its own ring, and without authored rows only
## the cross and grid draw here, at unit scale (HudPos.scope_mask_* nvg_lens).
func set_mask_state(up: bool, draw_crosshair: bool, nvg_lens := false) -> void:
	if up == _mask_up and draw_crosshair == _draw_crosshair and nvg_lens == _nvg_lens:
		return
	_mask_up = up
	_draw_crosshair = draw_crosshair
	_nvg_lens = nvg_lens
	visible = up
	queue_redraw()


func _resolve(surface: Vector2) -> void:
	if surface.x <= 0.0 or surface.y <= 0.0:
		_cached = false
		return
	if _cached and surface == _cached_size and _draw_crosshair == _cached_crosshair \
			and _nvg_lens == _cached_lens:
		return
	# The cross/grid unit keys on the full surface width (retail's screen-width
	# global), which equals the viewport width here.
	var width := int(surface.x)
	_points.clear()
	_colors.clear()
	_indices.clear()
	for batch in BATCHES:
		_points.append(HudPos.scope_mask_points(surface, width, _draw_crosshair, batch,
				-1, _nvg_lens))
		_colors.append(HudPos.scope_mask_colors(surface, width, _draw_crosshair, batch,
				-1, _nvg_lens))
		_indices.append(HudPos.scope_mask_indices(surface, width, _draw_crosshair, batch,
				-1, _nvg_lens))
	_cached = true
	_cached_size = surface
	_cached_crosshair = _draw_crosshair
	_cached_lens = _nvg_lens


func _draw() -> void:
	if not _mask_up:
		return
	_resolve(size)
	if not _cached:
		return
	var item := get_canvas_item()
	# The mask is the original's window coordinates, D3D9 pixel centres on the
	# integers (HudPos.d3d9_screen_offset).
	RenderingServer.canvas_item_add_set_transform(item,
			Transform2D(0.0, HudPos.d3d9_screen_offset()))
	# Submit order: the annulus, then (only with no authored rows) the reticle
	# cross and the cardinal grid ticks.
	for batch in BATCHES:
		if _points[batch].is_empty():
			continue
		RenderingServer.canvas_item_add_triangle_array(item, _indices[batch],
				_points[batch], _colors[batch])


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		_cached = false
		queue_redraw()
