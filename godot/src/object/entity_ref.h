#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace godot {

// The stable value identity of one presented entity model, carried on its
// ObjectModel (ObjectModel::entity_ref). A placed model gets the mission
// record's identity from MissionObjectPlacer.place (kind/index/bms_id,
// group/team/position for EntityIndex, item/graphic/attrib2 for the effect
// and shadow-attribution readers); a wire-spawned model gets the replica
// identity from WirePresentPass (wire_handle, the header origin kind/index,
// runtime type and packed character id). Absent ints read -1 (kind, origin
// kind, index, group, team, wire handle) or 0 (bms_id, item_id, attrib2,
// runtime type, character id). Read-write so a harness authors one.
#define ENTITY_REF_INT_FIELDS(X) \
	X(kind, -1)                  \
	X(origin_kind, -1)           \
	X(index, -1)                 \
	X(bms_id, 0)                 \
	X(item_id, 0)                \
	X(group, -1)                 \
	X(team, -1)                  \
	X(wire_handle, -1)           \
	X(runtime_type_id, 0)        \
	X(character_id, 0)

class EntityRef : public RefCounted {
	GDCLASS(EntityRef, RefCounted)

public:
#define ENTITY_REF_INT_ACCESSORS(m_name, m_default)         \
	int get_##m_name() const { return m_name##_; }          \
	void set_##m_name(int p_value) { m_name##_ = p_value; }
	ENTITY_REF_INT_FIELDS(ENTITY_REF_INT_ACCESSORS)
#undef ENTITY_REF_INT_ACCESSORS
	int64_t get_attrib2() const { return attrib2_; }
	void set_attrib2(int64_t p_value) { attrib2_ = p_value; }
	Vector3 get_position() const { return position_; }
	void set_position(const Vector3 &p_position) { position_ = p_position; }
	String get_graphic() const { return graphic_; }
	void set_graphic(const String &p_graphic) { graphic_ = p_graphic; }

	// True for a wire-spawned model (a joiner replica or a host-side wire row).
	bool has_wire_handle() const { return wire_handle_ >= 0; }

	// Fixture factory: the placed-identity core plus the wire handle.
	static Ref<EntityRef> make(int p_kind, int p_index, int p_bms_id, int p_item_id,
			int p_wire_handle);

protected:
	static void _bind_methods();

private:
#define ENTITY_REF_INT_MEMBER(m_name, m_default) int m_name##_ = m_default;
	ENTITY_REF_INT_FIELDS(ENTITY_REF_INT_MEMBER)
#undef ENTITY_REF_INT_MEMBER
	int64_t attrib2_ = 0;
	Vector3 position_; // mission-space
	String graphic_;
};

} // namespace godot
