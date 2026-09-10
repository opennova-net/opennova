extends GutTest

# The byte guard over the authored clip sets (the Godot analogue of the
# bad_roundtrip ctest for the model tool's clips): every ClipSetSource under
# godot/authoring/ re-projects in memory and its .bad and .adm artifacts in
# assets/ must equal that export byte for byte. Rebuild them with
# "OpenNova: Export all authored clips" or the headless --export-clips command
# when an Animation or a spec changes.


func test_every_clip_set_matches_its_tracked_artifacts() -> void:
	var sources := ClipExport.list_sources()
	assert_true(sources.size() >= 2, "the authoring tree carries the body and the rifle clip sets")
	for source in sources:
		var result := ClipExport.verify_source(source)
		if not result.ok and _only_unpulled(result.mismatches):
			pass_test("%s: artifacts are unpulled LFS pointers; skipped" % source)
			continue
		assert_true(result.ok, "%s: %s" % [source, result.error])
		assert_true(result.artifacts.size() >= 2, "%s names its clips and its table" % source)
		assert_true(result.artifacts[-1].ends_with(".adm"), "%s: the table comes last" % source)


func test_the_person_clip_set_is_the_documented_shape() -> void:
	var source := load("res://authoring/person/person_clips.tres") as ClipSetSource
	assert_not_null(source)
	if source == null:
		return
	assert_eq(source.adm_name, "person")
	assert_eq(source.fps, 30)
	assert_eq(source.output_directory, "res://../assets")
	assert_not_null(source.scene, "the rig scene with its AnimationPlayer")
	var reset := source.find_clip("person_rst")
	assert_not_null(reset, "the reset clip")
	if reset != null:
		assert_eq(reset.animation, "", "the reset holds the rest pose")
		assert_eq(reset.hold_frames, 2, "two intervals, the retail reset's length")
	assert_not_null(source.find_clip("failsafe"), "the every-mission-start clip under its literal name")
	var walk := source.find_clip("pers_walkf")
	assert_not_null(walk)
	if walk != null:
		assert_eq(walk.animation, "walk")
		assert_almost_eq(walk.forward_speed, 1.5, 0.0001, "root motion in metres per second")
		assert_true(walk.loop)
	var first := source.rows[0] as AnimSetRow
	assert_eq(first.key, "anim_reset", "the reset row leads (the rig's skeleton source)")
	assert_eq(first.variants, PackedStringArray(["person_rst"]))
	var keys := PackedStringArray()
	for row in source.rows:
		keys.append((row as AnimSetRow).key)
	for key in ["anim_idle", "anim_walk_forward", "anim_walk_backleft", "anim_run_forward", "anim_idle_crouch",
			"anim_walk_prone_left", "anim_jump_start", "anim_reload", "anim_death_fire",
			"anim_death_bullet_leftfoot_left"]:
		assert_true(keys.has(key), "the table binds " + key)


func test_the_rifle_clip_set_binds_the_weapon_keys() -> void:
	var source := load("res://authoring/akm/akm_clips.tres") as ClipSetSource
	assert_not_null(source)
	if source == null:
		return
	assert_eq(source.adm_name, "akm")
	var keys := PackedStringArray()
	for row in source.rows:
		keys.append((row as AnimSetRow).key)
	assert_eq(keys, PackedStringArray(["anim_reset", "anim_wpn_idle", "anim_wpn_fire", "anim_wpn_recoil",
			"anim_wpn_reload", "anim_wpn_empty", "anim_wpn_switchto", "anim_wpn_switchfrom", "anim_wpn_switchrank"]),
			"retail's AKM table's nine keys, in its order")
	var fire := source.find_clip("akm_fire")
	assert_not_null(fire)
	if fire != null:
		assert_false(fire.loop, "the fire kick is a one-shot")
		assert_almost_eq(fire.capsule_top, 0.6, 0.0001, "retail's rifle capsule")


func _only_unpulled(mismatches: PackedStringArray) -> bool:
	if mismatches.is_empty():
		return false
	for line in mismatches:
		if not line.ends_with("unpulled LFS pointer"):
			return false
	return true
