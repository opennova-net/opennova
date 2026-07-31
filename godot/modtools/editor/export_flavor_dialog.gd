class_name ExportFlavorDialog
extends ConfirmationDialog

## Native themed replacement for the in-card export format chooser. Holds the
## mutually-exclusive BHD vs JO/DFX toggles and relabels the OK button to
## "Export". Flavor values mirror TerrainEditorWorkspace.ExportFlavor
## { BHD = 0, DFX_JO = 1 } so get_flavor() can be passed straight to begin_export.

const FLAVOR_BHD := 0
const FLAVOR_DFX_JO := 1

var _bhd_button: CheckButton
var _jodfx_button: CheckButton


func _init() -> void:
	title = "Export"
	# Width floor only; the few toggles let the dialog hug its content height.
	min_size = Vector2i(360, 0)
	get_ok_button().text = "Export"
	get_cancel_button().text = "Cancel"
	_build()


func _build() -> void:
	var box := VBoxContainer.new()
	box.name = "ExportFlavorBox"
	box.add_theme_constant_override("separation", 8)
	add_child(box)

	# A shared ButtonGroup keeps the two formats mutually exclusive without the
	# old hand-synced toggled handlers.
	var group := ButtonGroup.new()

	_jodfx_button = CheckButton.new()
	_jodfx_button.name = "ExportFlavorJODFX"
	_jodfx_button.text = "JO/DFX"
	_jodfx_button.button_group = group
	_jodfx_button.button_pressed = true
	box.add_child(_jodfx_button)

	_bhd_button = CheckButton.new()
	_bhd_button.name = "ExportFlavorBHD"
	_bhd_button.text = "BHD"
	_bhd_button.button_group = group
	box.add_child(_bhd_button)


func select_flavor(flavor: int) -> void:
	if flavor == FLAVOR_BHD:
		_bhd_button.button_pressed = true
	else:
		_jodfx_button.button_pressed = true


func get_flavor() -> int:
	return FLAVOR_BHD if _bhd_button.button_pressed else FLAVOR_DFX_JO
