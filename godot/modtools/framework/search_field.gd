class_name SearchField
extends LineEdit

## The shared search/filter box (B8): placeholder + clear button + expand
## flags + a search_changed relay, so the six bespoke bars converge on one
## widget. Callers keep their node names and any extra signal wiring
## (text_submitted, gui_input) — this class only standardizes construction.

signal search_changed(text: String)


func _init(placeholder: String = "Search...") -> void:
	placeholder_text = placeholder
	clear_button_enabled = true
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	text_changed.connect(func(text: String) -> void: search_changed.emit(text))
