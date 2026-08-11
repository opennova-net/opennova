extends RefCounted

# The named Options CScrollWnd setup. Each row is [min, max, page], where page
# is the original inclusive-page field. The reimplementation does not yet own
# persisted render/audio/input settings, so current=min is the deterministic
# fallback; a settings owner can replace it through MenuDriver's range setter.
# [orig: options_screen_init @ 0x554800; UI_PopulateRenderAndAudioSettings
# @ 0x55c830; untouched page=10 default in CScrollWnd_Construct @ 0x64c450]
# Seed every witnessed Options slider present in this document; unrelated
# menus simply make this a no-op.
static func apply(driver: MenuDriver) -> void:
	apply_one(driver, "GAMMA", 5, 20, 2)
	apply_one(driver, "SOUNDFXVOLUME", 0, 255, 10)
	apply_one(driver, "DIALOGVOLUME", 0, 255, 10)
	apply_one(driver, "MUSICVOLUME", 0, 255, 10)
	apply_one(driver, "MOUSE_SENSITIVITY", 4, 511, 10)


static func apply_one(driver: MenuDriver, control_name: String,
		minimum: int, maximum: int, page: int) -> void:
	var id := driver.widget_id(control_name)
	if id < 0 or driver.widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		return
	driver.set_widget_scroll_range(id, minimum, maximum, page, minimum)
