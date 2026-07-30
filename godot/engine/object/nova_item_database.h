#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <unordered_map>
#include <vector>

struct DefItemDef;

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over libs/def items.def parsing (def_parse_items).
// Resolves a mission entity's item id -> its visual model (.3di basename) and a
// few type fields. This is the minimal "database" slice needed to place objects;
// weapon/ammo/hud definitions are gameplay and intentionally not surfaced here.
class NovaItemDatabase : public RefCounted {
	GDCLASS(NovaItemDatabase, RefCounted)

private:
	struct Item {
		int id = 0;
		int type = 0;
		uint32_t attrib = 0; // items.def ItemDefAttrib (+0x54); 0x100000 = AIData (AI class)
		uint32_t attrib2 = 0; // items.def ItemDefAttrib2 (+0x58); bit 6 = portal-weldable
		String display_name;
		String graphic;
		String anim_def;
		String sound_profile;
		// items.def *_function class-tag directives. The net layer maps these to a
		// §5.10b wire dispatch class (NovaNetClient::class_from_tag); the placer/
		// renderer doesn't use them. Stored raw so the object DB stays net-agnostic.
		String ai_function;
		String move_function;
		int hp = 0; // items.def hp = itemDef+0x17C healthMax (0 = none declared)
		float light_transfer = 0.0f; // ItemDef+0x218 interior daylight fraction
		float damage_reduc_pp = 0.0f;
		float damage_reduc_max = 0.0f;
		// Vehicle physics-property block, PRE-SCALED by the libs/def parser exactly like
		// the original loader [orig: ItemDef_ParsePhysicsProperty @0x49d870]. Consumed by
		// the sim's item-traits sweep (world::VehicleTraits). All 0 when absent.
		int physics = 0;      // +0x8DC selector — non-zero = ground-vehicle motor
		int acceleration = 0; // +0x8E0
		int deceleration = 0; // +0x8E4
		int player_speed = 0; // +0x8E8 (16.16 u/tick)
		int turn_rate = 0;    // +0x924 (BAM/tick)
		int turn_rate2 = 0;   // +0x928
		int torque = 0;       // +0x91C raw — collision speed-decay shift [orig: @0x49dcca]
		int unit_type = 0;    // minimap icon class [orig: Entity_ClassifyForMinimap @0x50FA70]
		// items.def soundloop_1..7 — the looping ambient sound-set names for a
		// "snd:"-prefixed `type marker` item (e.g. soundloop_1 LPNV_LIGHT). Time-of-day
		// slots; the runtime plays the first non-empty one resolvable in the .lwf bank.
		String soundloops[7];
		// items.def per-item particle-effect keys, verbatim authored names. Anchored
		// slots carry {effect, userpoint[, secondary]}; the death/fire/other family is
		// effect-name-only (their anchors are the fixed husk userpoint names Dead/Fire/
		// Other, resolved at runtime). [orig: ItemDef_ParseProperty @ 0x49eb00
		// particlefx family @ 0x4a13ad..0x4a179d; mission-start resolve + slot-A attach
		// resolve_item_materials_and_spawn_bone_trails @ 0x522ee0]
		struct ParticleFx {
			String effect;
			String userpoint;
			String secondary_effect;
		};
		ParticleFx particlefx;   // slot A — the always-on attached emitter (exhaust)
		ParticleFx particlefxs;  // slot B — slow/secondary wake tier (movement-driven)
		ParticleFx particlefxw[4]; // slots C..F — the fxw1..4 wake tiers
		String particledeath;
		String particleh2odeath;
		String particlefire;
		String particleother;
		String particlespawn;
		String particlefinale;
		// Person-item anim-fire weapon family (world-wac-ai-re §17.4): the
		// ammo_closeattack round NAME (JO riflemen author all four ammo_* slots =
		// the rifle round) + clipsize (the magazine reseed). Consumed by the sim's
		// AI weapon seed (D-AI-5). [orig: ItemDef_ParseProperty @ 0x4a1823 ->
		// def+0x56B / @ 0x49fa1c -> def+0x894]
		String ammo_closeattack;
		int clipsize = 0;
		// items.def deathtime in TICKS ((62*seconds or 496) + 62, scaled at parse);
		// 0 = none authored. The corpse timer's seed (entity+0x148 at the infantry
		// death edge). [orig: ItemDef_ParseProperty @ 0x49fa6c -> def+0x890]
		int deathtime_ticks = 0;
		// items.def primary_weapon — the weapon.def entry an ewep emplacement mounts
		// (the attach label's text source); empty if none authored.
		// [orig: -> ItemDef+0x54B primaryWeapon; docs/world/itemdef-re.md]
		String primary_weapon;
		struct EmplacementAttachment {
			String userpoint;
			int item_id = 0;
			int down_angle = 0;
			int up_angle = 0;
			int right_angle = 0;
			int left_angle = 0;
			int angle_count = 0;
			int kind = 0;
		};
		std::vector<EmplacementAttachment> emplacement_attachments;
		int emplacement_g_slot = 0;
		int emplacement_c_slot = 0;
		// Mounted skeletal selector metadata: authored items.def phrase_set is the
		// target itemDef+0x86C dword. Presence is separate because zero is valid.
		bool mount_config_valid = false;
		int mount_config = 0;
		// The destruction/husk block (docs/world/world-wac-ai-re.md §24):
		// the husk model stages, the death sound, the armor-class words, the kz
		// death-blast radius, and the per-section debris types.
		String husk;      // itemDef+0x70 — the destroyed-model stage
		String huskfinal; // itemDef+0x80 — the final (burned-out) stage
		String sounddeath;    // -> deathSoundId; Entity_InitDeathSounds @ 0x4939b0
		int armor_impact = 0; // def+0x190 (-1 = invulnerable)
		int armor_blast = 0;  // def+0x192
		int armor_kz = 0;     // compatibility mirror of armor_blast
		float kz = 0.0f;      // def+0x198 death-blast radius (units)
		float debris_scale = 0.0f; // def+0x1BC (0 = unset -> 1.0)
		int husk_sub_parts = 0;    // def+0x100
		uint8_t husk_sub_part_types[16] = {}; // def+0x101[] debris-type rows
	};
	std::unordered_map<int, Item> items;
	String source_path;
	String last_error;

