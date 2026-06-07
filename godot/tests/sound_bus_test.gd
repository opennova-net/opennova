extends GutTest

## Guards the project audio bus layout (res://default_bus_layout.tres). A malformed
## layout (e.g. the Master bus routed to itself) silently breaks the AudioServer bus
## graph and silences ALL playback; this caught that class of bug headlessly, with no
## audio device, after the first attempt shipped silent.

const BUSES := ["Master", "SFX", "Music", "Ambient", "Voice"]


func test_default_bus_layout_defines_routed_buses() -> void:
	var layout = ResourceLoader.load("res://default_bus_layout.tres")
	assert_not_null(layout, "default_bus_layout.tres should load")
	assert_true(layout is AudioBusLayout, "it is an AudioBusLayout resource")

	# Preserve and restore the live layout so this test doesn't perturb others.
	var prev := AudioServer.generate_bus_layout()
	AudioServer.set_bus_layout(layout)

	for name in BUSES:
		assert_true(AudioServer.get_bus_index(name) >= 0, "bus '%s' exists" % name)

	# Master must NOT route to itself (the original bug); every other bus routes to Master.
	var master_idx := AudioServer.get_bus_index("Master")
	assert_ne(AudioServer.get_bus_send(master_idx), StringName("Master"),
		"Master must not send to itself")
	for name in ["SFX", "Music", "Ambient", "Voice"]:
		var idx := AudioServer.get_bus_index(name)
		assert_eq(AudioServer.get_bus_send(idx), StringName("Master"),
			"bus '%s' routes to Master" % name)

	AudioServer.set_bus_layout(prev)
