#include <runtime/world/weapon_table_build.h>

#include <base/io/strutil.h>
#include <runtime/simassets/adm_clip_index.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

using namespace opennova::def;

namespace opennova::world {

// engine/runtime/world mirrors these DEF_* bits beside their consumers (world stays
// def-parser-free). npruntime legally sees both headers, so this TU pins every
// mirror to the canonical def.h value; renumbering either side breaks the build.
static_assert(static_cast<uint32_t>(world::weapon_flag::kScoped) == DEF_WEAPON_FLAG_SCOPED);
static_assert(static_cast<uint32_t>(world::weapon_flag::kSighted) == DEF_WEAPON_FLAG_SIGHTED);
static_assert(static_cast<uint32_t>(world::weapon_flag::kUnderwater) == DEF_WEAPON_FLAG_UNDERWATER);
static_assert(static_cast<uint32_t>(world::weapon_flag::kEmplaced) == DEF_WEAPON_FLAG_EMPLACED);
static_assert(static_cast<uint32_t>(world::weapon_flag::kArmor) == DEF_WEAPON_FLAG_ARMOR);
static_assert(static_cast<uint32_t>(world::weapon_flag::kForceCrouch) == DEF_WEAPON_FLAG_FORCECROUCH);
static_assert(static_cast<uint32_t>(world::weapon_flag::kUseSpreadTwo) == DEF_WEAPON_FLAG_USESPREADTWO);
static_assert(static_cast<uint32_t>(world::weapon_flag::kNoCardSwitch) == DEF_WEAPON_FLAG_NOCARDSWITCH);
static_assert(static_cast<uint32_t>(world::weapon_flag::kForceScoped) == DEF_WEAPON_FLAG_FORCESCOPED);
static_assert(static_cast<uint32_t>(world::weapon_flag2::kInset) == DEF_WEAPON_FLAG2_INSET);
static_assert(static_cast<uint32_t>(world::weapon_flag2::kInvisible) == DEF_WEAPON_FLAG2_INVISIBLE);
static_assert(world::kAmmoFlagIgnore == DEF_AMMO_FLAG_IGNORE);
static_assert(world::kAmmoFlagDetonateSatchels == DEF_AMMO_FLAG_DETONATESATCHELS);
static_assert(world::kAmmoFlagNoGravity == DEF_AMMO_FLAG_NOGRAVITY);
static_assert(world::kAmmoFlagHasItem == DEF_AMMO_FLAG_HASITEM);
static_assert(world::kAmmoFlagInstantKillZone == DEF_AMMO_FLAG_INSTANTKILLZONE);
static_assert(world::kAmmoFlagUseOwnMove == DEF_AMMO_FLAG_USEOWNMOVE);
static_assert(world::kAmmoFlagNoAge == DEF_AMMO_FLAG_NOAGE);
static_assert(world::kAmmoFlagForceTracer == DEF_AMMO_FLAG_FORCETRACER);
static_assert(world::kAmmoFlagShotgun == DEF_AMMO_FLAG_SHOTGUN);
static_assert(world::kAmmoFlagClaymore == DEF_AMMO_FLAG_CLAYMORE);
static_assert(world::kAmmoFlagNoOItems == DEF_AMMO_FLAG_NOOITEMS);
static_assert(world::kAmmoFlagNoMItems == DEF_AMMO_FLAG_NOMITEMS);
static_assert(world::kAmmoFlagNoDItems == DEF_AMMO_FLAG_NODITEMS);
static_assert(world::kAmmoFlagDesignateTarget == DEF_AMMO_FLAG_DESIGNATETARGET);
static_assert(world::kAmmoFlagIgnorFoilage == DEF_AMMO_FLAG_IGNORFOILAGE);
static_assert(world::kItemAttribEweap == DEF_ITEM_ATTRIB_EWEAP);
static_assert(world::kItemAttribLandable == DEF_ITEM_ATTRIB_LANDABLE);
static_assert(world::kItemAttribAIData == DEF_ITEM_ATTRIB_AIDATA);
static_assert(world::kItemAttribChangeTeam == DEF_ITEM_ATTRIB_CHANGETEAM);
static_assert(world::kItemAttribSpawnPoint == DEF_ITEM_ATTRIB_SPAWNPOINT);
static_assert(world::kItemAttribLeaveCorpse == DEF_ITEM_ATTRIB_LEAVECORPSE);
static_assert(world::kItemAttribNoDismember == DEF_ITEM_ATTRIB_NODISMEMBER);
static_assert(world::kItemAttribNoDie == DEF_ITEM_ATTRIB_NODIE);

namespace {

bool ci_equal(const char *a, const char *b) {
	while (*a != '\0' && *b != '\0' &&
	       std::tolower(static_cast<unsigned char>(*a)) ==
	               std::tolower(static_cast<unsigned char>(*b)))
		++a, ++b;
	return *a == '\0' && *b == '\0';
}

// One definition-local copy of the AnimMap slot heads. The action bake probes
// without advancing, then each automatic field serves and advances the named
// ring [orig: AnimMap_FindSlotByName @0x40cfa0; Anim_GetDurationTicks
// @0x53ee10]. Keeping this context inside one weapon iteration prevents an
// earlier definition's reads from rotating a later definition's slots.
struct WeaponTableClipRing {
	std::string key;
	std::vector<float> lengths;
	size_t head = 0;
};

struct WeaponTableClipContext {
	std::vector<WeaponTableClipRing> rings;
};

WeaponTableClipRing *find_clip_ring(
		WeaponTableClipContext &ctx, const char *key) {
	const std::string lower = strutil::to_lower(key != nullptr ? key : "");
	for (WeaponTableClipRing &ring : ctx.rings)
		if (ring.key == lower) return &ring;
	return nullptr;
}

int table_clip_resolves(void *opaque, const char *key) {
	return find_clip_ring(*static_cast<WeaponTableClipContext *>(opaque), key) != nullptr;
}

float table_clip_seconds(void *opaque, const char *key) {
	WeaponTableClipRing *ring = find_clip_ring(
			*static_cast<WeaponTableClipContext *>(opaque), key);
	if (ring == nullptr || ring->lengths.empty()) return -1.0f;
	const float seconds = ring->lengths[ring->head];
	ring->head = (ring->head + 1u) % ring->lengths.size();
	return seconds;
}

void build_clip_context(const DefWeaponDef &def, const ResourceIndex *resources,
		WeaponTableClipContext &out) {
	if (resources == nullptr) return;
	// No authored animadm = no anim object at Def+372, so every 'auto' field
	// collapses to zero through the empty-context lookup below ("Error, need
	// to define a anim adm") [orig: Anim_InitActions @0x542180..0x542198;
	// the per-block animadm buffer is consumed then cleared at each weapon
	// `end` by WeaponDefs_ResetParseState @0x53ff90].
	if (def.animadm[0] == '\0') return;
	simassets::AdmClipIndex clips;
	clips.load(resources, def.animadm);
	for (size_t i = 0; i < def.actions_count; ++i) {
		const char *key = def.actions[i].anim;
		if (key[0] == '\0') continue;
		const std::string lower = strutil::to_lower(key);
		bool duplicate = false;
		for (const WeaponTableClipRing &ring : out.rings) {
			if (ring.key == lower) {
				duplicate = true;
				break;
			}
		}
		if (duplicate) continue;
		const std::vector<float> *lengths = clips.lengths_for(key);
		if (lengths == nullptr || lengths->empty()) continue;
		out.rings.push_back(WeaponTableClipRing{lower, *lengths, 0});
	}
}

// The slot's TOTAL AMMO IN CLIPS [orig: WeaponSlot_GetTotalClips @0x5425F0]. The pool a fresh
// 0x2F accept fills is `requested != 0xFF ? min(requested, maxclips) * clipsize : startrounds`
// (§5.57 fill [orig: PlayerSlot_InitWeaponsFromLoadout @0x515550]); the count divides it back
// by clipsize. A negative result rides the signed byte to the wire (0xFF = the -1 "default /
// no count" sentinel the client answers with its own startrounds fallback).
int32_t clips_of(const world::WeaponTableEntry &e, uint8_t requested) {
	if (e.clipsize < 0) return -1; // no-clip weapon (knife/medpack/emplaced) [orig: @0x542669]
	const int32_t pool = requested != 0xFF
			? std::min<int32_t>(requested, e.maxclips) * e.clipsize
			: e.startrounds;
	int32_t clips = e.clipsize > 0 ? pool / e.clipsize : pool; // 0 divisor skipped [orig: @0x542670]
	if (clips > 127) clips = 127;                              // [orig: @0x542678]
	return clips;
}

} // namespace

uint8_t charfilter_bit(const char *token) {
	// [orig: token table @0x830EB0]
	if (token == nullptr) return 0;
	if (ci_equal(token, "medic")) return 0x01;
	if (ci_equal(token, "sniper")) return 0x02;
	if (ci_equal(token, "gunner")) return 0x04;
	if (ci_equal(token, "rifleman")) return 0x08;
	if (ci_equal(token, "engineer")) return 0x10;
	return 0; // unrecognized -> no bit (the original warns "unrecognized character type")
}

uint8_t teamfilter_bit(const char *token) {
	// [orig: token table @0x830ED8]
	if (token == nullptr) return 0;
	if (ci_equal(token, "red")) return 0x01;
	if (ci_equal(token, "blue")) return 0x02;
	return 0;
}

bool loadout_entry_permitted(const world::WeaponTableEntry &entry, uint8_t team,
                             uint8_t player_class) {
	// [orig: Server_SendWeaponSlotListToPlayer — team mask @0x502666, char mask @0x502693,
	//  the slot filter @0x502716]
	uint8_t team_mask;
	switch (team) {
		case 1:
		case 3:
			team_mask = 0x02; // blue
			break;
		case 2:
		case 4:
			team_mask = 0x01; // red
			break;
		default:
			team_mask = 0x03; // spectators/unknown see both sides
			break;
	}
	uint8_t char_mask = 0;
	if (player_class >= 5 && player_class <= 9)
		char_mask = static_cast<uint8_t>(1u << (player_class - 5));
	else if (player_class >= 1 && player_class <= 3)
		char_mask = 0xFF; // the pre-MP persona classes pass everything [orig: @0x50269a]
	return (team_mask & entry.teamfilter) != 0 && (char_mask & entry.charfilter) != 0;
}

LoadoutAmmoBytes resolve_loadout_ammo(const world::WeaponTable &table, uint8_t adm_index,
                                      uint8_t requested) {
	LoadoutAmmoBytes out;
	const world::WeaponTableEntry *e = table.by_index(adm_index);
	if (e == nullptr) return out;
	const int32_t primary = clips_of(*e, requested);
	out.primary = primary < 0 ? 0xFF : static_cast<uint8_t>(primary);
	// The alt-ammo byte: the FIRST sub-variant (parent+1..parent+LSC) whose ammo class differs
	// from the parent's — e.g. an M203HE under an M4M203AUTO — counted from its own startrounds
	// fill. [orig: @0x5027c8 walk, class compare @0x5027f8, count @0x502830-@0x502847]
	if (e->loadout_subclasses > 0) {
		for (uint8_t k = 1; k <= e->loadout_subclasses; ++k) {
			const world::WeaponTableEntry *sub =
					table.by_index(static_cast<uint8_t>(adm_index + k));
			if (sub == nullptr) continue;
			if (!ci_equal(sub->ammo_class.c_str(), e->ammo_class.c_str())) {
				const int32_t alt = clips_of(*sub, 0xFF);
				out.secondary = alt < 0 ? 0xFF : static_cast<uint8_t>(alt);
				break;
			}
		}
	}
	return out;
}

world::WeaponTable build_weapon_table(
		const DefWeaponsFile &weapons, const ResourceIndex *resources) {
	world::WeaponTable table;

	// Ammo classes share the engine's score-slot registry. Before weapon.def parses,
	// retail installs slot 0 plus ten built-ins; new `ammoclass_max_carry` names are
	// appended from slot 11. The numeric ids are observable in S2C 0x0F's fixed
	// 128-i32 player+88664 image, so a private first-seen registry is not equivalent.
	// [orig: fixed NameValue8 table @0x830F10; registration/cap parse @0x543873;
	// retail-ashi5a witness: GRENADEHE=12, .45=15, 5.56=23 under revx02]
	static constexpr const char *kBuiltinScoreSlots[] = {
			"", "CLASS_MANA", "CLASS_HP", "CLASS_POWER1", "CLASS_POWER2",
			"CLASS_POWER3", "CLASS_POWER4", "CLASS_POWER5", "CLASS_POWER6",
			"CLASS_POWER7", "CLASS_POWER8"};
	for (const char *name : kBuiltinScoreSlots) {
		table.ammo_class_names.emplace_back(name);
		table.ammo_class_caps.push_back(0);
	}
	auto ammo_class_register = [&table](const char *name) -> int {
		if (name == nullptr) name = "";
		int id = table.ammo_class_id_of(name);
		if (id >= 0) return id;
		table.ammo_class_names.emplace_back(name);
		table.ammo_class_caps.push_back(0);
		return static_cast<int>(table.ammo_class_names.size()) - 1;
	};

	// Top-level `ammoclass_max_carry <class> <n>` -> the per-class carry caps
	// [orig: parse @0x543873 -> the cap table @0x24E7DE0; clamp use @0x540b26].
	for (size_t i = 0; i < weapons.ammo_class_lines_count; ++i) {
		const char *line = weapons.ammo_class_lines[i];
		char cls[64] = {};
		int cap = 0;
		if (std::sscanf(line, "%*s %63s %d", cls, &cap) == 2) {
			int id = ammo_class_register(cls);
			table.ammo_class_caps[static_cast<size_t>(id)] = cap;
		}
	}

	// Entry 0: the engine-created "null" def — AnimDef_InitAll wipes the 255-entry table and
	// names slot 0 right before weapon.def parses [orig: @0x543615; Game_StartMission
	// @0x5254b3/@0x5254bd]. Its fields keep the InitEntryDefaults values.
	world::WeaponTableEntry null_entry;
	null_entry.name = "null";
	null_entry.valid = true;
	null_entry.ammo_class_id = 0;
	table.entries.push_back(std::move(null_entry));

	for (size_t i = 0; i < weapons.count; ++i) {
		const DefWeaponDef &d = weapons.entries[i];
		world::WeaponTableEntry e;
		e.name = d.weapon_name;
		e.valid = true;
		// Parse-time range clamps [orig: category @0x543999, rank @0x5439ed — warn + reset 0].
		e.category = (d.category >= 0 && d.category < 12) ? static_cast<uint8_t>(d.category) : 0;
		e.rank = (d.rank >= 0 && d.rank <= 64) ? static_cast<uint8_t>(d.rank) : 0;
		// The flat parser reads absent keys as 0; the engine entry defaults are clipsize 1 /
		// startrounds -1 [orig: AdmDef_InitEntryDefaults @0x53ff13/@0x53ff19]. No shipped
		// weapon.def uses an explicit 0 for either key, so 0 == absent here.
		e.clipsize = static_cast<int16_t>(d.clipsize == 0 ? 1 : d.clipsize);
		e.startrounds = static_cast<int16_t>(d.startrounds == 0 ? -1 : d.startrounds);
		e.turret_yaw_range_deg = static_cast<int16_t>(d.targetyawrange);
		e.turret_pitch_max_deg = static_cast<int16_t>(d.targetpitchmax);
		e.turret_pitch_min_deg = static_cast<int16_t>(d.targetpitchmin);
		e.maxclips = static_cast<int16_t>(d.maxclips);
		for (size_t c = 0; c < d.charfilter_count; ++c)
			e.charfilter |= charfilter_bit(d.charfilter[c]);
		for (size_t t = 0; t < d.teamfilter_count; ++t)
			e.teamfilter |= teamfilter_bit(d.teamfilter[t]);
		e.loadout_selectable = static_cast<uint8_t>(d.loadout_selectable != 0);
		e.loadout_subclasses = static_cast<uint8_t>(d.loadout_subclasses);
		e.ammo_class = d.ammo_class;
		e.ammo_class_count = static_cast<int16_t>(d.ammo_class_count);
		e.ammo_bucket = static_cast<int16_t>(d.ammobucket);
		e.round_type = d.round_type; // resolved to an AmmoTable index by
		                             // resolve_weapon_round_types (§5.60)
		e.attach_text_id = d.attach_text_id; // the attach-label Overlays key [orig: +0x3A0]
		e.flags = d.flags;
		e.flags2 = d.flags2;
		for (int row = 0; row < 6; ++row) e.error_fp16[row] = d.error_fp16[row];
		e.error_hip_theta_fp16 = d.error_hip_theta_fp16;
		e.error_up_theta_fp16 = d.error_up_theta_fp16;
		e.weaponweight_fp16 = d.weaponweight_fp16;
		e.clipweight_fp16 = d.clipweight_fp16;
		// The 3P body-channel triple — see the WeaponTableEntry contract. The motor
		// resolves these per ENTITY from its own equipped index, so they must live on
		// the table rather than on a local-player scalar.
		e.special_hold = d.special_hold;
		e.attack_anim = d.attack_anim;
		e.run_anim = d.run_anim;
		e.has_first_person_model_reference = d.gfx1[0] != '\0';
		e.third_person_model = d.gfx3; // the held 3P gun [orig: tpModel +0x170]
		// Bind this weapon's ACTION rows into the same 12-state descriptor table
		// consumed by a MountSlot. The production path supplies this definition's
		// ADM duration rings; assetless callers retain the witnessed unresolved-zero
		// fallback.
		// [orig: Anim_InitActions @0x541fa0; WeaponAction_ProcessFrame @0x540e60]
		std::vector<world::WeaponFsmActionRow> action_rows(d.actions_count);
		for (size_t a = 0; a < d.actions_count; ++a) {
			const DefWeaponAction &src = d.actions[a];
			world::WeaponFsmActionRow &dst = action_rows[a];
			std::memcpy(dst.name, src.name, sizeof(dst.name));
			std::memcpy(dst.anim, src.anim, sizeof(dst.anim));
			std::memcpy(dst.function, src.function, sizeof(dst.function));
			dst.delaystart = src.delaystart;
			dst.delayend = src.delayend;
			std::memcpy(dst.soundset, src.soundset, sizeof(dst.soundset));
			std::memcpy(dst.soundsetend, src.soundsetend, sizeof(dst.soundsetend));
			std::memcpy(dst.particle, src.particle, sizeof(dst.particle));
			std::memcpy(dst.particleuserpoint, src.particleuserpoint,
			            sizeof(dst.particleuserpoint));
		}
		WeaponTableClipContext clip_ctx;
		build_clip_context(d, resources, clip_ctx);
		world::weapon_fsm_bake(
				action_rows.data(), action_rows.size(),
				resources != nullptr ? table_clip_resolves : nullptr,
				resources != nullptr ? table_clip_seconds : nullptr,
				resources != nullptr ? &clip_ctx : nullptr, e.action_fsm);
		e.action_fsm.auto_fire = (d.flags & DEF_WEAPON_FLAG_AUTO) != 0;
		e.action_fsm.burst3 = (d.flags & DEF_WEAPON_FLAG_BURST) != 0;
		e.action_fsm.clip_capacity = e.clipsize;
		e.action_fsm.flags = d.flags;
		e.action_fsm.flags2 = d.flags2;
		// The heat model, already in the original's pre-divided 16.16 units
		// [orig: WeaponDef +0x36C/+0x370/+0x374].
		e.action_fsm.heat_per_shot = d.heat_per_shot;
		e.action_fsm.heat_decay_per_tick = d.heat_decay_per_tick;
		e.action_fsm.heat_glow_threshold = d.heat_glow_threshold;
		e.weapon_class_slot = d.weapon_class_slot;
		for (int k = 0; k < 7; ++k) e.classrounds[k] = d.classrounds[k];
		e.switchcategory = d.switchcategory;
		e.has_switchcategory = d.has_switchcategory != 0;
		e.ammo_class_id = static_cast<int16_t>(ammo_class_register(d.ammo_class));

		// Allocation rule [orig: WeaponDefs_ParseLineCallback @0x5436e1]: reuse an existing
		// same-name entry (re-parse override), else the LOWEST free slot [orig:
		// AdmDef_FindFreeSlot @0x53FC50] — append, since this lifecycle never frees entries.
		const int existing = table.index_of(d.weapon_name);
		if (existing >= 0) {
			table.entries[static_cast<size_t>(existing)] = std::move(e);
		} else if (table.entries.size() < 255) { // table capacity [orig: 255 entries @0x53fc6b]
			table.entries.push_back(std::move(e));
		}
	}
	return table;
}

} // namespace opennova::world
