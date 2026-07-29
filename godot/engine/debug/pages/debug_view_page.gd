class_name DebugViewPage
extends VBoxContainer
## Render-debug toggles the host acts on (skeleton bone overlay, foliage, ...).
## Unlike the sim-fed panes this never reads the runtime: each checkbox holds
## its own state and only emits intent, which the overlay re-emits for the host
## that owns the world (the game's main_game, the editor's mission workspace).

signal skeleton_debug_toggled(enabled: bool)
signal user_points_toggled(enabled: bool)
signal collision_debug_toggled(enabled: bool)
signal foliage_hidden_toggled(hidden: bool)
signal viewmodel_forced_toggled(enabled: bool)
signal body_in_first_person_toggled(enabled: bool)

var _skeleton_check: CheckBox
var _user_points_check: CheckBox
var _collision_check: CheckBox
var _foliage_check: CheckBox
var _viewmodel_check: CheckBox
var _body_fp_check: CheckBox


func _init() -> void:
	name = "View"
	add_theme_constant_override("separation", 6)

	_skeleton_check = CheckBox.new()
	_skeleton_check.name = "ViewSkeletons"
	_skeleton_check.text = "Show skeletons"
	_skeleton_check.tooltip_text = "Draw character bones (joint-to-parent lines + axis crosses) over the world."
	_skeleton_check.button_pressed = false
	_skeleton_check.toggled.connect(_on_skeleton_toggled)
	add_child(_skeleton_check)

	_user_points_check = CheckBox.new()
	_user_points_check.name = "ViewUserPoints"
	_user_points_check.text = "Show user points"
	_user_points_check.tooltip_text = "Draw every named model user point as a cyan marker + label, following live animated bones and including static-batched mission objects."
	_user_points_check.button_pressed = false
	_user_points_check.toggled.connect(_on_user_points_toggled)
	add_child(_user_points_check)

	_collision_check = CheckBox.new()
	_collision_check.name = "ViewCollision"
	_collision_check.text = "Show collision"
	_collision_check.tooltip_text = "Draw object collision volumes (type-colored boxes) and the player's capsule test points over the world."
	_collision_check.button_pressed = false
	_collision_check.toggled.connect(_on_collision_toggled)
	add_child(_collision_check)

	_foliage_check = CheckBox.new()
	_foliage_check.name = "ViewHideFoliage"
	_foliage_check.text = "Hide foliage"
	_foliage_check.tooltip_text = "Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it."
	_foliage_check.button_pressed = false
	_foliage_check.toggled.connect(_on_foliage_toggled)
	add_child(_foliage_check)

	_viewmodel_check = CheckBox.new()
	_viewmodel_check.name = "ViewForceFpArms"
	_viewmodel_check.text = "Always draw FP arms"
	_viewmodel_check.tooltip_text = "Keep the first-person arms + weapon drawn in every camera mode (debug experiment)."
	_viewmodel_check.button_pressed = false
	_viewmodel_check.toggled.connect(_on_viewmodel_forced_toggled)
	add_child(_viewmodel_check)

	_body_fp_check = CheckBox.new()
	_body_fp_check.name = "ViewBodyInFirstPerson"
	_body_fp_check.text = "Show body in first person"
	_body_fp_check.tooltip_text = "Draw your own body in first person — look down to see your legs and feet (debug experiment; expect the head/shoulders to clip the camera)."
	_body_fp_check.button_pressed = false
	_body_fp_check.toggled.connect(_on_body_fp_toggled)
	add_child(_body_fp_check)


func _on_skeleton_toggled(pressed: bool) -> void:
	skeleton_debug_toggled.emit(pressed)


func _on_user_points_toggled(pressed: bool) -> void:
	user_points_toggled.emit(pressed)


func _on_collision_toggled(pressed: bool) -> void:
	collision_debug_toggled.emit(pressed)


func _on_foliage_toggled(pressed: bool) -> void:
	foliage_hidden_toggled.emit(pressed)


func _on_viewmodel_forced_toggled(pressed: bool) -> void:
	viewmodel_forced_toggled.emit(pressed)


func _on_body_fp_toggled(pressed: bool) -> void:
	body_in_first_person_toggled.emit(pressed)
