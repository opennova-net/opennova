extends GutTest

# NovaSimulation.debug_pick_entity: the F3 entity picker's native contract.
# A worldless sim must return the full typed-defaults dictionary with
# hit == false — this pins the binding registration (a stale DLL fails to
# parse here instead of silently greening) and the stable card shape the
# pick UI and snapshot writer rely on. Live-world hits are exercised by the
# game lifecycle test; geometry correctness is libs/world ctest territory.

const EXPECTED_TYPES := {
	"hit": TYPE_BOOL,
	"blocked": TYPE_STRING,
	"hit_class": TYPE_STRING,
	"entity_handle": TYPE_INT,
	"pool": TYPE_INT,
	"kind": TYPE_INT,
	"index": TYPE_INT,
	"bms_id": TYPE_INT,
	"net_id": TYPE_INT,
	"item_id": TYPE_INT,
	"name": TYPE_STRING,
	"position_godot": TYPE_VECTOR3,
	"bound_radius": TYPE_FLOAT,
	"hit_position_godot": TYPE_VECTOR3,
	"hit_normal_godot": TYPE_VECTOR3,
	"distance_units": TYPE_FLOAT,
	"section": TYPE_INT,
	"face": TYPE_INT,
	"bone": TYPE_INT,
	"hit_zone": TYPE_INT,
	"surface_type": TYPE_INT,
	"material_flags": TYPE_INT,
	"tick": TYPE_INT,
}


func test_worldless_pick_returns_the_full_typed_shape() -> void:
	var sim := NovaSimulation.new()
	add_child_autofree(sim)
	var pick: Dictionary = sim.debug_pick_entity(Vector3.ZERO, Vector3.FORWARD, 100.0)
	assert_false(bool(pick.get("hit", true)), "no world - never a hit")
	assert_eq(pick.size(), EXPECTED_TYPES.size(), "no undeclared keys")
	for key in EXPECTED_TYPES:
		assert_true(pick.has(key), "the card carries '%s'" % key)
		assert_eq(typeof(pick[key]), int(EXPECTED_TYPES[key]),
				"'%s' keeps its declared type" % key)
	assert_eq(String(pick["blocked"]), "",
			"worldless is empty-handed, not 'blocked' - no trace ever ran")


func test_zero_direction_is_a_clean_miss() -> void:
	var sim := NovaSimulation.new()
	add_child_autofree(sim)
	var pick: Dictionary = sim.debug_pick_entity(Vector3(1, 2, 3), Vector3.ZERO, 500.0)
	assert_false(bool(pick.get("hit", true)))
	assert_eq(String(pick.get("hit_class", "?")), "", "no class without a hit")
