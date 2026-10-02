// The sim-side items.def trait fold, moved verbatim from the shell binding's
// Simulation::resolve_item_traits / resolve_ai_weapons (ADR 0028). Every
// read that went through the item database's getter surface now reads the
// DefItemDef row directly; the getters were field-for-field projections, so
// the miss defaults (0 / empty / TYPE unset) are preserved exactly.

#include <runtime/mission/item_traits.h>
#include <runtime/assets/asset_store.h>
#include <runtime/audio/oneshot_play.h>
#include <formats/mission/mission.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/mission/seat_spec_extract.h> // stamp_minus_one_slot_window
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <runtime/terrain_query/height_field.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>

#include <string>
#include <utility>
#include <vector>

using namespace opennova::def;

namespace opennova::mission {

namespace {

// [orig: ItemDef_ResolveAllResources @0x49E5F0: primary profile, female
// profile, then explicit SHOT names. Explicit names replace only the sound;
// profile timing uses its already-Q16 words verbatim, without another x62.]
void bind_regional_sounds(world::World &world, const DefItemDef &def,
                         world::ItemDeathTraits &traits) {
    const auto *sets = world.tables.sound_sets;
    for (int region = 0; region < 4; ++region) {
        auto &shot = traits.regional_sounds[region];
        shot.name.clear();
        traits.regional_loops[region].clear();
        shot.base_ticks = def.shot_delay_ticks[region][0];
        shot.range_ticks = def.shot_delay_ticks[region][1];
    }
    for (const char *profile_name : {def.sound_profile, def.sound_profile_female}) {
        if (*profile_name == 0) continue;
        const auto *profile = world.tables.sound_profiles.find(profile_name);
        if (profile == nullptr || sets == nullptr) continue;
        for (int region = 0; region < 4; ++region) {
            if (sets->has(profile->set_names[region]))
                traits.regional_loops[region] = profile->set_names[region];
            const int slot = audio::kSlotShotDawn + region;
            if (!sets->has(profile->set_names[slot])) continue;
            auto &shot = traits.regional_sounds[region];
            shot.name = profile->set_names[slot];
            shot.base_ticks = profile->param2_q16[slot];
            shot.range_ticks = profile->param3_q16[slot];
        }
    }
    const char *names[] = {def.dawnshot, def.dayshot, def.duskshot, def.nightshot};
    for (int region = 0; region < 4; ++region) {
        if (def.soundloops[region][0])
            traits.regional_loops[region] = sets && sets->has(def.soundloops[region]) ?
                    def.soundloops[region] : "";
        if (*names[region] == 0) continue;
        traits.regional_sounds[region].name =
                sets != nullptr && sets->has(names[region]) ? names[region] : "";
    }
}

// The by-id view over the parsed file: an id resolves to its FIRST row in
// load order. Every retail lookup of a type id is the linear scan from row 0
// that returns the first match, and the entity's ItemDef is the row at that
// index, so a later row repeating an id is never reached (the shipped ITEMS.DEF
// repeats 102044: "Map Named Location" first, "Power Up Med Pack Infinite"
// later). [orig: ItemList_FindIndexByTypeId @0x49E100 — `cmp [ecx],esi; jz`
// @0x49E120..0x49E122 returns the first hit; Entity_SpawnFromBMSRecord
// ItemTypeIndex @0x40EBFC, ItemDef = g_ItemDefs + index @0x40EBFF..0x40EC07]
std::unordered_map<int, const DefItemDef *> index_items(
        const DefItemsFile &items) {
    std::unordered_map<int, const DefItemDef *> by_id;
    by_id.reserve(items.count);
    for (size_t i = 0; i < items.count; ++i)
        by_id.emplace(items.entries[i].id, &items.entries[i]);
    return by_id;
}

const DefItemDef *find_item(
        const std::unordered_map<int, const DefItemDef *> &by_id, int def_id) {
    const auto it = by_id.find(def_id);
    return it == by_id.end() ? nullptr : it->second;
}

// The case-folded fourcc prefix of an items.def class tag: the retail
// callback table keys 4-byte tags and items.def authors longer tokens onto
// them (`cbike`, `ctank`, `catv`, mixed-case `CHel`) — whole-string matching
// sent the shipped Motorcycle down the Ground motor. Same rule as replication's
// motion_family_from_tag; the two classifiers must agree (ADR 0026 §4).
std::string fourcc_prefix(const char *tag) {
    std::string out;
    for (int i = 0; i < 4 && tag[i] != '\0'; ++i)
        out.push_back(strutil::ascii_tolower(tag[i]));
    return out;
}

} // namespace

// Stamp every live entity's items.def-derived wire traits from the parsed def rows:
// - Entity::is_ai_capable from ItemDefAttrib & 0x100000 (AIData): the host's pool-1 0x0D stream
//   emits its AI-trailer iff AI-capable, matching the stock 0x0D decoder's own gate exactly
//   (itemDef.attrib & 0x100000 @0x433327) — byte-faithful AND crash-safe (D-NET-97).
// - Entity::net_class_code from the items.def class tag (ai_function, else move_function — the
//   directive that drives the ItemDef+356 serialize-callback lookup [orig: ingame_decode.h §5.10b])
//   via the injected wire-class supplier. Load-bearing: only witnessed callback classes may be
//   serialized into the 0x0A event loop — classifying a pool-1 ewep emplacement as a vehicle
//   desyncs the retail client mid-frame (retail-join v13, 2026-07-02).
// - Entity::health_max (+ health lift) from items.def hp (itemDef+0x17C healthMax): the original
//   spawns Health = healthMax [orig: Entity_InitFromItemDef @0x49e550]; entities still at the
//   promotion default (100) are lifted to full health. Feeds the §5.13 vehicle health word (a
//   too-small value renders every vehicle burning) and the §5.10 field-17 tier denominator.
//
// The registry's for_each is const-only, so collect the live handles first, then re-fetch each as
// a mutable Entity*.
//
// ID SPACE (load-bearing): Entity::item_id is the WIRE type id — the small on-disk .bms type that
// build_pool*_batch puts on the wire verbatim (e.g. 0x050E). items.def rows key the def id, which
// is wire + kItemIdOffset (mission_bms_test: bms_type_id 1291 -> item_id 101291; the net client's
// wire id = def_id - 100000). The offset here is mandatory: without it every pool-1 lookup misses.
// [orig: NapiNPClientMsg_0x00D @0x432c40; docs/net/novaworld-net-re.md D-NET-97]
void resolve_item_traits(world::World &world, const DefItemsFile &items,
                         const ItemWireClassFn &wire_class, world::EntityHandle only) {
    const std::unordered_map<int, const DefItemDef *> by_id = index_items(items);
    // Cache the Player template's items.def hp at world level so LATE-JOINER spawns (which happen
    // after this sweep) seed full health without an item-db reach-back [orig:
    // Entity_InitFromItemDef @0x49e550 — spawn Health = itemDef->healthMax]. (D-NET-144)
    const int player_def_id =
            static_cast<int>(world::kPlayerInfantryTypeId) + mission::kItemIdOffset;
    const DefItemDef *player_def = find_item(by_id, player_def_id);
    // The user-waypoint row Waypoint_CreateForPlayer seeds pool 4 from
    // (world/user_waypoints.h) [orig: ItemList_FindIndexByTypeId(6089) @0x4dfcf0].
    const DefItemDef *waypoint_def = find_item(by_id,
            static_cast<int>(world::kUserWaypointTypeId) + mission::kItemIdOffset);
    world.tables.user_waypoint_type_index =
            waypoint_def != nullptr ? static_cast<int32_t>(waypoint_def - items.entries) : 0;
    world.tables.user_waypoint_item_type =
            static_cast<uint8_t>(waypoint_def != nullptr ? waypoint_def->type : 0);
    world.tables.player.has_item_def = player_def != nullptr;
    world.tables.player.item_type_index =
            player_def != nullptr ? static_cast<int32_t>(player_def - items.entries) : 0;
    world.tables.player.item_hp =
            world::retail_signed_i16(player_def != nullptr ? player_def->hp : 0);
    world.tables.player.critical_hp =
            world::retail_signed_i16(player_def != nullptr ? player_def->critical_hp : 0);
    world.tables.player.item_type =
            static_cast<uint8_t>(player_def != nullptr ? player_def->type : 0);
    world.tables.player.item_attrib = player_def != nullptr ? player_def->attrib : 0u;
    world.tables.player.armor_impact = world::retail_signed_i16(
            player_def != nullptr ? player_def->armor_impact : 0);
    world.tables.player.armor_kz = world::retail_signed_i16(
            player_def != nullptr ? player_def->armor_kz : 0);
    world.tables.player.damage_reduc_pp =
            player_def != nullptr ? player_def->damage_reduc_pp : 0.0f;
    world.tables.player.damage_reduc_max =
            player_def != nullptr ? player_def->damage_reduc_max : 0.0f;
    world.tables.player.radar_sig = player_def != nullptr ? (player_def->radar_sig & 0xFFFF) : 0;
    world.tables.player.heat_sig = player_def != nullptr ? (player_def->heat_sig & 0xFFFF) : 0;
    std::vector<world::EntityHandle> handles;
    world.registry.for_each(
            [&](const world::Entity &e) {
                if (!only.valid() || e.handle == only) handles.push_back(e.handle);
            });
    for (const world::EntityHandle h : handles) {
        world::Entity *e = world.registry.get(h);
        if (!e) continue;
        const int def_id =
                static_cast<int>(e->item_id) + mission::kItemIdOffset;
        const DefItemDef *def = find_item(by_id, def_id);
        e->has_item_def = def != nullptr;
        // The resolved row's load-order ordinal, 0 when no row matches (the
        // "Null" row is ordinal 0 too): each `begin` appends the next row.
        // [orig: entity+0x1C = ItemList_FindIndexByTypeId(type) @0x40EBFC; the
        //  `begin` arm of ItemDef_ParseProperty @0x49EBA8 allocates the row,
        //  ItemDef_AllocateWithDefaults @0x49E3BE bumps g_ItemCount]
        e->item_type_index = def != nullptr ? static_cast<int32_t>(def - items.entries) : 0;
        // The org1 initializer seeds this magazine even without an ammo name.
        // Bind the definition value here; a later traits refresh must not refill it.
        // [orig: Entity_InitOrganicAI @0x4BFE0D, def+0x894]
        if (world::AiEntity *body = world.ai.for_handle(h))
            body->profile.clip_size = def != nullptr ? def->clipsize : 0;
		e->vehicle_spawn_ids.clear();
		e->vehicle_spawn_groups.clear();
		e->vehicle_bay_flags = 0;
		if (def != nullptr) {
			for (int group = 0; group < items.vehicle_spawn_id_count; ++group)
				if ((def->vehicle_spawn_mask & (uint32_t(1) << group)) != 0) {
					e->vehicle_spawn_ids.push_back(items.vehicle_spawn_ids[group]);
					e->vehicle_spawn_groups.push_back(static_cast<uint8_t>(group));
					// A bay ORs each spawn group's flags, taken from the
					// group item's unitType (an unresolved id reads the
					// Null row, whose unitType sets none).
					// [orig: ItemDefs_LoadAndValidate @0x4a1fa7..0x4a1fcf,
					//  @0x4a2010..0x4a2058]
					if ((def->attrib2 & 1u) == 0) continue;
					const DefItemDef *member =
							find_item(by_id, items.vehicle_spawn_ids[group]);
					const int unit = member != nullptr ? member->unit_type : 0;
					if (unit == 1 || unit == 2 || unit == 12) e->vehicle_bay_flags |= 1u;
					else if (unit == 3 || unit == 4) e->vehicle_bay_flags |= 2u;
					else if (unit >= 5 && unit <= 8) e->vehicle_bay_flags |= 4u;
				}
		}
		e->item_type = static_cast<uint8_t>(def != nullptr ? def->type : 0);
		// entity+0x30: the def's named graphic (the model Entity_InitFromModel
		// attaches); a marker never carries one.
		// [orig: Entity_InitFromModel @0x40df06; the persistent-bank gate
		//  MapOverlay_RenderAllByLayer @0x5BE6C4]
		e->has_graphic_model = def != nullptr && def->graphic[0] != '\0' &&
				e->kind != world::EntityKind::Marker;
		// The physics callback table's ewep row selects the gun update.
		// [orig: g_EntityClassPhysicsTable row @0x82ABE0 -> Entity_UpdateTransformAndTurret @0x440CA0]
		e->emplaced_update = def != nullptr && strutil::iequals(def->move_function, "ewep");
		// The render tag picks the def+0x144 CTRL callback; the ewep row's
		// publishes the gun words (world/mount_controls.h).
		// [orig: EntityDef_InitAllCallbacks @0x4A5AEA..0x4A5B03 ->
		//  BoneCallback_LookupByTag @0x4E32ED..0x4E3306, row 'ewep' @0x82CFA0]
		e->emplaced_ctrl_publisher =
				def != nullptr && fourcc_prefix(def->render_function) == "ewep";
		e->render_sway = def != nullptr && fourcc_prefix(def->render_function) == "sway";
		e->light_transfer = def != nullptr ? def->light_transfer : 0.0f;
        e->reverb = def != nullptr ? int16_t(def->reverb) : 0;
		e->uniform_scale_q16 = def != nullptr ? def->scale_q16 : 0;
		// Both ItemDefAttrib words and the per-entity facts derived from them
        // (AI-capable, the AS zone gates, LeaveCorpse) go through the ONE
        // stamp a runtime override also uses (world/entity.h stamp_item_attrib).
        // [orig: def+84 gates in ZoneSlotChain_BuildFromMission @0x4a2de0 /
        // Server_ResolveSpawnTargetHandle @0x4fe110; net-re §5.61; LeaveCorpse
        // ItemDef_ParseProperty @0x4a09d3, consumer Entity_UpdateInfantryAI
        // @0x4b9e54; world-wac-ai-re §19]
        const uint32_t attrib = def != nullptr ? def->attrib : 0u;
        world::stamp_item_attrib(*e, attrib, def != nullptr ? def->attrib2 : 0u);
        // The injected catalog supplies both the authoritative host stamp and
        // the decoded-client record width. A missing or unresolved definition
        // fails closed as Unknown (0).
        e->net_class_code = wire_class ? wire_class(def_id) : 0;
        // items.def hp -> healthMax; lift spawn-default health to full [orig: @0x49e550].
        const int hp = world::retail_signed_i16(def != nullptr ? def->hp : 0);
        e->health = world::retail_signed_i16(e->health);
        e->health_max = world::retail_signed_i16(e->health_max);
        e->critical_hp = world::retail_signed_i16(def != nullptr ? def->critical_hp : 0);
        e->mana_max = e->mana = world::retail_signed_i16(def != nullptr ? def->mana : 0);
        e->music_location = world::retail_signed_i16(def != nullptr ? def->music_location : 0);
        e->armor_impact = world::retail_signed_i16(
                def != nullptr ? def->armor_impact : 0);
        e->armor_kz = world::retail_signed_i16(
                def != nullptr ? def->armor_kz : 0);
        e->damage_reduc_pp = def != nullptr ? def->damage_reduc_pp : 0.0f;
        e->damage_reduc_max = def != nullptr ? def->damage_reduc_max : 0.0f;
        // items.def radarsig/heatsig -> the AI acquisition per-candidate engage caps
        // [orig: Entity_InitFromModel @0x40e136-0x40e15d — entity+422 = def+376
        // radarSig (primary-FOV cap), entity+420 = def+378 heatSig (secondary)].
        e->radar_sig = def != nullptr ? (def->radar_sig & 0xFFFF) : 0;
        e->heat_sig = def != nullptr ? (def->heat_sig & 0xFFFF) : 0;
        if (hp != 0) {
            e->health_max = hp;
            if (e->health == 100) e->health = hp; // still at the promotion default
        }
        // Indestructible item (def hp == 0): entity Flags |= 0x4000000 and subType = 0xFF —
        // the def-sourced half of the 0x10 static record's flag dword / flag-0x80 byte
        // (D-NET-147; every golden ASH_I5A building carries both) — Health = 1, and the
        // def's two armor words become the invulnerable 0xFFFF whatever it authored, so
        // every armor reader sees them: the AI target walk skips the item (a pair of -1
        // armor words) and the damage gates zero its hits. Resolved defs only — a
        // missing items.def id stays untouched. [orig: Entity_InitFromModel @0x40dc8e:
        // !itemDef->healthMax -> Flags |= 0x4000000 @0x40DC8E, def+0x190 / def+0x192 =
        // 0xFFFF @0x40DC95 / @0x40DC9F, Health = 1 @0x40DCA6, subType = -1 @0x40DCAF;
        // the armor gate Entity_FindTargets @0x53AC3F..0x53AC59]
        if (hp == 0 && def != nullptr) {
            e->engine_flags |= 0x4000000u;
            e->health = 1;
            e->sub_type = 0xFF;
            e->armor_impact = -1;
            e->armor_kz = -1;
            // On an addeweap child this runs after the spawn stamped the slot
            // subType and before the ewep class init reads it: the init then
            // reads its anchor index at subType -1, the byte before the carrier
            // def's slot table (the top byte of slot 4's raw child type, 0 for
            // any id below 2^24; 0xFF when that id sits under 100000), neither a
            // userpoint index, so the child rides its carrier's root with no
            // anchor offset, and no C/G designation slot matches it.
            // [orig: Entity_SpawnWeaponOverlays @0x40F40E then Entity_InitFromModel
            //  @0x40F414 (subType = -1 @0x40DCAF) then the init callback @0x40F43E;
            //  Entity_InitBoneReferences @0x4415F1..0x44164A; ItemDef_ParseProperty
            //  addeweap type - 100000 @0x4A1BA3..0x4A1BAD]
            if (e->emplacement_parent.valid()) {
                e->emplacement_bone = 0;
                e->emplacement_anchor_subobject = -1;
                e->emplacement_local = {};
                e->emplacement_yaw_offset = 0;
                e->emplacement_attachment_flags = 0;
                // Its turret window is the carrier def's slot tables read at -1
                // (stamp_minus_one_slot_window): light_transfer's bits and slot
                // 4's down/up/right, straight from the def rows.
                const world::Entity *carrier = world.registry.get(e->emplacement_parent);
                const DefItemDef *carrier_def = carrier != nullptr
                        ? find_item(by_id, static_cast<int>(carrier->item_id) +
                                  mission::kItemIdOffset)
                        : nullptr;
                const DefItemEmplacementAttachment *slot4 =
                        carrier_def != nullptr && carrier_def->emplacement_attachments != nullptr &&
                                carrier_def->emplacement_attachments_count > 3
                        ? &carrier_def->emplacement_attachments[3]
                        : nullptr;
                stamp_minus_one_slot_window(*e,
                        carrier_def != nullptr ? carrier_def->light_transfer : 0.0f,
                        slot4 != nullptr ? slot4->down_angle : 0,
                        slot4 != nullptr ? slot4->up_angle : 0,
                        slot4 != nullptr ? slot4->right_angle : 0);
            }
        }
        // Death-presentation timing: deathtime (def+0x890, parse-scaled ticks)
        // seeds the corpse timer at the death edge; LeaveCorpse rides the stamp
        // above. [orig: ItemDef_ParseProperty @0x49fa6c; consumer
        // Entity_UpdateInfantryAI @0x4b9c97; world-wac-ai-re §19]
        e->deathtime_ticks = def != nullptr ? def->deathtime_ticks : 0;
        if (def != nullptr && !e->destroy_timer_initialized) {
            // [orig: Entity_InitFromItemDef @0x49E550]
            e->destroy_timer = def->destroy_timing_ticks[0];
            e->destroy_timer_initialized = true;
            // The move-function row selects only the update callback, the
            // squib motor [orig: the "squib" row @0x82AC88 of the 12-byte move
            // table @0x82AC40 -> Entity_ProcessProjectileTravel @0x448D50].
            if (strutil::iequals(def->move_function, "squib")) e->squib.motor = true;
            // The +0x160/+0x26C/+0x2B0/+0x2C8 init is the ai_function class
            // row's second slot, run through def+0x148 whatever the move
            // function [orig: sub_448CE0 @0x448CE0, row @0x813120 +12;
            //  Entity_LookupRenderCallbacks stores it @0x407E36].
            if (strutil::iequals(def->ai_function, "squib")) {
                e->equipped_adm_index = 0;
                e->squib.spread_q16 = static_cast<int32_t>(def->door_type);
                if (def->primary_weapon[0]) {
                    e->equipped_adm_index = world.tables.weapons.index_of(def->primary_weapon);
                    const auto *weapon = world.tables.weapons.by_index(e->equipped_adm_index);
                    if (weapon) e->squib.ammo_index = e->squib.damage_ammo_index =
                            std::max(0, int(weapon->ammo_index));
                }
            }
            if (strutil::iequals(def->move_function, "upfx"))
                e->death_motion = world::DeathMotionMode::BuildingEffects;
            else if (strutil::iequals(def->move_function, "psec"))
                e->death_motion = world::DeathMotionMode::PalmPiece;
            // The model init builds the entity matrix once and marks it built
            // for a def no mover will rebuild: a decoration, building or
            // powerup/object type, or an entity with no update callback — never
            // true of a resolved def, whose callback lookup falls back to the
            // "null" row [orig: Entity_InitFromModel @0x40E0D4..0x40E105;
            //  EntityDef_LookupPhysicsCallback @0x4A9272]. Once per entity, as
            //  the init runs once; a later clear (teleport, death) stays.
            if (def->type == DEF_ITEM_TYPE_DECORATION || def->type == DEF_ITEM_TYPE_BUILDING ||
                    def->type == DEF_ITEM_TYPE_POWERUP)
                e->engine_flags |= world::kEntityFlagMatrixBuilt;
        }
        if (def && (strutil::iequals(def->ai_function, "palm") ||
                strutil::iequals(def->ai_function, "psec")))
            e->palm_sections = true;
        // The ai_function palm row's callback is WeaponOverlay_HandleDamage and
        // the move_function psec row's update is Entity_UpdatePhysicsStep: the two
        // callbacks the load serializers test before streaming entity+0x270.
        // [orig: class rows palm @0x813268 (callback @0x813278), psec @0x82AD4C
        //  (update @0x82AD5C); the tests @0x503F4C..0x503F65, @0x504554..0x50456D]
        e->palm_state_streamed = def != nullptr &&
                (strutil::iequals(def->ai_function, "palm") ||
                 strutil::iequals(def->move_function, "psec"));
        e->door_event = def != nullptr && fourcc_prefix(def->ai_function) == "door";
        e->door_motion = def != nullptr && fourcc_prefix(def->move_function) == "door";
        // Every def carries its two +0x6F3/+0x70B sound names; the door
        // commands play them, and so does the drop of a carried object off
        // its carrier (world::drop_object_from_carrier).
        // [orig: ItemDef doorOpenSound +0x6F3 / doorCloseSound +0x70B;
        //  Entity_DropCarriedObject @0x439f06]
        if (def != nullptr) {
            std::memcpy(e->door_open_sound, def->door_open_sound, sizeof(e->door_open_sound));
            std::memcpy(e->door_close_sound, def->door_close_sound, sizeof(e->door_close_sound));
        }
        // The mission spawn allocates closed rows for a building or decoration
        // door; a joiner's pool-2 rows are the S2C 0x10 record's instead: any
        // door def, its open sections from the record's section word.
        // [orig: Entity_SpawnFromBMSRecord @0x40F25D..0x40F2DA (types 5/2, attrib
        //  0x80); NapiNPClientMsg_0x010 @0x4336B5..0x433745 (attrib 0x80 alone)]
        const bool wire_doors = !world.rules.logic_authority && e->handle.pool() == 2;
        if (def != nullptr && (def->attrib & DEF_ITEM_ATTRIB_DOOR) != 0 &&
                (wire_doors || def->type == DEF_ITEM_TYPE_BUILDING ||
                 def->type == DEF_ITEM_TYPE_DECORATION)) {
            e->door_count = static_cast<int8_t>(def->deathtime_ticks);
            e->door_first_bone = static_cast<int8_t>(static_cast<uint32_t>(def->deathtime_ticks) >> 8);
            if (wire_doors)
                world.doors.initialize_from_wire(*e, def->door_open_rate_q16,
                        def->door_max_angle_bam, e->section_mask);
            else
                world.doors.initialize(*e, def->door_open_rate_q16, def->door_max_angle_bam);
        }
        // The item's display name, once per distinct id (tooling: the
        // inspection records name an entity by its item, not only its label).
        if (def != nullptr && world.tables.item_names.get(e->item_id) == nullptr)
            world.tables.item_names.set(e->item_id, def->display_name);
        // Destruction traits (world/destruction.h; world-wac-ai-re §24): the death
        // chain's def fields, keyed by item id. Fills once per distinct id.
        // [orig: the ItemDef fields Entity_ApplyWeaponDamage / the death dispatch /
        // Entity_InitDeathSounds read — armor +0x190/+0x192, unitType +0x196, kz
        // +0x198, huskSubPart* +0x100.., debrisScale +0x1BC, soundDeath +0x860,
        // the particledeath family +0x412..; the event/death callback row
        // def+0x138 resolved from the ai_function tag by the whole-string
        // stricmp walk of g_EntityClassEventCallbackTable @0x813000
        // (Entity_LookupRenderCallbacks @0x407dc0 via EntityDef_InitAllCallbacks
        // @0x4a5aa9, "Null" for an empty tag)]
		e->hud_image = def != nullptr ? def->hud_image : "";
        e->item_unit_type = def != nullptr ? def->unit_type : 0;
        // [orig: Score_ProcessKillEvent @0x4FD400 (the def+0x194 read @0x4FD422)]
        e->item_score = def != nullptr ? def->score : 0;
        if (world.tables.item_death_traits.get(e->item_id) == nullptr &&
                def != nullptr) {
            world::ItemDeathTraits t;
            t.death_class = world::item_death_class_from_tag(def->ai_function);
            bind_regional_sounds(world, *def, t);
            std::copy(std::begin(def->destroy_timing_ticks), std::end(def->destroy_timing_ticks),
                    std::begin(t.destroy_timing_ticks));
            t.physics = def->physics;
            t.squib_distance_q16 = def->clipsize;
            t.squib_ammo = def->ammo_marker3;
            t.particlefx = def->particlefx.effect;
            t.unit_type = def->unit_type;
            t.kz = def->kz;
            // An hp-0 def's armor words already read 0xFFFF here: its entity's
            // init overwrote them [orig: Entity_InitFromModel @0x40DC95 / @0x40DC9F].
            const bool hp_zero = world::retail_signed_i16(def->hp) == 0;
            t.armor_impact = hp_zero ? -1 : def->armor_impact;
            t.armor_blast = hp_zero ? -1 : def->armor_blast;
            // The S&D/A&D objective target's same-team blast immunity [orig: the
            // blast applier's same-team gate, jo-c 261654: attacker team == target
            // team && itemDef->attrib & 0x8000 -> return].
            t.team_protect = (attrib & DEF_ITEM_ATTRIB_SD) != 0;
            t.no_die = (attrib & DEF_ITEM_ATTRIB_NODIE) != 0;
            t.static_death =
                    (def->attrib2 & DEF_ITEM_ATTRIB2_STATICDEATH) != 0;
            t.has_husk = def->husk[0] != '\0' || def->huskfinal[0] != '\0';
            t.is_decoration = def->type == DEF_ITEM_TYPE_DECORATION;
            t.husk_sub_part_count = static_cast<uint8_t>(
                    std::clamp(def->husk_sub_parts, 0, 255));
            for (int s = 0; s < 16; ++s)
                t.husk_sub_part_types[s] = def->husk_sub_part_types[s];
            t.debris_scale = def->debris_scale;
			t.sound_profile = def->sound_profile;
			t.sound_death = def->sounddeath;
			t.particlespawn = def->particlespawn;
			t.particledeath = def->particledeath;
			t.particleh2odeath = def->particleh2odeath;
            t.particlefire = def->particlefire;
            t.particleother = def->particleother;
            t.particlefinale = def->particlefinale;
            // The collision-instance sweep enriches this row with live
            // husk-model state and the active first-stage husk's KZ user points.
            world.tables.item_death_traits.set(e->item_id, std::move(t));
        }
        // Vehicle mover traits: the pre-scaled items.def block + PlayerControl
        // attrib (0x40), keyed by item id. Ground-family dispatchers test the
        // `physics` selector, but CHel/cpln dispatch DIRECTLY to the air mover and
        // shipped CHel rows legitimately omit that ground selector. [orig:
        // g_EntityClassPhysicsTable @0x82abc8; ground dispatch @0x48ef90..0x48f060;
        // Entity_UpdateAircraftPhysics @0x490310]
        if (e->handle.pool() == 1 &&
                world.vehicles.traits.get(e->item_id) == nullptr &&
                def != nullptr) {
            const std::string fam = fourcc_prefix(def->move_function);
            const bool direct_air_mover = fam == "chel" || fam == "cpln";
			if (def->physics != 0 || direct_air_mover || fam == "cveh" || fam == "ctan" ||
					fam == "cbik" || fam == "cbot" || fam == "catv" || fam == "ctrn") {
				world::VehicleTraits vt;
				vt.physics = def->physics;
				vt.player_speed = def->player_speed;
                vt.acceleration = def->acceleration;
				vt.slip_speed = def->slip_speed;
				vt.deceleration = def->deceleration;
				vt.turn_rate = def->turn_rate;
				vt.turn_rate2 = def->turn_rate2;
                vt.unit_type = def->unit_type;
                vt.torque = def->torque;
                vt.water_speed = def->water_speed;
                vt.climb_speed = def->climb_speed;
                vt.turn_roll = def->turn_roll;
                vt.speed_pitch = def->speed_pitch;
                // The platform slope thresholds + tuning block. The def parser's
                // "pitch"/"pitch_velocity" tokens are the traits' bow-lift pair
                // (pitch_lift/pitch_lift_vel) — speed_pitch above is the distinct
                // air pitch-rate cap.
                vt.max_slope = def->max_slope;
                vt.slip_slope = def->slip_slope;
                vt.mass = def->mass;
                vt.lean = def->lean;
                vt.lean_velocity = def->lean_velocity;
                vt.pitch_lift = def->pitch;
                vt.pitch_lift_vel = def->pitch_velocity;
                vt.bob = def->bob;
                vt.flip = def->flip;
                vt.hand_brake = def->hand_brake;
				vt.tire_slip = def->tire_slip;
				vt.min_ai = def->min_ai;
				vt.critical_hp = def->critical_hp;
				vt.critical_drain = def->critical_drain;
                vt.non_critical_regen = def->non_critical_regen;
                vt.spring = def->spring;
                vt.spring_comp = def->spring_comp;
                vt.shock = def->shock;
				// The two afloat boat lanes stay paired with their authored
				// model anchors. Their live speed controls are sampled by the
				// shared watercraft mover; W1/W2 and particlefxs feed their
				// corresponding ground, transition and skid paths.
				vt.skid_effect = def->particlefxs.effect;
				vt.skid_snow_effect = def->particlefxs.secondary_effect;
				vt.skid_userpoint = def->particlefxs.userpoint;
				const def::DefItemParticleFx *trail_defs[4] = { &def->particlefxw1,
					&def->particlefxw2, &def->particlefxw3, &def->particlefxw4 };
				for (int i = 0; i < 4; ++i) {
					vt.trails[i].effect = trail_defs[i]->effect;
					vt.trails[i].secondary_effect = trail_defs[i]->secondary_effect;
					vt.trails[i].userpoint = trail_defs[i]->userpoint;
				}
				// def->top_heavy is parsed for parity but dead in retail —
				// no consumer, so the traits do not carry it.
				vt.player_control = (attrib & DEF_ITEM_ATTRIB_PLAYERCONTROL) != 0;
				// The `Parent` byte (ItemDef+0x548): the gunner-attachment gate
				// VehicleSystem::setup_gunner_attachments tests [orig:
				// Entity_InitVehicleAIFromDef @0x46895A; Entity_InitHelicopterAIFromDef
				// @0x468688].
				vt.attrib_parent = def->attrib_parent != 0;
				// The per-frame physics mover is selected exclusively by the
                // move_function callback resolved into itemDef+0x158; ai_function
                // selects the event/brain callback through its own lookup (the
                // brain_class below). [orig: EntityDef_LookupPhysicsCallback
                // @0x4a9240; §5.38e movers]
                if (fam == "cbot") {
                    vt.family = world::VehicleFamily::Watercraft;
                } else if (fam == "chel") {
                    vt.family = world::VehicleFamily::Helicopter;
                } else if (fam == "cpln") {
                    vt.family = world::VehicleFamily::Plane;
                } else if (fam == "cbik") {
                    vt.family = world::VehicleFamily::Bike;
                } else if (fam == "ctan") {
                    // The shipped M1A1/T80 author `ctank`; the 4-byte key
                    // is ctan — its own class-table row routes the tank
                    // mover + the wheeled contact solve [orig: @0x82ABC0
                    // ctan -> @0x48f000 ->
                    // Entity_UpdateTankVehiclePhysics @0x488AB0].
                    vt.family = world::VehicleFamily::Tank;
                } else {
                    vt.family = world::VehicleFamily::Ground;
                    // catv rides the generic dispatcher, which passes
                    // hasWaterLevel=2 into the ground mover — arming the
                    // contact solve's pad water-support forces (the
                    // Stryker/BTR-80 are catv) [orig: @0x48f010 push 2 vs
                    // the cveh/ctrn dispatchers' push 0 @0x48efce/@0x48f06e].
                    vt.amphibian = fam == "catv";
                }
				const std::string render = fourcc_prefix(def->render_function);
				vt.render_family = render == "cveh" ? world::VehicleRenderFamily::Ground
						: render == "tank"			? world::VehicleRenderFamily::Tank
						: render == "chel"			? world::VehicleRenderFamily::Helicopter
						: render == "cpln"			? world::VehicleRenderFamily::Plane
													: world::VehicleRenderFamily::None;
				// The brain machine class is keyed by ai_function through the class
				// event-callback table (stricmp, 24-byte rows): CHel @0x8132a0 and
				// the cpln thunk @0x8133a8 -> EntityAI_ProcessAirStateMachine
				// @0x4581b0 (the air machine); cveh @0x813378, cbot @0x813390 and
				// ctrn @0x8133c0 -> EntityAI_ProcessGroundStateMachine @0x4583c0.
				// [orig: g_EntityClassEventCallbackTable @0x813000 resolved by
				// EntityDef_InitAllCallbacks @0x4a5aae -> Entity_LookupRenderCallbacks
				// @0x407dc0]
				const std::string brain = fourcc_prefix(def->ai_function);
				vt.brain_class = brain == "chel" || brain == "cpln"
						? world::VehicleBrainClass::Air
						: brain == "cveh" || brain == "cbot" || brain == "ctrn"
						? world::VehicleBrainClass::Ground
						: world::VehicleBrainClass::Unset;
				// Vehicle audio belongs to the vehicle ItemDef, not to the
				// mounted NPC's AiProfile. Resolve the profile name and the
				// item-level soundloop overrides once at this portable boundary.
				// [orig: ItemDef_ResolveAllResources @0x49e5f0/@0x49e7f0]
				vt.sound_profile = def->sound_profile;
				for (size_t i = 0; i < vt.sound_loops.size(); ++i)
                    vt.sound_loops[i] = def->soundloops[i];
                world.vehicles.traits.set(e->item_id, vt);
			}
		}
	}
	if (only.valid()) return; // runtime allocation must not reset active zones
	// Throwable class bindings: every items.def entry whose ai_function /
    // move_function names a throwable class (nade/schl/clym/vmne/lndm) lands a
    // row keyed by type id (id - 100000, the ammo TrcrID space), with the def
    // hp/armor the placed device spawns at. [orig: EntityDef_InitAllCallbacks
    // @ 0x4a5a70 resolves the class tables into every item def at load;
    // world-wac-ai-re §27.]
    world.throwables.classes.clear();
    for (const auto &entry : by_id) {
        const int def_id = entry.first;
        const DefItemDef *def = entry.second;
        const world::ThrowClass think =
                world::throw_class_from_tag(def->ai_function);
        const world::ThrowClass motor =
                world::throw_class_from_tag(def->move_function);
        if (think == world::ThrowClass::kNone &&
                motor == world::ThrowClass::kNone)
            continue;
        world::ThrowableClassRow row;
        row.item_id = def_id - mission::kItemIdOffset;
        row.item_type_index = static_cast<int32_t>(def - items.entries);
        row.think = think;
        row.motor = motor;
        row.health_max = world::retail_signed_i16(def->hp);
        row.armor_impact = world::retail_signed_i16(def->armor_impact);
        row.armor_kz = world::retail_signed_i16(def->armor_kz);
        world.throwables.classes.set(row);
    }
    // The AS zone-slot chain — built AFTER the trait stamp (zone registration keys on
    // is_capture_trigger), then the secure latch seeds each rear zone's control to 1.0.
    // [orig: ZoneSlotChain_BuildFromMission @0x4a2de0 from Game_StartMission @0x526126;
    // the latch is Server_UpdateCaptureZoneEntities' first act @0x519764; net-re §5.61]
    world.zones.capture.clear(); // [orig: CaptureCtx_Reset @0x53BD00]
    world.zones.build_chain_from_mission();
    world.zones.latch_control();
}

// Organic initialization resolves each authored ammo name directly into its
// entity byte (+0x358..0x35B) and each launch name into a one-based model
// userpoint byte (+0x365..0x367). These are explicit stores, not a block copy.
// The magazine is a signed word seeded from def+0x894.
// [orig: Entity_InitOrganicAI @ 0x4BFCC0; ammo/point stores @0x4BFE21..0x4BFF82;
// Entity_ResetToSpawnState @ 0x4B97A9]
int resolve_ai_weapons(world::World &world, const DefItemsFile &items,
                       world::EntityHandle only, const assets::AssetStore *models) {
    const std::unordered_map<int, const DefItemDef *> by_id = index_items(items);
    int armed = 0;
    // Bind every body's sound-profile pair first — persons AND vehicles carry
    // one (the female slot tracks primary unless explicitly detached by the
    // parser), and unarmed defs (the player) must not skip it (e.g. DBuggy01
    // -> SP_DuneBuggy). An unauthored key resolves to "default" via the emit-side
    // fallback (index stays -1). [orig: the def+0x268 parse binding
    // @ 0x49fb0f-0x49fb64; alloc seed @ 0x49e3f5]
    if (!world.tables.sound_profiles.empty()) {
        // The same pair, resolved PER DEF and retained by item type so a
        // wire-fed body — which has no AiEntity to carry a bound index — can
        // resolve its footstep/foley sets at presentation time. Per-def, not
        // per-spawned-instance: retail resolves every def's sound region at
        // load, and a joiner (whose world spawns no mission AI) still needs
        // every replicated type's binding. [orig: ItemDef_ResolveAllResources
        // @ 0x49e5f0 — the sound-region resolve runs for each def]
        for (size_t i = 0; i < items.count; ++i) {
            const DefItemDef &def = items.entries[i];
            if (def.id < mission::kItemIdOffset) continue;
            // A later row repeating an id is never the def its type resolves to
            // [orig: ItemList_FindIndexByTypeId @0x49E100, first match].
            if (find_item(by_id, def.id) != &def) continue;
            audio::OrganicSoundProfile op;
            if (def.sound_profile[0] != '\0')
                op.primary = static_cast<int16_t>(
                        world.tables.sound_profiles.index_of(def.sound_profile));
            if (def.sound_profile_female[0] != '\0')
                op.female = static_cast<int16_t>(
                        world.tables.sound_profiles.index_of(def.sound_profile_female));
            world.tables.organic_sound_profiles.set(
                    def.id - mission::kItemIdOffset, op);
        }
        for (int i = 0; i < world.ai.count(); ++i) {
            world::AiEntity *ae = world.ai.at(i);
            if (ae == nullptr || (only.valid() && ae->handle != only)) continue;
            const world::Entity *e = world.registry.get(ae->handle);
            if (e == nullptr) continue;
            const int def_id =
                    static_cast<int>(e->item_id) + mission::kItemIdOffset;
            const DefItemDef *def = find_item(by_id, def_id);
            if (def == nullptr) continue;
            if (def->sound_profile[0] != '\0')
                ae->profile.sound_profile = static_cast<int16_t>(
                        world.tables.sound_profiles.index_of(def->sound_profile));
            if (def->sound_profile_female[0] != '\0')
                ae->profile.sound_profile_female = static_cast<int16_t>(
                        world.tables.sound_profiles.index_of(def->sound_profile_female));
        }
    }
    for (int i = 0; i < world.ai.count(); ++i) {
        world::AiEntity *ae = world.ai.at(i);
        if (ae == nullptr || (only.valid() && ae->handle != only)) continue;
        const world::Entity *e = world.registry.get(ae->handle);
        if (e == nullptr) continue;
        const int def_id =
                static_cast<int>(e->item_id) + mission::kItemIdOffset;
        const DefItemDef *def = find_item(by_id, def_id);
        if (def == nullptr) continue;
        auto &weapons = ae->profile.organic;
        const char *ammo_names[] = {def->ammo_closeattack, def->ammo_easyrocket,
                                    def->ammo_advancedrocket, def->ammo_marker3};
        for (size_t slot = 0; slot < weapons.ammo.size(); ++slot) {
            if (ammo_names[slot][0] == '\0') continue;
            const int id = world.tables.ammo.index_of(ammo_names[slot]);
            weapons.ammo[slot] = static_cast<uint8_t>(id >= 0 ? id : 0);
        }
        // ModelGPM_FindUserpointByName returns the FIRST case-insensitive
        // match. The index-plus-one stores wrap to a byte, as in retail.
        // [orig: Entity_InitOrganicAI @0x4BFE8F..0x4BFF82]
        const auto *model = models != nullptr ? models->model(def->graphic).get() : nullptr;
        const char *point_names[] = {def->launchups_closeattack,
                                     def->launchups_rocket, def->launchups_marker3};
        for (size_t slot = 0; slot < weapons.launch.size(); ++slot) {
            weapons.launch[slot] = 0;
            if (!model || !model->user_points || point_names[slot][0] == '\0') continue;
            for (size_t point = 0; point < model->user_point_count; ++point) {
                if (strutil::iequals(model->user_points[point].name, point_names[slot])) {
                    weapons.launch[slot] = static_cast<uint8_t>(point + 1);
                    break;
                }
            }
        }
        ae->profile.clip_size = def->clipsize;
        ae->inf.magazine = static_cast<int16_t>(def->clipsize);
        if (std::any_of(weapons.ammo.begin(), weapons.ammo.end(),
                       [](uint8_t id) { return id != 0; })) ++armed;
    }
    // The SM weapon blocks' authored ammo names (.aip "primary_weap"/
    // "secondary_weap"), resolved against the same loaded table — retail
    // resolves at parse time through the ammo-def registry [orig:
    // AIProfile_ParseProperty -> AmmoDef_LookupByName -> profile+148/+180];
    // our parse keeps the name because the table loads after promotion.
    for (int i = 0; i < world.ai.count(); ++i) {
        world::AiEntity *ae = world.ai.at(i);
        if (ae == nullptr || (only.valid() && ae->handle != only)) continue;
        for (world::AiProfile::WeaponFire *wf : {&ae->profile.fire_a, &ae->profile.fire_b}) {
            if (wf->ammo_name.empty()) continue;
            wf->ammo_index = world.tables.ammo.index_of(wf->ammo_name.c_str());
            if (wf->ammo_index >= 0) ++armed;
        }
        // The vehicle class init's ammo copy: a block's count seeds its brain
        // word only when the block's ammo resolved (the byte
        // AmmoDef_LookupByName stored, 0 for a miss or the null AT_NULL row),
        // else zero. [orig: Entity_InitVehicleAIFromDef @0x468882..0x4688B7;
        //  Entity_InitHelicopterAIFromDef @0x468555..0x46858D]
        const world::Entity *body = world.registry.get(ae->handle);
        if (body != nullptr && body->kind == world::EntityKind::Item) {
            const world::AiProfile &p = ae->profile;
            ae->brain.f[world::AiBrain::kAmmoA] = p.fire_a.ammo_index > 0 ? p.fire_a.ammo_cap : 0;
            ae->brain.f[world::AiBrain::kAmmoB] = p.fire_b.ammo_index > 0 ? p.fire_b.ammo_cap : 0;
        }
    }
    return armed;
}

void resolve_item_event_sounds(world::World &world, const DefItemsFile &items) {
    const auto by_id = index_items(items);
    for (auto &row : world.tables.item_death_traits.rows) {
        const auto it = by_id.find(row.first + mission::kItemIdOffset);
        if (it != by_id.end()) bind_regional_sounds(world, *it->second, row.second);
    }
}

} // namespace opennova::mission

