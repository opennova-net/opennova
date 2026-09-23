#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <formats/def/def.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace godot {

class ResourceRoot;

// Thin GDExtension wrapper over engine/formats/def items.def parsing (def_parse_items).
// Resolves a mission entity's item id -> its visual model (.3di basename) and a
// few type fields. This is the minimal "database" slice needed to place objects;
// weapon/ammo/hud definitions are gameplay and intentionally not surfaced here.
class ItemDatabase : public RefCounted {
	GDCLASS(ItemDatabase, RefCounted)

private:
	// The retained items.def parse (ADR 0028): the ONE store. The sim's
	// engine-side trait fold (mission::resolve_item_traits /
	// resolve_ai_weapons) and the replication replication catalog read DefItemDef
	// rows directly from here; every accessor below converts at the call.
	// Freed at the top of every load attempt (the id index clears first) and
	// in the destructor.
	opennova::def::DefItemsFile items_file_ = {};
	bool items_file_loaded_ = false;
	// id -> row index into items_file_.entries, built in file order so a
	// duplicate id keeps its FIRST row (the public lookup collapses duplicates;
	// the replication catalog walks the rows itself).
	std::unordered_map<int, size_t> index_;
	// Every unique id in a stable display order (natural, case-insensitive
	// display_name, then id), computed once at load so an enumeration is
	// reproducible across loads (the index is unordered).
	PackedInt32Array sorted_ids_;
	String source_path;
	String last_error;
	uint64_t revision = 0; // increments before every load attempt

	void release_native_items();
	// Retains a successful parse and builds index_ / sorted_ids_ over it (both
	// load paths adopt through here, so the views can never diverge).
	void adopt_(const opennova::def::DefItemsFile &p_file);
	// The row an id resolves to; nullptr for an unknown id.
	const opennova::def::DefItemDef *row_(int p_id) const;

protected:
	static void _bind_methods();

public:
	// DefItemDef.type values — the witnessed engine ItemDefType at ItemDef+0x5C,
	// mirroring DefItemType in engine/formats/def/def.h (static_asserts in the
	// .cpp pin the mirror). Non-injective by engine design: DECORATION==FOLIAGE
	// and POWERUP==OBJECT share values; 7 is unused, 0 = unset/unknown.
	// (engine: formats/def/def.h)
	enum {
		TYPE_UNKNOWN = 0,
		TYPE_VEHICLE = 1,
		TYPE_DECORATION = 2,
		TYPE_FOLIAGE = 2,
		TYPE_PERSON = 3,
		TYPE_MARKER = 4,
		TYPE_BUILDING = 5,
		TYPE_POWERUP = 6,
		TYPE_OBJECT = 6,
		TYPE_EFFECT = 8,
	};

	// ItemDefAttrib bits GDScript composes against get_attrib()/get_attrib2()
	// results — mirrors DEF_ITEM_ATTRIB_* (static_asserts in the .cpp pin them).
	enum {
		ATTRIB_POWERUP = 0x2,
		ATTRIB_PLAYER_CONTROL = 0x40,
		ATTRIB_ARMORY = 0x80000,
	};

	~ItemDatabase();

	Error load(const String &path);
	// Load items.def by flat name through the mounted resource root (VFS), so the item
	// database resolves from PFF archives at runtime. Mirrors the other *_from_resource_root.
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	// The retained parse the engine-side trait fold and the replication replication
	// catalog consume (empty — entries nullptr, count 0 — until a load succeeds).
	const opennova::def::DefItemsFile &native_items() const noexcept { return items_file_; }
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	// The number of unique item ids (a duplicate id counts once).
	int get_count() const;
	uint64_t get_revision() const { return revision; }

	bool has_item(int id) const;
	// Model basename without extension (e.g. "tank"); empty if unknown/none.
	String get_graphic(int id) const;
	// The items.def `sid` token, the key hudpos.def's VEHICLE_HUD blocks commit
	// against; empty if unknown/none.
	String get_sid(int id) const;
	String get_anim_def(int id) const;
	// items.def *_function class tags (raw); empty if the item declares none. The
	// net layer turns these into a wire dispatch class.
	String get_ai_function(int id) const;
	String get_render_function(int id) const;
	String get_move_function(int id) const;
	// DefItemDef.type (the TYPE_* mirror above); TYPE_UNKNOWN for unknown ids.
	int get_item_type(int id) const;
	// Effective authored model scale source, signed Q16.16. Zero is retail's
	// unscaled sentinel (a visual/collision scale of 1.0).
	// (engine: formats/def/def.h)
	int32_t get_model_scale_q16(int id) const;
	// Building-interior daylight fraction from items.def light_transfer
	// (authored percent clamped to 0..100 at parse; 0.0 for unknown/absent).
	float get_light_transfer(int id) const;
	// The raw items.def ItemDefAttrib dword (itemDef+0x54); 0 for unknown ids. AS zone traits
	// read 0x20000 "ChangeTeam" / 0x40000 "SpawnPoint". [net-re §5.61]
	uint32_t get_attrib(int id) const;
	// The raw items.def ItemDefAttrib2 dword (itemDef+0x58); 0 for unknown ids. The
	// render-occlusion weld pass reads bit 6 ("weldable") (engine: runtime/mission/collision_resolve.cpp).
	uint32_t get_attrib2(int id) const;
	// The authored items.def `shadow` blob decal (C++ seam for the placer):
	// false when the item authors none; dims = (width, length, offset_x,
	// offset_y) in the decal's own units.
	bool get_shadow_decal(int id, String &r_texture, Vector4 &r_dims) const;
	// The pre-scaled vehicle physics block as [physics, player_speed, acceleration,
	// deceleration, turn_rate, turn_rate2, unit_type, torque, water_speed, climb_speed,
	// turn_roll, speed_pitch, max_slope, slip_slope, mass, lean, lean_velocity, pitch,
	// pitch_velocity, bob, flip]; empty for unknown ids. Feeds the sim's
	// world::VehicleTraits table (resolve_item_traits). (engine: formats/def/def.h)
	PackedInt32Array get_vehicle_physics(int id) const;
	String get_display_name(int id) const;
	String get_launchups_closeattack(int id) const;
	// items.def husk / huskfinal — the destroyed-model stages the render and
	// collision swap to at death (Flags & 4); empty if none authored.
	// (engine: runtime/mission/collision_resolve.cpp)
	String get_husk(int id) const;
	String get_huskfinal(int id) const;
	// Native slot-A definition for the effect director; unknown ids return
	// an empty slot. No ClassDB record or Godot string copy along this path.
	opennova::def::DefItemParticleFx get_particle_fx(int id) const;

	// Every item id in a stable display order (natural, case-insensitive
	// display_name, then id) — sorted_ids_, computed at load.
	PackedInt32Array get_item_ids() const;
};

} // namespace godot
