extends GutTest

## Guards the project audio bus layout (res://default_bus_layout.tres): the Master bus
## alone. The engine mixes every sound into one stream (ADR 0077 d4, AudioOut), which
## plays on Master, whose gain is the only level Godot keeps.


func test_default_bus_layout_is_master_alone() -> void:
	var layout = ResourceLoader.load("res://default_bus_layout.tres")
	assert_not_null(layout, "default_bus_layout.tres should load")
	assert_true(layout is AudioBusLayout, "it is an AudioBusLayout resource")

	# Preserve and restore the live layout so this test doesn't perturb others.
	var prev := AudioServer.generate_bus_layout()
	AudioServer.set_bus_layout(layout)
	assert_eq(AudioServer.bus_count, 1, "the layout holds one bus")
	assert_eq(AudioServer.get_bus_name(0), "Master", "the one bus is Master")
	AudioServer.set_bus_layout(prev)
