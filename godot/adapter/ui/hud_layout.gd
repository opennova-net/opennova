class_name HudLayout
extends RefCounted

## Maps hudpos.def positions (authored in a fixed virtual design space) onto the real
## HUD surface. The original scales every HUD element this way before drawing.
## [orig: Viewport_ScaleToVirtualCoords @0x5d2b20] — see docs/interface/hud-re.md.
##
## Shell-neutral: pure math, no nodes. Shared by the ONED HUD preview and the runtime
## HUD overlay so the two interpret hudpos.def identically.

const DESIGN_WIDTH := 1024.0
const DESIGN_HEIGHT := 768.0


## Scale a design-space point to surface pixels, with the original's round-to-nearest
## (the engine's one impl — hud/hud_math.h scale_axis).
static func scale_point(design: Vector2, surface: Vector2) -> Vector2:
	return NovaHudPos.scale_point(design, surface)


## Convert an output-pixel delta back into virtual design units. Adding this to an
## authored anchor before scale_point() preserves exact pixel-relative placement at
## every surface size (integer pixel deltas commute with its round-to-nearest).
static func pixel_delta_to_design(delta: Vector2, surface: Vector2) -> Vector2:
	return NovaHudPos.pixel_delta_to_design(delta, surface)


## Scale a design-space rect by scaling both corners as points (matching the original,
## which scales x1,y1 and x2,y2 independently, then takes the difference as the size).
static func scale_rect(design: Rect2, surface: Vector2) -> Rect2:
	var p0 := scale_point(design.position, surface)
	var p1 := scale_point(design.position + design.size, surface)
	return Rect2(p0, p1 - p0)


## Convenience for the [x1,y1,x2,y2] corner rects engine/formats/def stores (health/heat/...).
static func rect_from_corners(x1: int, y1: int, x2: int, y2: int) -> Rect2:
	return Rect2(x1, y1, x2 - x1, y2 - y1)
