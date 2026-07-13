class_name EditorMcpObjectTools
extends RefCounted

## The object-workspace MCP tools: drive the Anims workflow the way its
## inspector does — bind a .adm animation set to the open model, play or scrub
## clips, load the first-person arms overlay — and read the posed rig back for
## validation. Every mutation routes through ObjectPreview's own seams
## (load_animation_set / play_animation / load_arms); there is no new
## preview behavior here.

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("object_load_anims",
			"Bind a skeletal animation set (.adm) to the model open in the Object workspace and start its first clip, exactly like the Anims inspector's Load. adm defaults to the model's basename + \".adm\" (the stock naming convention). arms optionally loads a second .3di (e.g. \"armsG.3di\") that rides the same skeleton — the first-person arms overlay. Returns the clip list. Requires an open model (open_in_workspace workspace=\"object\") and a mounted resource directory.",
			{
				"adm": { "type": "string" },
				"arms": { "type": "string" },
			}), Callable(self, "_tool_load_anims"))
	registry.register(McpToolDef.make("object_play_clip",
			"Play or scrub one clip of the loaded animation set in the Object workspace preview. playhead (seconds) poses that instant even while paused; playing=false pauses there (a still for screenshots). Load a set with object_load_anims first; clip keys come from its result (or object_rig_state).",
			{
				"clip": { "type": "string" },
				"playhead": { "type": "number" },
				"playing": { "type": "boolean", "default": true },
			}, ["clip"]), Callable(self, "_tool_play_clip"))
	registry.register(McpToolDef.make("object_rig_state",
			"Read the Object workspace's animation state: the open model, loaded clips with lengths, the current clip and playhead, and (include_bones) the skeleton — per bone name, parent, local rest offset, the accumulated rest-pose world position, and (when a clip is current) the posed world position at the playhead. Read-only; the numeric ground truth screenshots can't give.",
			{
				"include_bones": { "type": "boolean", "default": false },
			}, [], false), Callable(self, "_tool_rig_state"))


# --- shared guards ----------------------------------------------------------------

