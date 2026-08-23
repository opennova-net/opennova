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
			"Pack the mounted asset root into a runnable game dir at out_dir: every packable file into a single resource.pff, with the .sbf music banks and earlyerr.txt copied loose beside it (both are read outside the archives). Authoring sources under src/, existing archives, and the retail runtime are skipped. The archive is named resource.pff because retail's boot table probes six fixed names and an arbitrary one never mounts. Returns what was archived, copied loose, and skipped.",
			{
				"out_dir": { "type": "string", "description": "Absolute directory to write the game dir into." },
			}, ["out_dir"]), Callable(self, "_tool_pack_game"))


func _tool_pack_game(args: Dictionary, ctx: McpToolContext) -> Variant:
	var out_dir := String(args.get("out_dir", "")).strip_edges()
	if out_dir.is_empty() or not out_dir.is_absolute_path():
		return McpToolResult.error("out_dir must be an absolute directory path.")
	return _pack(out_dir, ctx)


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
