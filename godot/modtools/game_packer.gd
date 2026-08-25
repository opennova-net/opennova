class_name GamePacker
extends RefCounted

## Turns a game-data directory into a runnable game directory. Retail staging
## accepts either loose files or an existing packed PFF layout; the hidden
## `--pack-game` CLI creates release archives from loose sources.
##
##  - [method stage_loose] is the DEV/retail-test flavor: every file loose plus the
##    20-byte zero-entry `resource.pff` boot token, run under `/d`. Stage & Run Retail
##    stages this; it is also the layout the dev zip ships (without the token — our own
##    runtime's bundled-assets default mounts the loose tree directly).
##  - [method pack] + [method export_game] are the TAGGED-release flavor: everything
##    archived into one `localres.pff` beside the loose-by-contract files. The release
##    workflow builds shipped game zips with it through the headless CLI.
##
## Everything that can live in an archive goes into ONE archive. Retail's boot table probes six
## fixed names and mounts each into a secondary slot at its table index, so slot order IS lookup
## precedence [orig: PFF_OpenAllArchives @ 0x4a4310, table @ 0x829f90] — but precedence only
## decides duplicates, and a single archive has none. Retail ships three (language/localres/
## resource) as an editorial split of its own catalogue; copying that split would mean teaching
## the packer which of retail's archives each KIND of file belongs in, and that convention is
## already written down in three places. One archive needs no such knowledge and mounts
## identically.
##
## The archive is named `localres.pff`, and the name is load-bearing twice over:
##  - It must be one of the six the boot table probes; an arbitrary name never mounts (D-VFS-2)
##    and dies at the zero-archives gate with ShowEarlyError(3). Any of the six satisfies that
##    gate — it counts archives OPENED, not which slot they filled.
##  - Retail builds its mission list from two scans: a loose `FindFirstFile *.bms` walk of the
##    working directory, and a per-archive entry walk over the localres/language volumes
##    [orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF
##    @ 0x562910]. `resource.pff` is in neither: a mission archived THERE mounts, resolves by
##    name, and never appears in any mission list (witnessed 2026-08-23 — the menu was simply
##    empty). Retail keeps its own missions where the scan looks: every one of stock JO's 116
##    `.bms` lives in `localres.pff` (none in resource.pff), with the title `.bin`s in
##    language.pff, resolved through the ordinary by-name front door. Naming our single archive
##    `localres.pff` puts missions exactly where retail ships them, archived, with no loose
##    special case to keep in step.
## (The C++ minimal-install fixture writes a zero-entry `resource.pff` boot token beside a fully
## LOOSE layout run under `/d`; there the loose walk lists the missions, so that name is fine.)

## Files that must stay LOOSE in the game dir rather than going into the archive.
##  - `.sbf` music banks stream by path and never resolve through the archives
##    [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60].
##  - `earlyerr.txt` is the pre-archive error text, read before any mount
##    [orig: Game_ShowEarlyError @ 0x4a68a0] — inside an archive it could never be read.
const LOOSE_EXTENSIONS := [".sbf", ".txt"]

const ARCHIVE_NAME := "localres.pff"
const BOOT_ARCHIVE_NAMES := ["language.pff", "localres.pff", "resource.pff"]

## Never packed: the retail runtime, its own writes, and any archive already
## present (packing an archive into an archive). Retail staging accepts PFFs;
## runtime binaries still come only from the configured retail install.
const EXCLUDED_EXTENSIONS := [".pff", ".exe", ".dll", ".sav", ".log", ".ini", ".cfg",
	# Repo metadata that lives beside the assets but is not game data.
	".md", ".gitignore", ".gitattributes"]

## Subdirectories of the asset root that hold authoring sources, not game files. `src/` is
## `.blend`/`.ase` — retail resolves bare filenames at the root, so nothing there is loadable.
## Any OTHER subdirectory is reported in `skipped_dirs`: the packer walks the root only, and a
## data directory that silently vanished from the pack is the bug that gets debugged in retail.
const EXCLUDED_DIRS := ["src/"]

## PFF entry names are 16 BYTES; a name that fits in 16 characters can still overflow in UTF-8.
const PFF_NAME_BYTES := 16

## Written into every packed dir. Stage & Run Retail stages into ONED's own
## user://packed directory, while release packaging takes an arbitrary output dir. The marker
## identifies our output and lets a repack wipe stale files; a non-empty directory without it
## is somebody else's and is never touched.
const MARKER_NAME := ".gitignore"
const MARKER_HEADER := "# OpenNova packed game dir -- regenerated on every pack; nothing here is a source."
const MARKER_TEXT := MARKER_HEADER + "\n*\n"


