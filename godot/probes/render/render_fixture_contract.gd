class_name RenderFixtureContract
extends RefCounted

## The render-fixture capture contract: catalog lookup and the mission/godot
## frame conversion, the capture variants and profiles, the post-spawn and
## realized-pose/TOD/reflection checks, the comparison contract (the exact
## retail spawn kit, teleport and witnesses), provenance, minute selection,
## and the file helpers the probe and the publication share. Pure functions
## over the shell's public seams; the probe orchestrates, the GUT companion
## pins every rule.

const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")
const CaptureVariant := preload(
		"res://probes/render/render_capture_variant.gd")

const CAPTURE_PROFILE_CANONICAL := "canonical"
const CAPTURE_PROFILE_SHADOW_ATTRIBUTION := "shadow_attribution"
const DEFAULT_CAPTURE_SIZE := Vector2i(1600, 900)
const MIN_CAPTURE_AXIS := 320
const MAX_CAPTURE_AXIS := 4096
const REFLECTION_ORIGIN_TOLERANCE := 1.0e-4
const PLAYER_POSE_TOLERANCE := 1.0e-3
# Armory apply seeds this pool after an already-loaded 30-round clip exists;
# requesting nine reserve magazines realizes retail's visible 30 / 270 state.
const COMPARISON_PRIMARY_CLIPS := 9
const COMPARISON_WEAPON_CLIP := 30
const COMPARISON_WEAPON_RESERVE := 270
const COMPARISON_WEAPON_NAME := "WPN_M16BURST"
const COMPARISON_PLAYER_CLASS := 9
const COMPARISON_CHARACTER_ID := 0x0402
const COMPARISON_ARMS_GRAPHIC := "IndoArms.3di"
const COMPARISON_ARMS_CAMO := [1, 0, 0]
const COMPARISON_HUD_DETAIL_LEVEL := 3
const COMPARISON_BLUE_TEAM := 0
const COMPARISON_BLUE_NATIONALITY_INDEX := 2
const COMPARISON_BLUE_DIVISION_INDEX := 0
const COMPARISON_BLUE_COMBO_INDEX := 1
const COMPARISON_RED_TEAM := 1
const COMPARISON_RED_NATIONALITY_INDEX := 7
const COMPARISON_RED_DIVISION_INDEX := 0
const COMPARISON_RED_COMBO_INDEX := 0


static func mission_to_godot(position: Vector3) -> Vector3:
	return Vector3(position.x, position.z, -position.y)


static func godot_to_mission(position: Vector3) -> Vector3:
	return Vector3(position.x, -position.z, position.y)


static func camera_basis(yaw_deg: float, pitch_deg: float) -> Basis:
	return Basis(Vector3.UP, deg_to_rad(-yaw_deg)) \
			* Basis(Vector3.RIGHT, deg_to_rad(pitch_deg))


static func fixture_by_id(catalog: Dictionary, id: String) -> Dictionary:
	if not FixturePublication.publication_name_is_canonical(id):
		return {}
	var fixtures: Variant = catalog.get("fixtures", [])
	if not (fixtures is Array):
		return {}
	for value: Variant in fixtures:
		if value is Dictionary and String(value.get("id", "")) == id:
			return (value as Dictionary).duplicate(true)
	return {}


static func capture_variants() -> Array:
	return [
		CaptureVariant.new("beauty", 0, true, true),
		CaptureVariant.new("shadows_off", 0, false, false),
		CaptureVariant.new("lighting_only", 2, true, true),
		CaptureVariant.new("unshaded", 1, true, true),
		CaptureVariant.new("directional_shadow_atlas", 10, true, true),
	]


static func capture_variant_tile_cache_is_realized(
		variant, diagnostics: Dictionary) -> bool:
	if variant == null or not (variant is CaptureVariant):
		return false
	var static_enabled := bool(variant.static_terrain_shadow_enabled)
	if not diagnostics.has("available") \
			or not bool(diagnostics.available) \
			or not diagnostics.has("tile_overlay_required") \
			or (bool(diagnostics.tile_overlay_required) \
					and (not diagnostics.has("tile_overlay_available") \
							or not bool(diagnostics.tile_overlay_available))) \
			or not diagnostics.has("shadow_raster_available") \
			or not bool(diagnostics.shadow_raster_available) \
			or int(diagnostics.get("upload_failures", -1)) != 0 \
			or int(diagnostics.get("shadow_raster_failures", -1)) != 0:
		return false
	if bool(diagnostics.get("shadow_provider_enabled", not static_enabled)) \
			!= static_enabled:
		return false
	var frame_requests := int(diagnostics.get("frame_requests", 0))
	var frame_compose_jobs := int(diagnostics.get("frame_compose_jobs", -1))
	var frame_ready_hits := int(diagnostics.get("frame_ready_hits", -1))
	if frame_requests <= 0 \
			or int(diagnostics.get("frame_capacity_fallbacks", -1)) != 0 \
			or int(diagnostics.get("pending_jobs", -1)) != 0 \
			or frame_compose_jobs != 0 \
			or frame_ready_hits != frame_requests \
			or int(diagnostics.get("frame_selected_ready_pages", 0)) <= 0 \
			or int(diagnostics.get("ready_pages", 0)) <= 0 \
			or int(diagnostics.get(
					"shadow_provider_frame_plan_failures", -1)) != 0 \
			or int(diagnostics.get("frame_shadow_rgb_changed_bytes", -1)) != 0:
		return false
	if static_enabled:
		# Worker evidence is cumulative only for the current invalidation epoch.
		# The final refresh above is an all-ready hit frame with no compiler work.
		return bool(diagnostics.get("shadow_provider_snapshot_exact", false)) \
				and int(diagnostics.get(
						"shadow_provider_admitted_count", 0)) > 0 \
				and int(diagnostics.get(
						"shadow_provider_resolved_casters", 0)) > 0 \
				and int(diagnostics.get("shadow_epoch_raster_jobs", 0)) > 0 \
				and int(diagnostics.get(
						"shadow_epoch_pages_with_draws", 0)) > 0 \
				and int(diagnostics.get(
						"shadow_epoch_projection_draws", 0)) > 0 \
				and int(diagnostics.get(
						"shadow_epoch_plan_failures", -1)) == 0 \
				and int(diagnostics.get(
						"shadow_epoch_unsupported_draw_count", -1)) == 0 \
				and int(diagnostics.get(
						"shadow_epoch_unsupported_attribution_truncated", -1)) == 0
	# The disabled epoch must contain no static-raster publication.
	return int(diagnostics.get("shadow_epoch_raster_jobs", -1)) == 0 \
			and int(diagnostics.get(
					"frame_shadow_alpha_changed_bytes", -1)) == 0


