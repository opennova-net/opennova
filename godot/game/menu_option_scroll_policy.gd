extends RefCounted

# Seeds every witnessed Options slider present in this document from the
# engine's ranges (engine/runtime/menu/options_policy.h, re-exported by
# MenuFrame); unrelated menus make this a no-op. current = min is the
# deterministic seed until a settings owner replaces it through MenuDriver's
# range setter.
static func apply(driver: MenuDriver) -> void:
	for range in MenuFrame.options_scroll_ranges():
		apply_one(driver, String(range["control"]), int(range["minimum"]),
				int(range["maximum"]), int(range["page"]))


static func apply_one(driver: MenuDriver, control_name: String,
		minimum: int, maximum: int, page: int) -> void:
	var id := driver.widget_id(control_name)
	if id < 0 or driver.widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		return
	driver.set_widget_scroll_range(id, minimum, maximum, page, minimum)
