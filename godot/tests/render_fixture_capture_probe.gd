extends Node

## Exact-pose, production-world capture harness for docs/render/render-fixtures-v1.json.

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")
const FirstPersonArmsWitness := preload(
		"res://game/world/first_person_arms_witness.gd")
const CaptureVariant := preload(
		"res://tests/support/render_capture_variant.gd")
const ShadowAttributionCaptureSession := preload(
		"res://tests/support/shadow_attribution_capture_session.gd")

const DEFAULT_CATALOG := "res://../docs/render/render-fixtures-v1.json"
const DEFAULT_OUTPUT_ROOT := "res://../.scratch/golden/render/fixtures"
const DEFAULT_SHADOW_ATTRIBUTION_OUTPUT_ROOT := \
		"res://../.scratch/golden/render/shadow-attribution"
const CAPTURE_PROFILE_CANONICAL := "canonical"
const CAPTURE_PROFILE_SHADOW_ATTRIBUTION := "shadow_attribution"
const DEFAULT_EXPANSION := "jox01"
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
# Upper bound on the frames spent waiting for the first-person weapon's idle
# clip to reach its hold before the pose is frozen (see _settle_viewmodel_hold).
const VIEWMODEL_HOLD_MAX_FRAMES := 240
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
const PUBLICATION_TRANSACTION_SUFFIX := ".publication-transaction"
const PUBLICATION_OWNER_RECORD := "owner.json"
const PUBLICATION_INSTALL_RECORD := "installing.json"
const PUBLICATION_INSTALLED_RECORD := "installed.json"
const PUBLICATION_ROLLBACK_RECORD := "rolling-back.json"
const PUBLICATION_STAGING_DIR := "staging"
const PUBLICATION_BACKUP_DIR := "previous"
const PUBLICATION_CREATING_SEGMENT := ".creating."
const PUBLICATION_RECOVERING_SEGMENT := ".recovering."
const PUBLICATION_TRANSACTION_SCHEMA := \
		"opennova.fixture-publication-transaction.v1"


class StaticTerrainShadowWarmupSuspension:
	extends RefCounted

	var _terrain: Object = null
	var _original_enabled := false
	var _active := false


	func begin(terrain: Object) -> Error:
		if _active:
			return ERR_ALREADY_IN_USE
		if terrain == null or not is_instance_valid(terrain) \
				or not terrain.has_method("is_static_terrain_shadow_enabled") \
				or not terrain.has_method("set_static_terrain_shadow_enabled"):
			return ERR_UNCONFIGURED
		_terrain = terrain
		_original_enabled = bool(terrain.call(
				"is_static_terrain_shadow_enabled"))
		_active = true
		if _original_enabled:
			terrain.call("set_static_terrain_shadow_enabled", false)
		return OK


	func finish() -> void:
		if not _active:
			return
		var terrain := _terrain
		var original_enabled := _original_enabled
		_terrain = null
		_original_enabled = false
		_active = false
		if terrain != null and is_instance_valid(terrain) \
				and terrain.has_method("is_static_terrain_shadow_enabled") \
				and terrain.has_method("set_static_terrain_shadow_enabled") \
				and bool(terrain.call("is_static_terrain_shadow_enabled")) \
				!= original_enabled:
			terrain.call("set_static_terrain_shadow_enabled", original_enabled)


var _game: Node
var _world: GameWorld
var _capture_size := DEFAULT_CAPTURE_SIZE
var _expected_mission_time_fixed24 := -1
var _failed := false
var _shutdown_started := false
var _shadow_capture_session
var _static_shadow_warmup_suspension: StaticTerrainShadowWarmupSuspension
var _fixture_publication_staging_abs := ""


static func mission_to_godot(position: Vector3) -> Vector3:
	return Vector3(position.x, position.z, -position.y)


static func godot_to_mission(position: Vector3) -> Vector3:
	return Vector3(position.x, -position.z, position.y)


static func camera_basis(yaw_deg: float, pitch_deg: float) -> Basis:
	return Basis(Vector3.UP, deg_to_rad(-yaw_deg)) \
			* Basis(Vector3.RIGHT, deg_to_rad(pitch_deg))


static func fixture_by_id(catalog: Dictionary, id: String) -> Dictionary:
	if not publication_name_is_canonical(id):
		return {}
	var fixtures: Variant = catalog.get("fixtures", [])
	if not (fixtures is Array):
		return {}
	for value: Variant in fixtures:
		if value is Dictionary and String(value.get("id", "")) == id:
			return (value as Dictionary).duplicate(true)
	return {}


static func publication_name_is_canonical(value: String) -> bool:
	if value.is_empty() or value.length() > 180:
		return false
	var bytes := value.to_ascii_buffer()
	for code: int in bytes:
		var alpha_numeric := (code >= 48 and code <= 57) \
				or (code >= 65 and code <= 90) \
				or (code >= 97 and code <= 122)
		if not alpha_numeric and code != 45 and code != 95:
			return false
	var first := int(bytes[0])
	var last := int(bytes[bytes.size() - 1])
	return ((first >= 48 and first <= 57) \
			or (first >= 65 and first <= 90) \
			or (first >= 97 and first <= 122)) \
			and ((last >= 48 and last <= 57) \
			or (last >= 65 and last <= 90) \
			or (last >= 97 and last <= 122))


static func publication_child_path(output_abs: String, filename: String) -> String:
	var output := output_abs.simplify_path()
	if output.is_empty() or not output.is_absolute_path() \
			or filename.is_empty() or filename in [".", ".."] \
			or filename.contains("/") or filename.contains("\\") \
			or filename.contains(":"):
		return ""
	var child := output.path_join(filename).simplify_path()
	if not _same_absolute_path(child.get_base_dir(), output):
		return ""
	return child


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
	return _path_is_strict_descendant(output_path, scratch_root)


static func begin_fixture_publication(
		output_abs: String,
		trusted_root_abs: String,
		owner_is_running: Callable = Callable(),
		after_recovery_claim: Callable = Callable()) -> Dictionary:
	var output := _normalized_publication_path(output_abs)
	var trusted_root := _normalized_publication_path(trusted_root_abs)
	if output.is_empty() or trusted_root.is_empty() \
			or not output.is_absolute_path() \
			or not trusted_root.is_absolute_path() \
			or not _path_is_strict_descendant(output, trusted_root):
		return {"error": (
				"fixture publication output must be a strict descendant of its trusted root")}
	if not _publication_path_has_safe_ancestors(output, trusted_root):
		return {"error": "fixture publication output has an unsafe ancestor: %s" \
				% output}
	var parent_error := DirAccess.make_dir_recursive_absolute(
			output.get_base_dir())
	if parent_error != OK:
		return {"error": "cannot create fixture publication parent: %s" \
				% error_string(parent_error)}
	if not _publication_path_has_safe_ancestors(output, trusted_root):
		return {"error": "fixture publication output has an unsafe ancestor: %s" \
				% output}
	if FileAccess.file_exists(output):
		return {"error": "fixture publication output is a file: %s" % output}

	var transaction_root := _publication_transaction_root(output)
	if not _publication_path_has_safe_ancestors(
			transaction_root, trusted_root):
		return {"error": "fixture publication journal has an unsafe ancestor: %s" \
				% transaction_root}
	var recovery_error := _recover_abandoned_publication(
			output, trusted_root, owner_is_running, after_recovery_claim)
	if recovery_error != OK:
		return {"error": "cannot recover prior fixture publication: %s" \
				% error_string(recovery_error)}
	return _create_fixture_publication(output, trusted_root)


