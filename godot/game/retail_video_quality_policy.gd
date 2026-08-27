extends RefCounted

# Applies the engine's pinned VIDEO configuration to the authored Options
# rows: OpenNova ports exactly one retail renderer path, and its controls stay
# visible as documentation, locked. The values, the gamma reference and the
# preset-button set are engine/runtime/menu/options_policy.h (MenuFrame
# re-exports them); widget selection and locking are the shell's.


static func apply(driver: MenuDriver) -> void:
	for control in MenuFrame.video_quality_controls():
		_select_semantic_value(driver, String(control["control"]),
				String(control["value"]))

	# Gamma is calibration, not a quality rung: the engine's comparison-profile
	# reference, then locked.
	var gamma := driver.widget_id("GAMMA")
	if gamma >= 0 and driver.widget_kind_of(gamma) == MnuDocument.TYPE_SCROLL:
		var range := driver.get_widget_scroll_range(gamma)
		if range != null:
			driver.set_widget_scroll_range(gamma, range.minimum, range.maximum,
					range.page, MenuFrame.video_gamma_reference())
		driver.set_widget_disabled(gamma, true)

	# Some retail menu revisions expose RESOLUTION while JO comments it out.
	# When present, show the last/highest authored mode and lock it as well.
	var resolution := driver.widget_id("RESOLUTION")
	if resolution >= 0:
		var count := driver.item_count(resolution)
		if count > 0:
			driver.select_row(resolution, count - 1, false)
		driver.set_widget_disabled(resolution, true)

	for button_name in MenuFrame.video_preset_buttons():
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
