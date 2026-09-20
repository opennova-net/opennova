class_name RetailData
extends RefCounted

## The GUT twin of tests/common/retail_paths.h: the documented machine roots
## (docs/asset-gated-tests.md) and nothing else a test reads from the
## environment. Each root getter returns "" when its variable is unset or the
## directory is missing; a gated test then marks itself pending() and returns.
## Machine paths go in .claude/settings.local.json env, never tracked.
## Cross-language twin on the script side: scripts/net/lib.ps1 Get-OpenNova*.


## A packed retail install (Jointops.exe beside its .pff archives): OPENNOVA_JO_DIR.
static func install() -> String:
	return _dir("OPENNOVA_JO_DIR")


## The extracted (loose) retail asset tree: OPENNOVA_JO_ASSETS.
static func assets() -> String:
	return _dir("OPENNOVA_JO_ASSETS")


## `<assets>/fixtures/<rel>`: the retail-interop fixture set the reference tree
## mirrors under its `fixtures/` subtree (the retail files the parsers prove they
## read as shipped: menus, string tables, defs, rigs, ...; they never live in
## this repository). "" when the tree is unset or the file is absent; a test then
## pends with fixture_pending_text(rel) so the CI attestation sees the root's name.
static func fixture(rel: String) -> String:
	var root := assets()
	if root.is_empty():
		return ""
	var path := root.path_join("fixtures").path_join(rel)
	return path if FileAccess.file_exists(path) else ""


## The pending() text for a missing reference fixture; names OPENNOVA_JO_ASSETS
## because scripts/ci/retail_gates_ran.py keys on the root's variable.
static func fixture_pending_text(rel: String) -> String:
	return "OPENNOVA_JO_ASSETS/fixtures/%s (the reference fixture set) is required" % rel


const DEF_RELS := ["def/weapon.def", "def/ammo.def", "def/hudpos.def"]
static var _def_root_cache := ""


## A flat resource root carrying the shipped weapon.def / ammo.def / hudpos.def
## from the reference fixture set beside the authored fixtures/def/items.def,
## staged once per run under the cache dir (ResourceRoot indexes flat names and
## rejects user://). "" when the set is absent; a test then pends with
## fixture_pending_text("def/weapon.def") or skips its script.
static func def_root() -> String:
	if not _def_root_cache.is_empty() and DirAccess.dir_exists_absolute(_def_root_cache):
		return _def_root_cache
	var sources := {
		"items.def": ProjectSettings.globalize_path("res://../fixtures/def/items.def"),
	}
	for rel in DEF_RELS:
		var path := fixture(rel)
		if path.is_empty():
			return ""
		sources[rel.get_file()] = path
	var dir := OS.get_cache_dir().path_join("opennova_retail_defs")
	if DirAccess.make_dir_recursive_absolute(dir) != OK:
		return ""
	for name in sources:
		var target := dir.path_join(name)
		if FileAccess.file_exists(target):
			DirAccess.remove_absolute(target)
		if DirAccess.copy_absolute(sources[name], target) != OK:
			return ""
	_def_root_cache = dir
	return dir


## The should_skip_script() verdict of a script that needs def_root(): the
## pending text naming the root when the set is absent, else false.
static func def_root_skip():
	if def_root().is_empty():
		return fixture_pending_text("def/weapon.def")
	return false


## The should_skip_script() verdict of a script that stages reference
## fixtures: the pending text of the first absent `rels` entry, else false.
static func fixtures_skip(rels: Array):
	for rel in rels:
		if fixture(String(rel)).is_empty():
			return fixture_pending_text(String(rel))
	return false


## The expansion names the install carries (the engine's own enumeration of
## <install>/expansion), sorted; empty without an install.
static func expansions() -> PackedStringArray:
	var out := PackedStringArray()
	var root := install()
	if root.is_empty():
		return out
	out.append_array(ResourceRoot.new().list_expansions(root))
	out.sort()
	return out


## Mount the install base and then each expansion in turn, returning the first
## runtime mount that serves `witness`; null without an install or when nothing
## serves it. A test that needs one retail file names it instead of guessing
## which expansion packs it.
static func mount_install_with(witness: String, game: String = "jo") -> ResourceRoot:
	var root := install()
	if root.is_empty():
		return null
	var candidates := PackedStringArray([""])
	candidates.append_array(expansions())
	for expansion in candidates:
		var mount := ResourceRoot.new()
		if mount.mount_runtime(root, expansion, false, game) != OK:
			continue
		if mount.has_file(witness):
			return mount
	return null


static func _dir(variable: String) -> String:
	var value := OS.get_environment(variable).strip_edges()
	if value.is_empty() or not DirAccess.dir_exists_absolute(value):
		return ""
	return value