namespace opennova::mission {
namespace {
uint8_t point_type(const char *name) {
    if (strutil::iequals(name, "smlmarked")) return 1;
    if (strutil::iequals(name, "small")) return 2;
    if (strutil::iequals(name, "lrgmarked")) return 3;
    if (strutil::iequals(name, "large")) return 4;
    return 0;
}
uint32_t ammo_id(const world::World &world, const char *name) {
    const int index = world.tables.ammo.index_of(name);
    return index < 0 ? 0u : static_cast<uint32_t>(index);
}
}

// [orig: Entity_InitHardpoints @ 0x4417D0]
// lndm's bone callback is BoneCallback_Identity @ 0x4E20A0. Its model-global
// userpoints therefore retain the authored rest position through the bone walk.
void resolve_minefields(world::World &world, const def::DefItemsFile &items,
                        const assets::AssetStore &models) {
    const auto definitions = index_items(items);
    world.registry.for_each([&](const world::Entity &row) {
        if (row.minefield.initialized) return;
        const auto it = definitions.find(row.item_id + mission::kItemIdOffset);
        if (it == definitions.end()) return;
        const auto &def = *it->second;
        const bool think = strutil::iequals(std::string_view(def.ai_function).substr(0, 4), "lndm");
        const bool render = mission::uses_submodel_renderer(def.render_function);
        if (!think && !render) return;
        world::Entity *entity = world.registry.get(row.handle);
        const auto *model = models.model(def.graphic).get();
        std::vector<world::MinefieldPoint> points;
        if (think && model != nullptr) {
            const world::CollisionMatrix placement = world::entity_placement_matrix(*entity);
            for (size_t i = 0; i < model->user_point_count && points.size() < 14; ++i) {
                const auto &up = model->user_points[i];
                const uint8_t type = point_type(up.name);
                if (!type) continue;
                const int32_t local[3] = {up.x, up.y, up.z};
                int32_t transformed[3]{};
                // The attachment helper converts to float before transforming.
                // Preserve that float storage boundary and truncate back to Q16.
                for (int axis = 0; axis < 3; ++axis) {
                    // Math_TransformPointByMatrix4x4 @ 0x40CF20: translation
                    // is last; render X (mission -Y) sums X,Z,Y products.
                    const int order[3] = {0, axis == 1 ? 2 : 1, axis == 1 ? 1 : 2};
                    double value = 0;
                    for (int k : order)
                        value += static_cast<double>(static_cast<float>(
                                placement.m[4*axis+k] / 4194304.0f)) *
                                static_cast<float>(local[k] * io::kInvFp16One);
                    value += static_cast<float>(placement.m[4*axis+3] * io::kInvFp16One);
                    transformed[axis] = static_cast<int32_t>(
                            static_cast<float>(value) * io::kFp16One);
                }
                transformed[2] = world.tables.terrain ? static_cast<int32_t>(
                        terrain::height_field_height_world_bilinear(*world.tables.terrain,
                            transformed[0] * io::kInvFp16One,
                            -transformed[1] * io::kInvFp16One) * io::kFp16One) : 0;
                points.push_back({type, {transformed[0], transformed[1], transformed[2]}});
            }
        }
        world.minefields.initialize(world, *entity, think, render, model != nullptr,
                ammo_id(world, def.ammo_closeattack), ammo_id(world, def.ammo_marker3),
                def.huskfinal, def.husk, points);
    });
}
} // namespace opennova::mission
