extends GutTest

## Guarded end-to-end probe for mission DIALOG audio (the .dbf path) on real JO data
## (Desktop/JOX). Loads 00TRg, confirms its .DBF dialog ids resolve to sound
## sets the loaded banks contain, and reports how many PlayDialog commands fire on
## sim-ungated ticks (pins the sim-trigger-gating follow-up). Skips without the data.


# The loose JOX dump root is the documented OPENNOVA_JO_ASSETS gate
# (docs/asset-gated-tests.md, RetailData.assets()); machine paths go in
# .claude/settings.local.json env, never tracked.
var JO_DIR := RetailData.assets()


func test_00trg_mission_dialog_resolves() -> void:
	if JO_DIR.is_empty():
		pending("OPENNOVA_JO_ASSETS (the JOX extract) is required for the dialog probe")
		return
	var root := ResourceRoot.new()
	root.set_root_dir(JO_DIR)
	if not root.has_file("00TRg.bms"):
		pending("00TRg.bms is not resolvable from OPENNOVA_JO_ASSETS")
		return

	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "00TRg.bms"), OK, "mission parses")
	var item_db := ItemDatabase.new()
	item_db.load_from_resource_root(root, "items.def")

	var container := Node3D.new()
	add_child_autofree(container)
	var audio = MissionAudio.new(root, item_db)
	var stats := audio.setup(mission, "00TRg.bms", container)
	gut.p("00TRg audio stats: %s" % str(stats.to_dict()))

	var dialog_count := int(stats.dialogs)
	assert_gt(dialog_count, 0, "the mission .DBF loaded with dialog groups")

	# Every dialog id should resolve to a set the loaded banks (mission .LWF +
	# game.lwf + gamelocl.LWF) actually contain -- proving the .dbf -> bank pipeline
	# end-to-end without an audio device.
	var resolved := 0
	for i in range(1, dialog_count + 1):
		var set_name := audio.resolve_dialog_set(i)
		if not set_name.is_empty():
			resolved += 1
	gut.p("resolved %d / %d dialog ids to loaded sets" % [resolved, dialog_count])
	assert_gt(resolved, 0, "at least one dialog id resolves to a playable set")

	# The dialog/zone wavs are IMA-ADPCM (audioFormat 0x11) -- they must now decode
	# to a non-empty 16-bit stream (previously WavLoader returned null -> silence).
	var wbytes := root.read_file("z00gr100.wav")
	if not wbytes.is_empty():
		var stream := WavLoader.from_bytes(wbytes)
		assert_not_null(stream, "IMA-ADPCM dialog wav decodes")
		if stream != null:
			assert_eq(stream.format, AudioStreamWAV.FORMAT_16_BITS)
			assert_gt(stream.data.size(), 0, "decoded PCM is non-empty")
			gut.p("z00gr100.wav (IMA-ADPCM) decoded: %d bytes 16-bit @ %d Hz" % [stream.data.size(), stream.mix_rate])

	# Diagnostic: how many "dialog" effects fire on ungated ticks? (If zero, the mission's
	# dialog is all sim-trigger-gated -> the documented follow-up.)
	var sim := Simulation.new()
	if sim.load_from_mission_data(mission):
		var fired := 0
		# 64 ticks = one full quarter-list event cycle (every normal event evaluated once).
		for _frame in range(64):
			sim.step()
			for e in sim.drain_effects():
				if (e as MissionEffect).kind == "dialog":
					fired += 1
		gut.p("dialog effects fired across one 64-tick event cycle: %d" % fired)

	audio.teardown()