## Pack `root_dir`'s files into `out_dir` as a runnable game dir.
##
## Returns { ok, archive, archived, loose, skipped, skipped_dirs, error } — `archived`/`loose`
## are name arrays so a caller can report exactly what shipped. A previous pack's output in
## `out_dir` is removed first, so a renamed or deleted asset cannot survive as a stale loose
## file that retail's `/d` lookup would prefer over the freshly archived one.
static func pack(root_dir: String, out_dir: String) -> Dictionary:
	var result := {
		"ok": false, "archive": "", "archived": PackedStringArray(),
		"loose": PackedStringArray(), "skipped": PackedStringArray(),
		"skipped_dirs": PackedStringArray(), "error": "",
	}
	root_dir = ProjectSettings.globalize_path(root_dir.strip_edges()).simplify_path()
	if root_dir.is_empty() or not DirAccess.dir_exists_absolute(root_dir):
		result["error"] = "Resource directory not found: %s" % root_dir
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
		result["error"] = "Resource directory has no files: %s" % root_dir
		return result

	var skipped_dirs := PackedStringArray()
	for dir_name in DirAccess.get_directories_at(root_dir):
		var lower_dir := String(dir_name).to_lower() + "/"
		if not _in_excluded_dir(lower_dir):
			skipped_dirs.append(String(dir_name))
	skipped_dirs.sort()
	if not skipped_dirs.is_empty():
		push_warning("Packer: subdirectories are not packed (root files only): %s"
				% ", ".join(skipped_dirs))

	var prepare_error := _prepare_out_dir(out_dir)
	if not prepare_error.is_empty():
		result["error"] = prepare_error
		return result

	var archive := PffDocument.new()
	var archived := PackedStringArray()
	var loose := PackedStringArray()
	var skipped := PackedStringArray()

	for name in names:
		var lower := String(name).to_lower()
		if _in_excluded_dir(lower) or _has_extension(lower, EXCLUDED_EXTENSIONS):
			skipped.append(name)
			continue

		var src := root_dir.path_join(name)
		if not FileAccess.file_exists(src):
			skipped.append(name)
			continue

		if _has_extension(lower, LOOSE_EXTENSIONS):
			var copy_err := _copy_file(src, out_dir.path_join(name.get_file()))
			if copy_err != OK:
				result["error"] = "Copying %s into %s failed: %s" % [
						name, out_dir, error_string(copy_err)]
				return result
			loose.append(name)
			continue

		# store_name is the BARE name: retail resolves archive entries by name, and PFF entry
		# names cap at 16 bytes.
		var store := String(name).get_file()
		if store.to_utf8_buffer().size() > PFF_NAME_BYTES:
			skipped.append(name)
			push_warning("Packer: '%s' exceeds the %d-byte PFF entry name limit; not packed."
					% [store, PFF_NAME_BYTES])
			continue
		if archive.add_file_from_disk(src, store, false) == OK:
			archived.append(store)
		else:
			skipped.append(name)

	var archive_path := out_dir.path_join(ARCHIVE_NAME)
	var save_err := archive.save_as(archive_path)
	if save_err != OK:
		result["error"] = "Writing %s failed: %s" % [archive_path, archive.get_last_error()]
		return result

	result["ok"] = true
	result["archive"] = archive_path
	result["archived"] = archived
	result["loose"] = loose
	result["skipped"] = skipped
	result["skipped_dirs"] = skipped_dirs
	return result


## Make `out_dir` an empty pack target: create it, or clear a previous pack's files out of it.
## Returns an error message, or "" when the dir is ready (marker written).
static func _prepare_out_dir(out_dir: String) -> String:
	if DirAccess.dir_exists_absolute(out_dir):
		var existing := DirAccess.get_files_at(out_dir)
		if not existing.is_empty() and not _is_pack_output(out_dir):
			return ("Refusing to pack into %s: it is not empty and was not written by a previous "
					+ "pack (no %s marker). Point at an empty directory, or delete it first.") % [
					out_dir, MARKER_NAME]
		for file_name in existing:
			var stale := out_dir.path_join(String(file_name))
			var rm_err := DirAccess.remove_absolute(stale)
			if rm_err != OK:
				return "Removing stale %s failed: %s" % [stale, error_string(rm_err)]
	else:
		var err := DirAccess.make_dir_recursive_absolute(out_dir)
		if err != OK and not DirAccess.dir_exists_absolute(out_dir):
			return "Cannot create output directory: %s" % out_dir

	var marker := FileAccess.open(out_dir.path_join(MARKER_NAME), FileAccess.WRITE)
	if marker == null:
		return "Cannot write %s into %s: %s" % [
				MARKER_NAME, out_dir, error_string(FileAccess.get_open_error())]
	marker.store_string(MARKER_TEXT)
	marker.close()
	return ""