static func _create_fixture_publication(
		output: String, trusted_root: String) -> Dictionary:
	var transaction_root := _publication_transaction_root(output)
	var reservation := "%s%s%d.%d" % [
		transaction_root, PUBLICATION_CREATING_SEGMENT,
		OS.get_process_id(), Time.get_ticks_usec(),
	]
	if not _publication_path_has_safe_ancestors(reservation, trusted_root):
		return {"error": "fixture publication reservation has an unsafe ancestor"}
	var reservation_error := DirAccess.make_dir_absolute(reservation)
	if reservation_error != OK:
		return {"error": "cannot reserve fixture publication journal: %s" \
				% error_string(reservation_error)}
	if not _publication_path_has_safe_ancestors(reservation, trusted_root):
		return {"error": "fixture publication reservation became unsafe"}
	var owner_error := _write_new_atomic_json(
			reservation.path_join(PUBLICATION_OWNER_RECORD), {
				"schema": PUBLICATION_TRANSACTION_SCHEMA,
				"output": output,
				"trusted_root": trusted_root,
				"owner_pid": OS.get_process_id(),
			})
	if owner_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot write fixture publication journal: %s" \
				% error_string(owner_error)}
	var reserved_staging := reservation.path_join(PUBLICATION_STAGING_DIR)
	if not _publication_path_has_safe_ancestors(
			reserved_staging, trusted_root):
		return {"error": "fixture publication staging reservation became unsafe"}
	var staging_error := DirAccess.make_dir_absolute(reserved_staging)
	if staging_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot create fixture publication staging path: %s" \
				% error_string(staging_error)}
	if not _publication_path_has_safe_ancestors(
			reserved_staging, trusted_root):
		return {"error": "fixture publication staging reservation became unsafe"}

	# A complete owner/staging journal becomes visible atomically. No competitor
	# can mistake the short construction window for a dead or corrupt journal.
	var claims := _publication_recovery_claims(output, trusted_root)
	if claims.has("error") or not (claims.get("paths", []) as Array).is_empty() \
			or DirAccess.dir_exists_absolute(transaction_root) \
			or FileAccess.file_exists(transaction_root):
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "fixture publication is busy"}
	var publish_error := DirAccess.rename_absolute(reservation, transaction_root)
	if publish_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot publish fixture transaction owner: %s" \
				% error_string(publish_error)}
	claims = _publication_recovery_claims(output, trusted_root)
	if claims.has("error") or not (claims.get("paths", []) as Array).is_empty():
		_recover_fixture_publication(
				output, transaction_root, trusted_root)
		return {"error": "fixture publication recovery is busy"}
	var staging := transaction_root.path_join(PUBLICATION_STAGING_DIR)
	if not _publication_path_has_safe_ancestors(staging, trusted_root):
		_recover_fixture_publication(
				output, transaction_root, trusted_root)
		return {"error": "fixture publication staging has an unsafe ancestor"}
	return {"staging_path": staging}


static func commit_fixture_publication(
		staging_abs: String,
		output_abs: String,
		install_rename: Callable = Callable(),
		restore_rename: Callable = Callable(),
		installed_marker_write: Callable = Callable(),
		cleanup_transaction: Callable = Callable()) -> Error:
	var staging := _normalized_publication_path(staging_abs)
	var output := _normalized_publication_path(output_abs)
	var transaction_root := _publication_transaction_root(output)
	if staging.is_empty() or output.is_empty() \
			or not output.is_absolute_path() \
			or staging != transaction_root.path_join(PUBLICATION_STAGING_DIR):
		return ERR_INVALID_PARAMETER
	var owner := _read_publication_owner(transaction_root, output)
	if owner.is_empty() or int(owner.get("owner_pid", -1)) != OS.get_process_id():
		return ERR_UNAUTHORIZED
	var trusted_root := String(owner.get("trusted_root", ""))
	for path in [output, transaction_root, staging]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	if not DirAccess.dir_exists_absolute(staging):
		return ERR_DOES_NOT_EXIST
	if FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALL_RECORD)) \
			or FileAccess.file_exists(transaction_root.path_join(
					PUBLICATION_INSTALLED_RECORD)):
		return ERR_ALREADY_IN_USE
	if FileAccess.file_exists(output):
		return ERR_ALREADY_EXISTS

	var backup := transaction_root.path_join(PUBLICATION_BACKUP_DIR)
	if not _publication_path_has_safe_ancestors(backup, trusted_root) \
			or DirAccess.dir_exists_absolute(backup) \
			or FileAccess.file_exists(backup):
		return ERR_ALREADY_EXISTS
	var had_output := DirAccess.dir_exists_absolute(output)
	var install_record_error := _write_new_atomic_json(
			transaction_root.path_join(PUBLICATION_INSTALL_RECORD), {
				"had_output": had_output,
			})
	if install_record_error != OK:
		return install_record_error
	if had_output:
		# Re-check the root immediately before the first rename. A directory link or
		# junction is never renamed into the owned journal and never traversed.
		if not _publication_path_has_safe_ancestors(output, trusted_root) \
				or not _publication_path_has_safe_ancestors(
						backup, trusted_root):
			return ERR_INVALID_PARAMETER
		var backup_error := DirAccess.rename_absolute(output, backup)
		if backup_error != OK:
			return backup_error

	if not _publication_path_has_safe_ancestors(staging, trusted_root) \
			or not _publication_path_has_safe_ancestors(output, trusted_root):
		return ERR_INVALID_PARAMETER
	var install_error := _rename_absolute_with(
			staging, output, install_rename)
	if install_error == OK and (not DirAccess.dir_exists_absolute(output) \
			or DirAccess.dir_exists_absolute(staging)):
		install_error = ERR_CANT_CREATE
	if install_error != OK:
		var rollback_error := _write_new_atomic_json(
				transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD), {
					"had_output": had_output,
				})
		if rollback_error != OK:
			return rollback_error
		var restore_error := _restore_prior_fixture_publication(
				output, backup, had_output, restore_rename, trusted_root)
		if restore_error != OK:
			return restore_error
		var cleanup_error := _cleanup_publication_transaction_with(
				transaction_root, output, trusted_root, cleanup_transaction)
		return cleanup_error if cleanup_error != OK else install_error

	var installed_error := _write_new_atomic_json_with(
			transaction_root.path_join(PUBLICATION_INSTALLED_RECORD), {
				"complete": true,
			}, installed_marker_write)
	if installed_error != OK:
		var rollback_error := _write_new_atomic_json(
				transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD), {
					"had_output": had_output,
				})
		if rollback_error != OK:
			return rollback_error
		var restore_error := _restore_prior_fixture_publication(
				output, backup, had_output, restore_rename, trusted_root)
		if restore_error != OK:
			return restore_error
		var cleanup_error := _cleanup_publication_transaction_with(
				transaction_root, output, trusted_root, cleanup_transaction)
		return cleanup_error if cleanup_error != OK else installed_error
	return _cleanup_publication_transaction_with(
			transaction_root, output, trusted_root, cleanup_transaction)


static func abort_fixture_publication(staging_abs: String) -> Error:
	var staging := _normalized_publication_path(staging_abs)
	if staging.is_empty() or staging.get_file() != PUBLICATION_STAGING_DIR:
		return ERR_INVALID_PARAMETER
	var transaction_root := staging.get_base_dir()
	if not transaction_root.ends_with(PUBLICATION_TRANSACTION_SUFFIX):
		return ERR_INVALID_PARAMETER
	var output := transaction_root.trim_suffix(PUBLICATION_TRANSACTION_SUFFIX)
	if not output.is_absolute_path():
		return ERR_INVALID_PARAMETER
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	var owner := _read_publication_owner(transaction_root, output)
	if owner.is_empty() or int(owner.get("owner_pid", -1)) != OS.get_process_id():
		return ERR_UNAUTHORIZED
	return _recover_fixture_publication(
			output, transaction_root, String(owner.trusted_root))


static func _publication_transaction_root(output_abs: String) -> String:
	return output_abs.simplify_path() + PUBLICATION_TRANSACTION_SUFFIX


static func _normalized_publication_path(path: String) -> String:
	return ProjectSettings.globalize_path(path).simplify_path() \
			.replace("\\", "/").trim_suffix("/")


static func _path_is_strict_descendant(path: String, root: String) -> bool:
	var normalized_path := _normalized_publication_path(path)
	var normalized_root := _normalized_publication_path(root)
	if normalized_path.is_empty() or normalized_root.is_empty():
		return false
	if OS.get_name() == "Windows":
		normalized_path = normalized_path.to_lower()
		normalized_root = normalized_root.to_lower()
	return normalized_path.begins_with(normalized_root + "/")


static func _publication_path_has_safe_ancestors(
		path: String, trusted_root: String, allow_root: bool = false) -> bool:
	var normalized_path := _normalized_publication_path(path)
	var normalized_root := _normalized_publication_path(trusted_root)
	if normalized_path.is_empty() or normalized_root.is_empty() \
			or not normalized_path.is_absolute_path() \
			or not normalized_root.is_absolute_path():
		return false
	var same_as_root := _same_absolute_path(normalized_path, normalized_root)
	if (not allow_root and same_as_root) \
			or (not same_as_root \
					and not _path_is_strict_descendant(
							normalized_path, normalized_root)):
		return false
	if not DirAccess.dir_exists_absolute(normalized_root) \
			or FileAccess.file_exists(normalized_root) \
			or _path_entry_is_link(normalized_root):
		return false
	if same_as_root:
		return true

	var relative := normalized_path.substr(normalized_root.length()).trim_prefix("/")
	var components := relative.split("/", false)
	var current := normalized_root
	for index in range(components.size()):
		current = current.path_join(components[index])
		# A dangling symlink/junction often reports neither file_exists nor
		# dir_exists. Ask the parent directory about the entry unconditionally.
		if _path_entry_is_link(current):
			return false
		var is_file := FileAccess.file_exists(current)
		if is_file and index + 1 < components.size():
			return false
	return true


static func _path_entry_is_link(path: String) -> bool:
	if path.is_empty() or path.get_file().is_empty():
		return false
	var parent := DirAccess.open(path.get_base_dir())
	return parent != null and parent.is_link(path.get_file())


