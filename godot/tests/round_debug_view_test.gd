extends GutTest

# RoundDebugView's world label must expose both person-section results: retail
# uses the primary section for reactions/death and the final overlapping
# secondary section for the normal-infantry damage multiplier.

const ViewScript := preload("res://game/debug/round_debug_view.gd")


func _organic(section: int, secondary_section: int, fallback := false, material := 0,
		effect_tag_name := "", entity_handle := WireHandle.INVALID,
		entity_name := "") -> RoundDebugEvent:
	var event := RoundDebugEvent.new()
	event.kind = 0
	event.kind_name = "organic"
	event.section = section
	event.secondary_section = secondary_section
	event.fallback = fallback
	event.material = material
	event.effect_tag_name = effect_tag_name
	event.entity_handle = entity_handle
	event.entity_name = entity_name
	return event


func test_organic_label_names_primary_and_secondary_bones() -> void:
	var description: String = ViewScript.describe_event(
			_organic(14, 3, false, 19, "flesh", 7, "Target"))
	assert_string_contains(description, "reaction bone 14")
	assert_string_contains(description, "damage zone 3")
	assert_string_contains(description, "mat 19 -> flesh")


func test_missing_secondary_bone_is_explicit() -> void:
	var description: String = ViewScript.describe_event(_organic(1, -1))
	assert_string_contains(description, "damage zone -")


func test_unresolved_person_is_labeled_as_neutral_fallback() -> void:
	var description: String = ViewScript.describe_event(
			_organic(1, 1, true, 19, "player"))
	assert_string_contains(description, "neutral fallback sphere")
	assert_string_contains(description, "reaction stand-in 1")
	assert_false(description.contains("damage zone 1"))
