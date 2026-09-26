extends GutTest

# The round-event ARM SPLIT in the fire present pass (EntityPresenter's
# FirePresenter member) — the EFFECT legs.
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
# The muzzle anchor is the presenter's own muzzle_world_for over a REAL held
# weapon model (the gun fixture with its authored MFlash01 userpoint) adopted
# through the register_wire_held_weapon injection seam.

const WIRE_EYE := Vector3(10.0, 1.8, -4.0)
const BODY_ORIGIN := Vector3(10.0, 0.0, -4.0)
const WEAPON_POSITION := Vector3(10.6, 1.55, -4.7)
const SHOOTER := 0x0011
# The gun fixture (fixtures/README.md) carries MFlash01 on the barrel.
const WEAPON_3DI := "res://../fixtures/threedi/synth/gun.3di"
const WEAPON_GRAPHIC := "gun"
const MUZZLE_USERPOINT := "MFLASH01"

# The two effect names the event rows address: the ammo def's own muzzle
# effect and the addressed def's FIRE action-row effect.
const CATALOG_EFFECTS: PackedStringArray = ["AMMO_EFFECT", "EFFECT_M16MF"]

var _fx: EffectWorld
var _audio: MissionAudio
var _container: Node3D
var _fire: EntityPresenter
var _muzzle := Vector3.INF  # the held weapon's MFlash01 world point


# One synthetic in-memory particle catalog authoring every effect the rows
# name (the effect_world_test recipe): one burst definition shared by one
# effect per name, so the real EffectWorld interns and spawns them without a
# resource root.
func _catalog_file(effect_names: PackedStringArray) -> ParticleFile:
	return ParticleFixture.catalog(self, "flash dots",
			"emit_dur = 0.1;\nemit_rate = 50;\nemit_burst = 4;\nage = 0.2;\nalpha = 1;\nscale = 1;\n", effect_names)


# A REAL held-weapon model built from the gun fixture through the placer's
# graphic registry, placed at WEAPON_POSITION under `parent`.
func _fixture_weapon(parent: Node) -> ObjectModel:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(WEAPON_3DI)), OK,
			"the gun fixture model loads")
	placer.register_object_data(WEAPON_GRAPHIC, data)
	var model: ObjectModel = placer.build_model_from_graphic(WEAPON_GRAPHIC, "", parent, "")
	assert_not_null(model, "the fixture weapon builds")
	if model != null:
		model.position = WEAPON_POSITION
	return model


# The world position of the weapon's authored userpoint (the anchor
# muzzle_world_for resolves), through the model's own public read-back.
func _userpoint_world(weapon: ObjectModel, userpoint: String) -> Vector3:
	var data: ObjectData = weapon.get_object_data()
	for i in range(data.get_user_point_count()):
		var info: ModelUserPoint = data.get_user_point_info(i)
		if info.name.nocasecmp_to(userpoint) == 0:
			return weapon.global_transform * info.position
	return Vector3.INF


func _presenter_over(container: Node3D) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(container, null, null, _audio, _fx, null, null, null)
	return presenter


func before_each() -> void:
	_fx = EffectWorld.new()
	add_child_autofree(_fx)
	_fx.load_particle_file(_catalog_file(CATALOG_EFFECTS))
	_audio = MissionAudio.create(null, null)
	autofree(_audio)
	_container = Node3D.new()
	add_child_autofree(_container)
	_fire = _presenter_over(_container)
	# The muzzle anchor: this shooter's rendered held weapon, adopted as the
	# wire ADM edge would have built it.
	var weapon := _fixture_weapon(_container)
	if weapon != null:
		_fire.register_wire_held_weapon(SHOOTER, weapon)
		_muzzle = _userpoint_world(weapon, MUZZLE_USERPOINT)


# One event shaped like Simulation::drain_fire_presentation_events emits.
func _event(adm_arm: bool, action_effect: String = "EFFECT_M16MF") -> FirePresentationEvent:
	return FirePresentationEvent.make(WIRE_EYE, 0, SHOOTER, false, 0, Vector3(0.0, 0.0, -1.0),
			adm_arm, 24, "AMMO_EFFECT", action_effect, MUZZLE_USERPOINT)


func test_ammo_arm_keeps_the_ammo_def_effect_at_the_wire_position() -> void:
	_fire.present_fires([_event(false)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1, "the ammo arm spawns exactly one effect")
	if groups.size() == 1:
		var group := groups[0] as EffectGroupReport
		assert_eq(group.name, "AMMO_EFFECT",
				"the ammo arm uses the AMMO def's effect")
		assert_true(PresentPassFixture.emitter_position(group).is_equal_approx(WIRE_EYE),
				"the ammo arm spawns at the wire position, unmoved")


func test_adm_arm_uses_the_fire_row_at_the_weapon_anchor() -> void:
	assert_true(_muzzle.is_finite(), "the gun fixture authors the MFlash01 userpoint")
	assert_false(_muzzle.is_equal_approx(WIRE_EYE),
			"the weapon anchor is somewhere other than the wire eye")
	_fire.present_fires([_event(true)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1, "the adm arm still spawns exactly one effect")
	if groups.size() == 1:
		var group := groups[0] as EffectGroupReport
		assert_eq(group.name, "EFFECT_M16MF",
				"the adm arm uses the FIRE action row's effect, not the ammo def's")
		assert_true(PresentPassFixture.emitter_position(group).is_equal_approx(_muzzle),
				"the adm arm spawns at the weapon's userpoint, NOT the wire eye position")


func test_adm_arm_falls_back_to_the_body_origin_not_the_eye() -> void:
	# An unresolvable weapon anchor must NOT quietly revert to the wire position
	# — that is the eye, i.e. the bug. The presenter's resolver owns its own
	# fallback: a shooter with a wire body but no held weapon anchors at the
	# body origin (retail's deepest fallback is the entity origin).
	var container := Node3D.new()
	add_child_autofree(container)
	var unarmed := _presenter_over(container)
	var body := ObjectModel.new()
	container.add_child(body)
	body.position = BODY_ORIGIN
	unarmed.register_wire_node(SHOOTER, body)
	unarmed.present_fires([_event(true)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1)
	if groups.size() == 1:
		var group := groups[0] as EffectGroupReport
		assert_eq(group.name, "EFFECT_M16MF",
				"the row choice does not depend on the anchor resolve")
		assert_true(PresentPassFixture.emitter_position(group).is_equal_approx(BODY_ORIGIN),
				"an unarmed shooter anchors at its body origin, not the wire eye")


func test_adm_arm_with_no_wire_body_keeps_the_wire_value() -> void:
	# With no body at all the resolver answers non-finite and the wire value is
	# the only origin there is.
	var container := Node3D.new()
	add_child_autofree(container)
	var bare := _presenter_over(container)
	bare.present_fires([_event(true)])
	var groups := _fx.get_debug_group_report()
	assert_eq(groups.size(), 1)
	if groups.size() == 1:
		var group := groups[0] as EffectGroupReport
		assert_eq(group.name, "EFFECT_M16MF",
				"the row choice does not depend on the anchor resolve")
		assert_true(PresentPassFixture.emitter_position(group).is_equal_approx(WIRE_EYE),
				"with no body at all the wire value is the only origin there is")


func test_a_row_with_no_authored_effect_spawns_nothing() -> void:
	var ev := _event(true, "")
	_fire.present_fires([ev])
	assert_true(_fx.get_debug_group_report().is_empty(),
			"an unauthored fire row spawns no effect")
	assert_eq(_fx.live_group_count(), 0)
	assert_true(_audio.recent_fired_soundsets().is_empty(),
			"and this pass plays no sound of its own")
