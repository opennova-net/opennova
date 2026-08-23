class_name EditorMcpMusicTools
extends RefCounted

## Music-workspace MCP tools: mint a script + bank pair, put tracks in the bank, wire them into
## the script, and save the pair under an exact basename.
##
## Music is the editor's one two-file document — retail loads MENUMUS.SBF beside MENUMUS.BIN,
## and GAMEMUS the same way — so saving means picking the .sbf name and letting the .bin follow
## its basename, which is what music_save_as does.
##
## Everything routes through MusicEditorDocument's own seams (new_script / new_bank / add_track /
## insert_play / save_as_pair), the same ones the workspace's inspectors drive.
##
## The one capability that exists only here is synthesized silence: the bank's sole UI producer
## is a WAV file picker, so authoring a bank with no audio files on disk was impossible. A
## silent track is what the boot pair actually needs — the script's `play` wants a real entry to
## address, and the minimal game ships no music.

const SAMPLE_RATE := 22050


var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("music_new",
			"Start a fresh music project in the Music workspace: a compiled script whose single entry section is empty, plus an empty bank. script_name becomes the script's chunk name and is NOT derived from the save path, so mint one project per output name (menumus, gamemus).",
			{
				"script_name": { "type": "string", "description": "Script chunk name. Default \"gamescript\"." },
			}), Callable(self, "_tool_new"))
	registry.register(McpToolDef.make("music_add_track",
			"Add a track to the open bank. Give either seconds (synthesizes that many seconds of digital silence at 22050 Hz — what the boot pair needs, since the minimal game ships no music) or samples (explicit mono floats in -1..1). Returns the new track index for music_insert_play.",
			{
				"name": { "type": "string", "description": "Track name inside the bank." },
				"seconds": { "type": "number", "description": "Length of silence to synthesize." },
				"samples": { "type": "array", "items": { "type": "number" }, "description": "Explicit mono samples instead of silence." },
			}, ["name"]), Callable(self, "_tool_add_track"))
	registry.register(McpToolDef.make("music_add_section",
			"Add a named section to the open script. Section 0 is the entry point the engine runs; extra sections are jump targets.",
			{
				"section": { "type": "string" },
			}, ["section"]), Callable(self, "_tool_add_section"))
	registry.register(McpToolDef.make("music_insert_play",
			"Append a `play` statement for a bank track to a script section. track is the index music_add_track returned; the opcode operand is one byte, so 0..255.",
			{
				"section": { "type": "string" },
				"track": { "type": "integer" },
			}, ["section", "track"]), Callable(self, "_tool_insert_play"))
	registry.register(McpToolDef.make("music_save_as",
			"Write the pair to an exact absolute .sbf path; the compiled script lands on the .bin sibling of the same basename. Retail loads menumus.sbf + menumus.bin and gamemus.sbf + gamemus.bin, so use those names verbatim for the boot pair.",
			{
				"path": { "type": "string", "description": "Absolute path ending in .sbf." },
			}, ["path"]), Callable(self, "_tool_save_as"))
	registry.register(McpToolDef.make("music_state",
			"Read the Music workspace: current paths, unsaved state, whether a bank and script are loaded, the bank's tracks, and the script's sections. Read-only.",
			{}, [], false), Callable(self, "_tool_state"))


# --- shared guard -----------------------------------------------------------------

