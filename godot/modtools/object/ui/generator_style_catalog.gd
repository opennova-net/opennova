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
## Friendly labels live here (an editor concern) rather than in the portable
## C++ core, which keeps CODE-style names (SET_WAVE_SMOOTH_RANDOM) for debug output.
## generator_style_catalog_test.gd parity-checks that every canonical code is named
## and pins the consumer-specific control-register matrix.


class StyleInfo:
	extends InspectorForms.IdOption

	var reads_control_value: bool
	var parameter_is_ctrl_reference: bool

	func _init(p_id: int, p_label: String, p_reads_control_value: bool,
			p_parameter_is_ctrl_reference: bool) -> void:
		super(p_id, p_label)
		reads_control_value = p_reads_control_value
		parameter_is_ctrl_reference = p_parameter_is_ctrl_reference


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

# Raw codes whose 0x7X interpretation varies by retail consumer.
const STYLE_CONTROL_SET := 0x71
const STYLE_CONTROL_ADD := 0x72
const STYLE_CONTROL_SKEW := 0x73
const STYLE_CONTROL_MULTIPLY := 0x74
const STYLE_CONTROL_ROTATE := 0x75

# Private storage for every code in kControlEntries, in canonical order. Typed
# StyleInfo records are the cross-object contract; dictionaries do not escape
# this module.
const _STYLES := [
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
static func style_info(consumer: String, id: int) -> StyleInfo:
	if not _is_consumer(consumer):
		return null
	for base_style in _STYLES:
		if int(base_style.get("id", -1)) != id:
			continue
		return StyleInfo.new(
				id,
				_label_for_consumer(consumer, id, String(base_style.get("label", ""))),
				reads_control_value(consumer, id),
				parameter_is_ctrl_reference(id))
	return null


# Canonical ids are copied so callers cannot mutate the catalog implementation.
static func style_ids() -> PackedInt32Array:
	var result := PackedInt32Array()
	result.resize(_STYLES.size())
	for index in range(_STYLES.size()):
		result[index] = int(_STYLES[index].get("id", -1))
	return result


static func style_count() -> int:
	return _STYLES.size()


# Full typed option list for one retail consumer.
static func options_for_consumer(consumer: String) -> Array[StyleInfo]:
	var result: Array[StyleInfo] = []
	for style in _STYLES:
		var info := style_info(consumer, int(style.get("id", -1)))
		if info != null:
			result.append(info)
	return result


# Options filtered to the given ids, in the order ids are listed. Used by PANM,
# whose semantic authoring interface intentionally exposes a smaller subset.
static func options_for_ids(consumer: String, ids: Array) -> Array[StyleInfo]:
	var result: Array[StyleInfo] = []
	for id in ids:
		var info := style_info(consumer, int(id))
		if info != null:
			result.append(info)
	return result


static func reads_control_value(consumer: String, id: int) -> bool:
	match consumer:
		CONSUMER_UV:
			return id >= STYLE_CONTROL_SET and id <= STYLE_CONTROL_ROTATE
		CONSUMER_RGB, CONSUMER_LIGHT:
			return id == STYLE_CONTROL_SET or id == STYLE_CONTROL_ADD
		CONSUMER_ALPHA, CONSUMER_PANM:
			return id == STYLE_CONTROL_SET
		_:
			return false


static func parameter_is_ctrl_reference(id: int) -> bool:
	return id > 0x70


static func _is_consumer(consumer: String) -> bool:
	return CONSUMERS.has(consumer)


# The generic names describe UV operations. Retail's other consumers route the
# same raw codes either through their direct CTRL branch or through the wave
# table selected by the low nibble.
static func _label_for_consumer(consumer: String, id: int, base_label: String) -> String:
	match consumer:
		CONSUMER_RGB, CONSUMER_LIGHT:
			match id:
				STYLE_CONTROL_SKEW:
					return "Wave: triangle"
				STYLE_CONTROL_MULTIPLY:
					return "Wave: saw"
				STYLE_CONTROL_ROTATE:
					return "Wave: inverse saw"
		CONSUMER_ALPHA, CONSUMER_PANM:
			match id:
				STYLE_CONTROL_ADD:
					return "Wave: sine"
				STYLE_CONTROL_SKEW:
					return "Wave: triangle"
				STYLE_CONTROL_MULTIPLY:
					return "Wave: saw"
				STYLE_CONTROL_ROTATE:
					return "Wave: inverse saw"
	return base_label
