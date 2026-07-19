extends GutTest

# RoundDebugView's world label must expose both person-section results: retail
# uses the primary section for reactions/death and the final overlapping
# secondary section for the normal-infantry damage multiplier.

const ViewScript := preload("res://engine/debug/round_debug_view.gd")


func test_organic_label_names_primary_and_secondary_bones() -> void:
	var description: String = ViewScript.describe_event({
		"kind": 0,
		"kind_name": "organic",
		"entity_handle": 7,
		"entity_name": "Target",
		"section": 14,
		"secondary_section": 3,
		"material": 19,
		"effect_tag_name": "flesh",
	})
	assert_string_contains(description, "reaction bone 14")
	assert_string_contains(description, "damage zone 3")
	assert_string_contains(description, "mat 19 -> flesh")


func test_missing_secondary_bone_is_explicit() -> void:
	var description: String = ViewScript.describe_event({
		"kind": 0,
		"kind_name": "organic",
		"section": 1,
		"secondary_section": -1,
	})
	assert_string_contains(description, "damage zone -")


func test_unresolved_person_is_labeled_as_neutral_fallback() -> void:
	var description: String = ViewScript.describe_event({
		"kind": 0,
		"kind_name": "organic",
		"section": 1,
		"secondary_section": 1,
		"fallback": true,
		"material": 19,
		"effect_tag_name": "player",
	})
	assert_string_contains(description, "neutral fallback sphere")
	assert_string_contains(description, "reaction stand-in 1")
	assert_false(description.contains("damage zone 1"))