	// The one DefItemDef -> Item copy (both load paths adopt through it, so new
	// items.def fields land in one place).
	static Item item_from_entry(const ::DefItemDef &entry);

	// Items in a stable display order (by display name, then id), since the backing
	// store is unordered. Shared by get_item_ids() / get_items().
	std::vector<const Item *> sorted_items() const;

protected:
	static void _bind_methods();

public:
	// DefItemDef.type values — the witnessed engine ItemDefType at ItemDef+0x5C,
	// mirroring DefItemType in libs/def/include/def/def.h (static_asserts in the
	// .cpp pin the mirror). Non-injective by engine design: DECORATION==FOLIAGE
	// and POWERUP==OBJECT share values; 7 is unused, 0 = unset/unknown.
	// [orig: ItemDef_ParseProperty @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]
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
		EMPLACEMENT_ADDEWEAP = 0,
		EMPLACEMENT_ADDEWEAP_G = 1,
		EMPLACEMENT_ADDEWEAP_C = 2,
	};

	// ItemDefAttrib bits GDScript composes against get_attrib()/get_attrib2()
	// results — mirrors DEF_ITEM_ATTRIB_* (static_asserts in the .cpp pin them).
	enum {
		ATTRIB_POWERUP = 0x2,
		ATTRIB_PLAYER_CONTROL = 0x40,
		ATTRIB_ARMORY = 0x80000,
	};

	Error load(const String &path);
	// Load items.def by flat name through the mounted resource root (VFS), so the item
	// database resolves from PFF archives at runtime. Mirrors the other *_from_resource_root.
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	int get_count() const;

	bool has_item(int id) const;
	// Model basename without extension (e.g. "tank"); empty if unknown/none.
	String get_graphic(int id) const;
	String get_anim_def(int id) const;
	// items.def *_function class tags (raw); empty if the item declares none. The
	// net layer turns these into a wire dispatch class.
	String get_ai_function(int id) const;
	String get_move_function(int id) const;
	// items.def hp (itemDef+0x17C healthMax); 0 if unknown/none declared.
	int get_hp(int id) const;
	int get_item_type(int id) const;
	// Building-interior daylight fraction from items.def light_transfer
	// (authored percent clamped to 0..100 at parse; 0.0 for unknown/absent).
	float get_light_transfer(int id) const;
	float get_damage_reduc_pp(int id) const;
	float get_damage_reduc_max(int id) const;
	int get_armor_impact(int id) const;
	int get_armor_kz(int id) const;
	// items.def ItemDefAttrib & 0x100000 (AIData): true when the item def is AI-capable. The
	// host's pool-1 0x0D stream gates the AI-trailer on this so the wire matches the stock
	// decoder's own gate (itemDef.attrib & 0x100000 @0x433327). [docs/world/itemdef-re.md;
	// docs/net/novaworld-net-re.md D-NET-97]
	bool is_ai_capable(int id) const;
	// The raw items.def ItemDefAttrib dword (itemDef+0x54); 0 for unknown ids. AS zone traits
	// read 0x20000 "ChangeTeam" / 0x40000 "SpawnPoint". [net-re §5.61]
	uint32_t get_attrib(int id) const;
	// The raw items.def ItemDefAttrib2 dword (itemDef+0x58); 0 for unknown ids. The
	// render-occlusion weld pass reads bit 6 ("weldable") [orig: the +88 >> 6 read in
	// Terrain_RegisterExteriorPortalFaces @ 0x5c5cce].
	uint32_t get_attrib2(int id) const;
	// The pre-scaled vehicle physics block as [physics, player_speed, acceleration,
	// deceleration, turn_rate, turn_rate2, unit_type]; empty for unknown ids. Feeds the sim's
	// world::VehicleTraits table (resolve_item_traits). [orig: ItemDef_ParsePhysicsProperty
	// @0x49d870; consumer Entity_UpdateVehiclePhysics @0x48af00]
	PackedInt32Array get_vehicle_physics(int id) const;
	String get_display_name(int id) const;
	// items.def ammo_closeattack — the person-item anim-fire round NAME, resolved
	// against the ammo table at mission load by the sim's AI weapon seed (D-AI-5);
	// empty if none authored. [orig: ItemDef_ParseProperty @ 0x4a1823 -> def+0x56B;
	// world-wac-ai-re §17.4]
	String get_ammo_closeattack(int id) const;
	// items.def clipsize — the respawn magazine reseed (word entity+0x35C); 0 if
	// none authored. [orig: @ 0x49fa1c -> def+0x894; Entity_ResetToSpawnState
	// @ 0x4b97a9]
	int get_clipsize(int id) const;
	// items.def deathtime in ticks (parse-scaled); 0 if none authored. Seeds the
	// corpse timer at the death edge. [orig: @ 0x49fa6c -> def+0x890;
	// Entity_UpdateInfantryAI @ 0x4b9c97]
	int get_deathtime_ticks(int id) const;
	// items.def primary_weapon — the ewep emplacement's mounted weapon.def entry
	// (the USEGUN attach label resolves its attachtextid); empty if none authored.
	// [orig: -> ItemDef+0x54B; consumer draw_vehicle_seat_and_armory_labels @ 0x5a351d]
	String get_primary_weapon(int id) const;
	// Ordered child-emplacement records from addeweap/addeweapG/addeweapC.
	// Every Dictionary retains the exact key variant plus source userpoint,
	// child item id, and optional down/up/right/left limits.
	Array get_emplacement_attachments(int id) const;
	// Last stored G/C attachment slot, 1-based; zero means absent.
	Dictionary get_emplacement_attachment_markers(int id) const;
	// {valid: bool, value: int} for the target definition's phrase_set +0x86C.
	// Always returns both fields so absent and authored zero stay distinct.
	// [orig: parse @ 0x49F9DB..0x49FA0A; mounted consumer @ 0x4B1884]
	Dictionary get_mount_config(int id) const;
	// items.def husk / huskfinal — the destroyed-model stages the render and
	// collision swap to at death (Flags & 4); empty if none authored.
	// [orig: itemDef+0x70/+0x80 -> huskModel/huskFinalModel (+0xF4/+0xF8);
	// render pick @ 0x413086, pieces prefer huskfinal @ 0x4934af]
	String get_husk(int id) const;
	String get_huskfinal(int id) const;
	// The destruction traits as one bundle: {unit_type, kz, armor_impact,
	// armor_blast, sounddeath, debris_scale, husk_sub_parts,
	// husk_sub_part_types (PackedInt32Array), has_husk}. Empty Dictionary =
	// unknown id. Feeds the sim's item-traits sweep (world::ItemDeathTraits).
	// [docs/world/world-wac-ai-re.md §24]
	Dictionary get_death_traits(int id) const;
	// items.def sound_profile (a sound-set name resolved against the loaded .lwf
	// banks at runtime); empty if the item declares none.
	String get_sound_profile(int id) const;
	// items.def soundloop_1..7 as a 7-entry array (empty strings for unused slots).
	// These are the looping ambient sound-set names for "snd:" marker items.
	PackedStringArray get_sound_loops(int id) const;
	// The item's particle-effect keys as authored, keyed by the ITEMS.DEF key names
	// ("particlefx"/"particlefxs"/"particlefxw1".."particlefxw4" -> {effect, userpoint,
	// secondary_effect} sub-dictionaries; "particledeath"/"particleh2odeath"/
	// "particlefire"/"particleother"/"particlespawn"/"particlefinale" -> effect name).
	// Empty strings = key absent; empty Dictionary = unknown id. [orig:
	// ItemDef_ParseProperty @ 0x49eb00; slot-A runtime attach witness
	// resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 ->
	// Entity_SpawnBoneTrailEffect @ 0x43bef0]
	Dictionary get_particle_effects(int id) const;
	Dictionary get_item(int id) const;

	// Enumeration for UI (e.g. the mission editor's place-object palette). Both are
	// sorted deterministically by (display_name, id) so the list is stable across
	// loads (the backing store is an unordered_map). get_items() returns the same
	// per-item dictionaries as get_item(); get_item_ids() is just the ids.
	PackedInt32Array get_item_ids() const;
	Array get_items() const;
};

} // namespace godot
