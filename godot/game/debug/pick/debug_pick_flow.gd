class_name DebugPickFlow
## The Shift+F6 pick flow: run the crosshair ray pick, file the card into the
## shell-owned DebugPickList (the F3 Entities window selects it and its
## Selection overlay marks it in the Game view), and confirm what happened with
## a brief toast over the HUD.

const PICK_TOAST_SECONDS := 1.6

var _toast: Label = null


## `presenter` is the presenter the surface draws through (null for a bare
## camera).
func pick_at_crosshair(sim: Simulation, camera: Camera3D,
		presenter: LocalPlayerPresenter, pick_list: DebugPickList,
		toast_mount: Node) -> void:
	var pick := DebugEntityPicker.pick_at_crosshair(sim, camera, presenter)
	if pick == null:
		return
	if not pick.hit:
		var blocked := pick.blocked
		if blocked.is_empty():
			_show_toast(toast_mount, "No entity in range.")
		else:
			_show_toast(toast_mount, "No entity (%s, %.0fu)." % [
					blocked, pick.distance_units])
		return
	pick_list.add(pick)
	var pick_name := pick.name
	if pick_name.is_empty():
		pick_name = pick.hit_class
	_show_toast(toast_mount, "Picked: %s #%d  (%.0fu)" % [
			pick_name, pick.bms_id,
			pick.distance_units])


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
