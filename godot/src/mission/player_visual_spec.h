#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3i.hpp>

namespace godot {

// The player's resolved visual: the combo its packed character id resolves to
// (head + body graphics drawn in the world with the entity's skeleton, the arms
// graphic in first person, each part's raw camo triplet), the entity's own
// item id, and the resolved head's avatar/sex bytes. `fallback` = no combo
// resolved: the entity draws its own item model. Every field is read-write so
// a test authors the spec a stub placer returns.
// (MissionObjectPlacer::resolve_player_visual_spec carries the witnesses.)
class PlayerVisualSpec : public RefCounted {
	GDCLASS(PlayerVisualSpec, RefCounted)

	int character_id_ = 0;
	int item_id_ = 0;
	String head_;
	Vector3i head_camo_;
	String body_;
	Vector3i body_camo_;
	String arms_;
	Vector3i arms_camo_;
	int avatar_ = 1;
	int sex_ = 0;
	int nationality_index_ = -1;
	int division_index_ = -1;
	int combo_index_ = -1;
	bool fallback_ = true;

protected:
	static void _bind_methods();

public:
#define PLAYER_VISUAL_SPEC_FIELD(m_type, m_name)                    \
	m_type get_##m_name() const { return m_name##_; }              \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	PLAYER_VISUAL_SPEC_FIELD(int, character_id)
	PLAYER_VISUAL_SPEC_FIELD(int, item_id)
	PLAYER_VISUAL_SPEC_FIELD(String, head)
	PLAYER_VISUAL_SPEC_FIELD(Vector3i, head_camo)
	PLAYER_VISUAL_SPEC_FIELD(String, body)
	PLAYER_VISUAL_SPEC_FIELD(Vector3i, body_camo)
	PLAYER_VISUAL_SPEC_FIELD(String, arms)
	PLAYER_VISUAL_SPEC_FIELD(Vector3i, arms_camo)
	// The resolved head's voice (the wire avatar byte) and sex.
	PLAYER_VISUAL_SPEC_FIELD(int, avatar)
	PLAYER_VISUAL_SPEC_FIELD(int, sex)
	PLAYER_VISUAL_SPEC_FIELD(int, nationality_index)
	PLAYER_VISUAL_SPEC_FIELD(int, division_index)
	PLAYER_VISUAL_SPEC_FIELD(int, combo_index)
	PLAYER_VISUAL_SPEC_FIELD(bool, fallback)
#undef PLAYER_VISUAL_SPEC_FIELD
};

} // namespace godot
