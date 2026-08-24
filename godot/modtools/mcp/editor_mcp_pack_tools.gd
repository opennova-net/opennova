class_name EditorMcpPackTools
extends RefCounted

## Packaging MCP tools: turn the mounted asset root into a game dir something can actually run.
##
## Launching retail on the result is NOT here - it is `run_game` with mode="retail", so it
## goes through ONED's one managed child (F8 stops it) instead of spawning a second process.
##
## Both tools route through [EditorGamePacker], the same seam the editor's Play-in-Retail action
## uses, so an agent cannot produce a game dir a human could not — the rule the mission tools set
## ("every mutation routes through the seams the editor UI itself uses").
##
## Retail is the ORACLE for authored assets: it is the original consumer, so an asset it loads
## and renders is a correct asset. Packing is what makes that provable.

const PackerScript := preload("res://modtools/editor/editor_game_packer.gd")


var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("pack_game",
			"Build the SHIPPABLE packed game from the mounted asset root into game_dir (the tagged-release flavor; the dev loop plays the loose tree directly and needs no packing). Every packable file (missions included) is archived into a single localres.pff, with the .sbf music banks and earlyerr.txt written loose beside it (both are read outside the archives). The archive is named localres.pff because retail's boot table probes six fixed names and its mission list is built from the localres/language volumes -- a mission archived in resource.pff loads by name but never lists. game_dir may already hold exes or a runtime; only the game's own artifact names are overwritten. Same seam as the editor's Export Game action and the --pack-game CLI. Returns what was archived and which artifacts were written.",
			{
				"game_dir": { "type": "string", "description": "Absolute directory to write the packed game into (may be a populated game dir)." },
			}, ["game_dir"]), Callable(self, "_tool_pack_game"))


func _tool_pack_game(args: Dictionary, ctx: McpToolContext) -> Variant:
	var game_dir := String(args.get("game_dir", "")).strip_edges()
	if game_dir.is_empty() or not game_dir.is_absolute_path():
		return McpToolResult.error("game_dir must be an absolute directory path.")
	var root: Variant = ctx.root()
	if root == null:
		return McpToolResult.error("No resource directory mounted — set one in the editor's Settings (gear) popup.")
	ctx.status("Exporting packed game %s -> %s" % [String(root.get_root_dir()), game_dir])
	var out: Dictionary = PackerScript.export_game(root, game_dir)
	if not bool(out.get("ok", false)):
		return McpToolResult.error(String(out.get("error", "Packing failed.")))
	return {
		"game_dir": out["game_dir"],
		"exported": out["exported"],
		"archived_count": out["archived"].size(),
		"archived": out["archived"],
		"skipped": out["skipped"],
		"skipped_dirs": out["skipped_dirs"],
	}
