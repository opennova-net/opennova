extends GutTest

# ANIMS workflow (B9): the first-class body-animation workspace — .adm clip
# list with metadata in the main pane, transport + scrub in the detail dock,
# arms overlay rows — promoted out of the PREVIEW smoke block. Drives the REAL
# workspace + preview over the committed Shed.3di + soldier.adm fixtures (a
# rigid model fake-skins into a real Skeleton3D headless).

const ObjectWorkspaceScript = preload("res://modtools/object/object_workspace.gd")
const SHED := "res://../fixtures/threedi/3di3/Shed.3di"
const MODEL_FIXTURES := "res://../fixtures/threedi/3di3"
const ANIM_FIXTURES := "res://../fixtures/anim"


func _anim_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root


# A workspace with the Shed fixture open, the preview mounted, soldier.adm
# loaded, and the ANIMS workflow built (main into `mount`, detail into `dock`).
func _anims_workspace() -> Dictionary:
	var ws = ObjectWorkspaceScript.new()
	ws.set_editor_shell(self)
	assert_eq(int(ws.open_file(ProjectSettings.globalize_path(SHED))), OK, "Shed fixture opens")
	var viewport_mount: Control = add_child_autofree(Control.new())
	ws.mount_viewport(viewport_mount)
	var preview: ObjectPreview = ws._preview
	assert_not_null(preview, "mounting the viewport builds the preview")
	var keys: PackedStringArray = preview.load_animation_set("soldier.adm", _anim_root())
	assert_false(keys.is_empty(), "soldier.adm loads: %s" % preview.get_animation_error())
	var dock: Control = add_child_autofree(PanelContainer.new())
	ws.set_asset_dock(dock)
	var mount: Control = add_child_autofree(Control.new())
	ws.build_workflow_inspector(ObjectWorkspaceScript.Workflow.ANIMS, mount)
	return {"ws": ws, "preview": preview, "mount": mount, "dock": dock, "keys": keys}


func test_anims_workflow_appended_with_stable_ids() -> void:
	# object_editor_test drives LODS by raw id 4; ANIMS must be APPENDED, never
	# renumber the existing ids.
	assert_eq(int(ObjectWorkspaceScript.Workflow.LODS), 4, "LODS stays raw id 4")
	assert_eq(int(ObjectWorkspaceScript.Workflow.ANIMS), 5, "ANIMS appended after LODS")
	var ws = ObjectWorkspaceScript.new()
	ws.set_editor_shell(self)
	var labels: Array = []
	for def in ws.get_workflows():
		labels.append(String(def.label))
	assert_eq(labels[0], "Preview", "tab order is the defs array")
	assert_eq(labels[1], "Anims", "the Anims tab sits right after Preview")


func test_clip_list_shows_every_clip_with_metadata() -> void:
	var parts := _anims_workspace()
	var mount: Control = parts.mount
	var keys: PackedStringArray = parts.keys
	var list := mount.find_child("AnimsClipList", true, false) as ItemList
	assert_not_null(list, "the main pane lists the loaded clips")
	if list == null:
		return
	assert_eq(list.item_count, keys.size(), "one row per .adm clip key")
	for i in range(keys.size()):
		assert_string_contains(list.get_item_text(i), String(keys[i]), "rows carry the clip key")
	assert_string_contains(list.get_item_text(0), "fr @", "rows carry frame-count/fps metadata")
	assert_string_contains(list.get_item_text(0), " s", "rows carry the clip length in seconds")
	assert_not_null(mount.find_child("AdmNameEdit", true, false), "the .adm name row moved here")
	assert_not_null(mount.find_child("AdmStatusLabel", true, false))


func test_selecting_a_clip_plays_it_and_scrub_poses_while_paused() -> void:
	var parts := _anims_workspace()
	var preview: ObjectPreview = parts.preview
	var list := (parts.mount as Control).find_child("AnimsClipList", true, false) as ItemList
	var slider := (parts.dock as Control).find_child("AnimsScrubSlider", true, false) as HSlider
	assert_not_null(slider, "the detail dock holds the scrub slider")
	if slider == null:
		return

	preview.set_playing(false)  # paused scrubbing is the point
	var walk := (parts.keys as PackedStringArray).find("anim_walk_forward")
	assert_gt(walk, -1, "the fixture has anim_walk_forward")
	list.select(walk)
	list.item_selected.emit(walk)
	assert_eq(preview.get_current_clip(), "anim_walk_forward", "selecting a row plays that clip")

	var skel: Skeleton3D = preview.get_object_model().get_skeleton()
	assert_not_null(skel, "the rigid Shed fake-skinned into a Skeleton3D")
	var sk = preview.get_skeletal_anim()
	var mid: float = sk.get_clip_length("anim_walk_forward") * 0.5
	assert_almost_eq(slider.max_value, sk.get_clip_length("anim_walk_forward"), 0.001,
		"the slider spans the selected clip")
	# The fixture clips carry root motion, not visually-moving bones: pin the
	# immediate re-pose by vandalizing a bone pose the scrub must overwrite.
	skel.set_bone_pose_position(0, Vector3(123.0, 456.0, 789.0))

	slider.value = mid  # a user scrub (Range.value emits value_changed)
	assert_almost_eq(preview.get_animation_playhead(), mid, 0.02, "the playhead followed the scrub")
	assert_ne(skel.get_bone_pose_position(0), Vector3(123.0, 456.0, 789.0),
		"the paused scrub re-posed the skeleton immediately")

	var time_label := (parts.dock as Control).find_child("AnimsTimeLabel", true, false) as Label
	assert_not_null(time_label)
	assert_string_contains(time_label.text, "/", "the readout shows playhead / length")


