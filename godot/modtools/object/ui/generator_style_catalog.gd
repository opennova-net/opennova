class_name GeneratorStyleCatalog
extends RefCounted

## Single source of artist-friendly metadata for the NovaLogic generator-style
## byte. The byte values are shared, but their dispatch is not: UV, RGB, alpha,
## light-color, and PANM consumers interpret the 0x71..0x75 range differently.
## Callers therefore select a consumer instead of treating the raw byte table as
## one universal enum.
##
## The byte is family (high nibble) + waveform (low nibble):
##   0x1X slide/set, 0x2X rotate, 0x3X set wave, 0x4X add wave, 0x5X skew wave,
##   0x6X multiply wave, 0x7X control register; waveform low nibble:
##   1 square, 2 sine, 3 triangle, 4 saw, 5 inverse saw, 6 random,
##   7 smooth random, 8 half sine, 9 pulse, A vibrate, F heartbeat.
##
## Friendly labels live here (an editor concern) rather than in the host-agnostic
## C++ core, which keeps CODE-style names (SET_WAVE_SMOOTH_RANDOM) for debug output.
## generator_style_catalog_test.gd parity-checks that every canonical code is named
## and pins the consumer-specific control-register matrix.

const CONSUMER_UV := "uv"
const CONSUMER_RGB := "rgb"
const CONSUMER_ALPHA := "alpha"
const CONSUMER_LIGHT := "light"
const CONSUMER_PANM := "panm"
const CONSUMERS := [
	CONSUMER_UV,
	CONSUMER_RGB,
	CONSUMER_ALPHA,
	CONSUMER_LIGHT,
	CONSUMER_PANM,
]

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

# Reading the global CTRL value is a property of the consumer dispatch, not the
# raw style byte. This is deliberately separate from the on-disk parameter:
# the loader rewrites that parameter from a model-local CTRL reference for every
# style > 0x70, even when a later consumer uses the resolved ordinal as phase.
# Treating both facts as one "uses register" flag either mislabels waveform
# fallbacks or exposes an inert phase editor.
# [orig: compute_uv_transform_matrix @ 0x5B1990; RgbGen_EvaluateColor
# @ 0x5B23D0; AlphaGen_EvaluateValue @ 0x5B2320; PANM_SampleTrack @ 0x5B2270;
# loader fixup sub_5B4640 @ 0x5B4640]
const VALUE_READ_STYLE_IDS_BY_CONSUMER := {
	CONSUMER_UV: [113, 114, 115, 116, 117],
	CONSUMER_RGB: [113, 114],
	CONSUMER_ALPHA: [113],
	CONSUMER_LIGHT: [113, 114],
	CONSUMER_PANM: [113],
}

# The generic names above describe the UV operation family. Other consumers
# route the same raw codes through either their direct CTRL branch or the wave
# table selected by the low nibble.
const LABEL_OVERRIDES_BY_CONSUMER := {
	CONSUMER_RGB: {
		115: "Wave: triangle",
		116: "Wave: saw",
		117: "Wave: inverse saw",
	},
	CONSUMER_ALPHA: {
		114: "Wave: sine",
		115: "Wave: triangle",
		116: "Wave: saw",
		117: "Wave: inverse saw",
	},
	CONSUMER_LIGHT: {
		115: "Wave: triangle",
		116: "Wave: saw",
		117: "Wave: inverse saw",
	},
	CONSUMER_PANM: {
		114: "Wave: sine",
		115: "Wave: triangle",
		116: "Wave: saw",
		117: "Wave: inverse saw",
	},
}


static func style_info(consumer: String, id: int) -> Dictionary:
	if not CONSUMERS.has(consumer):
		return {}
	for base_style in STYLES:
		if int(base_style.get("id", -1)) != id:
			continue
		var result: Dictionary = base_style.duplicate(true)
		var overrides: Dictionary = LABEL_OVERRIDES_BY_CONSUMER.get(consumer, {})
		if overrides.has(id):
			result["label"] = String(overrides[id])
		result["reads_control_value"] = reads_control_value(consumer, id)
		result["parameter_is_ctrl_reference"] = parameter_is_ctrl_reference(id)
		return result
	return {}


# Full option list for one retail consumer (duplicated so callers may mutate it).
static func options_for_consumer(consumer: String) -> Array:
	var result := []
	for style in STYLES:
		var info := style_info(consumer, int(style.get("id", -1)))
		if not info.is_empty():
			result.append(info)
	return result


# Options filtered to the given ids, in the order ids are listed. Used by PANM,
# whose semantic authoring interface intentionally exposes a smaller subset.
static func options_for_ids(consumer: String, ids: Array) -> Array:
	var result := []
	for id in ids:
		var info := style_info(consumer, int(id))
		if not info.is_empty():
			result.append(info)
	return result


static func reads_control_value(consumer: String, id: int) -> bool:
	var ids: Array = VALUE_READ_STYLE_IDS_BY_CONSUMER.get(consumer, [])
	return ids.has(id)


static func parameter_is_ctrl_reference(id: int) -> bool:
	return id > 0x70