static func capture_variant_matches_diagnostics(
		variant,
		diagnostics: Dictionary,
		realized_variant: Dictionary = {}) -> bool:
	if variant == null or not (variant is CaptureVariant):
		return false
	var world_value: Variant = diagnostics.get("world")
	var terrain_value: Variant = diagnostics.get("terrain")
	var renderer_value: Variant = diagnostics.get("renderer")
	var shadows_value: Variant = diagnostics.get("shadows")
	if not (world_value is Dictionary) \
			or not (terrain_value is Dictionary) \
			or not (renderer_value is Dictionary) \
			or not (shadows_value is Dictionary):
		return false
	var world := world_value as Dictionary
	var terrain := terrain_value as Dictionary
	var renderer := renderer_value as Dictionary
	var shadows := shadows_value as Dictionary
	var dynamic_value: Variant = shadows.get("dynamic")
	var static_value: Variant = shadows.get("static_terrain")
	if not (dynamic_value is Dictionary) or not (static_value is Dictionary):
		return false
	var dynamic := dynamic_value as Dictionary
	var static_terrain := static_value as Dictionary
	if not world.has("loaded") or not world.has("visible") \
			or not terrain.has("available") \
			or not terrain.has("visible") \
			or not terrain.has("visible_in_tree") \
			or not renderer.has("debug_draw") \
			or not dynamic.has("available") \
			or not dynamic.has("visible") \
			or not dynamic.has("visible_in_tree") \
			or not dynamic.has("processing") \
			or not dynamic.has("shadow_enabled") \
			or not static_terrain.has("available") \
			or not static_terrain.has("enabled") \
			or not static_terrain.has("suppressed_bms_ids"):
		return false
	for key in [
		"id",
		"debug_draw",
		"dynamic_shadow_enabled",
		"static_terrain_shadow_enabled",
		"suppressed_dynamic_caster_bms_ids",
		"suppressed_static_caster_bms_ids",
	]:
		if not realized_variant.has(key):
			return false
	var expected_dynamic := _normalized_suppressed_bms_ids(
			variant.suppressed_dynamic_caster_bms_ids)
	var expected_static := _normalized_suppressed_bms_ids(
			variant.suppressed_static_caster_bms_ids)
	var realized_dynamic := _normalized_suppressed_bms_ids(
			realized_variant.suppressed_dynamic_caster_bms_ids)
	var realized_static := _normalized_suppressed_bms_ids(
			realized_variant.suppressed_static_caster_bms_ids)
	var diagnostic_static := _normalized_suppressed_bms_ids(
			static_terrain.suppressed_bms_ids)
	for normalized in [expected_dynamic, expected_static, realized_dynamic,
			realized_static, diagnostic_static]:
		if not bool((normalized as Dictionary).get("valid", false)):
			return false
	if not bool(world.loaded) or not bool(world.visible) \
			or not bool(terrain.available) \
			or not bool(terrain.visible) \
			or not bool(terrain.visible_in_tree) \
			or not bool(dynamic.available) \
			or not bool(dynamic.visible) \
			or not bool(dynamic.visible_in_tree) \
			or not bool(dynamic.processing) \
			or not bool(static_terrain.available):
		return false
	return bool(dynamic.available) \
			and bool(static_terrain.available) \
			and int(renderer.debug_draw) == int(variant.debug_draw) \
			and bool(dynamic.shadow_enabled) \
					== bool(variant.dynamic_shadow_enabled) \
			and bool(static_terrain.enabled) \
					== bool(variant.static_terrain_shadow_enabled) \
			and String(realized_variant.id) == String(variant.id) \
			and int(realized_variant.debug_draw) == int(variant.debug_draw) \
			and bool(realized_variant.dynamic_shadow_enabled) \
					== bool(variant.dynamic_shadow_enabled) \
			and bool(realized_variant.static_terrain_shadow_enabled) \
					== bool(variant.static_terrain_shadow_enabled) \
			and (realized_dynamic as Dictionary).ids \
					== (expected_dynamic as Dictionary).ids \
			and (realized_static as Dictionary).ids \
					== (expected_static as Dictionary).ids \
			and (diagnostic_static as Dictionary).ids \
					== (expected_static as Dictionary).ids


static func _normalized_suppressed_bms_ids(value: Variant) -> Dictionary:
	if not (value is Array) and not (value is PackedInt32Array):
		return {"valid": false, "ids": []}
	var seen: Dictionary = {}
	var ids: Array = []
	for raw_id: Variant in value:
		if not (raw_id is int) or int(raw_id) <= 0 or seen.has(int(raw_id)):
			return {"valid": false, "ids": []}
		seen[int(raw_id)] = true
		ids.append(int(raw_id))
	ids.sort()
	return {"valid": true, "ids": ids}


static func shadow_attribution_variants(
		suppressed_static_ids: PackedInt32Array = PackedInt32Array()) -> Array:
	var variants: Array = [
		CaptureVariant.new("both_shadow_systems", 0, true, true),
		CaptureVariant.new("dynamic_shadow_only", 0, true, false),
		CaptureVariant.new("static_terrain_shadow_only", 0, false, true),
	]
	if not suppressed_static_ids.is_empty():
		variants.append(CaptureVariant.new(
				"static_without_selected", 0, false, true,
				PackedInt32Array(), suppressed_static_ids))
	variants.append(CaptureVariant.new("dynamic_without_bms58", 0, true, false,
			PackedInt32Array([58])))
	variants.append(CaptureVariant.new("shadows_off", 0, false, false))
	return variants


static func parse_static_shadow_suppression(value: String) -> Dictionary:
	var selected: Dictionary = {}
	for token_value: String in value.split(",", false):
		var token := token_value.strip_edges()
		if token.is_empty() or not token.is_valid_int():
			return {"error": "invalid static-shadow BMS id '%s'" % token}
		var bms_id := token.to_int()
		if bms_id <= 0:
			return {"error": "static-shadow BMS ids must be positive"}
		selected[bms_id] = true
	var ids: Array = selected.keys()
	ids.sort()
	return {"ids": PackedInt32Array(ids)}


