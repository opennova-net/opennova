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
	var rain_row := controls._action(&"environment_rain", &"Environment", "Rain",
			"rain(percent, seconds): rain percent over a transition.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(rain_row)
	rain_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 2:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_rain(int(args[0]), int(args[1]))
		return DebugControls._action_result(null)

	var snow_row := controls._action(&"environment_snow", &"Environment", "Snow",
			"snow(percent, seconds): snow percent over a transition.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(snow_row)
	snow_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 2:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_snow(int(args[0]), int(args[1]))
		return DebugControls._action_result(null)

	var overcast_row := controls._action(&"environment_overcast", &"Environment", "Overcast",
			"overcast(percent, seconds): overcast blend over a transition.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(overcast_row)
	overcast_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 2:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_overcast(int(args[0]), int(args[1]))
		return DebugControls._action_result(null)

	var fog_distance_row := controls._action(&"environment_fog_distance", &"Environment", "Fog distance",
			"fogdist(metres): the fog distance target (2 m .. the 1024 m reference).",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(fog_distance_row)
	fog_distance_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 1:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_fog_distance(int(args[0]))
		return DebugControls._action_result(null)

	var move_fog_row := controls._action(&"environment_move_fog", &"Environment", "Move fog",
			"movefog(metres, seconds): the fog distance target over a transition.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(move_fog_row)
	move_fog_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 2:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_move_fog(int(args[0]), int(args[1]))
		return DebugControls._action_result(null)

	var sky_speed_row := controls._action(&"environment_sky_speed", &"Environment", "Sky speed",
			"skyspeed(rate): the cloud scroll rate target.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(sky_speed_row)
	sky_speed_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 1:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_sky_speed(int(args[0]))
		return DebugControls._action_result(null)

	var quake_row := controls._action(&"environment_quake", &"Environment", "Quake",
			"quake(seconds): the earthquake jitter duration.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(quake_row)
	quake_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 1:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_quake(int(args[0]))
		return DebugControls._action_result(null)

	var fog_type_row := controls._action(&"environment_fog_type", &"Environment", "Fog type",
			"fogtype(type): the fog model 0..3.",
			DebugControls.TARGET_WEATHER, DebugControls.OWNER_ENGINE)
	controls._authoritative(fog_type_row)
	fog_type_row.invoke = func(args: Array) -> Dictionary:
		var weather := controls._weather()
		if weather == null:
			return DebugControls._action_error(ERR_UNAVAILABLE)
		if args.size() < 1:
			return DebugControls._action_error(ERR_INVALID_PARAMETER)
		weather.command_fog_type(int(args[0]))
		return DebugControls._action_result(null)

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