static func _is_pack_output(dir: String) -> bool:
	var marker_path := dir.path_join(MARKER_NAME)
	if not FileAccess.file_exists(marker_path):
		return false
	return FileAccess.get_file_as_string(marker_path).begins_with(MARKER_HEADER)


## Stage `root`'s game data into `out_dir`. Existing PFFs are copied as-is. A
## loose source root receives the zero-entry `resource.pff` boot token — the
## retail play-test layout. Retail's boot gate counts archives OPENED, not
## entries, so the 20-byte token clears it (witnessed 2026-08-23,
## docs/vfs/vfs-pff-mount-re.md), and under `/d` the loose `FindFirstFile *.bms` walk
## lists the mission. Same marker/wipe/refuse discipline as [method pack] on its own
## output dir; the 16-byte PFF name cap does not apply to loose files.
static func stage_loose(root_dir: String, out_dir: String) -> Dictionary:
	var result := {
		"ok": false, "staged": PackedStringArray(), "skipped": PackedStringArray(),
		"skipped_dirs": PackedStringArray(), "error": "",
	}
	root_dir = ProjectSettings.globalize_path(root_dir.strip_edges()).simplify_path()
	if root_dir.is_empty() or not DirAccess.dir_exists_absolute(root_dir):
		result["error"] = "Resource directory not found: %s" % root_dir
		return result

	var names := PackedStringArray()
	for file_name in DirAccess.get_files_at(root_dir):
		names.append(String(file_name))
	names.sort()
	if names.is_empty():
		result["error"] = "Resource directory has no files: %s" % root_dir
		return result

	var skipped_dirs := PackedStringArray()
	for dir_name in DirAccess.get_directories_at(root_dir):
		var lower_dir := String(dir_name).to_lower() + "/"
		if not _in_excluded_dir(lower_dir):
			skipped_dirs.append(String(dir_name))
	skipped_dirs.sort()
	if not skipped_dirs.is_empty():
		push_warning("Packer: subdirectories are not staged (root files only): %s"
				% ", ".join(skipped_dirs))

	var prepare_error := _prepare_out_dir(out_dir)
	if not prepare_error.is_empty():
		result["error"] = prepare_error
		return result

	var staged := PackedStringArray()
	var skipped := PackedStringArray()
	var has_boot_archive := false
	for name in names:
		var lower := String(name).to_lower()
		var excluded := _has_extension(lower, EXCLUDED_EXTENSIONS) \
				and not lower.ends_with(".pff")
		if _in_excluded_dir(lower) or excluded:
			skipped.append(name)
			continue
		var src := root_dir.path_join(name)
		if not FileAccess.file_exists(src):
			skipped.append(name)
			continue
		var copy_err := _copy_file(src, out_dir.path_join(name.get_file()))
		if copy_err != OK:
			result["error"] = "Copying %s into %s failed: %s" % [
					name, out_dir, error_string(copy_err)]
			return result
		staged.append(name)
		if BOOT_ARCHIVE_NAMES.has(lower):
			has_boot_archive = true

	if not has_boot_archive:
		var token := PffDocument.new()
		var token_err := token.save_as(out_dir.path_join("resource.pff"))
		if token_err != OK:
			result["error"] = "Writing the resource.pff boot token failed: %s" % token.get_last_error()
			return result

	result["ok"] = true
	result["staged"] = staged
	result["skipped"] = skipped
	result["skipped_dirs"] = skipped_dirs
	return result


## Pack `root` and drop the game's runtime artifacts into an EXISTING game dir — the archive
## plus every loose-by-contract file — overwriting only those names.
##
## This is the "update the shipped game" seam: the dir legitimately holds things that are not
## ours (opennova.exe, ONED, the DLL, a retail runtime), so unlike [method pack]'s own
## output dirs it is never wiped and never refused. The pack itself runs in a private scratch
## under user://, where pack()'s marker/wipe rules apply as usual.
##
## Consumer: the headless `--pack-game` release CLI.
static func export_game(root_dir: String, game_dir: String) -> Dictionary:
	var scratch := ProjectSettings.globalize_path("user://pack-export")
	var packed := pack(root_dir, scratch)
	if not bool(packed.get("ok", false)):
		return packed

	if not DirAccess.dir_exists_absolute(game_dir):
		var mk_err := DirAccess.make_dir_recursive_absolute(game_dir)
		if mk_err != OK and not DirAccess.dir_exists_absolute(game_dir):
			return { "ok": false, "error": "Cannot create game directory: %s" % game_dir }

	var artifacts := PackedStringArray([String(packed["archive"]).get_file()])
	for name in packed["loose"]:
		artifacts.append(String(name).get_file())

	var exported := PackedStringArray()
	for artifact in artifacts:
		var src := scratch.path_join(artifact)
		if not FileAccess.file_exists(src):
			continue
		var copy_err := _copy_file(src, game_dir.path_join(artifact))
		if copy_err != OK:
			packed["ok"] = false
			packed["error"] = "Copying %s into %s failed: %s" % [
					artifact, game_dir, error_string(copy_err)]
			return packed
		exported.append(artifact)

	packed["game_dir"] = game_dir
	packed["exported"] = exported
	return packed


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
	# PFFs and music banks can be gigabytes. Use the filesystem copy path rather
	# than materializing the entire source in a PackedByteArray.
	return DirAccess.copy_absolute(src, dst)


