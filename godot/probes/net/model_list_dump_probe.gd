extends GameProbe

## model_list_dump: the model-list readout (Simulation.debug_mission_start_model_nodes over
## engine runtime/inmatch/loaded_model_list.h), the model-def list this mission's load
## froze: the list the C2S 0x3D page answers from on a joiner and a host's S2C 0x68 wraps
## at. An instrument: it reads, it changes nothing a player or the wire sees.
##
## Each node, head first (the order retail's freeze walks g_ModelDefListHead): `name`
## ("<stem>.3DI"), `foliage` (the terrain's foliage pass marked it, so the page skips it),
## `load_index` (0 the first load) and `page_index` (its dword in the page, -1 when
## marked). The verdict carries the node count, the page count and the foliage count; the
## nodes go to model_list.json and, one name per line head first, to model_list.txt in the
## run directory, the form a diff against retail's list takes.

const SIM_TIMEOUT_MS := 60_000

var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var log_names := bool(ctx.args.get("log_names", false))
	# No wait for the local player: that wait dismisses the loading presentation, which a
	# joiner's final wait reads as its ESC. The list is frozen once the load has ended.
	var sim_deadline := Time.get_ticks_msec() + SIM_TIMEOUT_MS
	while ctx.sim() == null and Time.get_ticks_msec() < sim_deadline and not ctx.cancelled:
		await ctx.tree.process_frame
	var sim := ctx.sim()
	if sim == null:
		return ProbeVerdict.failed("no simulation to read")
	var rows: Array[ModelListNodeRow] = sim.debug_mission_start_model_nodes()
	var nodes: Array = []
	var names := PackedStringArray()
	var foliage := 0
	for row in rows:
		var value: Dictionary = row.to_json_value()
		nodes.append(value)
		names.append(String(value.name))
		if bool(value.foliage):
			foliage += 1
		if log_names:
			ctx.log("%d %s%s" % [int(value.load_index), String(value.name),
					" (foliage)" if bool(value.foliage) else ""])
	var page_count := sim.debug_loaded_model_list_count()
	var result := {
		"node_count": nodes.size(),
		"page_count": page_count,
		"foliage_count": foliage,
		"nodes": nodes,
	}
	var json_path := ctx.artifact_dir.path_join("model_list.json")
	var file := FileAccess.open(json_path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "\t"))
		file.close()
		ctx.artifact("model_list", json_path, "json")
	var text_path := ctx.artifact_dir.path_join("model_list.txt")
	var text := FileAccess.open(text_path, FileAccess.WRITE)
	if text != null:
		text.store_string("\n".join(names) + "\n")
		text.close()
		ctx.artifact("model_list_names", text_path, "txt")
	if nodes.is_empty():
		return ProbeVerdict.failed("no frozen model list: the load has not finalized it", result)
	return ProbeVerdict.passed("%d node(s), %d on the page, %d foliage" % [nodes.size(),
			page_count, foliage], result)
