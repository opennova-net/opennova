extends GutTest

# B11 guard: the accent orange lives in exactly ONE place — the editor theme's
# EditorPalette/colors/accent. Pins the palette value (consumers read it via
# get_theme_color(&"accent", &"EditorPalette"), alpha variants via
# Color(accent, a)) and sweeps modtools scripts for re-hardcoded accent
# triples, in both the full-precision and the drifted two-decimal spellings
# the six pre-B11 sites used.

const THEME_PATH := "res://modtools/editor/ui/theme/editor_theme.tres"
const ACCENT := Color(0.8392, 0.5529, 0.2902, 1.0)

const BANNED_PATTERNS := [
	"0.8392, 0.5529, 0.2902",
	"0.84, 0.55, 0.29",
]


func test_theme_defines_the_accent() -> void:
	var theme := load(THEME_PATH) as Theme
	assert_not_null(theme, "the editor theme loads from modtools/editor/ui/theme")
	if theme == null:
		return
	assert_true(theme.has_color(&"accent", &"EditorPalette"),
		"EditorPalette/colors/accent exists in the theme")
	assert_eq(theme.get_color(&"accent", &"EditorPalette"), ACCENT,
		"the accent palette value is pinned")


func test_no_modtools_script_hardcodes_the_accent() -> void:
	var offenders: Array[String] = []
	_scan_dir("res://modtools", offenders)
	assert_eq(offenders.size(), 0,
		"accent triples belong only in editor_theme.tres; use get_theme_color(&\"accent\", &\"EditorPalette\"). Offenders: %s" % [offenders])


func _scan_dir(path: String, offenders: Array[String]) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		var child := path.path_join(name)
		if dir.current_is_dir():
			if not name.begins_with("."):
				_scan_dir(child, offenders)
		elif name.ends_with(".gd"):
			var text := FileAccess.get_file_as_string(child)
			for pattern in BANNED_PATTERNS:
				if text.contains(pattern):
					offenders.append(child)
					break
		name = dir.get_next()
	dir.list_dir_end()
