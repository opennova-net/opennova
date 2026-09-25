#include "object/item_database.h"
#include "resource_index/resource_root.h"

#include "util/data_format.h"

#include <formats/def/def.h>

#include <algorithm>
#include <vector>

using namespace godot;
using namespace opennova::def;

// Pin the GDScript-facing TYPE_* mirror to the engine/formats/def source of truth so the
// two mappings can never drift again (docs/world/itemdef-re.md D-ITEMDEF-1).
static_assert(ItemDatabase::TYPE_UNKNOWN == DEF_ITEM_TYPE_UNSET, "TYPE_UNKNOWN drifted from DefItemType");
static_assert(ItemDatabase::TYPE_VEHICLE == DEF_ITEM_TYPE_VEHICLE, "TYPE_VEHICLE drifted from DefItemType");
static_assert(ItemDatabase::TYPE_DECORATION == DEF_ITEM_TYPE_DECORATION, "TYPE_DECORATION drifted from DefItemType");
static_assert(ItemDatabase::TYPE_FOLIAGE == DEF_ITEM_TYPE_FOLIAGE, "TYPE_FOLIAGE drifted from DefItemType");
static_assert(ItemDatabase::TYPE_PERSON == DEF_ITEM_TYPE_PERSON, "TYPE_PERSON drifted from DefItemType");
static_assert(ItemDatabase::TYPE_MARKER == DEF_ITEM_TYPE_MARKER, "TYPE_MARKER drifted from DefItemType");
static_assert(ItemDatabase::TYPE_BUILDING == DEF_ITEM_TYPE_BUILDING, "TYPE_BUILDING drifted from DefItemType");
static_assert(ItemDatabase::TYPE_POWERUP == DEF_ITEM_TYPE_POWERUP, "TYPE_POWERUP drifted from DefItemType");
static_assert(ItemDatabase::ATTRIB_POWERUP == DEF_ITEM_ATTRIB_POWERUP, "ATTRIB_POWERUP drifted from def.h");
static_assert(ItemDatabase::ATTRIB_PLAYER_CONTROL == DEF_ITEM_ATTRIB_PLAYERCONTROL, "ATTRIB_PLAYER_CONTROL drifted from def.h");
static_assert(ItemDatabase::ATTRIB_ARMORY == DEF_ITEM_ATTRIB_ARMORY, "ATTRIB_ARMORY drifted from def.h");
static_assert(ItemDatabase::TYPE_OBJECT == DEF_ITEM_TYPE_OBJECT, "TYPE_OBJECT drifted from DefItemType");
static_assert(ItemDatabase::TYPE_EFFECT == DEF_ITEM_TYPE_EFFECT, "TYPE_EFFECT drifted from DefItemType");

void ItemDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &ItemDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &ItemDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &ItemDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ItemDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ItemDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &ItemDatabase::get_count);
	ClassDB::bind_method(D_METHOD("has_item", "id"), &ItemDatabase::has_item);
	ClassDB::bind_method(D_METHOD("get_vehicle_physics", "id"), &ItemDatabase::get_vehicle_physics);
	ClassDB::bind_method(D_METHOD("get_graphic", "id"), &ItemDatabase::get_graphic);
	ClassDB::bind_method(D_METHOD("get_sid", "id"), &ItemDatabase::get_sid);
	ClassDB::bind_method(D_METHOD("get_display_name", "id"), &ItemDatabase::get_display_name);
	ClassDB::bind_method(D_METHOD("get_husk", "id"), &ItemDatabase::get_husk);
	ClassDB::bind_method(D_METHOD("get_huskfinal", "id"), &ItemDatabase::get_huskfinal);
	ClassDB::bind_method(D_METHOD("get_anim_def", "id"), &ItemDatabase::get_anim_def);
	ClassDB::bind_method(D_METHOD("get_ai_function", "id"), &ItemDatabase::get_ai_function);
	ClassDB::bind_method(D_METHOD("get_move_function", "id"), &ItemDatabase::get_move_function);
	ClassDB::bind_method(D_METHOD("get_item_type", "id"), &ItemDatabase::get_item_type);
	ClassDB::bind_method(D_METHOD("get_light_transfer", "id"), &ItemDatabase::get_light_transfer);
	ClassDB::bind_method(D_METHOD("get_attrib", "id"), &ItemDatabase::get_attrib);
	ClassDB::bind_method(D_METHOD("get_attrib2", "id"), &ItemDatabase::get_attrib2);
	ClassDB::bind_method(D_METHOD("get_item_ids"), &ItemDatabase::get_item_ids);

	BIND_CONSTANT(TYPE_VEHICLE);
	BIND_CONSTANT(TYPE_PERSON);
	BIND_CONSTANT(TYPE_BUILDING);
	BIND_CONSTANT(TYPE_POWERUP);
	BIND_CONSTANT(TYPE_OBJECT);
	BIND_CONSTANT(ATTRIB_POWERUP);
	BIND_CONSTANT(ATTRIB_PLAYER_CONTROL);
}

ItemDatabase::~ItemDatabase() {
	release_native_items();
}

void ItemDatabase::release_native_items() {
	// The index points into the parse: drop it before the rows go away.
	index_.clear();
	sorted_ids_ = PackedInt32Array();
	if (items_file_loaded_) {
		def_free_items(&items_file_);
		items_file_loaded_ = false;
	}
	items_file_ = {};
}