func test_selecting_a_shorter_clip_fires_no_phantom_scrub() -> void:
	# Range.set_max re-clamps the held value and EMITS value_changed: if the
	# transport resync shrank max while the slider still held the previous
	# clip's position, that emission scrubbed the freshly selected clip (a
	# one-shot would freeze at its final frame). Pin the mechanism: switching
	# from a longer to a shorter clip fires NO scrub and the new clip's
	# playhead stays at 0 (where play_animation put it).
	var parts := _anims_workspace()
	var preview: ObjectPreview = parts.preview
	var keys: PackedStringArray = parts.keys
	var sk = preview.get_skeletal_anim()
	var longer := -1
	var shorter := -1
	for i in range(keys.size()):
		for j in range(keys.size()):
			if sk.get_clip_length(String(keys[i])) > sk.get_clip_length(String(keys[j])) + 0.01:
				longer = i
				shorter = j
	if longer < 0:
		pass_test("All fixture clips share one length; the shrink path cannot be exercised here.")
		return

	var list := (parts.mount as Control).find_child("AnimsClipList", true, false) as ItemList
	var slider := (parts.dock as Control).find_child("AnimsScrubSlider", true, false) as HSlider
	preview.set_playing(false)
	list.select(longer)
	list.item_selected.emit(longer)
	# Scrub the longer clip past the shorter clip's length (slider holds it).
	slider.value = sk.get_clip_length(String(keys[longer])) - 0.01

	var phantom_scrubs: Array = []
	slider.value_changed.connect(func(v: float) -> void: phantom_scrubs.append(v))
	list.select(shorter)
	list.item_selected.emit(shorter)
	assert_eq(phantom_scrubs, [], "shrinking the slider range must not fire a user scrub")
	assert_almost_eq(preview.get_animation_playhead(), 0.0, 0.001,
		"the freshly selected clip starts at 0, not scrubbed to its end")
	assert_almost_eq(slider.max_value, sk.get_clip_length(String(keys[shorter])), 0.001)
	assert_lte(slider.value, slider.max_value)


func test_play_pause_reset_wiring() -> void:
	var parts := _anims_workspace()
	var preview: ObjectPreview = parts.preview
	var list := (parts.mount as Control).find_child("AnimsClipList", true, false) as ItemList
	list.select(0)
	list.item_selected.emit(0)
	var dock: Control = parts.dock
	var play := dock.find_child("AnimsPlayButton", true, false) as Button
	var reset := dock.find_child("AnimsResetButton", true, false) as Button
	var slider := dock.find_child("AnimsScrubSlider", true, false) as HSlider
	assert_not_null(play)
	assert_not_null(reset)
	if play == null or reset == null:
		return

	assert_eq(play.button_pressed, preview.is_playing(), "the toggle mirrors the preview transport")
	play.button_pressed = false  # user pause (emits toggled)
	assert_false(preview.is_playing(), "the toggle drives the preview transport")
	assert_eq(play.text, "Play", "paused shows the resume action")
	play.button_pressed = true
	assert_true(preview.is_playing())
	assert_eq(play.text, "Pause")

	preview.set_playing(false)
	slider.value = 0.2
	assert_gt(preview.get_animation_playhead(), 0.0)
	reset.pressed.emit()
	assert_almost_eq(preview.get_animation_playhead(), 0.0, 0.001, "Reset rewinds the playhead")


func test_arms_overlay_scrubs_in_lockstep() -> void:
	var parts := _anims_workspace()
	var preview: ObjectPreview = parts.preview
	var model_root := NovaResourceRoot.new()
	model_root.set_root_dir(ProjectSettings.globalize_path(MODEL_FIXTURES))
	assert_true(preview.load_arms("CharModel.3di", model_root), "arms fixture loads")
	assert_not_null((parts.dock as Control).find_child("ArmsLoadButton", true, false),
		"the arms rows live in the Anims detail dock now")

	preview.set_playing(false)
	preview.play_animation("anim_walk_forward")
	# Scrub WITHIN the clip (the fixture walk clip is ~0.27s and loops, so a
	# past-the-end scrub would wrap by design).
	var mid: float = preview.get_skeletal_anim().get_clip_length("anim_walk_forward") * 0.5
	preview.set_animation_playhead(mid)
	assert_almost_eq(preview.get_animation_playhead(), mid, 0.001)
	assert_almost_eq(float(preview._arms_model.get_animation_time()), mid, 0.001,
		"the arms overlay's playhead scrubs in lockstep with the body")


func test_preview_workflow_no_longer_carries_the_adm_block() -> void:
	var ws = ObjectWorkspaceScript.new()
	ws.set_editor_shell(self)
	assert_eq(int(ws.open_file(ProjectSettings.globalize_path(SHED))), OK)
	var viewport_mount: Control = add_child_autofree(Control.new())
	ws.mount_viewport(viewport_mount)
	var mount: Control = add_child_autofree(Control.new())
	ws.build_workflow_inspector(ObjectWorkspaceScript.Workflow.PREVIEW, mount)
	assert_not_null(mount.find_child("PreviewPlayButton", true, false),
		"PREVIEW keeps its pinned playback controls")
	assert_null(mount.find_child("AdmNameEdit", true, false), "the .adm block moved to ANIMS")
	assert_null(mount.find_child("AdmClipPicker", true, false))
	assert_null(mount.find_child("ArmsLoadButton", true, false))
	assert_false(ws.get_workflow_inspector(ObjectWorkspaceScript.Workflow.PREVIEW).has_detail(),
		"PREVIEW still mounts no detail dock")