static func select_capture_profile(
		value: String, suppressed_static_value: String = "") -> Dictionary:
	var requested := value.strip_edges().to_lower()
	if requested.is_empty() or requested == CAPTURE_PROFILE_CANONICAL:
		return {
			"id": CAPTURE_PROFILE_CANONICAL,
			"scratch_only": false,
			"variants": capture_variants(),
		}
	if requested == CAPTURE_PROFILE_SHADOW_ATTRIBUTION:
		var suppression := parse_static_shadow_suppression(
				suppressed_static_value)
		if suppression.has("error"):
			return suppression
		return {
			"id": CAPTURE_PROFILE_SHADOW_ATTRIBUTION,
			"scratch_only": true,
			"variants": shadow_attribution_variants(suppression.ids),
		}
	return {"error": "unknown NOVA_RENDER_CAPTURE_PROFILE '%s'" % requested}


static func capture_output_is_allowed(
		_profile: Dictionary, output_path: String, scratch_root: String) -> bool:
	return FixturePublication.path_is_strict_descendant(output_path, scratch_root)


static func capture_phase_contract() -> Dictionary:
	return {
		"weather": "canonical_reset_at_requested_tod",
		"water_noise": "one_pose_refresh_then_frozen_noncanonical",
		"particles": "frozen_noncanonical",
		"pixel_metrics": "within_run_variants_only",
	}


static func post_spawn_capture_state_matches(state: Dictionary) -> bool:
	return String(state.get("entry", "")) \
			== "single_player_auto_spawn_after_splash" \
			and String(state.get("observed_at", "")) \
			== "before_fixture_freeze" \
			and bool(state.get("world_loaded", false)) \
			and not bool(state.get("world_loading", true)) \
			and bool(state.get("runtime_playing", false)) \
			and bool(state.get("local_player_spawned", false)) \
			and bool(state.get("gameplay_input_active", false)) \
			and bool(state.get("gameplay_camera_current", false)) \
			and not bool(state.get("spawn_or_menu_active", true))


static func pin_weather_phase(world: Node) -> Error:
	if world == null or not is_instance_valid(world) \
			or not world.has_method("get_weather_node"):
		return ERR_INVALID_PARAMETER
	var weather: Variant = world.call("get_weather_node")
	if not (weather is Node) or not is_instance_valid(weather) \
			or not (weather as Node).has_method("prepare_world_driven"):
		return ERR_UNAVAILABLE
	(weather as Node).call("prepare_world_driven")
	return OK


static func diagnostic_variant_contract_matches(declared_value: Variant) -> bool:
	if not (declared_value is Array):
		return false
	var declared: Array = declared_value
	var variants := capture_variants()
	if declared.size() != variants.size():
		return false
	for index in range(variants.size()):
		if typeof(declared[index]) != TYPE_STRING \
				or String(declared[index]) != String(variants[index].id):
			return false
	return true


static func realized_tod_matches(
		diagnostics: Dictionary,
		expected_minute_of_day: float,
		expected_time_fixed24: int) -> bool:
	var environment: Variant = diagnostics.get("environment")
	if not (environment is Dictionary):
		return false
	var state := environment as Dictionary
	if not state.has("mission_minute_of_day") \
			or not state.has("mission_time_fixed24"):
		return false
	return float(state["mission_minute_of_day"]) == expected_minute_of_day \
			and int(state["mission_time_fixed24"]) == expected_time_fixed24


static func realized_reflection_pose_matches(diagnostics: Dictionary) -> bool:
	var water_value: Variant = diagnostics.get("water")
	if not (water_value is Dictionary):
		return false
	var water := water_value as Dictionary
	if not water.has("render_active"):
		return false
	if not bool(water["render_active"]):
		return true
	if not water.has("mesh_visible"):
		return false
	if not bool(water["mesh_visible"]):
		# Occlusion hid the water strip for this pose (an interior fixture with
		# the sea culled): Water::advance_frame early-outs before the mirror
		# retarget, and the cleared strip means the stale mirror is never
		# sampled, so its pose carries no pixel obligation for this frame.
		return true
	var reflection_value: Variant = water.get("reflection")
	if not (reflection_value is Dictionary):
		return false
	var reflection := reflection_value as Dictionary
	if not bool(reflection.get("available", false)):
		return false
	var camera_value: Variant = diagnostics.get("camera")
	var reflection_camera_value: Variant = reflection.get("camera")
	if not (camera_value is Dictionary) \
			or not (reflection_camera_value is Dictionary):
		return false
	var camera_transform: Variant = (camera_value as Dictionary).get(
			"global_transform")
	var reflection_transform: Variant = (reflection_camera_value as Dictionary).get(
			"global_transform")
	if not (camera_transform is Transform3D) \
			or not (reflection_transform is Transform3D):
		return false
	var height := float(water.get("height", NAN))
	if not is_finite(height):
		return false
	var source_origin := (camera_transform as Transform3D).origin
	var expected_origin := Vector3(
			source_origin.x, 2.0 * height - source_origin.y, source_origin.z)
	return (reflection_transform as Transform3D).origin.distance_squared_to(
			expected_origin) <= REFLECTION_ORIGIN_TOLERANCE \
			* REFLECTION_ORIGIN_TOLERANCE


static func parse_capture_resolution(capture_settings: Dictionary) -> Dictionary:
	var declared: Variant = capture_settings.get("resolution")
	if not (declared is Array) or (declared as Array).size() != 2:
		return {"error": "catalog capture.resolution must be a [width, height] pair"}
	var axes: Array[int] = []
	for value: Variant in declared as Array:
		var value_type := typeof(value)
		if value_type != TYPE_INT and value_type != TYPE_FLOAT:
			return {"error": "catalog capture.resolution must contain integers"}
		var numeric := float(value)
		if numeric != floorf(numeric) or numeric < MIN_CAPTURE_AXIS \
				or numeric > MAX_CAPTURE_AXIS:
			return {"error": "catalog capture.resolution axes must be integers in %d..%d"
					% [MIN_CAPTURE_AXIS, MAX_CAPTURE_AXIS]}
		axes.append(int(numeric))
	return {"size": Vector2i(axes[0], axes[1])}


