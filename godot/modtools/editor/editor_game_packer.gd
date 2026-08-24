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

## Never packed and never copied: the retail runtime, its own writes, and any archive already
## present (packing an archive into an archive). The runtime binaries are staged separately from
## the configured retail dir, not taken from the asset root.
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

## Written into every packed dir. F7 packs into the editor's own data dir (user://packed), but
## the MCP pack_game tool takes an arbitrary out_dir, so the dir stays self-ignoring wherever it
## lands -- and the marker is what lets a repack wipe stale output: a non-empty dir WITHOUT it
## is somebody else's and is never touched.
const MARKER_NAME := ".gitignore"
const MARKER_HEADER := "# OpenNova packed game dir -- regenerated on every pack; nothing here is a source."
const MARKER_TEXT := MARKER_HEADER + "\n*\n"


## Pack `root`'s files into `out_dir` as a runnable game dir.
##
## Returns { ok, archive, archived, loose, skipped, skipped_dirs, error } — `archived`/`loose`
## are name arrays so a caller can report exactly what shipped. A previous pack's output in
## `out_dir` is removed first, so a renamed or deleted asset cannot survive as a stale loose
## file that retail's `/d` lookup would prefer over the freshly archived one.
static func pack(root: Object, out_dir: String) -> Dictionary:
	var result := {
		"ok": false, "archive": "", "archived": PackedStringArray(),
		"loose": PackedStringArray(), "skipped": PackedStringArray(),
		"skipped_dirs": PackedStringArray(), "error": "",
	}
	if root == null:
		result["error"] = "No resource root mounted."
		return result

	var root_dir := String(root.get_root_dir())
	if root_dir.is_empty():
		result["error"] = "Mounted root has no directory."
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
	# A short write (disk full, a file the runtime still holds open) must not report a copy.
	var stored := out.store_buffer(bytes)
	var write_err := out.get_error()
	out.close()
	if not stored or write_err != OK:
		return write_err if write_err != OK else ERR_FILE_CANT_WRITE
	return OK


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


## Pack `root` into `out_dir` and stage the retail runtime beside it, ready to launch.
##
## Returns { ok, exe, packed_dir, archived, loose, error }. This is the seam
## ShellGameSession's RETAIL mode injects, so Play in Retail and the MCP tool cannot drift.
## Every runtime file is required: a stage that silently came up short would launch a stale
## exe, or reproduce the no-game.cfg hang, while reporting success.
static func pack_for_retail(root: Object, out_dir: String, retail_dir: String) -> Dictionary:
	var clean_retail := retail_dir.strip_edges()
	if clean_retail.is_empty() or not DirAccess.dir_exists_absolute(clean_retail):
		return { "ok": false, "exe": "", "error": "Retail install directory not found: %s" % retail_dir }

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

	var packed := pack(root, out_dir)
	if not bool(packed.get("ok", false)):
		return { "ok": false, "exe": "", "error": String(packed.get("error", "Packing failed.")) }

	for item in sources:
		var copy_err := _copy_file(String(item["src"]), String(item["dst"]))
		if copy_err != OK:
			return { "ok": false, "exe": "",
				"error": "Staging %s into %s failed: %s" % [
						String(item["src"]), out_dir, error_string(copy_err)] }

	packed["exe"] = out_dir.path_join("Jointops.exe")
	packed["packed_dir"] = out_dir
	return packed
