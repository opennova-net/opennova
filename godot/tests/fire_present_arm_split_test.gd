extends GutTest

# The round-event ARM SPLIT in FirePresentPass — the EFFECT legs.
#
# Retail's receive path has two mutually exclusive arms and only one of them is the
# ammo-def pair. Bit 0 is tested first: set -> the ammo-def arm spawns ammoDef+68
# AT THE WIRE POSITION. Clear with bit 1 set -> the adm-indexed arm spawns no
# ammo-def leg at all; it executes the addressed def's FIRE action row at that
# weapon's own userpoint.
# [orig: NetPacket_DeserializeRoundEvent @0x42f270 — arms @0x42f521 / @0x42f6ce;
#  ammo effect @0x42f6c2; the fire row @0x42f777 / @0x42f98f]
#
# This matters because the wire position IS the shooter's eye — retail sends
# Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so running the
# ammo-def leg on the adm arm draws every remote muzzle flash out of the shooter's face,
# about a metre behind the barrel.
#
# The SOUND legs of both arms run in the sim on the logic clock
# (world/fire_sound.h, pinned by the fire_sound ctest) and reach the audio bank
# through drain_fire_sounds — this pass presents effects only.
#
# Typed surfaces (ADR 0034): the event rows are pure data through the public
# present_fires data leg; the fx/audio sinks are a REAL EffectWorld over an
# in-memory catalog authoring both effect names (its group report is the read
# seam) and a REAL bank-less MissionAudio (its recent-fires ring stays empty).

const FirePresentPass := preload("res://game/world/fire_present_pass.gd")

const WIRE_EYE := Vector3(10.0, 1.8, -4.0)
const MUZZLE := Vector3(10.6, 1.55, -4.7)
const POSITION_EPS := Vector3(0.001, 0.001, 0.001)

# The two effect names the event rows address: the ammo def's own muzzle
# effect and the addressed def's FIRE action-row effect.
const CATALOG_EFFECTS: PackedStringArray = ["AMMO_EFFECT", "EFFECT_M16MF"]

var _fx: EffectWorld
var _audio: MissionAudio
var _fire: FirePresentPass


# One synthetic in-memory particle catalog authoring every effect the rows
# name (the effect_world_test recipe): one burst definition shared by one
# effect per name, so the real EffectWorld interns and spawns them without a
# resource root.
func _catalog_file(effect_names: PackedStringArray) -> ParticleFile:
	var def := ParticleDef.new()
	def.id = "flash dots"
	def.emit_dur = 0.1
	def.emit_rate = 50.0
	def.emit_burst = 4
	def.age = 0.2
	def.alpha = 1.0
	def.scale_value = 1.0
	var file := ParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	for effect_name in effect_names:
		var effect := ParticleEffect.new()
		effect.id = effect_name
		effect.pdefs = PackedStringArray(["flash dots"])
		effects.append(effect)
	file.effects = effects
	return file


func before_each() -> void:
	_fx = EffectWorld.new()
	add_child_autofree(_fx)
	_fx.load_particle_file(_catalog_file(CATALOG_EFFECTS))
	_audio = MissionAudio.new(null, null)
	var container := Node3D.new()
	add_child_autofree(container)
	_fire = FirePresentPass.new()
	_fire.setup(null, container,
			func(): return _audio,
			func(): return _fx,
			func(): return Vector3.ZERO,
			func(_handle: int, _userpoint: String): return MUZZLE)


# One event shaped like Simulation::drain_fire_presentation_events emits.
func _event(adm_arm: bool) -> FirePresentationEvent:
	var event := FirePresentationEvent.new()
	event.origin = WIRE_EYE
	event.forward = Vector3(0.0, 0.0, -1.0)
	event.shooter_handle = 0x0011
	event.adm_arm = adm_arm
	event.adm_index = 24
	event.effect = "AMMO_EFFECT"
	event.action_effect = "EFFECT_M16MF"
	event.action_userpoint = "MFLASH01"
	return event


# The emitter position of one group report row (the spawn point of a transient).
func _emitter_position(row: Dictionary) -> Vector3:
	var emitters: Array = row.get("emitters", [])
	if emitters.is_empty():
		return Vector3.INF
	return (emitters[0] as Dictionary).get("position", Vector3.INF)


func test_ammo_arm_keeps_the_ammo_def_effect_at_the_wire_position() -> void:
	_fire.present_fires([_event(false)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1, "the ammo arm spawns exactly one effect")
	if groups.size() == 1:
		var group := groups[0] as Dictionary
		assert_eq(String(group.get("name", "")), "AMMO_EFFECT",
				"the ammo arm uses the AMMO def's effect")
		assert_true(_emitter_position(group).is_equal_approx(WIRE_EYE),
				"the ammo arm spawns at the wire position, unmoved")


func test_adm_arm_uses_the_fire_row_at_the_weapon_anchor() -> void:
	_fire.present_fires([_event(true)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1, "the adm arm still spawns exactly one effect")
	if groups.size() == 1:
		var group := groups[0] as Dictionary
		assert_eq(String(group.get("name", "")), "EFFECT_M16MF",
				"the adm arm uses the FIRE action row's effect, not the ammo def's")
		assert_true(_emitter_position(group).is_equal_approx(MUZZLE),
				"the adm arm spawns at the weapon anchor, NOT the wire eye position")


func test_adm_arm_falls_back_to_the_anchor_provider_not_the_eye() -> void:
	# An unresolvable anchor must NOT quietly revert to the wire position — that is the
	# eye, i.e. the bug. The provider owns its own fallback (retail's is the entity
	# origin), so a non-finite return leaves the wire value only when there is no
	# provider at all.
	var container := Node3D.new()
	add_child_autofree(container)
	var bare := FirePresentPass.new()
	bare.setup(null, container,
			func(): return _audio,
			func(): return _fx,
			func(): return Vector3.ZERO)
	bare.present_fires([_event(true)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1)
	if groups.size() == 1:
		var group := groups[0] as Dictionary
		assert_eq(String(group.get("name", "")), "EFFECT_M16MF",
				"the row choice does not depend on the anchor provider")
		assert_true(_emitter_position(group).is_equal_approx(WIRE_EYE),
				"with no provider at all the wire value is the only origin there is")


func test_a_row_with_no_authored_effect_spawns_nothing() -> void:
	var ev := _event(true)
	ev.action_effect = ""
	_fire.present_fires([ev])
	assert_true(_fx.get_debug_group_report().is_empty(),
			"an unauthored fire row spawns no effect")
	assert_eq(_fx.live_group_count(), 0)
	assert_true(_audio.recent_fired_soundsets().is_empty(),
			"and this pass plays no sound of its own")
