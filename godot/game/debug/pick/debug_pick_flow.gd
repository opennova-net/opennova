class_name DebugPickFlow
## The Shift+F6 pick flow: run the crosshair ray pick, file the card into the
## shell-owned DebugPickList (the F3 Entities page renders it; snapshots embed
## it), and confirm what happened with a brief toast over the HUD.

const PICK_TOAST_SECONDS := 1.6

var _toast: Label = null


func pick_at_crosshair(sim: Simulation, camera: Camera3D,
		pick_list: DebugPickList, toast_mount: Node) -> void:
	var pick := DebugEntityPicker.pick_at_crosshair(sim, camera)
	if pick.is_empty():
		return
	if not bool(pick.get("hit", false)):
		var blocked := String(pick.get("blocked", ""))
		if blocked.is_empty():
			_show_toast(toast_mount, "No entity in range.")
		else:
			_show_toast(toast_mount, "No entity (%s, %.0fu)." % [
					blocked, float(pick.get("distance_units", 0.0))])
		return
	var row := pick_list.add(pick)
	if row < 0:
		_show_toast(toast_mount,
				"Pick list full (%d) — remove one on the F3 Entities page." %
				DebugPickList.MAX_PICKS)
		return
	var pick_name := String(pick.get("name", ""))
	if pick_name.is_empty():
		pick_name = String(pick.get("hit_class", "entity"))
	_show_toast(toast_mount, "Picked: %s #%d  (%.0fu)" % [
			pick_name, int(pick.get("bms_id", 0)),
			float(pick.get("distance_units", 0.0))])


func _show_toast(mount: Node, text: String) -> void:
	if _toast != null and is_instance_valid(_toast):
		_toast.queue_free()
	var label := Label.new()
	label.name = "PickToast"
	label.text = text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	label.offset_top = 96.0
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	mount.add_child(label)
	_toast = label
	var tween := label.create_tween()
	tween.tween_interval(PICK_TOAST_SECONDS)
	tween.tween_property(label, "modulate:a", 0.0, 0.4)
	tween.tween_callback(label.queue_free)