static func _write_new_atomic_json(path: String, value: Dictionary) -> Error:
	if FileAccess.file_exists(path) or DirAccess.dir_exists_absolute(path):
		return ERR_ALREADY_EXISTS
	var temp := "%s.tmp.%d.%d" % [
			path, OS.get_process_id(), Time.get_ticks_usec()]
	var write_error := _write_bytes(
			temp, JSON.stringify(McpJson.sanitize(value), "\t").to_utf8_buffer())
	if write_error != OK:
		DirAccess.remove_absolute(temp)
		return write_error
	if FileAccess.file_exists(path) or DirAccess.dir_exists_absolute(path):
		DirAccess.remove_absolute(temp)
		return ERR_ALREADY_EXISTS
	var rename_error := DirAccess.rename_absolute(temp, path)
	if rename_error != OK:
		DirAccess.remove_absolute(temp)
	return rename_error


static func _write_new_atomic_json_with(
		path: String, value: Dictionary, operation: Callable) -> Error:
	if not operation.is_valid():
		return _write_new_atomic_json(path, value)
	var result: Variant = operation.call(path, value)
	return int(result) if result is int else ERR_INVALID_DATA


static func _read_publication_owner(
		transaction_root: String,
		output: String,
		expected_trusted_root: String = "") -> Dictionary:
	var path := transaction_root.path_join(PUBLICATION_OWNER_RECORD)
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (parsed is Dictionary):
		return {}
	var owner := parsed as Dictionary
	var trusted_root := String(owner.get("trusted_root", ""))
	if String(owner.get("schema", "")) != PUBLICATION_TRANSACTION_SCHEMA \
			or not _same_absolute_path(String(owner.get("output", "")), output) \
			or trusted_root.is_empty() \
			or not trusted_root.is_absolute_path() \
			or not _path_is_strict_descendant(output, trusted_root) \
			or (not expected_trusted_root.is_empty() \
					and not _same_absolute_path(
							trusted_root, expected_trusted_root)) \
			or int(owner.get("owner_pid", -1)) <= 0:
		return {}
	return owner


static func _publication_recovery_claims(
		output: String, trusted_root: String) -> Dictionary:
	var transaction_root := _publication_transaction_root(output)
	var parent := transaction_root.get_base_dir()
	if not _publication_path_has_safe_ancestors(
			parent, trusted_root, true):
		return {"error": ERR_INVALID_PARAMETER}
	var dir := DirAccess.open(parent)
	if dir == null:
		return {"error": ERR_CANT_OPEN}
	var prefix := transaction_root.get_file() + PUBLICATION_RECOVERING_SEGMENT
	var comparison_prefix := prefix.to_lower() if OS.get_name() == "Windows" \
			else prefix
	for file_name in dir.get_files():
		var comparison_name := file_name.to_lower() \
				if OS.get_name() == "Windows" else file_name
		if comparison_name.begins_with(comparison_prefix):
			return {"error": ERR_FILE_CORRUPT}
	var paths: Array = []
	for directory_name in dir.get_directories():
		var comparison_name := directory_name.to_lower() \
				if OS.get_name() == "Windows" else directory_name
		if not comparison_name.begins_with(comparison_prefix):
			continue
		var claim := parent.path_join(directory_name)
		if not _publication_path_has_safe_ancestors(claim, trusted_root):
			return {"error": ERR_INVALID_PARAMETER}
		paths.append(claim)
	paths.sort()
	return {"paths": paths}


static func _recovery_claim_pid(claim: String, output: String) -> int:
	var prefix := _publication_transaction_root(output) \
			+ PUBLICATION_RECOVERING_SEGMENT
	var claim_key := claim.to_lower() if OS.get_name() == "Windows" else claim
	var prefix_key := prefix.to_lower() if OS.get_name() == "Windows" else prefix
	if not claim_key.begins_with(prefix_key):
		return -1
	var suffix := claim.substr(prefix.length())
	var pid_token := suffix.get_slice(".", 0)
	return pid_token.to_int() if pid_token.is_valid_int() \
			and pid_token.to_int() > 0 else -1


static func _publication_owner_is_running(
		pid: int, owner_is_running: Callable) -> bool:
	return bool(owner_is_running.call(pid)) if owner_is_running.is_valid() \
			else OS.is_process_running(pid)


static func _publication_directory_is_empty(
		path: String, trusted_root: String) -> bool:
	if not _publication_path_has_safe_ancestors(path, trusted_root):
		return false
	var dir := DirAccess.open(path)
	return dir != null and dir.get_directories().is_empty() \
			and dir.get_files().is_empty()


static func _recover_abandoned_publication(
		output: String,
		trusted_root: String,
		owner_is_running: Callable,
		after_recovery_claim: Callable) -> Error:
	var claims := _publication_recovery_claims(output, trusted_root)
	if claims.has("error"):
		return int(claims.error)
	var claim_paths := claims.paths as Array
	if claim_paths.size() > 1:
		return ERR_FILE_CORRUPT
	if claim_paths.size() == 1:
		var existing_claim := String(claim_paths[0])
		var claimant_pid := _recovery_claim_pid(existing_claim, output)
		if claimant_pid <= 0:
			return ERR_FILE_CORRUPT
		if _publication_directory_is_empty(existing_claim, trusted_root):
			var remove_error := DirAccess.remove_absolute(existing_claim)
			return OK if remove_error in [OK, ERR_DOES_NOT_EXIST] \
					else remove_error
		if _publication_owner_is_running(claimant_pid, owner_is_running):
			return ERR_BUSY
		return _claim_and_recover_publication(
				existing_claim, output, trusted_root, claimant_pid,
				owner_is_running, after_recovery_claim)

	var transaction_root := _publication_transaction_root(output)
	if FileAccess.file_exists(transaction_root):
		return ERR_INVALID_DATA
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	if not _publication_path_has_safe_ancestors(
			transaction_root, trusted_root):
		return ERR_INVALID_PARAMETER
	if _publication_directory_is_empty(transaction_root, trusted_root):
		var remove_error := DirAccess.remove_absolute(transaction_root)
		return OK if remove_error in [OK, ERR_DOES_NOT_EXIST] else remove_error
	var owner := _read_publication_owner(
			transaction_root, output, trusted_root)
	if owner.is_empty():
		return ERR_FILE_CORRUPT
	if _publication_owner_is_running(int(owner.owner_pid), owner_is_running):
		return ERR_BUSY
	return _claim_and_recover_publication(
			transaction_root, output, trusted_root, -1,
			owner_is_running, after_recovery_claim)


static func _claim_and_recover_publication(
		source_root: String,
		output: String,
		trusted_root: String,
		source_claimant_pid: int,
		owner_is_running: Callable,
		after_recovery_claim: Callable) -> Error:
	var original_owner := _read_publication_owner(
			source_root, output, trusted_root)
	if original_owner.is_empty():
		return ERR_FILE_CORRUPT
	if source_claimant_pid > 0 and _publication_owner_is_running(
			source_claimant_pid, owner_is_running):
		return ERR_BUSY
	if _publication_owner_is_running(
			int(original_owner.owner_pid), owner_is_running):
		return ERR_BUSY

	var claimed_root := "%s%s%d.%d" % [
		_publication_transaction_root(output), PUBLICATION_RECOVERING_SEGMENT,
		OS.get_process_id(), Time.get_ticks_usec(),
	]
	if not _publication_path_has_safe_ancestors(claimed_root, trusted_root) \
			or DirAccess.dir_exists_absolute(claimed_root) \
			or FileAccess.file_exists(claimed_root):
		return ERR_ALREADY_EXISTS
	var claim_error := DirAccess.rename_absolute(source_root, claimed_root)
	if claim_error != OK:
		return ERR_BUSY
	if not _publication_path_has_safe_ancestors(claimed_root, trusted_root):
		return ERR_INVALID_PARAMETER
	var claimed_owner := _read_publication_owner(
			claimed_root, output, trusted_root)
	if claimed_owner.is_empty() \
			or int(claimed_owner.owner_pid) != int(original_owner.owner_pid):
		return ERR_FILE_CORRUPT

	# Liveness is deliberately checked again after the atomic rename. A process
	# that became live between preflight and claim retains ownership untouched.
	var prior_owner_became_live := _publication_owner_is_running(
			int(claimed_owner.owner_pid), owner_is_running)
	var prior_claimant_became_live := source_claimant_pid > 0 \
			and _publication_owner_is_running(
					source_claimant_pid, owner_is_running)
	if prior_owner_became_live or prior_claimant_became_live:
		var release_error := DirAccess.rename_absolute(claimed_root, source_root)
		return ERR_BUSY if release_error == OK else release_error
	if after_recovery_claim.is_valid():
		after_recovery_claim.call(claimed_root)
	return _recover_fixture_publication(
			output, claimed_root, trusted_root)


