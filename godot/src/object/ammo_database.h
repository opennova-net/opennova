#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <formats/def/def.h>

namespace godot {
class ResourceRoot;
// Read-only access to the runtime's native ammo.def records.
class AmmoDatabase : public RefCounted {
	GDCLASS(AmmoDatabase, RefCounted)
public:
	~AmmoDatabase() override;
	Error load_from_resource_root(const Ref<ResourceRoot> &root, const String &name);
	int get_count() const { return int(file_.count); }
	int find_ammo(const String &name) const;
	String get_name(int index) const;
	int get_velocity(int index) const;
	int get_min_damage(int index) const;
	int get_max_damage(int index) const;
protected:
	static void _bind_methods();
private:
	const opennova::def::DefAmmoDef *row(int index) const;
	opennova::def::DefAmmoFile file_{};
};
} // namespace godot
