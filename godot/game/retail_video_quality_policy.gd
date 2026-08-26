extends RefCounted

# OpenNova currently ports exactly one retail renderer configuration: the
# highest-quality, pixel-shader path. Keep the authored JO controls visible as
# documentation, pin them to that contract, and make them read-only. These are
# semantic item values, not row numbers. A future settings owner will replace
# this policy with Godot-native quality tiers rather than reviving the obsolete
# retail renderer switches.
class QualityControl:
	extends RefCounted

	var control_name: StringName
	var semantic_value: String

	func _init(p_control_name: StringName, p_semantic_value: String) -> void:
		control_name = p_control_name
		semantic_value = p_semantic_value


const PRESET_BUTTONS := [
	"VIDEODEFAULT", "VIDEOPERFORMANCE", "VIDEOQUALITY",
]


static func _fixed_controls() -> Array[QualityControl]:
	return [
		QualityControl.new(&"16x9DISPLAY", "1"),
		QualityControl.new(&"TERRAINPOLY", "3"),
		QualityControl.new(&"TERRAINTEX", "3"),
		QualityControl.new(&"OBJECTPOLY", "3"),
		QualityControl.new(&"OBJECTTEX", "3"),
		# The authored 3..16 rows are adapter placeholders; mode 2 is the
		# highest multisample mode supported by the retail device contract.
		QualityControl.new(&"ANTIALIAS", "2"),
		QualityControl.new(&"SHADERUSAGE", "2"),
		QualityControl.new(&"WATERQUALITY", "3"),
		QualityControl.new(&"SHADOWQUALITY", "3"),
		QualityControl.new(&"PARTICLES", "2"),
		QualityControl.new(&"FBEFFECTS", "3"),
		QualityControl.new(&"TEXFILTER", "3"),
		# "Minimal" means minimal compression and therefore maximum fidelity.
		QualityControl.new(&"TEXCOMPRESSION", "2"),
	]


static func apply(driver: MenuDriver) -> void:
	for control in _fixed_controls():
		_select_semantic_value(driver, control.control_name,
				control.semantic_value)

	# 8 is the registered retail comparison profile's gamma reference. Gamma
	# is calibration, not a quality rung, so pushing it to the numeric maximum
	# would deliberately distort the parity target.
	var gamma := driver.widget_id("GAMMA")
	if gamma >= 0 and driver.widget_kind_of(gamma) == MnuDocument.TYPE_SCROLL:
		var range := driver.get_widget_scroll_range(gamma)
		if range != null:
			driver.set_widget_scroll_range(gamma, range.minimum, range.maximum,
					range.page, 8)
		driver.set_widget_disabled(gamma, true)

	# Some retail menu revisions expose RESOLUTION while JO comments it out.
	# When present, show the last/highest authored mode and lock it as well.
	var resolution := driver.widget_id("RESOLUTION")
	if resolution >= 0:
		var count := driver.item_count(resolution)
		if count > 0:
			driver.select_row(resolution, count - 1, false)
		driver.set_widget_disabled(resolution, true)

	for button_name in PRESET_BUTTONS:
		var button := driver.widget_id(button_name)
		if button >= 0:
			driver.set_widget_disabled(button, true)


static func _select_semantic_value(driver: MenuDriver, control_name: String,
		value: String) -> void:
	var id := driver.widget_id(control_name)
	if id < 0:
		return
	for row in driver.item_count(id):
		if driver.item_value(id, row) == value:
			driver.select_row(id, row, false)
			break
	driver.set_widget_disabled(id, true)