static func _recover_fixture_publication(
		output: String,
		transaction_root: String,
		trusted_root: String) -> Error:
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	for path in [output, transaction_root]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	var owner := _read_publication_owner(
			transaction_root, output, trusted_root)
	if owner.is_empty():
		return ERR_FILE_CORRUPT

	var staging := transaction_root.path_join(PUBLICATION_STAGING_DIR)
	var backup := transaction_root.path_join(PUBLICATION_BACKUP_DIR)
	for path in [staging, backup]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	var installing_path := transaction_root.path_join(PUBLICATION_INSTALL_RECORD)
	var installed := FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALLED_RECORD))
	var rollback_path := transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD)
	var rolling_back := FileAccess.file_exists(rollback_path)
	if installed and rolling_back:
		return ERR_FILE_CORRUPT
	if not FileAccess.file_exists(installing_path):
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)
	var install_value: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(installing_path))
	if not (install_value is Dictionary) \
			or not (install_value as Dictionary).has("had_output"):
		return ERR_FILE_CORRUPT
	var had_output := bool((install_value as Dictionary).had_output)
	if rolling_back:
		var rollback_value: Variant = JSON.parse_string(
				FileAccess.get_file_as_string(rollback_path))
		if not (rollback_value is Dictionary) \
				or not (rollback_value as Dictionary).has("had_output") \
				or bool((rollback_value as Dictionary).had_output) != had_output:
			return ERR_FILE_CORRUPT
	var output_exists := DirAccess.dir_exists_absolute(output)
	var backup_exists := DirAccess.dir_exists_absolute(backup)
	var staging_exists := DirAccess.dir_exists_absolute(staging)
	if FileAccess.file_exists(output) or FileAccess.file_exists(backup):
		return ERR_INVALID_DATA

	if installed:
		if not output_exists:
			if not backup_exists:
				return ERR_DOES_NOT_EXIST
			if not _publication_path_has_safe_ancestors(
					backup, trusted_root):
				return ERR_INVALID_PARAMETER
			var restore_error := DirAccess.rename_absolute(backup, output)
			if restore_error != OK:
				return restore_error
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)

	# No installed marker means the transaction never crossed its durable commit
	# point. Restore the prior complete fixture before this output can be reused.
	if backup_exists:
		if output_exists:
			var remove_error := _remove_tree_absolute(output, trusted_root)
			if remove_error != OK:
				return remove_error
		if not _publication_path_has_safe_ancestors(
				backup, trusted_root):
			return ERR_INVALID_PARAMETER
		var restore_error := DirAccess.rename_absolute(backup, output)
		if restore_error != OK:
			return restore_error
	elif had_output:
		# With staging still present, the backup rename never happened and output
		# remains the old complete fixture. Without either staging or backup, the
		# prior fixture cannot be proven recoverable, so fail closed.
		if not output_exists or (not staging_exists and not rolling_back):
			return ERR_FILE_CORRUPT
	elif output_exists:
		if staging_exists and not rolling_back:
			return ERR_BUSY
		var remove_error := _remove_tree_absolute(output, trusted_root)
		if remove_error != OK:
			return remove_error
	return _cleanup_publication_transaction(
			transaction_root, output, trusted_root)


static func _rename_absolute_with(
		source: String, target: String, operation: Callable) -> Error:
	if not operation.is_valid():
		return DirAccess.rename_absolute(source, target)
	var result: Variant = operation.call(source, target)
	return int(result) if result is int else ERR_INVALID_DATA


static func _restore_prior_fixture_publication(
		output: String,
		backup: String,
		had_output: bool,
		restore_rename: Callable,
		trusted_root: String) -> Error:
	for path in [output, backup]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	if FileAccess.file_exists(output) or FileAccess.file_exists(backup):
		return ERR_INVALID_DATA
	if DirAccess.dir_exists_absolute(output):
		var remove_error := _remove_tree_absolute(output, trusted_root)
		if remove_error != OK:
			return remove_error
	if not had_output:
		return OK
	if not DirAccess.dir_exists_absolute(backup):
		return ERR_DOES_NOT_EXIST
	if not _publication_path_has_safe_ancestors(backup, trusted_root):
		return ERR_INVALID_PARAMETER
	var restore_error := _rename_absolute_with(
			backup, output, restore_rename)
	if restore_error != OK:
		return restore_error
	if not DirAccess.dir_exists_absolute(output) \
			or DirAccess.dir_exists_absolute(backup):
		return ERR_CANT_CREATE
	return OK


static func _cleanup_publication_transaction_with(
		transaction_root: String,
		output: String,
		trusted_root: String,
		operation: Callable) -> Error:
	if not operation.is_valid():
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)
	var result: Variant = operation.call(
			transaction_root, output, trusted_root)
	return int(result) if result is int else ERR_INVALID_DATA


static func _cleanup_publication_transaction(
		transaction_root: String,
		output: String,
		trusted_root: String) -> Error:
	var canonical_root := _publication_transaction_root(output)
	var is_canonical := _same_absolute_path(transaction_root, canonical_root)
	var is_recovery_claim := transaction_root.begins_with(
			canonical_root + PUBLICATION_RECOVERING_SEGMENT)
	if transaction_root.is_empty() \
			or not transaction_root.is_absolute_path() \
			or (not is_canonical and not is_recovery_claim) \
			or not _publication_path_has_safe_ancestors(
					transaction_root, trusted_root):
		return ERR_INVALID_PARAMETER
	var dir := DirAccess.open(transaction_root)
	if dir == null:
		return OK if not DirAccess.dir_exists_absolute(transaction_root) \
				else ERR_CANT_OPEN
	var directories := dir.get_directories()
	for directory_name in directories:
		if directory_name not in [
			PUBLICATION_STAGING_DIR,
			PUBLICATION_BACKUP_DIR,
		]:
			return ERR_FILE_CORRUPT
	var files := dir.get_files()
	for file_name in files:
		var recognized := file_name == PUBLICATION_OWNER_RECORD \
				or file_name == PUBLICATION_INSTALL_RECORD \
				or file_name == PUBLICATION_INSTALLED_RECORD \
				or file_name == PUBLICATION_ROLLBACK_RECORD \
				or file_name.begins_with(PUBLICATION_OWNER_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_INSTALL_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_INSTALLED_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_ROLLBACK_RECORD + ".tmp.")
		if not recognized \
				or _path_entry_is_link(transaction_root.path_join(file_name)):
			return ERR_FILE_CORRUPT
	var committed := FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALLED_RECORD))

	# A rollback caller has already restored the prior fixture. Release its
	# installing marker before deleting staging so a crash cannot leave a state
	# that resembles a lost pre-commit output. A committed install keeps both
	# markers until its old/staged payloads are gone.
	if not committed:
		for file_name in files:
			if file_name == PUBLICATION_INSTALL_RECORD \
					or file_name.begins_with(
							PUBLICATION_INSTALL_RECORD + ".tmp."):
				var record_error := DirAccess.remove_absolute(
						transaction_root.path_join(file_name))
				if record_error != OK:
					return record_error

	# For a committed install, remove payload directories before either durable
	# marker. If cleanup is interrupted, owner.json and installed.json still
	# describe a state that the next begin_fixture_publication() can finish.
	for directory_name in [PUBLICATION_STAGING_DIR, PUBLICATION_BACKUP_DIR]:
		var child := transaction_root.path_join(directory_name)
		if _path_entry_is_link(child) or FileAccess.file_exists(child):
			return ERR_INVALID_DATA
		if DirAccess.dir_exists_absolute(child):
			var child_error := _remove_tree_absolute(child, trusted_root)
			if child_error != OK:
				return child_error

	dir = DirAccess.open(transaction_root)
	if dir == null:
		return ERR_CANT_OPEN
	if not dir.get_directories().is_empty():
		return ERR_FILE_CORRUPT

	# The owner record is the lock and is deliberately the final file removed.
	# The installed marker outlives the installing marker, so every intermediate
	# cleanup state either proves the new output committed or has no install
	# record and can simply finish cleanup while retaining that output.
	var remaining_records := [
		PUBLICATION_ROLLBACK_RECORD,
		PUBLICATION_INSTALLED_RECORD,
	]
	if committed:
		remaining_records.push_front(PUBLICATION_INSTALL_RECORD)
	for record_name in remaining_records:
		for file_name in files:
			if file_name == record_name \
					or file_name.begins_with(record_name + ".tmp."):
				var record_path := transaction_root.path_join(file_name)
				if not FileAccess.file_exists(record_path):
					continue
				var record_error := DirAccess.remove_absolute(
						record_path)
				if record_error != OK:
					return record_error
	for file_name in files:
		if file_name.begins_with(PUBLICATION_OWNER_RECORD + ".tmp."):
			var temp_error := DirAccess.remove_absolute(
					transaction_root.path_join(file_name))
			if temp_error != OK:
				return temp_error
	var owner_path := transaction_root.path_join(PUBLICATION_OWNER_RECORD)
	if FileAccess.file_exists(owner_path):
		var owner_error := DirAccess.remove_absolute(owner_path)
		if owner_error != OK:
			return owner_error

	dir = DirAccess.open(transaction_root)
	if dir == null:
		return ERR_CANT_OPEN
	if not dir.get_directories().is_empty() or not dir.get_files().is_empty():
		return ERR_FILE_CORRUPT
	return DirAccess.remove_absolute(transaction_root)


