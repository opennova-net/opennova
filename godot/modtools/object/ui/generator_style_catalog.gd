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
static func _consumer_id(consumer: String) -> int:
	match consumer:
		CONSUMER_UV:
			return ObjectData.GENERATOR_CONSUMER_UV
		CONSUMER_RGB:
			return ObjectData.GENERATOR_CONSUMER_RGB
		CONSUMER_ALPHA:
			return ObjectData.GENERATOR_CONSUMER_ALPHA
		CONSUMER_LIGHT:
			return ObjectData.GENERATOR_CONSUMER_LIGHT
		CONSUMER_PANM:
			return ObjectData.GENERATOR_CONSUMER_PANM
	return -1

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
# canonical id LIST is the engine's; this table only decorates it. Waveform
# families compose "<family> wave: <waveform>" from the byte's two nibbles.
const _WAVEFORM_LABELS: PackedStringArray = [
	"", "square", "sine", "triangle", "saw", "inverse saw", "random",
	"smooth random", "half sine", "pulse", "vibrate", "", "", "", "",
	"heartbeat",
]


static func _friendly_label(id: int) -> String:
	match id:
		0x00:
			return "None"
		0x10:
			return "Slide"
		0x11:
			return "Slide inverse"
		0x18:
			return "Set"
		0x20:
			return "Rotate CW"
		0x21:
			return "Rotate CCW"
		STYLE_CONTROL_SET:
			return "Set (control register)"
		STYLE_CONTROL_ADD:
			return "Add (control register)"
		STYLE_CONTROL_SKEW:
			return "Skew (control register)"
		STYLE_CONTROL_MULTIPLY:
			return "Multiply (control register)"
		STYLE_CONTROL_ROTATE:
			return "Rotate (control register)"
	var waveform := _WAVEFORM_LABELS[id & 0xF]
	if waveform.is_empty():
		return ""
	match id & 0xF0:
		0x30:
			return "Set wave: %s" % waveform
		0x40:
			return "Add wave: %s" % waveform
		0x50:
			return "Skew wave: %s" % waveform
		0x60:
			return "Multiply wave: %s" % waveform
	return ""

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
			_consumer_id(consumer), id)


# The loader's high-style parameter fixup threshold is the engine's
# (ObjectData.generator_style_parameter_is_ctrl_reference).
static func parameter_is_ctrl_reference(id: int) -> bool:
	return ObjectData.generator_style_parameter_is_ctrl_reference(id)


static func _is_consumer(consumer: String) -> bool:
	return CONSUMERS.has(consumer)


# The friendly label for a canonical id; an engine-listed code this table does
# not decorate surfaces the engine's CODE-style name (never an invented one).
static func _base_label(id: int) -> String:
	var friendly := _friendly_label(id)
	if not friendly.is_empty():
		return friendly
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
