#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The editor preview's projection of one mission entity record (ADR 0044):
// the node's transform is the exact placement the preview rendered for the
// record, the typed identity names the record in the mission document, and
// the local bounds frame the rendered model. GameWorld mints one per record
// on every preview load and reload and never gives it a scene owner, so the
// set is a transient projection of the native document, not a saved model.
class WorldEntityProxy : public Node3D {
	GDCLASS(WorldEntityProxy, Node3D)

public:
	// How the preview renders the record: a retained batched static instance
	// (the placer re-stamps its rows), an individual model node, or nothing
	// (a marker, or a graphic the source could not resolve).
	enum Representation { UNPLACED, STATIC_INSTANCE, MODEL };

private:
	int kind_ = -1;
	int index_ = -1;
	int bms_id_ = 0;
	int item_id_ = 0;
	String graphic_;
	AABB local_bounds_;
	Representation representation_ = UNPLACED;

protected:
	static void _bind_methods();

public:
	void set_kind(int p_value) { kind_ = p_value; }
	int get_kind() const { return kind_; }
	void set_index(int p_value) { index_ = p_value; }
	int get_index() const { return index_; }
	void set_bms_id(int p_value) { bms_id_ = p_value; }
	int get_bms_id() const { return bms_id_; }
	void set_item_id(int p_value) { item_id_ = p_value; }
	int get_item_id() const { return item_id_; }
	void set_graphic(const String &p_value) { graphic_ = p_value; }
	String get_graphic() const { return graphic_; }
	void set_local_bounds(const AABB &p_value) { local_bounds_ = p_value; }
	AABB get_local_bounds() const { return local_bounds_; }
	void set_representation(Representation p_value) { representation_ = p_value; }
	Representation get_representation() const { return representation_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::WorldEntityProxy::Representation)