# The Music workspace's document. get_editor_document() IS the MusicEditorDocument here (unlike
# Fonts, where it is the editor control), and the workspace is constructed at editor boot, so
# nothing below needs the viewport mounted.
func _require_music(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("music")
	if ws == null:
		return { "error": "Music workspace unavailable." }
	var doc: Variant = ws.get_editor_document()
	if doc == null:
		return { "error": "Music workspace has no document." }
	return { "ws": ws, "doc": doc }


# --- tools ------------------------------------------------------------------------

func _tool_new(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var doc: Variant = gate["doc"]
	var script_name := String(args.get("script_name", "gamescript")).strip_edges()
	if script_name.is_empty():
		script_name = "gamescript"
	# new_project() would work but hardcodes the name, and the chunk name is not recoverable
	# from the save path — so mint the two halves directly, which is all new_project does.
	var err: int = doc.new_script(script_name)
	if err != OK:
		return McpToolResult.error("Could not mint the script: %s" % error_string(err))
	err = doc.new_bank()
	if err != OK:
		return McpToolResult.error("Script minted, but the bank failed: %s" % error_string(err))
	return _state_of(gate["ws"], doc)


func _tool_add_track(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var doc: Variant = gate["doc"]
	if not doc.bank_loaded():
		return McpToolResult.error("No bank open — music_new first.")
	var name := String(args.get("name", "")).strip_edges()
	if name.is_empty():
		return McpToolResult.error("name is required.")

	var samples := PackedFloat32Array()
	if args.has("samples") and args["samples"] is Array:
		for v in (args["samples"] as Array):
			samples.append(clampf(float(v), -1.0, 1.0))
	elif args.has("seconds"):
		var seconds := float(args["seconds"])
		if seconds <= 0.0 or seconds > 600.0:
			return McpToolResult.error("seconds must be between 0 and 600 (got %f)." % seconds)
		samples.resize(int(round(seconds * SAMPLE_RATE)))  # zero-filled: digital silence
	else:
		return McpToolResult.error("Give either seconds (synthesized silence) or samples.")
	if samples.is_empty():
		return McpToolResult.error("The track would have no samples.")

	var before: int = _track_names(doc).size()
	var err: int = doc.add_track(StringName(name), samples)
	if err != OK:
		return McpToolResult.error("Could not add the track: %s" % error_string(err))
	var state := _state_of(gate["ws"], doc)
	state["track"] = before
	state["sample_count"] = samples.size()
	return state


func _tool_add_section(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var doc: Variant = gate["doc"]
	var section := String(args.get("section", "")).strip_edges()
	if section.is_empty():
		return McpToolResult.error("section is required.")
	if not doc.add_section(StringName(section)):
		return McpToolResult.error(_blocked_reason(doc, "Could not add section '%s'." % section))
	return _state_of(gate["ws"], doc)


func _tool_insert_play(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var doc: Variant = gate["doc"]
	var section := String(args.get("section", "")).strip_edges()
	if section.is_empty():
		return McpToolResult.error("section is required.")
	var track := int(args.get("track", -1))
	if track < 0 or track > 255:
		return McpToolResult.error("track must be 0..255 (the play operand is one byte).")
	if not doc.insert_play(StringName(section), track):
		return McpToolResult.error(_blocked_reason(doc,
				"Could not add `play %d` to section '%s' — check the section exists (music_state)." % [track, section]))
	return _state_of(gate["ws"], doc)


func _tool_save_as(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var path := String(args.get("path", "")).strip_edges()
	if path.is_empty() or not path.is_absolute_path():
		return McpToolResult.error("path must be an absolute file path.")
	if path.get_extension().to_lower() != "sbf":
		return McpToolResult.error("path must end in .sbf (got '%s'); the .bin sibling follows its basename." % path.get_file())
	var err: int = ws.save_as_file(path)
	if err != OK:
		return McpToolResult.error("Save failed: %s" % error_string(err))
	var bin := path.get_base_dir().path_join("%s.bin" % path.get_file().get_basename())
	ctx.status("Wrote %s + %s" % [path.get_file(), bin.get_file()])
	var state := _state_of(ws, gate["doc"])
	state["wrote"] = {
		path.get_file(): FileAccess.get_file_as_bytes(path).size(),
		bin.get_file(): FileAccess.get_file_as_bytes(bin).size(),
	}
	return state


func _tool_state(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_music(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	return _state_of(gate["ws"], gate["doc"])


# --- readout ----------------------------------------------------------------------

func _state_of(ws: Variant, doc: Variant) -> Dictionary:
	var out := {
		"bank_path": String(doc.bank_path),
		"script_path": String(doc.script_path),
		"bank_loaded": bool(doc.bank_loaded()),
		"script_loaded": bool(doc.script_loaded()),
		"unsaved": bool(ws.has_unsaved_changes()),
		"tracks": _track_names(doc),
	}
	if not doc.can_author():
		out["authoring_blocked"] = String(doc.authoring_blocked_reason())
	return out


func _track_names(doc: Variant) -> PackedStringArray:
	var names := PackedStringArray()
	if not doc.bank_loaded() or doc.bank == null:
		return names
	for entry in doc.bank.get_entries():
		names.append(String((entry as Dictionary).get("name", "")))
	return names


func _blocked_reason(doc: Variant, fallback: String) -> String:
	var reason := String(doc.authoring_blocked_reason())
	return reason if not reason.is_empty() else fallback
