extends GameProbe

## terrain_ground: the ground the game reads at a mission point on the live process, for the OpenNova Editor's terrain
## end to end (ADR 0046 S25): the engine's own pick ray (the plain trace the F3 picker casts, Simulation.
## debug_pick_entity) dropped straight down onto the point from high above, and what it meets: the terrain (its height
## in mission metres, and the surface class the hit reads there: the char map's, a placed tile's where one lies there),
## the water, or an entity standing there (named, the ground under it unread: ask beside it); the terrain's height
## there as the world's terrain data samples it; and the placed tiles whose 16-unit square holds the point, as the
## world loaded its tile info (each its cell, flags and layer). `x` east and `y` north, mission metres. Needs a mission.

const ABOVE := 4000.0 # mission metres above the point the ray starts
const RANGE := 8000.0


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_world_ready():
		return ProbeVerdict.failed("the mission's world did not finish loading")
	var sim := ctx.sim()
	var world := ctx.world()
	if sim == null or world == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	var x := float(ctx.args.get("x", 0.0))
	var y := float(ctx.args.get("y", 0.0))
	var data := {"at": [x, y]}
	# The ray straight down onto the point.
	var from := MissionObjectPlacer.bms_to_godot_position(Vector3(x, y, ABOVE))
	var below := MissionObjectPlacer.bms_to_godot_position(Vector3(x, y, ABOVE - 1.0))
	var pick: DebugPickCard = sim.debug_pick_entity(from, (below - from).normalized(), RANGE)
	if pick == null:
		return ProbeVerdict.failed("the pick ray answered nothing", data)
	data["blocked"] = pick.blocked
	data["entity"] = pick.name if pick.hit else ""
	data["surface"] = pick.surface_type if pick.blocked == "terrain" else -1
	var hit := MissionObjectPlacer.godot_to_bms_position(pick.hit_position_godot)
	data["height"] = hit.z if pick.blocked == "terrain" else null
	# The terrain data's own sample there.
	var terrain := world.get_terrain_data()
	if terrain != null:
		var sampled := terrain.get_height_world_bilinear(MissionObjectPlacer.bms_to_godot_position(Vector3(x, y, 0.0)))
		data["terrain_height"] = MissionObjectPlacer.godot_to_bms_position(Vector3(from.x, sampled, from.z)).z
	# The placed tiles whose square holds the point: x in [corner, corner + 16), y in (corner, corner + 16] over the
	# stored frame (the record's z the plane's y negated).
	var tiles: Array = []
	var info: TerrainTileInfo = terrain.get_tileinfo_resource() if terrain != null else null
	if info != null:
		for entry: TerrainTileEntry in info.get_entries():
			var x0 := float(entry.get_x_fixed()) / 65536.0
			var y0 := -float(entry.get_z_fixed()) / 65536.0
			if x >= x0 and x < x0 + 16.0 and y > y0 and y <= y0 + 16.0:
				tiles.append({"tile": entry.get_tile_index(), "flags": entry.get_flags(), "layer": entry.get_layer(),
						"corner": [x0, y0]})
	data["tiles"] = tiles
	ctx.log("at %s: %s, surface %d, height %s, %d tile(s)" % [str([x, y]),
			pick.blocked if not pick.hit else "entity " + pick.name, int(data["surface"]), str(data["height"]), tiles.size()])
	if pick.blocked != "terrain":
		return ProbeVerdict.failed("the ray met %s, not the terrain" % (pick.blocked if not pick.hit else pick.name), data)
	return ProbeVerdict.passed("the terrain at %s: height %.2f, surface %d, %d tile(s)" % [
			str([x, y]), float(data["height"]), int(data["surface"]), tiles.size()], data)