static func select_capture_mode(
		fixture: Dictionary, environment_selector: String) -> Dictionary:
	# The original diagnostic catalog predates presentation modes and is
	# world-only by definition. Retail comparison rows declare the mode.
	var declared := String(fixture.get("capture_mode", "world_only")).strip_edges()
	if declared != "world_only" and declared != "full_frame" \
			and declared != "hud_hidden":
		return {"error": (
				"fixture capture_mode must be 'world_only', 'full_frame', or 'hud_hidden'")}
	var selector := environment_selector.strip_edges()
	if not selector.is_empty() and selector != declared:
		return {"error": (
				"NOVA_RENDER_CAPTURE_MODE '%s' does not match fixture mode '%s'"
				% [selector, declared])}
	return {
		"mode": declared,
		"world_only": declared == "world_only",
	}


static func parse_comparison_contract(
		fixture: Dictionary, declared_value: Variant) -> Dictionary:
	if not (declared_value is Dictionary):
		return {"error": "full-frame comparison fixture has no comparison_contract"}
	var declared := declared_value as Dictionary
	var fixture_mode := String(fixture.get("capture_mode", "")).strip_edges()
	if fixture_mode == "hud_hidden":
		return _parse_hud_hidden_comparison_contract(fixture, declared)
	var expected_keys: Array[String] = [
		"capture_mode",
		"equipped_weapon",
		"retail_hud_weapon_label",
		"hud_enabled",
		"terrain_enabled",
		"viewmodel_enabled",
		"player_pose_source",
	]
	if declared.size() != expected_keys.size():
		return {"error": "comparison_contract must contain exactly %s" % [expected_keys]}
	for key in expected_keys:
		if not declared.has(key):
			return {"error": "comparison_contract is missing %s" % key}
	var contract_mode := String(declared.get("capture_mode", "")).strip_edges()
	if fixture_mode != "full_frame" or contract_mode != fixture_mode:
		return {"error": (
				"comparison_contract.capture_mode must match a full_frame fixture")}
	var weapon := String(declared.get("equipped_weapon", "")).strip_edges()
	var retail_label := String(
			declared.get("retail_hud_weapon_label", "")).strip_edges()
	if weapon.is_empty() or retail_label.is_empty():
		return {"error": "comparison_contract must name both engine and retail HUD weapon"}
	for enabled_key in ["hud_enabled", "terrain_enabled", "viewmodel_enabled"]:
		if typeof(declared.get(enabled_key)) != TYPE_BOOL \
				or not bool(declared.get(enabled_key)):
			return {"error": "comparison_contract.%s must be true" % enabled_key}
	if String(declared.get("player_pose_source", "")) \
			!= "retail_player_bms.applied":
		return {"error": (
				"comparison_contract.player_pose_source must be retail_player_bms.applied")}

	var retail_pose_value: Variant = fixture.get("retail_player_bms")
	if not (retail_pose_value is Dictionary):
		return {"error": "fixture has no retail_player_bms pose"}
	var applied_value: Variant = (retail_pose_value as Dictionary).get("applied")
	if not (applied_value is Array) or (applied_value as Array).size() != 3:
		return {"error": "retail_player_bms.applied must be a three-component position"}
	var applied := applied_value as Array
	for component: Variant in applied:
		if (typeof(component) != TYPE_INT and typeof(component) != TYPE_FLOAT) \
				or not is_finite(float(component)):
			return {"error": "retail_player_bms.applied components must be finite numbers"}
	var camera_value: Variant = fixture.get("camera_bms")
	if not (camera_value is Dictionary):
		return {"error": "fixture has no camera_bms yaw/pitch"}
	var camera := camera_value as Dictionary
	for angle_key in ["yaw_deg", "pitch_deg"]:
		var angle: Variant = camera.get(angle_key)
		if (typeof(angle) != TYPE_INT and typeof(angle) != TYPE_FLOAT) \
				or not is_finite(float(angle)):
			return {"error": "camera_bms.%s must be a finite number" % angle_key}
	return {
		"capture_mode": contract_mode,
		"weapon": weapon,
		"retail_hud_weapon_label": retail_label,
		"hud_enabled": true,
		"terrain_enabled": true,
		"viewmodel_enabled": true,
		"player_pose_source": "retail_player_bms.applied",
		"player_position_bms": Vector3(
				float(applied[0]), float(applied[1]), float(applied[2])),
		"yaw_deg": float(camera.yaw_deg),
		"pitch_deg": float(camera.pitch_deg),
	}


