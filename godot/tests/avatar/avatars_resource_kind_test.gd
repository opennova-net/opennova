extends GutTest

# Regression for "Avatars.def does not show in the resource browser / quick open".
# The browser/quick-open list a kind via NovaResourceIndex.get_resource_files(kind),
# and the Avatars workspace declares the "avatar" open-resource kind. The C++
# classifier (libs/resource_index) tags Avatars.def by NAME -> "avatar" (the .def
# extension is shared with weapon/items/ammo/hudpos.def, which stay unbrowsable).
const AVATARS_FIXTURE_DIR := "res://../fixtures/avatars"


func test_avatars_def_is_indexed_under_the_avatar_kind() -> void:
	var idx := NovaResourceIndex.new()
	var dir := ProjectSettings.globalize_path(AVATARS_FIXTURE_DIR)
	assert_eq(idx.scan(dir), OK, "fixtures/avatars scans")

	var entries := idx.get_resource_files("avatar")
	assert_eq(entries.size(), 1, "Avatars.def is listed under the 'avatar' kind")
	if entries.is_empty():
		return
	var e: Dictionary = entries[0]
	assert_eq(String(e.get("kind", "")), "avatar")
	assert_eq(String(e.get("logical_name", "")), "Avatars.def")
	assert_eq(String(e.get("display_name", "")), "Avatars")

	# It is also a recognized entry in the all-kinds listing (so a kind-agnostic
	# quick-open surfaces it), but it is NOT tagged as a bare "def" kind.
	assert_eq(idx.get_resource_files("*").size(), 1, "recognized in the all-kinds listing")
	assert_true(idx.get_resource_files("def").is_empty(), "no bare 'def' kind")
