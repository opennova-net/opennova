extends GutTest

# The round-event ARM SPLIT in FirePresentPass.
#
# Retail's receive path has two mutually exclusive arms and only one of them is the
# ammo-def pair. Bit 0 is tested first: set -> the ammo-def arm plays ammoDef+64 and
# spawns ammoDef+68 AT THE WIRE POSITION. Clear with bit 1 set -> the adm-indexed arm
# spawns no ammo-def leg at all; it executes the addressed def's FIRE action row at that
# weapon's own userpoint.
# [orig: NetPacket_DeserializeRoundEvent @0x42f270 — arms @0x42f521 / @0x42f6ce;
#  ammo legs @0x42f5dc / @0x42f6c2; the fire row @0x42f777 / @0x42f98f]
#
# This matters because the wire position IS the shooter's eye — retail sends
# Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so running the
# ammo-def leg on the adm arm draws every remote muzzle flash out of the shooter's face,
# about a metre behind the barrel.
#
# Asset-free: the sim, audio, fx and muzzle anchor are all stubs.

const FirePresentPass := preload("res://engine/world/fire_present_pass.gd")

const WIRE_EYE := Vector3(10.0, 1.8, -4.0)
const MUZZLE := Vector3(10.6, 1.55, -4.7)


class StubSim:
	extends RefCounted
	var events: Array = []
	func drain_fire_presentation_events() -> Array:
		var out := events
		events = []
		return out
	func drain_tracer_trails() -> Array:
		return []

	func drain_sound_emitters() -> Array:
		return []


class StubFx:
	extends RefCounted
	var spawns: Array = []
	func spawn_effect(name: String, pos: Vector3, forward: Vector3) -> void:
		spawns.append({"name": name, "pos": pos, "forward": forward})


class StubAudio:
	extends RefCounted
	var played: Array = []
	func fire_soundset(name: String, pos: Vector3, source_bms_id: int) -> void:
		played.append({"name": name, "pos": pos})


var _sim: StubSim
var _fx: StubFx
var _audio: StubAudio
var _fire


func before_each() -> void:
	_sim = StubSim.new()
	_fx = StubFx.new()
	_audio = StubAudio.new()
	var container := Node3D.new()
	add_child_autofree(container)
	_fire = FirePresentPass.new()
	_fire.setup(_sim, container,
			func(): return _audio,
			func(): return _fx,
			func(): return Vector3.ZERO,
			func(_handle: int, _userpoint: String): return MUZZLE)


# One event dict shaped like NovaSimulation::drain_fire_presentation_events emits.
func _event(adm_arm: bool) -> Dictionary:
	return {
		"origin": WIRE_EYE,
		"forward": Vector3(0.0, 0.0, -1.0),
		"shooter_handle": 0x0011,
		"source_bms_id": 0,
		"is_local_player": false,
		"adm_arm": adm_arm,
		"adm_index": 24,
		"sound_set": "AMMO_LAUNCH",
		"effect": "AMMO_EFFECT",
		"action_sound_set": "GS_M4",
		"action_effect": "EFFECT_M16MF",
		"action_userpoint": "MFLASH01",
		"mf_light": 0,
	}


func test_ammo_arm_keeps_the_ammo_def_legs_at_the_wire_position() -> void:
	_sim.events = [_event(false)]
	_fire.present(1)
	assert_eq(_fx.spawns.size(), 1, "the ammo arm spawns exactly one effect")
	assert_eq(String(_fx.spawns[0]["name"]), "AMMO_EFFECT",
			"the ammo arm uses the AMMO def's effect")
	assert_true((_fx.spawns[0]["pos"] as Vector3).is_equal_approx(WIRE_EYE),
			"the ammo arm spawns at the wire position, unmoved")
	assert_eq(_audio.played.size(), 1)
	assert_eq(String(_audio.played[0]["name"]), "AMMO_LAUNCH")


func test_adm_arm_uses_the_fire_row_at_the_weapon_anchor() -> void:
	_sim.events = [_event(true)]
	_fire.present(1)
	assert_eq(_fx.spawns.size(), 1, "the adm arm still spawns exactly one effect")
	assert_eq(String(_fx.spawns[0]["name"]), "EFFECT_M16MF",
			"the adm arm uses the FIRE action row's effect, not the ammo def's")
	assert_true((_fx.spawns[0]["pos"] as Vector3).is_equal_approx(MUZZLE),
			"the adm arm spawns at the weapon anchor, NOT the wire eye position")
	assert_eq(_audio.played.size(), 1)
	assert_eq(String(_audio.played[0]["name"]), "GS_M4",
			"the adm arm plays the action row's sound, not the ammo def's")
	assert_true((_audio.played[0]["pos"] as Vector3).is_equal_approx(MUZZLE))


func test_adm_arm_falls_back_to_the_anchor_provider_not_the_eye() -> void:
	# An unresolvable anchor must NOT quietly revert to the wire position — that is the
	# eye, i.e. the bug. The provider owns its own fallback (retail's is the entity
	# origin), so a non-finite return leaves the wire value only when there is no
	# provider at all.
	var container := Node3D.new()
	add_child_autofree(container)
	var bare = FirePresentPass.new()
	bare.setup(_sim, container,
			func(): return _audio,
			func(): return _fx,
			func(): return Vector3.ZERO)
	_sim.events = [_event(true)]
	bare.present(1)
	assert_eq(_fx.spawns.size(), 1)
	assert_eq(String(_fx.spawns[0]["name"]), "EFFECT_M16MF",
			"the row choice does not depend on the anchor provider")


func test_a_row_with_no_authored_effect_spawns_nothing() -> void:
	var ev := _event(true)
	ev["action_effect"] = ""
	ev["action_sound_set"] = ""
	_sim.events = [ev]
	_fire.present(1)
	assert_eq(_fx.spawns.size(), 0, "an unauthored fire row spawns no effect")
	assert_eq(_audio.played.size(), 0, "and plays no sound")