static func _parse_hud_hidden_comparison_contract(
		fixture: Dictionary, declared: Dictionary) -> Dictionary:
	var expected_keys: Array[String] = [
		"capture_mode",
		"equipped_weapon",
		"weapon_clip",
		"weapon_reserve",
		"character_id",
		"arms_graphic",
		"arms_camo",
		"gameplay_hud_visible",
		"hud_canvas_layer_active",
		"hud_detail_level",
		"player_view_effects_active",
		"viewmodel_enabled",
		"terrain_enabled",
		"ads_active",
		"big_map_active",
		"player_pose_source",
	]
	if declared.size() != expected_keys.size():
		return {"error": "comparison_contract must contain exactly %s" % [expected_keys]}
	for key in expected_keys:
		if not declared.has(key):
			return {"error": "comparison_contract is missing %s" % key}
	if String(declared.get("capture_mode", "")) != "hud_hidden":
		return {"error": "comparison_contract.capture_mode must match hud_hidden fixture"}
	if String(declared.get("equipped_weapon", "")) != COMPARISON_WEAPON_NAME \
			or not _numeric_contract_value_equals(
					declared.get("weapon_clip"), COMPARISON_WEAPON_CLIP) \
			or not _numeric_contract_value_equals(
					declared.get("weapon_reserve"), COMPARISON_WEAPON_RESERVE):
		return {"error": "comparison_contract must stage WPN_M16BURST at 30/270"}
	if not _numeric_contract_value_equals(
			declared.get("character_id"), COMPARISON_CHARACTER_ID) \
			or String(declared.get("arms_graphic", "")) \
			!= COMPARISON_ARMS_GRAPHIC:
		return {"error": "comparison_contract must stage retail slot 0's bare arms"}
	var camo_value: Variant = declared.get("arms_camo")
	if not (camo_value is Array) or (camo_value as Array).size() != 3:
		return {"error": "comparison_contract.arms_camo must be [1, 0, 0]"}
	var camo := camo_value as Array
	for index in 3:
		if not _numeric_contract_value_equals(
				camo[index], int(COMPARISON_ARMS_CAMO[index])):
			return {"error": "comparison_contract.arms_camo must be [1, 0, 0]"}
	var expected_bools := {
		"gameplay_hud_visible": false,
		"hud_canvas_layer_active": true,
		"player_view_effects_active": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"ads_active": false,
		"big_map_active": false,
	}
	for key: String in expected_bools:
		if typeof(declared.get(key)) != TYPE_BOOL \
				or bool(declared.get(key)) != bool(expected_bools[key]):
			return {"error": "comparison_contract.%s must be %s" \
					% [key, str(expected_bools[key])]}
	if not _numeric_contract_value_equals(
			declared.get("hud_detail_level"), COMPARISON_HUD_DETAIL_LEVEL):
		return {"error": "comparison_contract.hud_detail_level must be 3"}
	if String(declared.get("player_pose_source", "")) \
			!= "retail_player_bms.applied":
		return {"error": (
				"comparison_contract.player_pose_source must be retail_player_bms.applied")}

	var retail_pose_value: Variant = fixture.get("retail_player_bms")
	if not (retail_pose_value is Dictionary):
		return {"error": "fixture has no retail_player_bms pose"}
	var applied_value: Variant = (retail_pose_value as Dictionary).get("applied")
	if not (applied_value is Array) or (applied_value as Array).size() != 3:
		return {"error": "retail_player_bms.applied must be a three-component position"}
	var applied := applied_value as Array
	for component: Variant in applied:
		if (typeof(component) != TYPE_INT and typeof(component) != TYPE_FLOAT) \
				or not is_finite(float(component)):
			return {"error": "retail_player_bms.applied components must be finite numbers"}
	var camera_value: Variant = fixture.get("camera_bms")
	if not (camera_value is Dictionary):
		return {"error": "fixture has no camera_bms yaw/pitch"}
	var camera := camera_value as Dictionary
	for angle_key in ["yaw_deg", "pitch_deg"]:
		var angle: Variant = camera.get(angle_key)
		if (typeof(angle) != TYPE_INT and typeof(angle) != TYPE_FLOAT) \
				or not is_finite(float(angle)):
			return {"error": "camera_bms.%s must be a finite number" % angle_key}
	var parsed := declared.duplicate(true)
	parsed["weapon_clip"] = COMPARISON_WEAPON_CLIP
	parsed["weapon_reserve"] = COMPARISON_WEAPON_RESERVE
	parsed["character_id"] = COMPARISON_CHARACTER_ID
	parsed["arms_camo"] = COMPARISON_ARMS_CAMO.duplicate()
	parsed["hud_detail_level"] = COMPARISON_HUD_DETAIL_LEVEL
	parsed["player_position_bms"] = Vector3(
			float(applied[0]), float(applied[1]), float(applied[2]))
	parsed["yaw_deg"] = float(camera.yaw_deg)
	parsed["pitch_deg"] = float(camera.pitch_deg)
	return parsed


static func _numeric_contract_value_equals(value: Variant, expected: int) -> bool:
	return (typeof(value) == TYPE_INT or typeof(value) == TYPE_FLOAT) \
			and is_finite(float(value)) and float(value) == float(expected)


static func comparison_weapon_name(contract: Dictionary) -> String:
	# hud_hidden's catalog boundary uses the canonical evidence key; legacy
	# full_frame fixtures retain their normalized internal `weapon` field.
	return String(contract.get(
			"equipped_weapon", contract.get("weapon", ""))).strip_edges()


static func comparison_spawn_profile(contract: Dictionary) -> Dictionary:
	var profile := {
		# Preserve the committed full_frame profile exactly. The matched identity
		# selection below is a hud_hidden evidence constraint, not a legacy spawn
		# behavior change.
		"player_class": COMPARISON_PLAYER_CLASS,
		"primary": comparison_weapon_name(contract),
		"primary_clips": COMPARISON_PRIMARY_CLIPS,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
	}
	if String(contract.get("capture_mode", "")) != "hud_hidden":
		return profile
	profile["team"] = COMPARISON_BLUE_TEAM
	profile["side_profiles"] = [
		# Retail profile slot 0 in the comparison install: blue Australia /
		# SASR combo 002 (bare IndoArms), red Kopassus combo 001. These are
		# Avatars.def tree indices; NetSessionDrive is still the authority that
		# resolves and packs them for spawn admission.
		{
			"team": COMPARISON_BLUE_TEAM,
			"nationality": COMPARISON_BLUE_NATIONALITY_INDEX,
			"division": COMPARISON_BLUE_DIVISION_INDEX,
			"combo": COMPARISON_BLUE_COMBO_INDEX,
			"player_class": COMPARISON_PLAYER_CLASS,
		},
		{
			"team": COMPARISON_RED_TEAM,
			"nationality": COMPARISON_RED_NATIONALITY_INDEX,
			"division": COMPARISON_RED_DIVISION_INDEX,
			"combo": COMPARISON_RED_COMBO_INDEX,
			"player_class": COMPARISON_PLAYER_CLASS,
		},
	]
	return profile