# The Object workspace's live preview, or an error result. The preview exists only
# while the workspace is (or has been) active — open_in_workspace mounts it.
func _require_preview(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("object")
	if ws == null or not ws.has_method("get_preview"):
		return { "error": "Object workspace unavailable." }
	var preview: Variant = ws.get_preview()
	if preview == null or not is_instance_valid(preview):
		return { "error": "No model open — open_in_workspace(workspace=\"object\", path=...) first." }
	if preview.object_data == null:
		return { "error": "No model open — open_in_workspace(workspace=\"object\", path=...) first." }
	return { "ws": ws, "preview": preview }


# --- tools ------------------------------------------------------------------------

func _tool_load_anims(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_preview(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var preview: Variant = gate["preview"]
	var root: Variant = ws.get_resource_root() if ws.has_method("get_resource_root") else null
	if root == null:
		return McpToolResult.error("No resource directory mounted — set_resource_root first.")
	var adm := String(args.get("adm", "")).strip_edges()
	if adm.is_empty():
		var model_name := String(preview.object_data.get_object_name()).get_file().get_basename().strip_edges()
		if model_name.is_empty() or model_name == "untitled":
			return McpToolResult.error("Pass adm — the open model has no name to derive one from.")
		adm = model_name + ".adm"
	var keys: PackedStringArray = preview.load_animation_set(adm, root)
	if keys.is_empty():
		return McpToolResult.error("No animations loaded from %s: %s" % [adm, String(preview.get_animation_error())])
	var out := {
		"adm": adm,
		"clips": _clip_rows(preview),
		"has_skeleton": bool(preview.has_skeleton()),
	}
	var arms := String(args.get("arms", "")).strip_edges()
	if not arms.is_empty():
		if preview.load_arms(arms, root):
			out["arms"] = arms
		else:
			out["arms_error"] = String(preview.get_arms_error())
	# Mirror the inspector's Load: start the first clip so the preview poses.
	preview.play_animation(String(keys[0]))
	out["playing_clip"] = String(keys[0])
	return out


func _tool_play_clip(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_preview(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var preview: Variant = gate["preview"]
	var sk: Variant = preview.get_skeletal_anim()
	if sk == null:
		return McpToolResult.error("No animation set loaded — object_load_anims first.")
	var clip := String(args.get("clip", ""))
	if not clip in Array(sk.get_clip_keys()):
		return McpToolResult.error("Unknown clip '%s' — loaded clips: %s" % [clip, ", ".join(sk.get_clip_keys())])
	preview.play_animation(clip)
	if args.has("playhead"):
		preview.set_animation_playhead(maxf(0.0, float(args["playhead"])))
	preview.set_playing(bool(args.get("playing", true)))
	return {
		"clip": clip,
		"playhead": float(preview.get_animation_playhead()),
		"length": float(sk.get_clip_length(clip)),
		"playing": bool(preview.is_playing()),
	}


func _tool_rig_state(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_preview(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var preview: Variant = gate["preview"]
	var out := {
		"model": String(preview.object_data.get_object_name()),
		"lod": int(preview.get_active_lod()),
		"has_skeleton": bool(preview.has_skeleton()),
		"has_arms": bool(preview.has_arms()),
	}
	var sk: Variant = preview.get_skeletal_anim()
	if sk == null:
		out["clips"] = []
		return out
	out["clips"] = _clip_rows(preview)
	out["current_clip"] = String(preview.get_current_clip())
	out["playhead"] = float(preview.get_animation_playhead())
	out["playing"] = bool(preview.is_playing())
	if bool(args.get("include_bones", false)):
		out["bones"] = _bone_rows(sk, String(preview.get_current_clip()),
				float(preview.get_animation_playhead()))
	return out


# --- helpers ----------------------------------------------------------------------

func _clip_rows(preview: Variant) -> Array:
	var sk: Variant = preview.get_skeletal_anim()
	var rows: Array = []
	if sk == null:
		return rows
	for k in sk.get_clip_keys():
		var key := String(k)
		rows.append({
			"key": key,
			"frames": int(sk.get_clip_frame_count(key)),
			"fps": float(sk.get_clip_fps(key)),
			"length": float(sk.get_clip_length(key)),
			"loops": bool(sk.is_clip_looping(key)),
		})
	return rows


# Per-bone rows: the local rest offset, the parent-accumulated rest-pose world
# position (the same FK walk the preview's Skeleton3D performs on its rests), and —
# when a clip is current — the posed world position at the playhead (eval_pose FK).
func _bone_rows(sk: Variant, clip: String, playhead: float) -> Array:
	var bones: Array = sk.get_skeleton_bones()
	var world: Array[Transform3D] = []
	var rows: Array = []
	for i in range(bones.size()):
		var b: Dictionary = bones[i]
		var parent := int(b.get("parent_index", -1))
		var rest: Transform3D = b.get("rest", Transform3D())
		var w: Transform3D = rest if parent < 0 or parent >= i else world[parent] * rest
		world.append(w)
		rows.append({
			"index": i,
			"name": String(b.get("name", "")),
			"parent": parent,
			"rest_local": [rest.origin.x, rest.origin.y, rest.origin.z],
			"rest_world": [w.origin.x, w.origin.y, w.origin.z],
		})
	if not clip.is_empty() and sk.has_clip(clip):
		var pose_world: Array[Transform3D] = []
		var pose: Array = sk.eval_pose(clip, playhead)
		for i in range(mini(pose.size(), rows.size())):
			var t: Transform3D = pose[i]
			var parent := int((rows[i] as Dictionary)["parent"])
			var w: Transform3D = t if parent < 0 or parent >= i else pose_world[parent] * t
			pose_world.append(w)
			(rows[i] as Dictionary)["pose_world"] = [w.origin.x, w.origin.y, w.origin.z]
	return rows
