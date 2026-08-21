// The sim-side items.def trait fold, moved verbatim from the shell adapter's
// Simulation::resolve_item_traits / resolve_ai_weapons (ADR 0028). Every
// read that went through the item database's getter surface now reads the
// DefItemDef row directly; the getters were field-for-field projections, so
// the miss defaults (0 / empty / TYPE unset) are preserved exactly.
#include "simassets/item_traits.h"

#include <io/strutil.h>
#include <mission/mission.h>
#include <world/ai.h>
#include <world/player_spawn.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::simassets {

namespace {

// Last-wins by-id view over the parsed file: duplicate definition ids
// overwrite earlier rows — the same load-order semantics the adapter's
// id-keyed item map exposed (operator[] assignment per entry). The net
// catalog separately RETAINS duplicates so it can classify them ambiguous and
// fail closed; that policy lives with the injected wire-class supplier, not
// here.
std::unordered_map<int, const DefItemDef *> index_items(
        const DefItemsFile &items) {
    std::unordered_map<int, const DefItemDef *> by_id;
    by_id.reserve(items.count);
    for (size_t i = 0; i < items.count; ++i)
        by_id[items.entries[i].id] = &items.entries[i];
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
// sent the shipped Motorcycle down the Ground motor. Same rule as netsim's
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
                         const ItemWireClassFn &wire_class) {
    const std::unordered_map<int, const DefItemDef *> by_id = index_items(items);
    // Cache the Player template's items.def hp at world level so LATE-JOINER spawns (which happen
    // after this sweep) seed full health without an item-db reach-back [orig:
    // Entity_InitFromItemDef @0x49e550 — spawn Health = itemDef->healthMax]. (D-NET-144)
    const int player_def_id =
            static_cast<int>(world::kPlayerInfantryTypeId) + mission::kItemIdOffset;
    const DefItemDef *player_def = find_item(by_id, player_def_id);
    world.player_has_item_def = player_def != nullptr;
    world.player_item_hp =
            world::retail_signed_i16(player_def != nullptr ? player_def->hp : 0);
    world.player_item_type =
            static_cast<uint8_t>(player_def != nullptr ? player_def->type : 0);
    world.player_item_attrib = player_def != nullptr ? player_def->attrib : 0u;
    world.player_armor_impact = world::retail_signed_i16(
            player_def != nullptr ? player_def->armor_impact : 0);
    world.player_armor_kz = world::retail_signed_i16(
            player_def != nullptr ? player_def->armor_kz : 0);
    world.player_damage_reduc_pp =
            player_def != nullptr ? player_def->damage_reduc_pp : 0.0f;
    world.player_damage_reduc_max =
            player_def != nullptr ? player_def->damage_reduc_max : 0.0f;
    world.player_radar_sig = player_def != nullptr ? (player_def->radar_sig & 0xFFFF) : 0;
    world.player_heat_sig = player_def != nullptr ? (player_def->heat_sig & 0xFFFF) : 0;
    std::vector<world::EntityHandle> handles;
    world.registry.for_each(
            [&](const world::Entity &e) { handles.push_back(e.handle); });
    for (const world::EntityHandle h : handles) {
        world::Entity *e = world.registry.get(h);
        if (!e) continue;
        const int def_id =
                static_cast<int>(e->item_id) + mission::kItemIdOffset;
        const DefItemDef *def = find_item(by_id, def_id);
        e->has_item_def = def != nullptr;
        e->item_type = static_cast<uint8_t>(def != nullptr ? def->type : 0);
        e->item_attrib2 = def != nullptr ? def->attrib2 : 0u;
        e->is_ai_capable =
                def != nullptr && (def->attrib & DEF_ITEM_ATTRIB_AIDATA) != 0;
        // The injected catalog supplies both the authoritative host stamp and
        // the decoded-client record width. Missing/ambiguous definitions fail
        // closed as Unknown (0).
        e->net_class_code = wire_class ? wire_class(def_id) : 0;
        // items.def hp -> healthMax; lift spawn-default health to full [orig: @0x49e550].
        const int hp = world::retail_signed_i16(def != nullptr ? def->hp : 0);
        e->health = world::retail_signed_i16(e->health);
        e->health_max = world::retail_signed_i16(e->health_max);
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
        // (D-NET-147; every golden ASH_I5A building carries both). Resolved defs only — a
        // missing items.def id stays untouched. [orig: Entity_InitFromModel @0x40dc8e:
        // !itemDef->healthMax -> Flags |= 0x4000000, Health = 1, subType = -1]
        if (hp == 0 && def != nullptr) {
            e->engine_flags |= 0x4000000u;
            e->sub_type = 0xFF;
        }
        // AS zone traits from the attrib dword: 0x20000 "ChangeTeam" = capture trigger,
        // 0x40000 "SpawnPoint" = deploy-selectable (the ASH_I5A "Change Team & Spawn
        // Volume" objects carry both). [orig: def+84 gates in ZoneSlotChain_BuildFromMission
        // @0x4a2de0 / Server_ResolveSpawnTargetHandle @0x4fe110; net-re §5.61]
        const uint32_t attrib = def != nullptr ? def->attrib : 0u;
        e->item_attrib = attrib;
        e->is_capture_trigger = (attrib & DEF_ITEM_ATTRIB_CHANGETEAM) != 0;
        e->is_spawn_point = (attrib & DEF_ITEM_ATTRIB_SPAWNPOINT) != 0;
        // Death-presentation traits: LeaveCorpse (attrib 0x400000) keeps the corpse
        // forever; deathtime (def+0x890, parse-scaled ticks) seeds the corpse timer at
        // the death edge. [orig: ItemDef_ParseProperty @0x4a09d3 / @0x49fa6c; consumers
        // Entity_UpdateInfantryAI @0x4b9e54 / @0x4b9c97; world-wac-ai-re §19]
        e->leave_corpse = (attrib & DEF_ITEM_ATTRIB_LEAVECORPSE) != 0;
        e->deathtime_ticks = def != nullptr ? def->deathtime_ticks : 0;
        // Destruction traits (world/destruction.h; world-wac-ai-re §24): the death
        // chain's def fields, keyed by item id. Fills once per distinct id.
        // [orig: the ItemDef fields Entity_ApplyWeaponDamage / the death dispatch /
        // Entity_InitDeathSounds read — armor +0x190/+0x192, unitType +0x196, kz
        // +0x198, huskSubPart* +0x100.., debrisScale +0x1BC, soundDeath +0x860,
        // the particledeath family +0x412..]
        e->item_unit_type = def != nullptr ? def->unit_type : 0;
        if (world.item_death_traits.get(e->item_id) == nullptr &&
                def != nullptr) {
            world::ItemDeathTraits t;
            t.unit_type = def->unit_type;
            t.kz = def->kz;
            t.armor_impact = def->armor_impact;
            t.armor_blast = def->armor_blast;
            t.team_protect = (attrib & 0x8000u) != 0; // 0x8000 is NOT in the witnessed attrib token table — stays raw
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
            t.sound_death = def->sounddeath;
            t.particledeath = def->particledeath;
            t.particleh2odeath = def->particleh2odeath;
            t.particlefire = def->particlefire;
            t.particleother = def->particleother;
            t.particlefinale = def->particlefinale;
            // The collision-instance sweep enriches this row with live
            // husk-model state and the active first-stage husk's KZ user points.
            world.item_death_traits.set(e->item_id, std::move(t));
        }
        // Vehicle mover traits: the pre-scaled items.def block + PlayerControl
        // attrib (0x40), keyed by item id. Ground-family dispatchers test the
        // `physics` selector, but CHel/cpln dispatch DIRECTLY to the air mover and
        // shipped CHel rows legitimately omit that ground selector. [orig:
        // g_EntityClassPhysicsTable @0x82abc8; ground dispatch @0x48ef90..0x48f060;
        // Entity_UpdateAircraftPhysics @0x490310]
        if (e->handle.pool() == 1 &&
                world.vehicle_traits.get(e->item_id) == nullptr &&
                def != nullptr) {
            const std::string fam = fourcc_prefix(def->move_function);
            const bool direct_air_mover = fam == "chel" || fam == "cpln";
            if (def->physics != 0 || direct_air_mover) {
                world::VehicleTraits vt;
                vt.physics = def->physics;
                vt.player_speed = def->player_speed;
                vt.acceleration = def->acceleration;
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
                vt.spring = def->spring;
                vt.spring_comp = def->spring_comp;
                vt.shock = def->shock;
                // def->top_heavy is parsed for parity but dead in retail —
                // no consumer, so the traits do not carry it.
                vt.player_control =
                        (attrib & DEF_ITEM_ATTRIB_PLAYERCONTROL) != 0;
                // The per-frame physics mover is selected exclusively by the
                // move_function callback resolved into itemDef+0x158. ai_function
                // selects the event/brain callback and may deliberately differ: the
                // shipped Dune Buggy is ai_function chel + move_function cveh and
                // therefore still runs the ground mover. [orig:
                // EntityDef_LookupPhysicsCallback @0x4a9240; §5.38e movers]
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
                // Vehicle audio belongs to the vehicle ItemDef, not to the
                // mounted NPC's AiProfile. Resolve the profile name and the
                // item-level soundloop overrides once at this portable boundary.
                // [orig: ItemDef_ResolveAllResources @0x49e5f0/@0x49e7f0]
                vt.sound_profile = def->sound_profile;
                for (size_t i = 0; i < vt.sound_loops.size(); ++i)
                    vt.sound_loops[i] = def->soundloops[i];
                world.vehicle_traits.set(e->item_id, vt);
            }
        }
    }
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
    world::zone_chain_build_from_mission(world, world.zone_chain);
    world::zone_chain_latch_control(world, world.zone_chain);
}

// The D-AI-5 host weapon seed. The original resolves the items.def ammo_closeattack/
// easyrocket/advancedrocket/marker3 names into ammo-def ids on the def and block-copies
// them onto the entity (+0x358..0x35B; the copy site is the open world-wac-ai-re §17.7
// item 1 — no per-field writer exists). Until that copy is witnessed, the port carries
// ONE ammo id + clipsize per NPC (AiProfile — JO riflemen author all four slots to the
// same rifle round), stamped here from the def rows against the loaded ammo table.
// Also seeds the spawn magazine: word entity+0x35C = itemDef+0x894 clipsize [orig:
// Entity_ResetToSpawnState @ 0x4b97a9/0x4b97b5]. Consumption stays motor-gated: only
// the infantry fire pass reads ammo_primary (host-side NPCs; never the local player).
// [orig: ItemDef_ParseProperty @ 0x4a1823 (-> def+0x56B) / @ 0x49fa1c (-> def+0x894);
// docs/divergence-ledger.md D-AI-5]
int resolve_ai_weapons(world::World &world, const DefItemsFile &items) {
    if (world.ai == nullptr) return 0;
    const std::unordered_map<int, const DefItemDef *> by_id = index_items(items);
    int armed = 0;
    // Bind every body's sound-profile pair first — persons AND vehicles carry
    // one (the female slot tracks primary unless explicitly detached by the
    // parser), and unarmed defs (the player) must not skip it (e.g. DBuggy01
    // -> SP_DuneBuggy). An unauthored key resolves to "default" via the emit-side
    // fallback (index stays -1). [orig: the def+0x268 parse binding
    // @ 0x49fb0f-0x49fb64; alloc seed @ 0x49e3f5]
    if (!world.sound_profiles.empty()) {
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
            audio::OrganicSoundProfile op;
            if (def.sound_profile[0] != '\0')
                op.primary = static_cast<int16_t>(
                        world.sound_profiles.index_of(def.sound_profile));
            if (def.sound_profile_female[0] != '\0')
                op.female = static_cast<int16_t>(
                        world.sound_profiles.index_of(def.sound_profile_female));
            world.organic_sound_profiles.set(
                    def.id - mission::kItemIdOffset, op);
        }
        for (int i = 0; i < world.ai->count(); ++i) {
            world::AiEntity *ae = world.ai->at(i);
            if (ae == nullptr) continue;
            const world::Entity *e = world.registry.get(ae->handle);
            if (e == nullptr) continue;
            const int def_id =
                    static_cast<int>(e->item_id) + mission::kItemIdOffset;
            const DefItemDef *def = find_item(by_id, def_id);
            if (def == nullptr) continue;
            if (def->sound_profile[0] != '\0')
                ae->profile.sound_profile = static_cast<int16_t>(
                        world.sound_profiles.index_of(def->sound_profile));
            if (def->sound_profile_female[0] != '\0')
                ae->profile.sound_profile_female = static_cast<int16_t>(
                        world.sound_profiles.index_of(def->sound_profile_female));
        }
    }
    if (world.ammo.empty()) return 0; // no ammo.def loaded — NPCs stay unarmed
    for (int i = 0; i < world.ai->count(); ++i) {
        world::AiEntity *ae = world.ai->at(i);
        if (ae == nullptr) continue;
        const world::Entity *e = world.registry.get(ae->handle);
        if (e == nullptr) continue;
        const int def_id =
                static_cast<int>(e->item_id) + mission::kItemIdOffset;
        const DefItemDef *def = find_item(by_id, def_id);
        if (def == nullptr || def->ammo_closeattack[0] == '\0')
            continue; // def authors no anim-fire round (e.g. the player)
        const int ammo = world.ammo.index_of(def->ammo_closeattack);
        if (ammo < 0) continue; // name not in this mission's ammo.def — stay unarmed
        ae->profile.ammo_primary = ammo;
        ae->profile.clip_size = def->clipsize;
        ae->inf.magazine = static_cast<int16_t>(ae->profile.clip_size);
        ++armed;
    }
    // The SM weapon blocks' authored ammo names (.aip "primary_weap"/
    // "secondary_weap"), resolved against the same loaded table — retail
    // resolves at parse time through the ammo-def registry [orig:
    // AIProfile_ParseProperty -> AmmoDef_LookupByName -> profile+148/+180];
    // our parse keeps the name because the table loads after promotion.
    for (int i = 0; i < world.ai->count(); ++i) {
        world::AiEntity *ae = world.ai->at(i);
        if (ae == nullptr) continue;
        for (world::AiProfile::WeaponFire *wf : {&ae->profile.fire_a, &ae->profile.fire_b}) {
            if (wf->ammo_name.empty()) continue;
            wf->ammo_index = world.ammo.index_of(wf->ammo_name.c_str());
            if (wf->ammo_index >= 0) ++armed;
        }
    }
    return armed;
}

} // namespace opennova::simassets