static func apply_comparison_weapon_fallback(
		game: Object, world: Object, contract: Dictionary) -> Dictionary:
	if game == null or world == null or not world.has_method("get_sim"):
		return {"error": "comparison Armory fallback has no live game world"}
	var sim: Variant = world.call("get_sim")
	if not (sim is Object):
		return {"error": "comparison Armory fallback has no live simulation"}
	var sim_object := sim as Object
	for method_name in ["apply_local_player_loadout", "get_local_player_inventory"]:
		if not sim_object.has_method(method_name):
			return {"error": "comparison simulation has no %s Armory seam" % method_name}
	if not world.has_method("set_local_player_weapon_by_name"):
		return {"error": "comparison world has no Armory presentation seam"}
	if not (game is Node):
		return {"error": "comparison game has no presenter tree"}
	var presenter: Node = (game as Node).get_node_or_null("LocalPlayerPresenter")
	if presenter == null or not presenter.has_method("refresh_viewmodel"):
		return {"error": "comparison game has no viewmodel refresh seam"}

	var weapon := comparison_weapon_name(contract)
	if weapon.is_empty():
		return {"error": "comparison Armory fallback has no requested weapon"}
	var profile := comparison_spawn_profile(contract)
	var soldier_class := int(profile.get("player_class", 0))
	var kit: Array[WeaponKitEntry] = [
		WeaponKitEntry.make(weapon, COMPARISON_PRIMARY_CLIPS)]
	if not bool(sim_object.call(
			"apply_local_player_loadout", kit, soldier_class)):
		return {"error": "production Armory apply rejected comparison weapon %s" % weapon}
	var inventory_value: Variant = sim_object.call("get_local_player_inventory")
	if not (inventory_value is PlayerInventory):
		return {"error": "production Armory apply produced no inventory witness"}
	var inventory := inventory_value as PlayerInventory
	var equipped := inventory.equipped_name
	if not inventory.valid or equipped != weapon:
		return {"error": (
				"production Armory apply equipped %s, expected %s") % [equipped, weapon]}
	if not bool(world.call("set_local_player_weapon_by_name", equipped)):
		return {"error": "production Armory presentation rejected %s" % equipped}
	presenter.call("refresh_viewmodel")
	return {
		"weapon_install_source": "production_armory_fallback_after_spawn_override",
		"equipped_weapon": equipped,
		"player_class": soldier_class,
	}


static func teleport_comparison_player(world: Object, contract: Dictionary) -> Dictionary:
	if world == null or not world.has_method("get_sim"):
		return {"error": "comparison world has no simulation"}
	var sim: Variant = world.call("get_sim")
	if not (sim is Object) or not (sim as Object).has_method(
			"debug_teleport_local_player"):
		return {"error": "comparison simulation cannot teleport the local player"}
	var teleport_value: Variant = (sim as Object).call(
			"debug_teleport_local_player",
			contract.get("player_position_bms", Vector3.ZERO),
			float(contract.get("yaw_deg", 0.0)),
			float(contract.get("pitch_deg", 0.0)))
	if typeof(teleport_value) != TYPE_INT or int(teleport_value) != OK:
		var teleport_error := int(teleport_value) \
				if typeof(teleport_value) == TYPE_INT else ERR_UNAVAILABLE
		return {"error": "could not apply comparison player pose: %s" \
				% error_string(teleport_error)}
	return {
		"position_bms": contract.get("player_position_bms", Vector3.ZERO),
		"yaw_deg": float(contract.get("yaw_deg", 0.0)),
		"pitch_deg": float(contract.get("pitch_deg", 0.0)),
	}


static func verify_comparison_spawn(
		world: Object, contract: Dictionary) -> Dictionary:
	if world == null or not world.has_method("get_sim"):
		return {"error": "comparison world is unavailable"}
	var sim: Variant = world.call("get_sim")
	if not (sim is Object):
		return {"error": "comparison simulation is unavailable"}
	var sim_object := sim as Object
	for method_name in [
		"get_local_player_inventory",
		"get_local_player_class",
		"get_local_player_weapon_name",
		"get_local_player_weapon_state",
	]:
		if not sim_object.has_method(method_name):
			return {"error": "comparison simulation has no %s witness" % method_name}
	var expected_weapon := comparison_weapon_name(contract)
	var inventory_value: Variant = sim_object.call("get_local_player_inventory")
	if not (inventory_value is PlayerInventory):
		return {"error": "comparison spawn produced no inventory witness"}
	var inventory := inventory_value as PlayerInventory
	var equipped := inventory.equipped_name
	var sim_weapon := String(sim_object.call("get_local_player_weapon_name"))
	if not inventory.valid or equipped != expected_weapon \
			or sim_weapon != expected_weapon:
		return {"error": (
				"comparison spawn weapon mismatch: inventory=%s sim=%s expected=%s") \
				% [equipped, sim_weapon, expected_weapon]}
	if not world.has_method("local_player_weapon_name"):
		return {"error": "comparison world has no presented-weapon witness"}
	var presented_weapon := String(world.call("local_player_weapon_name"))
	if presented_weapon != expected_weapon:
		return {"error": "comparison world presents %s, expected %s" \
				% [presented_weapon, expected_weapon]}
	var weapon_state_value: Variant = sim_object.call(
			"get_local_player_weapon_state")
	if not (weapon_state_value is PlayerWeaponView):
		return {"error": "comparison spawn produced no weapon-state witness"}
	var weapon_state := weapon_state_value as PlayerWeaponView
	var weapon_clip := weapon_state.clip
	var weapon_reserve := weapon_state.reserve
	if not weapon_state.active \
			or weapon_clip != COMPARISON_WEAPON_CLIP \
			or weapon_reserve != COMPARISON_WEAPON_RESERVE:
		return {"error": (
				"comparison weapon ammo mismatch: clip=%d reserve=%d expected=%d/%d") \
				% [weapon_clip, weapon_reserve,
						COMPARISON_WEAPON_CLIP, COMPARISON_WEAPON_RESERVE]}
	return {
		"equipped_weapon": equipped,
		"player_class": int(sim_object.call("get_local_player_class")),
		"weapon_clip": weapon_clip,
		"weapon_reserve": weapon_reserve,
	}


