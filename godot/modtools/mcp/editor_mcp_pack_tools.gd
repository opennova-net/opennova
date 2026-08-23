class_name EditorMcpPackTools
extends RefCounted

## Packaging MCP tools: turn the mounted asset root into a game dir something can actually run.
##
## Both tools route through [EditorGamePacker], the same seam the editor's Play-in-Retail action
## uses, so an agent cannot produce a game dir a human could not — the rule the mission tools set
## ("every mutation routes through the seams the editor UI itself uses").
##
## Retail is the ORACLE for authored assets: it is the original consumer, so an asset it loads
## and renders is a correct asset. That is what `play_in_retail` is for — not shipping, proving.

const PackerScript := preload("res://modtools/editor/editor_game_packer.gd")

## Copied from the configured retail install into the packed dir. `binkw32_.dll` is the real
## Bink; a JOTAC install's `binkw32.dll` is an unrelated hook shim, so prefer the underscored one.
const RETAIL_BINARIES := [
	{ "from": "Jointops.exe", "to": "Jointops.exe" },
	{ "from": "binkw32_.dll", "to": "binkw32.dll", "fallback": "binkw32.dll" },
	# Retail's first launch with no config hangs in video enumeration before the menu, so the
	# install's own config is seeded rather than letting it be written from defaults. Machine
	# state, not game content.
	{ "from": "game.cfg", "to": "game.cfg" },
]

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("pack_game",
			"Pack the mounted asset root into a runnable game dir at out_dir: every packable file into a single resource.pff, with the .sbf music banks and earlyerr.txt copied loose beside it (both are read outside the archives). Authoring sources under src/, existing archives, and the retail runtime are skipped. The archive is named resource.pff because retail's boot table probes six fixed names and an arbitrary one never mounts. Returns what was archived, copied loose, and skipped.",
			{
				"out_dir": { "type": "string", "description": "Absolute directory to write the game dir into." },
			}, ["out_dir"]), Callable(self, "_tool_pack_game"))
	registry.register(McpToolDef.make("play_in_retail",
			"Pack the mounted assets and launch retail Jointops.exe on them — the asset oracle. Requires a retail install directory configured in the editor's settings (or passed as retail_dir): Jointops.exe, binkw32.dll and game.cfg are staged from there into the packed dir, which is what gets launched. Retail runs with /w /d, plus /FRISK so every resolved load is logged to _filelog.txt for diagnosis. Returns the packed dir, the launch command, and the pid.",
			{
				"out_dir": { "type": "string", "description": "Where to pack and launch. Defaults to a 'packed' dir beside the asset root." },
				"retail_dir": { "type": "string", "description": "Retail install to stage the runtime from; defaults to the configured one." },
				"frisk": { "type": "boolean", "default": true, "description": "Pass /FRISK so loads are logged." },
			}), Callable(self, "_tool_play_in_retail"))


func _tool_pack_game(args: Dictionary, ctx: McpToolContext) -> Variant:
	var out_dir := String(args.get("out_dir", "")).strip_edges()
	if out_dir.is_empty() or not out_dir.is_absolute_path():
		return McpToolResult.error("out_dir must be an absolute directory path.")
	return _pack(out_dir, ctx)


func _tool_play_in_retail(args: Dictionary, ctx: McpToolContext) -> Variant:
	var root: Variant = ctx.root()
	if root == null:
		return McpToolResult.error("No resource directory mounted — set one in the editor's Settings (gear) popup.")

	var retail_dir := String(args.get("retail_dir", "")).strip_edges()
	if retail_dir.is_empty():
		retail_dir = _configured_retail_dir()
	if retail_dir.is_empty():
		return McpToolResult.error(
			"No retail install configured. Set one in the editor's Settings popup, or pass retail_dir.")
	if not DirAccess.dir_exists_absolute(retail_dir):
		return McpToolResult.error("Configured retail directory does not exist: %s" % retail_dir)

	var out_dir := String(args.get("out_dir", "")).strip_edges()
	if out_dir.is_empty():
		out_dir = String(root.get_root_dir()).path_join("..").simplify_path().path_join("packed")

	var packed: Variant = _pack(out_dir, ctx)
	if packed is McpToolResult and packed.is_error:
		return packed

	var staged := PackedStringArray()
	for entry in RETAIL_BINARIES:
		var src := retail_dir.path_join(String(entry["from"]))
		if not FileAccess.file_exists(src) and entry.has("fallback"):
			src = retail_dir.path_join(String(entry["fallback"]))
		if not FileAccess.file_exists(src):
			continue
		if PackerScript._copy_file(src, out_dir.path_join(String(entry["to"]))) == OK:
			staged.append(String(entry["to"]))

	if not staged.has("Jointops.exe"):
		return McpToolResult.error("Jointops.exe not found in %s — cannot launch." % retail_dir)

	# /w windowed, /d loose-first (the packed dir keeps the .sbf banks and earlyerr.txt loose),
	# /FRISK logs every resolved load to _filelog.txt.
	var launch_args := PackedStringArray(["/w", "/d"])
	if bool(args.get("frisk", true)):
		launch_args.append("/FRISK")
	var exe := out_dir.path_join("Jointops.exe")
	var pid := OS.create_process(exe, launch_args, false)
	if pid <= 0:
		return McpToolResult.error("Failed to launch %s" % exe)

	ctx.status("Playing in retail from %s" % out_dir)
	return {
		"packed_dir": out_dir,
		"retail_dir": retail_dir,
		"staged": staged,
		"command": "%s %s" % [exe, " ".join(launch_args)],
		"pid": pid,
		"pack": packed,
		"note": "Loads are logged to %s" % out_dir.path_join("_filelog.txt"),
	}


func _pack(out_dir: String, ctx: McpToolContext) -> Variant:
	var root: Variant = ctx.root()
	if root == null:
		return McpToolResult.error("No resource directory mounted — set one in the editor's Settings (gear) popup.")
	ctx.status("Packing %s -> %s" % [String(root.get_root_dir()), out_dir])
	var out: Dictionary = PackerScript.pack(root, out_dir)
	if not bool(out.get("ok", false)):
		return McpToolResult.error(String(out.get("error", "Packing failed.")))
	return {
		"archive": out["archive"],
		"archived_count": out["archived"].size(),
		"archived": out["archived"],
		"loose": out["loose"],
		"skipped": out["skipped"],
	}


func _configured_retail_dir() -> String:
	if service != null and service.has_method("get_retail_dir"):
		return String(service.get_retail_dir())
	return ""