// Retain the parse (ADR 0028) and index it. The id index walks the rows in
// file order and keeps the FIRST row per id: the engine's type-id resolution
// (mission::find_item_def, witnessed there) never reaches a later duplicate.
// The rows themselves stay as parsed.
void ItemDatabase::adopt_(const DefItemsFile &p_file) {
	items_file_ = p_file;
	items_file_loaded_ = true;

	index_.reserve(items_file_.count);
	for (size_t i = 0; i < items_file_.count; ++i) {
		index_.emplace(items_file_.entries[i].id, i);
	}

	// The index is unordered, so callers that enumerate get a stable order only
	// if we impose one. Sort by display name (natural, case-insensitive, the
	// order a user scans a palette), breaking ties by id so the order is total
	// and reproducible.
	struct SortKey {
		String display_name;
		int id;
	};
	std::vector<SortKey> keys;
	keys.reserve(index_.size());
	for (const auto &pair : index_) {
		keys.push_back({String(items_file_.entries[pair.second].display_name), pair.first});
	}
	std::sort(keys.begin(), keys.end(), [](const SortKey &a, const SortKey &b) {
		const int name_cmp = a.display_name.naturalnocasecmp_to(b.display_name);
		if (name_cmp != 0) {
			return name_cmp < 0;
		}
		return a.id < b.id;
	});
	sorted_ids_.resize(static_cast<int>(keys.size()));
	for (size_t i = 0; i < keys.size(); ++i) {
		sorted_ids_.set(static_cast<int>(i), keys[i].id);
	}
}

const opennova::def::DefItemDef *ItemDatabase::row_(int p_id) const {
	const auto it = index_.find(p_id);
	return it == index_.end() ? nullptr : &items_file_.entries[it->second];
}

Error ItemDatabase::load(const String &path) {
	++revision;
	source_path = path;
	last_error = String();
	release_native_items();

	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		last_error = String("Cannot open item database: ") + path;
		return ERR_CANT_OPEN;
	}

	DefItemsFile file = {};
	if (def_parse_items_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("def_parse_items_memory failed for ") + path;
		return ERR_CANT_OPEN;
	}

	adopt_(file);
	return OK;
}

Error ItemDatabase::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	++revision;
	last_error = String();
	release_native_items();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) {
		last_error = "Item database filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) {
		last_error = String("Item database not found in resource root: ") + file_name;
		return ERR_FILE_NOT_FOUND;
	}

	DefItemsFile file = {};
	if (def_parse_items_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("def_parse_items_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}

	adopt_(file);
	source_path = file_name;
	return OK;
}

bool ItemDatabase::is_loaded() const {
	return !index_.empty();
}

String ItemDatabase::get_source_path() const {
	return source_path;
}

String ItemDatabase::get_last_error() const {
	return last_error;
}

int ItemDatabase::get_count() const {
	return static_cast<int>(index_.size());
}

bool ItemDatabase::has_item(int id) const {
	return row_(id) != nullptr;
}

String ItemDatabase::get_graphic(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->graphic);
}

String ItemDatabase::get_sid(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->sid);
}

String ItemDatabase::get_anim_def(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->anim_def);
}

String ItemDatabase::get_render_function(int id) const {
    const auto *row = row_(id);
    return row == nullptr ? String() : String(row->render_function);
}

String ItemDatabase::get_ai_function(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->ai_function);
}

String ItemDatabase::get_move_function(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->move_function);
}

int ItemDatabase::get_item_type(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? static_cast<int>(TYPE_UNKNOWN) : row->type;
}

int32_t ItemDatabase::get_model_scale_q16(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? 0 : row->scale_q16;
}

float ItemDatabase::get_light_transfer(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? 0.0f : row->light_transfer;
}

// The raw items.def ItemDefAttrib dword (itemDef+0x54); 0 for unknown ids. The AS zone
// traits read bits 0x20000 "ChangeTeam" (capture trigger) and 0x40000 "SpawnPoint"
// (deploy-selectable). [docs/world/itemdef-re.md; net-re §5.61]
uint32_t ItemDatabase::get_attrib(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? 0u : static_cast<uint32_t>(row->attrib);
}

uint32_t ItemDatabase::get_attrib2(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? 0u : static_cast<uint32_t>(row->attrib2);
}

PackedInt32Array ItemDatabase::get_vehicle_physics(int id) const {
	PackedInt32Array out;
	const opennova::def::DefItemDef *row = row_(id);
	if (row == nullptr) return out;
	out.push_back(row->physics);
	out.push_back(row->player_speed);
	out.push_back(row->acceleration);
	out.push_back(row->deceleration);
	out.push_back(row->turn_rate);
	out.push_back(row->turn_rate2);
	out.push_back(row->unit_type);
	out.push_back(row->torque);
	out.push_back(row->water_speed);
	out.push_back(row->climb_speed);
	out.push_back(row->turn_roll);
	out.push_back(row->speed_pitch);
	out.push_back(row->max_slope);
	out.push_back(row->slip_slope);
	out.push_back(row->mass);
	out.push_back(row->lean);
	out.push_back(row->lean_velocity);
	out.push_back(row->pitch);
	out.push_back(row->pitch_velocity);
	out.push_back(row->bob);
	out.push_back(row->flip);
	return out;
}

String ItemDatabase::get_display_name(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->display_name);
}

// The native launchups_closeattack field; its parser witness lives in def_items.cpp.
String ItemDatabase::get_launchups_closeattack(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->launchups_closeattack);
}

String ItemDatabase::get_husk(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->husk);
}

String ItemDatabase::get_huskfinal(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? String() : String(row->huskfinal);
}

opennova::def::DefItemParticleFx ItemDatabase::get_particle_fx(int id) const {
	const opennova::def::DefItemDef *row = row_(id);
	return row == nullptr ? opennova::def::DefItemParticleFx{} : row->particlefx;
}

PackedInt32Array ItemDatabase::get_item_ids() const {
	return sorted_ids_;
}