static func observe_comparison_contract(
		game: Object, world: Object, viewport: Object,
		contract: Dictionary,
		captured_hud_witness: Dictionary = {}) -> Dictionary:
	if game == null or world == null or viewport == null \
			or not world.has_method("get_sim"):
		return {"error": "comparison presentation is unavailable"}
	var spawn_witness := verify_comparison_spawn(world, contract)
	if spawn_witness.has("error"):
		return spawn_witness
	var sim: Variant = world.call("get_sim")
	if not (sim is Object):
		return {"error": "comparison simulation is unavailable"}
	var sim_object := sim as Object
	for method_name in [
		"get_local_player_weapon_name",
		"get_local_player_class",
		"get_local_player_position",
	]:
		if not sim_object.has_method(method_name):
			return {"error": "comparison simulation has no %s witness" % method_name}
	var weapon := String(spawn_witness.equipped_weapon)
	var player_position_godot: Variant = sim_object.call("get_local_player_position")
	if not (player_position_godot is Vector3):
		return {"error": "comparison player position witness is unavailable"}
	var player_position_bms := godot_to_mission(player_position_godot as Vector3)
	var requested_position: Vector3 = contract.get(
			"player_position_bms", Vector3.ZERO)
	if player_position_bms.distance_to(requested_position) > PLAYER_POSE_TOLERANCE:
		return {"error": "comparison player pose drifted: observed=%s expected=%s" \
				% [str(player_position_bms), str(requested_position)]}

	var hud: CanvasLayer = (game as Node).get_node_or_null("HUD") as CanvasLayer \
			if game is Node else null
	if hud == null or not hud.visible:
		return {"error": "comparison HUD CanvasLayer is absent or hidden"}
	# The FP gun draws inside the beauty pass; the presenter owns its node.
	var presenter: Object = (game as Node).get_node_or_null(
			"LocalPlayerPresenter") if game is Node else null
	var viewmodel: Node3D = presenter.call("viewmodel") as Node3D \
			if presenter != null and presenter.has_method("viewmodel") else null
	if viewmodel == null or not viewmodel.visible:
		return {"error": "comparison first-person viewmodel is absent or hidden"}
	if not world.has_method("get_terrain_data") \
			or world.call("get_terrain_data") == null:
		return {"error": "comparison terrain data is unavailable"}
	if not world.has_method("get_terrain_node"):
		return {"error": "comparison terrain node is unavailable"}
	var terrain_value: Variant = world.call("get_terrain_node")
	if not (terrain_value is Node3D) \
			or not (terrain_value as Node3D).is_visible_in_tree():
		return {"error": "comparison terrain node is absent or hidden"}
	var observed := {
		"observed_at": "after_pose_settle_before_fixture_freeze",
		"equipped_weapon": weapon,
		"player_class": int(spawn_witness.player_class),
		"weapon_clip": int(spawn_witness.weapon_clip),
		"weapon_reserve": int(spawn_witness.weapon_reserve),
		"player_position_bms": player_position_bms,
		"requested_player_pose_bms": {
			"position": requested_position,
			"yaw_deg": float(contract.get("yaw_deg", 0.0)),
			"pitch_deg": float(contract.get("pitch_deg", 0.0)),
		},
		"hud_canvas_layer_visible": true,
		"viewmodel_canvas_layer_visible": true,
		"terrain_data_available": true,
		"terrain_node_visible": true,
	}
	if String(contract.get("capture_mode", "")) != "hud_hidden":
		return observed

	var hud_witness_value := captured_hud_witness
	if hud_witness_value.is_empty():
		# Direct contract probes may inspect a live transaction. Published capture
		# paths pass the completed-draw witness embedded by GameDebugAdapter.
		if not game.has_method("hud_hidden_capture_witness"):
			return {"error": "comparison game has no HUD-hidden presentation witness"}
		var hud_value: Variant = game.call("hud_hidden_capture_witness")
		if not (hud_value is HudHiddenCaptureWitness):
			return {"error": "comparison HUD-hidden presentation witness is untyped"}
		var hud_witness := hud_value as HudHiddenCaptureWitness
		if not hud_witness.is_valid():
			return {"error": "comparison HUD-hidden presentation is invalid: %s" \
					% hud_witness.error}
		hud_witness_value = {
			"hud_detail_level": hud_witness.hud_detail_level,
			"gameplay_hud_visible": hud_witness.gameplay_hud_visible,
			"player_view_effects_active": hud_witness.player_view_effects_active,
			"ads_active": hud_witness.ads_active,
			"big_map_active": hud_witness.big_map_active,
			"hud_canvas_layer_active": hud_witness.hud_canvas_layer_active,
		}
	for key in [
		"hud_detail_level", "gameplay_hud_visible",
		"player_view_effects_active", "ads_active", "big_map_active",
		"hud_canvas_layer_active",
	]:
		if not hud_witness_value.has(key):
			return {"error": "captured HUD-hidden witness is missing %s" % key}
	if not world.has_method("local_player_first_person_arms_witness"):
		return {"error": "comparison world has no first-person arms witness"}
	var arms_value: Variant = world.call(
			"local_player_first_person_arms_witness")
	if not (arms_value is FirstPersonArmsWitness):
		return {"error": "comparison first-person arms witness is untyped"}
	var arms_witness: FirstPersonArmsWitness = arms_value
	if not arms_witness.is_valid():
		return {"error": "comparison first-person arms are invalid: %s" \
				% arms_witness.error}

	# Dictionary construction is confined to this evidence/JSON boundary. Every
	# value below is read from a typed production owner before it is compared
	# against the catalog contract.
	var canonical_observed := {
		"capture_mode": "hud_hidden",
		"equipped_weapon": weapon,
		"weapon_clip": int(spawn_witness.weapon_clip),
		"weapon_reserve": int(spawn_witness.weapon_reserve),
		"character_id": arms_witness.character_id,
		"arms_graphic": arms_witness.arms_graphic,
		"arms_camo": Array(arms_witness.arms_camo),
		"gameplay_hud_visible": bool(hud_witness_value.gameplay_hud_visible),
		"hud_canvas_layer_active": bool(hud_witness_value.hud_canvas_layer_active),
		"hud_detail_level": int(hud_witness_value.hud_detail_level),
		"player_view_effects_active": bool(
				hud_witness_value.player_view_effects_active),
		"viewmodel_enabled": viewmodel.visible,
		"terrain_enabled": (terrain_value as Node3D).is_visible_in_tree(),
		"ads_active": bool(hud_witness_value.ads_active),
		"big_map_active": bool(hud_witness_value.big_map_active),
		"player_pose_source": "retail_player_bms.applied",
	}
	for key: String in canonical_observed:
		if canonical_observed[key] != contract.get(key):
			return {"error": (
					"comparison presentation mismatch for %s: observed=%s expected=%s") \
					% [key, str(canonical_observed[key]), str(contract.get(key))]}
	observed.merge(canonical_observed, true)
	return observed