static func _remove_tree_absolute(
		path: String, trusted_root: String = "") -> Error:
	if path.is_empty() or not path.is_absolute_path():
		return ERR_INVALID_PARAMETER
	if (not trusted_root.is_empty() \
			and not _publication_path_has_safe_ancestors(path, trusted_root)) \
			or _path_entry_is_link(path):
		return ERR_INVALID_PARAMETER
	if FileAccess.file_exists(path):
		return DirAccess.remove_absolute(path)
	var dir := DirAccess.open(path)
	if dir == null:
		return OK if not DirAccess.dir_exists_absolute(path) else ERR_CANT_OPEN
	for name in dir.get_files():
		var file_error := DirAccess.remove_absolute(path.path_join(name))
		if file_error != OK:
			return file_error
	for name in dir.get_directories():
		var child := path.path_join(name)
		var child_error := DirAccess.remove_absolute(child) \
				if dir.is_link(name) else _remove_tree_absolute(child, trusted_root)
		if child_error != OK:
			return child_error
	return DirAccess.remove_absolute(path)


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
	var kit: Array = [{
		"name": weapon,
		"ammo_primary": COMPARISON_PRIMARY_CLIPS,
		"ammo_secondary": -1,
		"flags": -1,
	}]
	if not bool(sim_object.call(
			"apply_local_player_loadout", kit, soldier_class)):
		return {"error": "production Armory apply rejected comparison weapon %s" % weapon}
	var inventory_value: Variant = sim_object.call("get_local_player_inventory")
	if not (inventory_value is Dictionary):
		return {"error": "production Armory apply produced no inventory witness"}
	var inventory := inventory_value as Dictionary
	var equipped := String(inventory.get("equipped_name", ""))
	if not bool(inventory.get("valid", false)) or equipped != weapon:
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
	if not (inventory_value is Dictionary):
		return {"error": "comparison spawn produced no inventory witness"}
	var inventory := inventory_value as Dictionary
	var equipped := String(inventory.get("equipped_name", ""))
	var sim_weapon := String(sim_object.call("get_local_player_weapon_name"))
	if not bool(inventory.get("valid", false)) or equipped != expected_weapon \
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
	if not (weapon_state_value is Dictionary):
		return {"error": "comparison spawn produced no weapon-state witness"}
	var weapon_state := weapon_state_value as Dictionary
	var weapon_clip := int(weapon_state.get("clip", -1))
	var weapon_reserve := int(weapon_state.get("reserve", -1))
	if not bool(weapon_state.get("active", false)) \
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
	var viewmodel: CanvasLayer = (viewport as Node).get_node_or_null(
			"ViewmodelPass") as CanvasLayer if viewport is Node else null
	if viewmodel == null or not viewmodel.visible:
		return {"error": "comparison ViewmodelPass CanvasLayer is absent or hidden"}
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
			"fps_counter_visible": hud_witness.fps_counter_visible,
		}
	for key in [
		"hud_detail_level", "gameplay_hud_visible",
		"player_view_effects_active", "ads_active", "big_map_active",
		"hud_canvas_layer_active",
		"fps_counter_visible",
	]:
		if not hud_witness_value.has(key):
			return {"error": "captured HUD-hidden witness is missing %s" % key}
	if bool(hud_witness_value.fps_counter_visible):
		return {"error": "captured FPS counter is visible"}
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


