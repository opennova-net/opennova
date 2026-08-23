class_name EditorGamePacker
extends RefCounted

## Packs a mounted asset root into a runnable game dir.
##
## This is the "ship it" half of authoring: the editor writes assets loose, and retail (or our
## own runtime) needs them as a game dir it can mount. Headless by design — the Play-in-Retail
## action and the MCP tool both call [method pack], so neither owns the rules.
##
## Everything that can live in an archive goes into ONE archive. Retail's boot table probes six
## fixed names and mounts each into a secondary slot at its table index, so slot order IS lookup
## precedence [orig: PFF_OpenAllArchives @ 0x4a4310, table @ 0x829f90] — but precedence only
## decides duplicates, and a single archive has none. Retail ships three (language/localres/
## resource) as an editorial split of its own catalogue; copying that split would mean teaching
## the editor which of retail's archives each KIND of file belongs in, and that convention is
## already written down in three places. One archive needs no such knowledge and mounts
## identically.
##
## The archive is named `resource.pff` because it must be one of the six the boot table probes;
## an arbitrary name never mounts (D-VFS-2) and dies at the zero-archives gate with
## ShowEarlyError(3).

## Files that must stay LOOSE in the game dir rather than going into the archive.
##  - `.sbf` music banks stream by path and never resolve through the archives
##    [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60].
##  - `earlyerr.txt` is the pre-archive error text, read before any mount
##    [orig: Game_ShowEarlyError @ 0x4a68a0] — inside an archive it could never be read.
const LOOSE_EXTENSIONS := [".sbf", ".txt"]

## Missions must ALSO stay loose, and this one is not obvious from the mount rules.
##
## Retail builds its mission table from two scans: a loose `FindFirstFile *.bms` walk of the
## working directory, and a per-archive entry walk that visits only the localres/language volume
## PAIRS [orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF
## @ 0x562910]. `resource.pff` is in neither, so a mission archived there is mounted and
## perfectly loadable BY NAME and still never appears in any mission list — the file resolves,
## the menu is simply empty. Witnessed 2026-08-23: identical boots, the archived mission absent
## from the boot scan, the loose one logged as `LOADED FILE: mnml.bms` + `mnml.bin`.
##
## The sibling `<stem>.bin` carries the mission's `[Info] TITLE` and BRIEFING and is looked up
## alongside it, so it travels with the .bms. Every OTHER `.bin` — the boot string tables and the
## music scripts — resolves from the archive normally and must stay there.
const MISSION_EXTENSION := ".bms"

## Never packed and never copied: the retail runtime, its own writes, and any archive already
## present (packing an archive into an archive). The runtime binaries are staged separately from
## the configured retail dir, not taken from the asset root.
const EXCLUDED_EXTENSIONS := [".pff", ".exe", ".dll", ".sav", ".log", ".ini", ".cfg",
	# Repo metadata that lives beside the assets but is not game data.
	".md", ".gitignore", ".gitattributes"]

## Subdirectories of the asset root that hold authoring sources, not game files. `src/` is
## `.blend`/`.ase` — retail resolves bare filenames at the root, so nothing there is loadable.
## DirAccess.get_files_at does not descend, so this is belt-and-braces for a name that
## arrives with a directory prefix.
const EXCLUDED_DIRS := ["src/"]


