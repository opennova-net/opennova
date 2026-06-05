class_name GeneratorStyleCatalog
extends RefCounted

## Single source of artist-friendly names for the NovaLogic generator-style byte
## shared by material generators, light color generators, and part-animation
## drivers. Mirrors the canonical C++ enum kControlEntries in
## libs/threedi/src/threedi_panm.cpp (consumed by threedi_control_func_info),
## which carries the original-tool provenance "matches sub_4350B0".
##
## The byte is family (high nibble) + waveform (low nibble):
##   0x1X slide/set, 0x2X rotate, 0x3X set wave, 0x4X add wave, 0x5X skew wave,
##   0x6X multiply wave, 0x7X control register; waveform low nibble:
##   1 square, 2 sine, 3 triangle, 4 saw, 5 inverse saw, 6 random,
##   7 smooth random, 8 half sine, 9 pulse, A vibrate, F heartbeat.
##
## Friendly labels live here (an editor concern) rather than in the host-agnostic
## C++ core, which keeps CODE-style names (SET_WAVE_SMOOTH_RANDOM) for debug output.
## generator_style_catalog_test.gd parity-checks that every canonical code is named.

# {id, label} for every code in kControlEntries, in canonical order.
const STYLES := [
	{"id": 0, "label": "None"},
	{"id": 16, "label": "Slide"},
	{"id": 17, "label": "Slide inverse"},
	{"id": 24, "label": "Set"},
	{"id": 32, "label": "Rotate CW"},
	{"id": 33, "label": "Rotate CCW"},
	{"id": 49, "label": "Set wave: square"},
	{"id": 50, "label": "Set wave: sine"},
	{"id": 51, "label": "Set wave: triangle"},
	{"id": 52, "label": "Set wave: saw"},
	{"id": 53, "label": "Set wave: inverse saw"},
	{"id": 54, "label": "Set wave: random"},
	{"id": 55, "label": "Set wave: smooth random"},
	{"id": 56, "label": "Set wave: half sine"},
	{"id": 57, "label": "Set wave: pulse"},
	{"id": 58, "label": "Set wave: vibrate"},
	{"id": 63, "label": "Set wave: heartbeat"},
	{"id": 65, "label": "Add wave: square"},
	{"id": 66, "label": "Add wave: sine"},
	{"id": 67, "label": "Add wave: triangle"},
	{"id": 68, "label": "Add wave: saw"},
	{"id": 69, "label": "Add wave: inverse saw"},
	{"id": 70, "label": "Add wave: random"},
	{"id": 71, "label": "Add wave: smooth random"},
	{"id": 72, "label": "Add wave: half sine"},
	{"id": 73, "label": "Add wave: pulse"},
	{"id": 74, "label": "Add wave: vibrate"},
	{"id": 79, "label": "Add wave: heartbeat"},
	{"id": 81, "label": "Skew wave: square"},
	{"id": 82, "label": "Skew wave: sine"},
	{"id": 83, "label": "Skew wave: triangle"},
	{"id": 84, "label": "Skew wave: saw"},
	{"id": 85, "label": "Skew wave: inverse saw"},
	{"id": 86, "label": "Skew wave: random"},
	{"id": 87, "label": "Skew wave: smooth random"},
	{"id": 88, "label": "Skew wave: half sine"},
	{"id": 89, "label": "Skew wave: pulse"},
	{"id": 90, "label": "Skew wave: vibrate"},
	{"id": 95, "label": "Skew wave: heartbeat"},
	{"id": 97, "label": "Multiply wave: square"},
	{"id": 98, "label": "Multiply wave: sine"},
	{"id": 99, "label": "Multiply wave: triangle"},
	{"id": 100, "label": "Multiply wave: saw"},
	{"id": 101, "label": "Multiply wave: inverse saw"},
	{"id": 102, "label": "Multiply wave: random"},
	{"id": 103, "label": "Multiply wave: smooth random"},
	{"id": 104, "label": "Multiply wave: half sine"},
	{"id": 105, "label": "Multiply wave: pulse"},
	{"id": 106, "label": "Multiply wave: vibrate"},
	{"id": 111, "label": "Multiply wave: heartbeat"},
	{"id": 113, "label": "Set (control register)"},
	{"id": 114, "label": "Add (control register)"},
	{"id": 115, "label": "Skew (control register)"},
	{"id": 116, "label": "Multiply (control register)"},
	{"id": 117, "label": "Rotate (control register)"},
]

# Control-register styles bind a control register instead of phase (kControlEntries
# is_register_func = 1 for 0x71..0x75); the inspectors swap to a register picker.
const REGISTER_STYLE_IDS := [113, 114, 115, 116, 117]


# Full option list (duplicated copy so callers can mutate freely).
static func options() -> Array:
	return STYLES.duplicate(true)


# Options filtered to the given ids, in the order ids are listed. Used by the
# part-animation driver, whose C++ mode-string API supports only a subset.
static func options_for_ids(ids: Array) -> Array:
	var by_id := {}
	for style in STYLES:
		by_id[int(style.get("id", 0))] = style
	var result := []
	for id in ids:
		var key := int(id)
		if by_id.has(key):
			result.append((by_id[key] as Dictionary).duplicate(true))
	return result


static func uses_register(id: int) -> bool:
	return REGISTER_STYLE_IDS.has(id)