func _ready() -> void:
	var fixture_id := OS.get_environment("NOVA_RENDER_FIXTURE_ID").strip_edges()
	if fixture_id.is_empty():
		_fail("set NOVA_RENDER_FIXTURE_ID to one catalog fixture")
		return
	var catalog_path := OS.get_environment(
			"NOVA_RENDER_FIXTURE_CATALOG").strip_edges()
	if catalog_path.is_empty():
		catalog_path = DEFAULT_CATALOG
	var catalog := _read_json(catalog_path)
	if catalog.is_empty():
		_fail("could not parse fixture catalog: %s" % catalog_path)
		return
	if not diagnostic_variant_contract_matches(
			catalog.get("diagnostic_variants")):
		_fail("catalog diagnostic_variants does not exactly match the capture driver")
		return
	var capture_profile := select_capture_profile(
			OS.get_environment("NOVA_RENDER_CAPTURE_PROFILE"),
			OS.get_environment("NOVA_RENDER_STATIC_SHADOW_SUPPRESS_BMS_IDS"))
	if capture_profile.has("error"):
		_fail(String(capture_profile.error))
		return
	var fixture := fixture_by_id(catalog, fixture_id)
	if fixture.is_empty():
		_fail("fixture '%s' is not present in %s" % [fixture_id, catalog_path])
		return
	var capture_mode := select_capture_mode(
			fixture, OS.get_environment("NOVA_RENDER_CAPTURE_MODE"))
	if capture_mode.has("error"):
		_fail("fixture %s: %s" % [fixture_id, String(capture_mode.error)])
		return
	var world_only := bool(capture_mode.world_only)
	var catalog_contract: Variant = catalog.get("comparison_contract")
	var comparison_contract: Dictionary = {}
	if catalog.has("comparison_contract") or not world_only:
		comparison_contract = parse_comparison_contract(fixture, catalog_contract)
		if comparison_contract.has("error"):
			_fail("fixture %s: %s" % [
				fixture_id, String(comparison_contract.error)])
			return
	var gdextension_path := OS.get_environment(
			"NOVA_GDEXTENSION_BINARY").strip_edges()
	if gdextension_path.is_empty():
		_fail("set NOVA_GDEXTENSION_BINARY to the exact loaded extension binary")
		return
	var provenance := build_capture_provenance(
			OS.get_environment("NOVA_EVIDENCE_SOURCE_COMMIT").strip_edges(),
			OS.get_executable_path(), gdextension_path)
	if provenance.has("error"):
		_fail(String(provenance.error))
		return
	var mission_name := String(fixture.get("mission", ""))
	var mission_root := OS.get_environment("NOVA_MISSION_RESOURCE_DIR").strip_edges()
	if mission_root.is_empty():
		mission_root = ResourceDirSettings.get_resource_dir()
	var runtime_root := OS.get_environment("NOVA_RUNTIME_RESOURCE_DIR").strip_edges()
	var expansion := OS.get_environment("NOVA_EXPANSION").strip_edges()
	if expansion.is_empty():
		expansion = DEFAULT_EXPANSION
	if not ResourceDirSettings.is_valid_root(mission_root):
		_fail("invalid loose mission root: %s" % mission_root)
		return
	if not ResourceRoot.is_valid_root(runtime_root):
		_fail("invalid packed runtime root: %s" % runtime_root)
		return
	var mission_path := Paths.resolve_file(mission_root, mission_name)
	if mission_path.is_empty():
		_fail("%s is absent from loose mission root %s" % [mission_name, mission_root])
		return
	var expected_hash := String(catalog.get("missions", {}).get(mission_name, ""))
	var actual_hash := FileAccess.get_sha256(mission_path).to_lower()
	if expected_hash.is_empty() or actual_hash != expected_hash.to_lower():
		_fail("mission hash mismatch for %s: expected=%s actual=%s" % [
			mission_name, expected_hash, actual_hash])
		return

	var output_root := OS.get_environment("NOVA_RENDER_FIXTURE_OUTPUT").strip_edges()
	if output_root.is_empty():
		output_root = (DEFAULT_SHADOW_ATTRIBUTION_OUTPUT_ROOT \
				if String(capture_profile.id) \
						== CAPTURE_PROFILE_SHADOW_ATTRIBUTION \
				else DEFAULT_OUTPUT_ROOT).path_join(fixture_id)
	var output_abs := ProjectSettings.globalize_path(output_root).simplify_path()
	var scratch_abs := ProjectSettings.globalize_path(
			"res://../.scratch").simplify_path()
	var scratch_error := DirAccess.make_dir_recursive_absolute(scratch_abs)
	if scratch_error != OK:
		_fail("cannot create trusted capture scratch root: %s" \
				% error_string(scratch_error))
		return
	if not capture_output_is_allowed(capture_profile, output_abs, scratch_abs):
		_fail("capture profile %s output must be a strict descendant of %s" % [
				String(capture_profile.id), scratch_abs])
		return
	var publication := begin_fixture_publication(output_abs, scratch_abs)
	if publication.has("error"):
		_fail(String(publication.error))
		return
	var publication_abs := String(publication.staging_path)
	_fixture_publication_staging_abs = publication_abs

	var resolution := parse_capture_resolution(catalog.get("capture", {}))
	if resolution.has("error"):
		_fail(String(resolution.error))
		return
	_capture_size = resolution.size
	get_window().mode = Window.MODE_WINDOWED
	get_window().size = _capture_size
	var local_player_profile := comparison_spawn_profile(comparison_contract) \
			if not comparison_contract.is_empty() else {}
	var session: Dictionary = await StandaloneProbe.boot(
			self, runtime_root, mission_name, expansion, mission_path,
			local_player_profile)
	if not String(session.get("error", "")).is_empty():
		_fail(String(session.error))
		return
	_game = session.game
	_world = session.world
	var camera := session.camera as Camera3D
	var viewport := session.viewport as Viewport
	if camera == null or viewport == null or _world == null or not _world.is_loaded():
		_fail("production world/camera/viewport did not become capturable")
		return
	var probe_terrain: Terrain = _world.get_terrain_node()
	if probe_terrain != null:
		# Captures assert on the byte-level page hash/diff diagnostics that
		# steady-state play leaves off.
		probe_terrain.set_tile_cache_capture_diagnostics(true)
	_static_shadow_warmup_suspension = \
			StaticTerrainShadowWarmupSuspension.new()
	var warmup_shadow_error: Error = \
			_static_shadow_warmup_suspension.begin(probe_terrain)
	if warmup_shadow_error != OK:
		_fail("could not suspend static terrain shadow projection during live " \
				+ "capture warmup: %s" % error_string(warmup_shadow_error))
		return
	var capture_settings: Dictionary = catalog.get("capture", {})
	await _settle(int(capture_settings.get("load_settle_frames", 132)))
	if Vector2i(viewport.get_visible_rect().size) != _capture_size:
		_fail("game viewport is %s, expected %s" % [
			str(Vector2i(viewport.get_visible_rect().size)), str(_capture_size)])
		return
	var runtime := _world.get_runtime()
	var gameplay_input_active := bool(_game.is_gameplay_input_active())
	var post_spawn_state := {
		"entry": "single_player_auto_spawn_after_splash",
		"observed_at": "before_fixture_freeze",
		"world_loaded": _world.is_loaded(),
		"world_loading": bool(_game.is_world_loading()),
		"runtime_playing": runtime != null and runtime.is_playing(),
		"local_player_spawned": runtime != null and runtime.has_player(),
		"gameplay_input_active": gameplay_input_active,
		"gameplay_camera_current": camera.is_current()
				and viewport.get_camera_3d() == camera,
		# SP has no deploy-choice screen: its production-equivalent proof is the
		# shell's WORLD input gate plus the auto-spawned local player above.
		"spawn_or_menu_active": not gameplay_input_active,
	}
	if not post_spawn_capture_state_matches(post_spawn_state):
		_fail("production shell did not reach the post-spawn gameplay state: %s" \
				% JSON.stringify(post_spawn_state))
		return
	if not comparison_contract.is_empty():
		var spawn_witness := verify_comparison_spawn(_world, comparison_contract)
		if spawn_witness.has("error"):
			# Some missions carry an explicit spawn kit which correctly outranks the
			# PLAYER_INFO profile. For matched evidence, run the same public Armory
			# ACCEPT chain used in play, then require all sim and presentation
			# witnesses to converge before capture.
			var fallback := apply_comparison_weapon_fallback(
					_game, _world, comparison_contract)
			if fallback.has("error"):
				_fail("fixture %s: %s; fallback failed: %s" % [
						fixture_id, String(spawn_witness.error),
						String(fallback.error)])
				return
			await _settle(int(capture_settings.get(
					"visibility_settle_frames", 3)))
			spawn_witness = verify_comparison_spawn(_world, comparison_contract)
			if spawn_witness.has("error"):
				_fail("fixture %s: Armory fallback did not converge: %s" % [
						fixture_id, String(spawn_witness.error)])
				return
		# The Armory re-mount restarts the first-person weapon's idle clip. Retail
		# holds a non-looping idle's LAST frame once it has played out (the FP
		# channel plays the clip once and parks; the hip pose the registered retail
		# frames settle into is that hold), so capturing mid-clip bakes a random
		# phase -- up to ~1 cm of gun travel on the M4 clip set -- into the
		# evidence. Let the hold establish before the pose is frozen.
		await _settle_viewmodel_hold(VIEWMODEL_HOLD_MAX_FRAMES)

	# ViewmodelPass is a sibling of MainGame under the gameplay viewport, not a
	# descendant of MainGame. Query the same ownership seam used by
	# mcp_begin_world_only_capture() so the manifest reports the real capture
	# precondition rather than a scene-tree approximation.
	var has_viewmodel_pass := viewport.get_node_or_null("ViewmodelPass") != null
	var camera_spec: Dictionary = fixture.get("camera_bms", {})
	var raw_position: Array = camera_spec.get("position", [])
	if raw_position.size() != 3:
		_fail("fixture %s has no three-component camera_bms.position" % fixture_id)
		return
	var desired_position := mission_to_godot(Vector3(
			float(raw_position[0]), float(raw_position[1]), float(raw_position[2])))
	var yaw_deg := float(camera_spec.get("yaw_deg", 0.0))
	var pitch_deg := float(camera_spec.get("pitch_deg", 0.0))
	var fov_deg := float(camera_spec.get(
			"vertical_fov_deg", capture_settings.get("vertical_fov_deg", 50.534)))
	var minute_selection := select_fixture_minutes(
			fixture.get("minutes_of_day", []),
			OS.get_environment("NOVA_RENDER_FIXTURE_MINUTE"))
	if minute_selection.has("error"):
		_fail("fixture %s: %s" % [fixture_id, String(minute_selection.error)])
		return
	var minute_values: Array = minute_selection.values

	var manifest := {
		"schema": "opennova.render-fixture-captures.v1",
		"catalog_schema": String(catalog.get("schema", "")),
		"catalog_sha256": String(catalog.get("catalog_sha256", "")),
		"fixture": fixture,
		"mission": {
			"file": mission_name,
			"sha256": actual_hash,
			"source_kind": "loose_authoring_file",
			"runtime_mount": "packed_game_install",
			"expansion": expansion,
		},
		"capture": {
			"resolution": [_capture_size.x, _capture_size.y],
			"mode": String(capture_mode.mode),
			"world_only": world_only,
			"viewmodel_hidden": has_viewmodel_pass and world_only,
			"post_spawn": post_spawn_state,
			"phase_contract": capture_phase_contract(),
			"comparison_contract": (catalog_contract as Dictionary).duplicate(true) \
					if catalog_contract is Dictionary else {},
			"camera_bms": {
				"position": raw_position.duplicate(),
				"yaw_deg": yaw_deg,
				"pitch_deg": pitch_deg,
				"vertical_fov_deg": fov_deg,
			},
			"camera_position_godot": desired_position,
			"camera_basis": camera_basis(yaw_deg, pitch_deg),
			"fov_deg": fov_deg,
		},
		"provenance": provenance,
		"artifacts": [],
	}
	if bool(capture_profile.scratch_only):
		(manifest["capture"] as Dictionary)["diagnostic_profile"] = \
				String(capture_profile.id)
	var adapter: GameDebugAdapter = _game.get_game_debug_adapter()
	for minute_value: Variant in minute_values:
		if not await _prepare_pose(
				camera, desired_position, yaw_deg, pitch_deg, fov_deg,
					float(minute_value), int(capture_settings.get(
							"visibility_settle_frames", 3)), comparison_contract):
			return
		_shadow_capture_session = ShadowAttributionCaptureSession.new()
		var shadow_session_error: Error = _shadow_capture_session.begin(
				_world, viewport)
		if shadow_session_error != OK:
			_fail("fixture %s: could not begin shadow capture session: %s" % [
					fixture_id, error_string(shadow_session_error)])
			return
		var presentation_mode := String(capture_mode.mode)
		if bool(capture_profile.scratch_only) \
				and not (manifest["capture"] as Dictionary).has(
						"shadow_attribution"):
			var dynamic_caster_inventory: Array = []
			for caster in _shadow_capture_session.get_dynamic_caster_inventory():
				dynamic_caster_inventory.append(caster.to_json_value())
			var static_caster_inventory: Array = []
			for caster in _shadow_capture_session.get_static_caster_inventory():
				static_caster_inventory.append(caster.to_json_value())
			(manifest["capture"] as Dictionary)["shadow_attribution"] = {
				"scratch_only": true,
				"dynamic_caster_inventory": dynamic_caster_inventory,
				"static_caster_inventory": static_caster_inventory,
			}
		var capture_rows: Array = manifest["artifacts"]
		for variant in capture_profile.variants:
			var variant_error: Error = _shadow_capture_session.apply_variant(variant)
			if variant_error != OK:
				_fail("fixture %s: could not apply capture variant %s: %s" % [
						fixture_id, String(variant.id), error_string(variant_error)])
				return
			# _prepare_pose freezes GameWorld after its exact-camera refresh. Process
			# frames alone cannot run Terrain while that owner is disabled, so every
			# shadow control transition needs the same explicitly non-time-owning
			# refresh before the adapter snapshots renderer state or pixels.
			var realization: Dictionary = await _realize_capture_variant_cache(
					variant, camera)
			if realization.has("error"):
				_fail("fixture %s: capture variant %s: %s" % [fixture_id,
						String(variant.id), String(realization.error)])
				return
			var tile_cache_diagnostics: Dictionary = realization.diagnostics
			var label := "%s-m%04d-%s" % [
				fixture_id, int(minute_value), String(variant.id)]
			var result: Variant = await adapter.capture_mcp_render_bundle({
				"label": label,
				"settle_frames": int(capture_settings.get(
						"visibility_settle_frames", 3)),
				"world_only": world_only,
				"presentation_mode": presentation_mode,
			})
			if not (result is Dictionary) or result.has("error"):
				_fail("%s capture failed: %s" % [
					label, String(result.get("error", "invalid capture result")) \
					if result is Dictionary else "invalid capture result"])
				return
			var result_dict := result as Dictionary
			var comparison_witness: Dictionary = {}
			if not comparison_contract.is_empty():
				var diagnostics: Dictionary = result_dict.get("diagnostics", {})
				var presentation: Dictionary = diagnostics.get("presentation", {})
				var captured_hud: Dictionary = presentation.get(
						"hud_hidden_capture", {})
				comparison_witness = observe_comparison_contract(
						_game, _world, viewport, comparison_contract, captured_hud)
				if comparison_witness.has("error"):
					_fail("fixture %s: %s" % [
							fixture_id, String(comparison_witness.error)])
					return
				(manifest["capture"] as Dictionary)[
						"comparison_contract_witness"] = comparison_witness
			if not comparison_witness.is_empty():
				result_dict["comparison_contract_witness"] = comparison_witness
			var realized_variant_record: RenderCaptureVariant = \
					_shadow_capture_session.get_variant_diagnostics()
			var realized_variant: Dictionary = {}
			if realized_variant_record != null:
				var realized_value: Variant = \
						realized_variant_record.to_json_value()
				if realized_value is Dictionary:
					realized_variant = realized_value as Dictionary
			if not _valid_realized_camera(
					result_dict, desired_position, camera_basis(yaw_deg, pitch_deg),
					fov_deg, float(minute_value), _expected_mission_time_fixed24,
					variant, realized_variant):
				_fail("%s diagnostics did not preserve the requested pose/TOD" % label)
				return
			var row := publish_bundle(result_dict, publication_abs, label)
			if row.has("error"):
				_fail(String(row.error))
				return
			row["variant"] = String(variant.id)
			if bool(capture_profile.scratch_only):
				row["shadow_attribution"] = realized_variant
			capture_rows.append(row)
		_shadow_capture_session.finish()
		_shadow_capture_session = null

	var manifest_path := publication_child_path(
			publication_abs, "%s-manifest.json" % fixture_id)
	if manifest_path.is_empty():
		_fail("fixture %s produced an unsafe manifest path" % fixture_id)
		return
	var manifest_bytes := JSON.stringify(McpJson.sanitize(manifest), "\t").to_utf8_buffer()
	var write_error := _write_bytes(manifest_path, manifest_bytes)
	if write_error != OK:
		_fail("could not write fixture manifest: %s" % error_string(write_error))
		return
	var commit_error := commit_fixture_publication(publication_abs, output_abs)
	if commit_error != OK:
		_fail("could not commit complete fixture publication: %s" \
				% error_string(commit_error))
		return
	_fixture_publication_staging_abs = ""
	manifest_path = publication_child_path(
			output_abs, "%s-manifest.json" % fixture_id)
	if manifest_path.is_empty():
		_fail("fixture %s produced an unsafe committed manifest path" % fixture_id)
		return
	print("[render-fixture] PASS fixture=", fixture_id,
			" captures=", (manifest["artifacts"] as Array).size(),
			" manifest=", manifest_path)
	_shutdown(0)