## Staged from the configured retail install into the packed dir. `binkw32_.dll` is the real
## Bink; a JOTAC install's `binkw32.dll` is an unrelated hook shim, so prefer the underscored
## one. `game.cfg` matters: a FIRST launch with no config hangs in retail's video enumeration
## before the menu ever appears (reproduced 2026-08-23). It is machine state, not game content,
## and it is never committed.
const RETAIL_RUNTIME := [
	{ "from": "Jointops.exe", "to": "Jointops.exe" },
	{ "from": "binkw32_.dll", "to": "binkw32.dll", "fallback": "binkw32.dll" },
	{ "from": "game.cfg", "to": "game.cfg",
		"why": "retail's first launch with no game.cfg hangs in video enumeration before the menu; launch retail once from its install dir to create it" },
]


## Return an actionable error when the selected retail install cannot be staged.
static func retail_install_error(retail_dir: String) -> String:
	var clean_retail := retail_dir.strip_edges()
	if clean_retail.is_empty() or not DirAccess.dir_exists_absolute(clean_retail):
		return "Retail install directory not found: %s" % retail_dir
	for entry in RETAIL_RUNTIME:
		var src := clean_retail.path_join(String(entry["from"]))
		if not FileAccess.file_exists(src) and entry.has("fallback"):
			src = clean_retail.path_join(String(entry["fallback"]))
		if not FileAccess.file_exists(src):
			var why := String(entry.get("why", ""))
			return "No %s in %s — cannot stage retail.%s" % [
					String(entry["from"]), clean_retail,
					(" " + why + ".") if not why.is_empty() else ""]
	return ""


## Stage `root` LOOSE (plus the boot token) into `out_dir` with the retail runtime beside
## it, ready to launch `/w /d /FRISK`.
##
## Returns { ok, exe, packed_dir, staged, skipped, error }. This is the seam
## GameRunSession's retail mode calls this directly.
## Every runtime file is required: a stage that silently came up short would launch a stale
## exe, or reproduce the no-game.cfg hang, while reporting success.
static func stage_retail(root_dir: String, retail_dir: String) -> Dictionary:
	var out_dir := ProjectSettings.globalize_path("user://packed")
	var clean_retail := retail_dir.strip_edges()
	var install_error := retail_install_error(clean_retail)
	if not install_error.is_empty():
		return { "ok": false, "exe": "", "error": install_error }

	# Resolve every runtime source BEFORE packing: a missing file fails fast instead of after
	# a full archive write.
	var sources: Array[Dictionary] = []
	for entry in RETAIL_RUNTIME:
		var src := clean_retail.path_join(String(entry["from"]))
		if not FileAccess.file_exists(src) and entry.has("fallback"):
			src = clean_retail.path_join(String(entry["fallback"]))
		if not FileAccess.file_exists(src):
			var why := String(entry.get("why", ""))
			return { "ok": false, "exe": "",
				"error": "No %s in %s — cannot stage retail.%s" % [
						String(entry["from"]), clean_retail, (" " + why + ".") if not why.is_empty() else ""] }
		sources.append({ "src": src, "dst": out_dir.path_join(String(entry["to"])) })

	var packed := stage_loose(root_dir, out_dir)
	if not bool(packed.get("ok", false)):
		return { "ok": false, "exe": "", "error": String(packed.get("error", "Staging failed.")) }

	for item in sources:
		var copy_err := _copy_file(String(item["src"]), String(item["dst"]))
		if copy_err != OK:
			return { "ok": false, "exe": "",
				"error": "Staging %s into %s failed: %s" % [
						String(item["src"]), out_dir, error_string(copy_err)] }

	packed["exe"] = out_dir.path_join("Jointops.exe")
	packed["packed_dir"] = out_dir
	return packed
