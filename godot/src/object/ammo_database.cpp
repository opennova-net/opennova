#include "object/ammo_database.h"
#include "resource_index/resource_root.h"
#include "util/string_convert.h"

namespace godot {
AmmoDatabase::~AmmoDatabase() { opennova::def::def_free_ammo(&file_); }
void AmmoDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "root", "name"), &AmmoDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("get_count"), &AmmoDatabase::get_count);
	ClassDB::bind_method(D_METHOD("find_ammo", "name"), &AmmoDatabase::find_ammo);
	ClassDB::bind_method(D_METHOD("get_name", "index"), &AmmoDatabase::get_name);
	ClassDB::bind_method(D_METHOD("get_velocity", "index"), &AmmoDatabase::get_velocity);
	ClassDB::bind_method(D_METHOD("get_min_damage", "index"), &AmmoDatabase::get_min_damage);
	ClassDB::bind_method(D_METHOD("get_max_damage", "index"), &AmmoDatabase::get_max_damage);
}
Error AmmoDatabase::load_from_resource_root(const Ref<ResourceRoot> &root, const String &name) {
	opennova::def::def_free_ammo(&file_);
	if (root.is_null() || root->get_root_dir().is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = root->read_file(name.get_file());
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;
	return opennova::def::def_parse_ammo_memory(bytes.ptr(), size_t(bytes.size()), &file_) == 0 ? OK : ERR_PARSE_ERROR;
}
const opennova::def::DefAmmoDef *AmmoDatabase::row(int index) const {
	return index >= 0 && size_t(index) < file_.count ? &file_.entries[index] : nullptr;
}
int AmmoDatabase::find_ammo(const String &name) const {
	for (size_t i = 0; i < file_.count; ++i) if (name.nocasecmp_to(opennova::to_gd(file_.entries[i].name)) == 0) return int(i);
	return -1;
}
String AmmoDatabase::get_name(int index) const { const auto *r = row(index); return r ? opennova::to_gd(r->name) : String(); }
int AmmoDatabase::get_velocity(int index) const { const auto *r = row(index); return r ? r->velocity : 0; }
int AmmoDatabase::get_min_damage(int index) const { const auto *r = row(index); return r ? r->min_damage : 0; }
int AmmoDatabase::get_max_damage(int index) const { const auto *r = row(index); return r ? r->max_damage : 0; }
} // namespace godot
