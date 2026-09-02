## The Environment rows of the debug-controls table: the WAC weather commands
## (world::WeatherState carries the handler cites) and the weather-home read,
## each on the Weather node's command seam — the bound Simulation's command
## layer on a mission, the standalone home otherwise. Split out of
## debug_controls.gd for the shipping-script size ratchet; the table order is
## the registration order, so `register` runs where the rows used to sit.
extends RefCounted


static func register(controls: DebugControls) -> void:
	# The WAC weather commands (world::WeatherState carries the handler cites),
	# each on the Weather node's command seam: the bound Simulation's command
	# layer on a mission, the standalone home otherwise. Args mirror the WAC
	# signatures.
	_weather_command(controls, &"environment_rain", "Rain",
			"rain(percent, seconds): rain percent over a transition.",
			[DebugArgSpec.integer("percent"), DebugArgSpec.integer("seconds")],
			func(weather: Weather, args: Array) -> void:
				weather.command_rain(args[0], args[1]))
	_weather_command(controls, &"environment_snow", "Snow",
			"snow(percent, seconds): snow percent over a transition.",
			[DebugArgSpec.integer("percent"), DebugArgSpec.integer("seconds")],
			func(weather: Weather, args: Array) -> void:
				weather.command_snow(args[0], args[1]))
	_weather_command(controls, &"environment_overcast", "Overcast",
			"overcast(percent, seconds): overcast blend over a transition.",
			[DebugArgSpec.integer("percent"), DebugArgSpec.integer("seconds")],
			func(weather: Weather, args: Array) -> void:
				weather.command_overcast(args[0], args[1]))
	_weather_command(controls, &"environment_fog_distance", "Fog distance",
			"fogdist(metres): the fog distance target (2 m .. the 1024 m reference).",
			[DebugArgSpec.integer("metres")],
			func(weather: Weather, args: Array) -> void:
				weather.command_fog_distance(args[0]))
	_weather_command(controls, &"environment_move_fog", "Move fog",
			"movefog(metres, seconds): the fog distance target over a transition.",
			[DebugArgSpec.integer("metres"), DebugArgSpec.integer("seconds")],
			func(weather: Weather, args: Array) -> void:
				weather.command_move_fog(args[0], args[1]))
	_weather_command(controls, &"environment_sky_speed", "Sky speed",
			"skyspeed(rate): the cloud scroll rate target.",
			[DebugArgSpec.integer("rate")],
			func(weather: Weather, args: Array) -> void:
				weather.command_sky_speed(args[0]))
	_weather_command(controls, &"environment_quake", "Quake",
			"quake(seconds): the earthquake jitter duration.",
			[DebugArgSpec.integer("seconds")],
			func(weather: Weather, args: Array) -> void:
				weather.command_quake(args[0]))
	_weather_command(controls, &"environment_fog_type", "Fog type",
			"fogtype(type): the fog model 0..3.",
			[DebugArgSpec.integer("type")],
			func(weather: Weather, args: Array) -> void:
				weather.command_fog_type(args[0]))

	# The weather home as the render owner sees it: the WeatherState snapshot
	# plus the smoothed color blocks and the combined terrain light (the
	# precipitation diffuse) — a read, no authority needed.
	var snapshot_row := controls._action(&"environment_weather_snapshot", &"Environment",
			"Weather snapshot",
			"Read the weather home: clock, springs, sequencers, the smoothed color blocks.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	snapshot_row.invoke = func(_args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		var snapshot: Dictionary = weather.get_weather_snapshot()
		snapshot["smooth_fill"] = weather.get_smooth_fill()
		snapshot["smooth_sun"] = weather.get_smooth_sun()
		snapshot["smooth_fog"] = weather.get_smooth_fog()
		snapshot["smooth_sky"] = weather.get_smooth_sky()
		snapshot["terrain_light_combined_rgb"] = weather.get_terrain_light_combined_rgb()
		return DebugControls._action_result(snapshot)


## One authoritative WAC weather command row: `command` runs over the resolved
## Weather owner with the marshalled args.
static func _weather_command(
		controls: DebugControls,
		id: StringName,
		label: String,
		tooltip: String,
		args: Array[DebugArgSpec],
		command: Callable) -> void:
	var row := controls._action(id, &"Environment", label, tooltip,
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE, args)
	controls._authoritative(row)
	row.invoke = func(call_args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		command.call(weather, call_args)
		return DebugControls._action_result(null)