static func build_capture_provenance(
		source_commit: String,
		godot_executable_path: String,
		gdextension_path: String) -> Dictionary:
	if source_commit.length() != 40:
		return {"error": (
				"NOVA_EVIDENCE_SOURCE_COMMIT must be a full lowercase 40-character Git SHA")}
	for value: int in source_commit.to_ascii_buffer():
		if not (value >= 48 and value <= 57) \
				and not (value >= 97 and value <= 102):
			return {"error": (
					"NOVA_EVIDENCE_SOURCE_COMMIT must be a full lowercase 40-character Git SHA")}
	if not FileAccess.file_exists(godot_executable_path):
		return {"error": "Godot executable is not hashable: %s" % godot_executable_path}
	if not FileAccess.file_exists(gdextension_path):
		return {"error": "GDExtension binary is not hashable: %s" % gdextension_path}
	var godot_sha := FileAccess.get_sha256(godot_executable_path).to_lower()
	var extension_sha := FileAccess.get_sha256(gdextension_path).to_lower()
	if godot_sha.length() != 64 or extension_sha.length() != 64:
		return {"error": "capture binaries did not produce SHA-256 identities"}
	return {
		"source_commit": source_commit,
		"godot_executable_name": godot_executable_path.get_file(),
		"godot_executable_sha256": godot_sha,
		"gdextension_name": gdextension_path.get_file(),
		"gdextension_sha256": extension_sha,
	}


static func select_fixture_minutes(
		declared_value: Variant,
		minute_selector: String) -> Dictionary:
	if not (declared_value is Array) or (declared_value as Array).is_empty():
		return {"error": "fixture has no minutes_of_day"}
	var declared: Array = declared_value
	var values: Array = []
	for value: Variant in declared:
		var value_type := typeof(value)
		if value_type != TYPE_INT and value_type != TYPE_FLOAT:
			return {"error": "fixture minutes_of_day must contain integers from 0 to 1439"}
		var numeric := float(value)
		if not is_finite(numeric) or numeric != floorf(numeric) \
				or numeric < 0.0 or numeric >= 1440.0:
			return {"error": "fixture minutes_of_day must contain integers from 0 to 1439"}
		values.append(int(numeric))
	var selector := minute_selector.strip_edges()
	if selector.is_empty():
		return {"values": values}
	if not selector.is_valid_int():
		return {"error": "NOVA_RENDER_FIXTURE_MINUTE must be an integer selector"}
	var selected := selector.to_int()
	if not values.has(selected):
		return {"error": (
				"NOVA_RENDER_FIXTURE_MINUTE %d is not declared by this fixture"
				% selected)}
	return {"values": [selected]}


static func read_json(path: String) -> Dictionary:
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return {}
	var parsed: Variant = JSON.parse_string(bytes.get_string_from_utf8())
	return parsed if parsed is Dictionary else {}


static func sha256_bytes(bytes: PackedByteArray) -> String:
	var hash := HashingContext.new()
	if hash.start(HashingContext.HASH_SHA256) != OK:
		return ""
	hash.update(bytes)
	return hash.finish().hex_encode()


static func capture_source_state_contract_error(
		bundle: Dictionary,
		state: Dictionary,
		source_png: String) -> String:
	if String(state.get("schema", "")) != GameRenderCapture.SCHEMA \
			or String(bundle.get("schema", "")) != GameRenderCapture.SCHEMA:
		return "capture state has the wrong schema"
	var capture_value: Variant = state.get("capture")
	var state_diagnostics_value: Variant = state.get("diagnostics")
	var bundle_diagnostics_value: Variant = bundle.get("diagnostics")
	if not (capture_value is Dictionary) \
			or not (state_diagnostics_value is Dictionary) \
			or (state_diagnostics_value as Dictionary).is_empty() \
			or not (bundle_diagnostics_value is Dictionary) \
			or (bundle_diagnostics_value as Dictionary).is_empty():
		return "capture state requires nonempty capture diagnostics"
	var capture := capture_value as Dictionary
	var artifact: Dictionary = bundle.get("artifact", {})
	var capture_id := String(capture.get("id", ""))
	if capture_id.is_empty() \
			or capture_id != String(bundle.get("capture_id", "")) \
			or String(capture.get("label", "")).is_empty():
		return "capture state does not identify the captured bundle"
	var captured_png_path := String(capture.get("png_path", ""))
	if captured_png_path.is_empty() \
			or not same_absolute_path(captured_png_path, source_png):
		return "capture state does not identify the source PNG path"
	var width := int(artifact.get("width", 0))
	var height := int(artifact.get("height", 0))
	if width <= 0 or height <= 0 \
			or int(capture.get("width", 0)) != width \
			or int(capture.get("height", 0)) != height:
		return "capture state dimensions do not match the artifact"
	if String(artifact.get("mime", "")) != "image/png" \
			or String(capture.get("mime", "")) != "image/png":
		return "capture state does not describe a PNG artifact"
	var sanitized_diagnostics: Variant = McpJson.sanitize(
			bundle_diagnostics_value)
	# Compare at the JSON wire boundary: JSON.parse_string materializes numeric
	# values uniformly, while the live bundle still distinguishes int and float.
	var normalized_diagnostics: Variant = JSON.parse_string(
			JSON.stringify(sanitized_diagnostics))
	if normalized_diagnostics != state_diagnostics_value:
		return "capture state diagnostics do not match the captured bundle"
	var diagnostic_frame_value: Variant = \
			(state_diagnostics_value as Dictionary).get("frame")
	if not capture.has("process_frame") \
			or int(capture.process_frame) < 0 \
			or not (diagnostic_frame_value is Dictionary) \
			or int((diagnostic_frame_value as Dictionary).get("process", -1)) \
					!= int(capture.process_frame):
		return "capture state frame does not match its diagnostics"
	return ""


static func same_absolute_path(left: String, right: String) -> bool:
	var left_abs := ProjectSettings.globalize_path(left).simplify_path() \
			.replace("\\", "/")
	var right_abs := ProjectSettings.globalize_path(right).simplify_path() \
			.replace("\\", "/")
	if OS.get_name() == "Windows":
		left_abs = left_abs.to_lower()
		right_abs = right_abs.to_lower()
	return left_abs == right_abs


static func write_bytes(path: String, bytes: PackedByteArray) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_buffer(bytes)
	file.flush()
	var error := file.get_error()
	file.close()
	return error