func _prepare_pose(
		camera: Camera3D,
		position: Vector3,
		yaw_deg: float,
		pitch_deg: float,
		fov_deg: float,
		minute_of_day: float,
		settle_frames: int,
		comparison_contract: Dictionary = {}) -> bool:
	_game.process_mode = Node.PROCESS_MODE_INHERIT
	_world.process_mode = Node.PROCESS_MODE_INHERIT
	var runtime := _world.get_runtime()
	if runtime == null:
		_fail("mission runtime is unavailable while preparing an exact fixture pose")
		return false
	# Let render presentation settle while the mission clock is pinned. A paused
	# runtime keeps GameWorld's environment/material publication live but prevents
	# Weather.advance_world_driven() from advancing the selected fixed24 value.
	runtime.pause()
	if runtime.is_playing():
		_fail("mission runtime could not pause for an exact fixture pose")
		return false
	if not comparison_contract.is_empty():
		var pose := teleport_comparison_player(_world, comparison_contract)
		if pose.has("error"):
			_fail(String(pose.error))
			return false
	var minute_error: Error = _world.debug_set_mission_minute_of_day(minute_of_day)
	if minute_error != OK:
		_fail("could not set minute %.3f: %s" % [
			minute_of_day, error_string(minute_error)])
		return false
	var phase_error := pin_weather_phase(_world)
	if phase_error != OK:
		_fail("could not pin canonical weather phase: %s" % error_string(phase_error))
		return false
	var environment := _world.get_environment_node()
	if environment == null:
		_fail("mission environment is unavailable while preparing an exact fixture pose")
		return false
	_expected_mission_time_fixed24 = environment.get_mission_time_fixed24()
	await _settle(settle_frames)
	if environment.get_mission_minute_of_day() != minute_of_day \
			or environment.get_mission_time_fixed24() \
			!= _expected_mission_time_fixed24:
		_fail("mission clock advanced while settling exact fixture minute %.3f" \
				% minute_of_day)
		return false
	# Freeze the production shell after the environment has published the chosen
	# minute. Rendering and frame_post_draw continue, while sim, weather, camera
	# presenter, particles, and cloud offsets retain one exact state.
	_game.process_mode = Node.PROCESS_MODE_DISABLED
	_world.process_mode = Node.PROCESS_MODE_DISABLED
	camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	camera.keep_aspect = Camera3D.KEEP_HEIGHT
	camera.fov = fov_deg
	camera.near = 0.05
	camera.h_offset = 0.0
	camera.v_offset = 0.0
	camera.global_transform = Transform3D(camera_basis(yaw_deg, pitch_deg), position)
	camera.make_current()
	_finish_static_shadow_warmup_suspension()
	var refresh_error := _world.debug_refresh_render_pose(camera)
	if refresh_error != OK:
		_fail("could not refresh production state at the exact fixture pose: %s" \
				% error_string(refresh_error))
		return false
	if environment.get_mission_minute_of_day() != minute_of_day \
			or environment.get_mission_time_fixed24() \
			!= _expected_mission_time_fixed24:
		_fail("mission clock moved while refreshing exact fixture render state")
		return false
	return true


func _valid_realized_camera(
		bundle: Dictionary,
		position: Vector3,
		basis: Basis,
		fov_deg: float,
		minute_of_day: float,
		mission_time_fixed24: int,
		variant,
		realized_variant: Dictionary) -> bool:
	var artifact: Dictionary = bundle.get("artifact", {})
	if int(artifact.get("width", 0)) != _capture_size.x \
			or int(artifact.get("height", 0)) != _capture_size.y:
		return false
	var diagnostics: Dictionary = bundle.get("diagnostics", {})
	if not capture_variant_matches_diagnostics(
			variant, diagnostics, realized_variant):
		return false
	var camera_state: Dictionary = diagnostics.get("camera", {})
	var realized: Variant = camera_state.get("global_transform")
	if not (realized is Transform3D):
		return false
	var transform := realized as Transform3D
	if not transform.origin.is_equal_approx(position) \
			or not transform.basis.is_equal_approx(basis) \
			or not is_equal_approx(float(camera_state.get("fov_deg", -1.0)), fov_deg):
		return false
	if not realized_reflection_pose_matches(diagnostics):
		return false
	return realized_tod_matches(
			diagnostics, minute_of_day, mission_time_fixed24)