## Pack `root`'s files into `out_dir` as a runnable game dir.
##
## Returns { ok, archive, archived, loose, skipped, error } — `archived`/`loose` are name arrays
## so a caller can report exactly what shipped.
static func pack(root: Object, out_dir: String) -> Dictionary:
	var result := {
		"ok": false, "archive": "", "archived": PackedStringArray(),
		"loose": PackedStringArray(), "skipped": PackedStringArray(), "error": "",
	}
	if root == null:
		result["error"] = "No resource root mounted."
		return result

	var root_dir := String(root.get_root_dir())
	if root_dir.is_empty():
		result["error"] = "Mounted root has no directory."
		return result

	var err := DirAccess.make_dir_recursive_absolute(out_dir)
	if err != OK and not DirAccess.dir_exists_absolute(out_dir):
		result["error"] = "Cannot create output directory: %s" % out_dir
		return result

	# Walk the directory. Deliberately NOT ResourceRoot.list_files() or the ResourceIndex:
	# both classify, and `scan()` drops whatever it cannot name a kind for
	# (`if (kind.empty()) continue;`). On this very asset set that reported 16 of 26 files —
	# it omitted items.def (fatal for retail), weapon.def, ammo.def, the .dbf, and every
	# image including the menu cursor. Those are browse indexes; a packer wants the game dir.
	var names := PackedStringArray()
	for file_name in DirAccess.get_files_at(root_dir):
		names.append(String(file_name))
	names.sort()
	if names.is_empty():
		result["error"] = "Mounted root has no files: %s" % root_dir
		return result

	var archive := PffDocument.new()
	var archived := PackedStringArray()
	var loose := PackedStringArray()
	var skipped := PackedStringArray()

	# The mission family: every <stem>.bms plus its sibling <stem>.bin.
	var mission_family := {}
	for name in names:
		var lower_name := String(name).to_lower()
		if lower_name.ends_with(MISSION_EXTENSION):
			mission_family[lower_name] = true
			mission_family["%s.bin" % lower_name.get_basename()] = true

	for name in names:
		var lower := String(name).to_lower()
		if _in_excluded_dir(lower) or _has_extension(lower, EXCLUDED_EXTENSIONS):
			skipped.append(name)
			continue

		var src := root_dir.path_join(name)
		if not FileAccess.file_exists(src):
			skipped.append(name)
			continue

		if _has_extension(lower, LOOSE_EXTENSIONS) or mission_family.has(lower):
			if _copy_file(src, out_dir.path_join(name.get_file())) == OK:
				loose.append(name)
			else:
				skipped.append(name)
			continue

		# store_name is the BARE name: retail resolves archive entries by name, and PFF entry
		# names cap at 16 bytes.
		var store := String(name).get_file()
		if store.length() > 16:
			skipped.append(name)
			push_warning("Packer: '%s' exceeds the 16-byte PFF entry name limit; not packed." % store)
			continue
		if archive.add_file_from_disk(src, store, false) == OK:
			archived.append(store)
		else:
			skipped.append(name)

	var archive_path := out_dir.path_join("resource.pff")
	var save_err := archive.save_as(archive_path)
	if save_err != OK:
		result["error"] = "Writing %s failed: %s" % [archive_path, archive.get_last_error()]
		return result

	result["ok"] = true
	result["archive"] = archive_path
	result["archived"] = archived
	result["loose"] = loose
	result["skipped"] = skipped
	return result


static func _has_extension(lower_name: String, extensions: Array) -> bool:
	for ext in extensions:
		if lower_name.ends_with(String(ext)):
			return true
	return false


static func _in_excluded_dir(lower_name: String) -> bool:
	var normalized := lower_name.replace("\\", "/")
	for dir in EXCLUDED_DIRS:
		if normalized.begins_with(String(dir)) or normalized.contains("/" + String(dir)):
			return true
	return false


static func _copy_file(src: String, dst: String) -> Error:
	var bytes := FileAccess.get_file_as_bytes(src)
	if bytes.is_empty() and FileAccess.get_open_error() != OK:
		return FileAccess.get_open_error()
	var out := FileAccess.open(dst, FileAccess.WRITE)
	if out == null:
		return FileAccess.get_open_error()
	out.store_buffer(bytes)
	out.close()
	return OK


## Staged from the configured retail install into the packed dir. `binkw32_.dll` is the real
## Bink; a JOTAC install's `binkw32.dll` is an unrelated hook shim, so prefer the underscored
## one. `game.cfg` matters: a FIRST launch with no config hangs in retail's video enumeration
## before the menu ever appears (reproduced 2026-08-23). It is machine state, not game content,
## and it is never committed.
const RETAIL_RUNTIME := [
	{ "from": "Jointops.exe", "to": "Jointops.exe" },
	{ "from": "binkw32_.dll", "to": "binkw32.dll", "fallback": "binkw32.dll" },
	{ "from": "game.cfg", "to": "game.cfg" },
]


## Pack `root` into `out_dir` and stage the retail runtime beside it, ready to launch.
##
## Returns { ok, exe, packed_dir, archived, loose, error }. This is the seam
## ShellGameSession's RETAIL mode injects, so Play in Retail and the MCP tool cannot drift.
static func pack_for_retail(root: Object, out_dir: String, retail_dir: String) -> Dictionary:
	var packed := pack(root, out_dir)
	if not bool(packed.get("ok", false)):
		return { "ok": false, "exe": "", "error": String(packed.get("error", "Packing failed.")) }

	var clean_retail := retail_dir.strip_edges()
	if clean_retail.is_empty() or not DirAccess.dir_exists_absolute(clean_retail):
		return { "ok": false, "exe": "", "error": "Retail install directory not found: %s" % retail_dir }

	for entry in RETAIL_RUNTIME:
		var src := clean_retail.path_join(String(entry["from"]))
		if not FileAccess.file_exists(src) and entry.has("fallback"):
			src = clean_retail.path_join(String(entry["fallback"]))
		if not FileAccess.file_exists(src):
			continue
		_copy_file(src, out_dir.path_join(String(entry["to"])))

	var exe := out_dir.path_join("Jointops.exe")
	if not FileAccess.file_exists(exe):
		return { "ok": false, "exe": "",
			"error": "No Jointops.exe in %s — cannot launch retail." % clean_retail }

	packed["exe"] = exe
	packed["packed_dir"] = out_dir
	return packed
