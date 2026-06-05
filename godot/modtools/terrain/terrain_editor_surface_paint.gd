extends RefCounted

## Surface-type (charmap) UI metadata for the legend and color swatches. The
## paint/sample kernel and heightmap<->map coordinate mapping now live in the
## C++ NovaTerrainSurfaceMap (sharing libs/foliage's index-grid kernel); this
## file is only the artist-facing label/color table.

const DEFAULT_SURFACE_INDEX := 1

const SURFACE_TYPES := [
	{"id": 0, "label": "Null", "color": Color8(0, 0, 0)},
	{"id": 1, "label": "Dirt", "color": Color8(153, 118, 61)},
	{"id": 2, "label": "Grass", "color": Color8(0, 210, 0)},
	{"id": 3, "label": "Snow", "color": Color8(204, 239, 244)},
	{"id": 4, "label": "Cement", "color": Color8(152, 152, 152)},
	{"id": 5, "label": "Sand", "color": Color8(255, 255, 0)},
	{"id": 6, "label": "Packed Dirt", "color": Color8(255, 128, 0)},
	{"id": 7, "label": "(Unused)", "color": Color8(0, 0, 255)},
	{"id": 8, "label": "(Unused)", "color": Color8(255, 0, 0)},
	{"id": 9, "label": "Mud", "color": Color8(114, 64, 0)},
	{"id": 10, "label": "Ice", "color": Color8(160, 190, 219)},
	{"id": 11, "label": "(Unused)", "color": Color8(161, 0, 161)},
	{"id": 12, "label": "Rock/Stone", "color": Color8(255, 0, 186)},
	{"id": 13, "label": "Wood", "color": Color8(158, 78, 0)},
	{"id": 14, "label": "Metal", "color": Color8(0, 201, 203)},
	{"id": 15, "label": "(Unused)", "color": Color8(255, 255, 255)},
]


static func get_surface_types() -> Array:
	return SURFACE_TYPES.duplicate(true)


static func get_surface_label(index: int) -> String:
	for entry in SURFACE_TYPES:
		if int(entry["id"]) == index:
			return String(entry["label"])
	return "Custom %d" % index


static func get_surface_color(index: int, palette: PackedByteArray = PackedByteArray()) -> Color:
	if palette.size() >= (index + 1) * 3 and index >= 0:
		var offset := index * 3
		return Color8(palette[offset], palette[offset + 1], palette[offset + 2])
	for entry in SURFACE_TYPES:
		if int(entry["id"]) == index:
			return entry["color"]
	var gray := clampi(index, 0, 255)
	return Color8(gray, gray, gray)