## Publish one captured bundle as a relocatable manifest sibling set. Public so
## the evidence-contract test exercises the same seam as the capture loop.
func publish_bundle(bundle: Dictionary, output_abs: String, label: String) -> Dictionary:
	if not publication_name_is_canonical(label):
		return {"error": "capture publication label is not canonical: %s" % label}
	var artifact: Dictionary = bundle.get("artifact", {})
	var source_png := String(artifact.get("png_path", ""))
	var source_state := String(artifact.get("state_path", ""))
	var output_png := publication_child_path(output_abs, label + ".png")
	var output_state := publication_child_path(
			output_abs, label + ".state.json")
	if output_png.is_empty() or output_state.is_empty():
		return {"error": "capture publication paths escape their fixture root"}
	var png_bytes := FileAccess.get_file_as_bytes(source_png)
	if png_bytes.is_empty():
		return {"error": "capture artifact is empty: %s" % source_png}
	var source_state_bytes := FileAccess.get_file_as_bytes(source_state)
	if source_state_bytes.is_empty():
		return {"error": "capture state is empty: %s" % source_state}
	var png_sha256 := _sha256_bytes(png_bytes)
	var source_state_sha256 := _sha256_bytes(source_state_bytes)
	if png_sha256.is_empty() or source_state_sha256.is_empty():
		return {"error": "could not hash captured bundle bytes"}
	if String(artifact.get("sha256", "")).to_lower() != png_sha256:
		return {"error": "capture artifact PNG hash does not match its bytes"}
	if String(artifact.get("state_sha256", "")).to_lower() \
			!= source_state_sha256:
		return {"error": "capture artifact state hash does not match its bytes"}
	var parsed_state: Variant = JSON.parse_string(
			source_state_bytes.get_string_from_utf8())
	if not (parsed_state is Dictionary):
		return {"error": "capture state is not a JSON object"}
	var published_state := parsed_state as Dictionary
	var state_contract_error := _capture_source_state_contract_error(
			bundle, published_state, source_png)
	if not state_contract_error.is_empty():
		return {"error": state_contract_error}
	var capture: Dictionary = published_state.get("capture", {})
	if capture.is_empty() or String(capture.get("png_sha256", "")).to_lower() \
			!= png_sha256:
		return {"error": "capture state does not bind the source PNG hash"}
	# capture_mcp_render_bundle writes its transient user:// path into the raw
	# sidecar. Preserve the raw sidecar hash below, but publish a deterministic
	# relocatable sidecar whose label and PNG reference match the manifest entry.
	# The transport label is filename-sanitized and capped at 64 characters;
	# fixture publication labels are canonical and may be longer.
	capture["label"] = label
	capture["png_path"] = output_png.get_file()
	published_state["capture"] = capture
	published_state["publication"] = {
		"transform": "rewrite_capture_label_and_png_path_to_publication_siblings",
		"source_state_sha256": source_state_sha256,
	}
	var comparison_witness: Variant = bundle.get("comparison_contract_witness")
	if comparison_witness is Dictionary \
			and not (comparison_witness as Dictionary).is_empty():
		published_state["comparison_contract_witness"] = \
				(comparison_witness as Dictionary).duplicate(true)
	var state_bytes := JSON.stringify(
			McpJson.sanitize(published_state), "\t").to_utf8_buffer()
	# Validate and transform the complete sibling set before the first publication
	# write. The fixture-level staging transaction owns final-directory atomicity.
	var png_error := _write_bytes(output_png, png_bytes)
	if png_error != OK:
		return {"error": "could not publish %s: %s" % [
			output_png, error_string(png_error)]}
	var state_error := _write_bytes(output_state, state_bytes)
	if state_error != OK:
		return {"error": "could not publish %s: %s" % [
			output_state, error_string(state_error)]}
	return {
		"label": label,
		# The manifest lives beside these artifacts. Filename-only references
		# keep the captured bundle relocatable and prevent a committed manifest
		# from silently resolving back into user:// or a scratch worktree.
		"png_path": output_png.get_file(),
		"state_path": output_state.get_file(),
		"png_sha256": png_sha256,
		"state_sha256": _sha256_bytes(state_bytes),
		"source_state_sha256": source_state_sha256,
		"width": int(artifact.get("width", 0)),
		"height": int(artifact.get("height", 0)),
	}


static func _read_json(path: String) -> Dictionary:
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return {}
	var parsed: Variant = JSON.parse_string(bytes.get_string_from_utf8())
	return parsed if parsed is Dictionary else {}


static func _sha256_bytes(bytes: PackedByteArray) -> String:
	var hash := HashingContext.new()
	if hash.start(HashingContext.HASH_SHA256) != OK:
		return ""
	hash.update(bytes)
	return hash.finish().hex_encode()


static func _capture_source_state_contract_error(
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
			or not _same_absolute_path(captured_png_path, source_png):
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


static func _same_absolute_path(left: String, right: String) -> bool:
	var left_abs := ProjectSettings.globalize_path(left).simplify_path() \
			.replace("\\", "/")
	var right_abs := ProjectSettings.globalize_path(right).simplify_path() \
			.replace("\\", "/")
	if OS.get_name() == "Windows":
		left_abs = left_abs.to_lower()
		right_abs = right_abs.to_lower()
	return left_abs == right_abs


static func _write_bytes(path: String, bytes: PackedByteArray) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_buffer(bytes)
	file.flush()
	var error := file.get_error()
	file.close()
	return error


## Wait until every first-person viewmodel part's active body clip has played
## to its end (a non-looping clip then holds its last frame, the state retail's
## settled hip idle shows), bounded by max_frames. Looping clips have no hold
## and are left alone: their phase is inherently unpinned on both engines.
func _settle_viewmodel_hold(max_frames: int) -> void:
	var presenter: Node = _game.get_node_or_null("LocalPlayerPresenter")
	if presenter == null or not presenter.has_method("vm_parts"):
		return
	for _i in max_frames:
		var held := true
		for part in presenter.call("vm_parts"):
			if not is_instance_valid(part):
				continue
			var model := part as ObjectModel
			if model == null:
				continue
			var skeletal: SkeletalAnim = model.get_skeletal_anim()
			var key: String = model.get_active_body_clip()
			if skeletal == null or key.is_empty() or skeletal.is_clip_looping(key):
				continue
			var frames := skeletal.get_clip_frame_count(key)
			var fps := skeletal.get_clip_fps(key)
			if frames <= 0 or fps <= 0.0:
				continue
			var duration_ms := int(ceil(1000.0 * float(frames) / fps))
			if model.get_animation_time_ms() < duration_ms:
				held = false
		if held:
			return
		await get_tree().process_frame


func _settle(frames: int) -> void:
	for _frame in range(maxi(frames, 0)):
		await get_tree().process_frame


func _realize_capture_variant_cache(variant, camera: Camera3D) -> Dictionary:
	var deadline_ms := Time.get_ticks_msec() + 30000
	var diagnostics: Dictionary = {}
	for _attempt in range(512):
		var refresh_error: Error = _world.debug_refresh_render_pose(camera)
		if refresh_error != OK:
			return {"error": "could not refresh renderer state: %s" %
					error_string(refresh_error)}
		var terrain := _world.get_terrain_node()
		diagnostics = terrain.get_tile_cache_diagnostics() \
				if terrain != null else {}
		if capture_variant_tile_cache_is_realized(variant, diagnostics):
			return {"diagnostics": diagnostics}
		if Time.get_ticks_msec() >= deadline_ms:
			break
		# GameWorld is frozen for exact-pose capture. Repeating this explicitly
		# non-time-owning refresh advances page demand/upload while the process
		# frame gives the two portable CPU workers scheduling time.
		await get_tree().process_frame
	return {"error": "terrain page cache did not become capture-ready: %s" %
			JSON.stringify(diagnostics)}


func _finish_static_shadow_warmup_suspension() -> void:
	if _static_shadow_warmup_suspension == null:
		return
	_static_shadow_warmup_suspension.finish()
	_static_shadow_warmup_suspension = null


func _fail(reason: String) -> void:
	if _failed:
		return
	_failed = true
	push_error("[render-fixture] FAIL: " + reason)
	_shutdown(1)


func _shutdown(exit_code: int) -> void:
	if _shutdown_started:
		return
	_shutdown_started = true
	call_deferred("_finish_shutdown", exit_code)


func _finish_shutdown(exit_code: int) -> void:
	_finish_static_shadow_warmup_suspension()
	if not _fixture_publication_staging_abs.is_empty():
		var abort_error := abort_fixture_publication(
				_fixture_publication_staging_abs)
		if abort_error != OK:
			push_warning("Could not clean fixture publication staging path %s: %s" % [
					_fixture_publication_staging_abs, error_string(abort_error)])
		_fixture_publication_staging_abs = ""
	if _shadow_capture_session != null:
		_shadow_capture_session.finish()
		_shadow_capture_session = null
	if _game != null and is_instance_valid(_game):
		_game.process_mode = Node.PROCESS_MODE_INHERIT
	if _world != null and is_instance_valid(_world):
		_world.process_mode = Node.PROCESS_MODE_INHERIT
	if _game != null and is_instance_valid(_game):
		_game.begin_runtime_shutdown()
		_game.finish_runtime_shutdown()
		_game.queue_free()
		await get_tree().process_frame
		await get_tree().process_frame
	get_tree().quit(exit_code)
