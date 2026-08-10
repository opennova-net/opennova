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

# The editor's consumer strings mapped onto the engine's consumer enum
# (ObjectData.GENERATOR_CONSUMER_*; the dispatch matrix lives at the engine
# home, engine/formats/threedi threedi_panm.h).
const _CONSUMER_IDS := {
	CONSUMER_UV: ObjectData.GENERATOR_CONSUMER_UV,
	CONSUMER_RGB: ObjectData.GENERATOR_CONSUMER_RGB,
	CONSUMER_ALPHA: ObjectData.GENERATOR_CONSUMER_ALPHA,
	CONSUMER_LIGHT: ObjectData.GENERATOR_CONSUMER_LIGHT,
	CONSUMER_PANM: ObjectData.GENERATOR_CONSUMER_PANM,
}

# Named mirrors of the control-register style ids used by the label overrides
# and callers' subset lists. The canonical id list is the engine's
# (ObjectData.get_generator_style_ids()); generator_style_catalog_test pins
# these against it.
const STYLE_CONTROL_SET := 0x71
const STYLE_CONTROL_ADD := 0x72
const STYLE_CONTROL_SKEW := 0x73
const STYLE_CONTROL_MULTIPLY := 0x74
const STYLE_CONTROL_ROTATE := 0x75

# Friendly labels per canonical style id — an editor concern, deliberately kept
# here (the engine keeps CODE-style names; see _base_label's fallback). The
# canonical id LIST is the engine's; this table only decorates it.
const _FRIENDLY_LABELS := {
	0: "None",
	16: "Slide",
	17: "Slide inverse",
	24: "Set",
	32: "Rotate CW",
	33: "Rotate CCW",
	49: "Set wave: square",
	50: "Set wave: sine",
	51: "Set wave: triangle",
	52: "Set wave: saw",
	53: "Set wave: inverse saw",
	54: "Set wave: random",
	55: "Set wave: smooth random",
	56: "Set wave: half sine",
	57: "Set wave: pulse",
	58: "Set wave: vibrate",
	63: "Set wave: heartbeat",
	65: "Add wave: square",
	66: "Add wave: sine",
	67: "Add wave: triangle",
	68: "Add wave: saw",
	69: "Add wave: inverse saw",
	70: "Add wave: random",
	71: "Add wave: smooth random",
	72: "Add wave: half sine",
	73: "Add wave: pulse",
	74: "Add wave: vibrate",
	79: "Add wave: heartbeat",
	81: "Skew wave: square",
	82: "Skew wave: sine",
	83: "Skew wave: triangle",
	84: "Skew wave: saw",
	85: "Skew wave: inverse saw",
	86: "Skew wave: random",
	87: "Skew wave: smooth random",
	88: "Skew wave: half sine",
	89: "Skew wave: pulse",
	90: "Skew wave: vibrate",
	95: "Skew wave: heartbeat",
	97: "Multiply wave: square",
	98: "Multiply wave: sine",
	99: "Multiply wave: triangle",
	100: "Multiply wave: saw",
	101: "Multiply wave: inverse saw",
	102: "Multiply wave: random",
	103: "Multiply wave: smooth random",
	104: "Multiply wave: half sine",
	105: "Multiply wave: pulse",
	106: "Multiply wave: vibrate",
	111: "Multiply wave: heartbeat",
	113: "Set (control register)",
	114: "Add (control register)",
	115: "Skew (control register)",
	116: "Multiply (control register)",
	117: "Rotate (control register)",
}

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
	if not style_ids().has(id):
		return null
	return StyleInfo.new(
			id,
			_label_for_consumer(consumer, id, _base_label(id)),
			reads_control_value(consumer, id),
			parameter_is_ctrl_reference(id))


# The canonical style-id list is the engine's (kControlEntries; the byte table
# lives at the engine home, engine/formats/threedi threedi_panm.cpp).
static func style_ids() -> PackedInt32Array:
	return ObjectData.get_generator_style_ids()


static func style_count() -> int:
	return style_ids().size()


# Full typed option list for one retail consumer, in canonical engine order.
static func options_for_consumer(consumer: String) -> Array[StyleInfo]:
	var result: Array[StyleInfo] = []
	for id in style_ids():
		var info := style_info(consumer, int(id))
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


# The per-consumer control-register dispatch is the engine's
# (ObjectData.generator_style_reads_control_value over the witnessed matrix).
static func reads_control_value(consumer: String, id: int) -> bool:
	if not _is_consumer(consumer):
		return false
	return ObjectData.generator_style_reads_control_value(
			int(_CONSUMER_IDS[consumer]), id)


# The loader's high-style parameter fixup threshold is the engine's
# (ObjectData.generator_style_parameter_is_ctrl_reference).
static func parameter_is_ctrl_reference(id: int) -> bool:
	return ObjectData.generator_style_parameter_is_ctrl_reference(id)


static func _is_consumer(consumer: String) -> bool:
	return CONSUMERS.has(consumer)


# The friendly label for a canonical id; an engine-listed code this table does
# not decorate surfaces the engine's CODE-style name (never an invented one).
static func _base_label(id: int) -> String:
	if _FRIENDLY_LABELS.has(id):
		return String(_FRIENDLY_LABELS[id])
	return ObjectData.generator_style_code_name(id)


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
