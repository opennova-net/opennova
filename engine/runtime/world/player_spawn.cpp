#include <runtime/world/player_spawn.h>

#include <runtime/world/angle.h>
#include <runtime/world/ai.h>           // AiSystem, AiEntity
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h> // entity_reset_to_spawn_state
#include <runtime/world/geom.h>         // to_fixed
#include <runtime/world/infantry.h>     // anim_state
#include <runtime/world/local_player.h> // LocalPlayer::radar (the lock tone reset)
#include <runtime/world/world.h>        // World, registry, cached

namespace opennova::world {

static_assert(PlayerTemplate{}.type_id == kPlayerInfantryTypeId);

int32_t player_cammo_type_id(const World &world, uint8_t player_class) {
    // The climate's property: 1 JUNGLE_CAMMO (10), 2 ARCTIC_CAMMO (12), any other
    // DESERT_CAMMO (11); the class row (class - 1) & 0xF.
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C399..0x43C3BE;
    //  CharAttr_GetCammoTypeId @0x4127B0]
    const int32_t climate = world.tables.bms_climate;
    const size_t property = climate == 1 ? 0 : climate == 2 ? 2 : 1;
    const size_t row = static_cast<size_t>((player_class - 1) & 0xF);
    return world.tables.class_cammo_type_ids[row][property];
}

int32_t item_index_by_type_id(const World &world, int32_t type_id) {
    // [orig: ItemList_FindIndexByTypeId @0x49E100 - the walk @0x49E117..0x49E12F,
    //  0 @0x49E12F / @0x49E131]
    const std::vector<PlayerTemplate> &rows = world.tables.item_rows;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].type_id == type_id) return static_cast<int32_t>(i);
    return 0;
}

namespace {

// The row a player spawn binds: the record's type word for a joiner's own body,
// else the class's camouflage item; the first row for a type no row carries. A
// World with no items rows (no traits sweep) or whose peer read no charattr.def
// (a native harness, a synthetic pack) spawns from the seed. [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the camouflage
//  pick @0x43C399..0x43C3BE, ItemList_FindIndexByTypeId @0x43C3CA, the row
//  @0x43C3CF..0x43C3D9; NapiNPClientMsg_0x00C @0x42E730 binds the record's type]
const PlayerTemplate &player_spawn_row(const World &world, const PlayerSpawn &spawn,
                                       int32_t &index) {
    const std::vector<PlayerTemplate> &rows = world.tables.item_rows;
    if (rows.empty() || (spawn.item_type_id < 0 && !world.tables.class_cammo_installed)) {
        index = world.tables.player.item_type_index;
        return world.tables.player;
    }
    const int32_t type_id = spawn.item_type_id >= 0
            ? spawn.item_type_id
            : player_cammo_type_id(world, spawn.item_class != 0 ? spawn.item_class
                                                                : spawn.player_class);
    index = item_index_by_type_id(world, type_id);
    return rows[static_cast<size_t>(index)];
}

// The shared faithful §5.2b spawn: alloc a pool-0 entity of the bound row, init health, place the pose,
// clear the movement gate, and mount the infantry motor. `is_local` selects the host's OWN
// player (input-ordered, publishes World::cached.local_player) vs a REMOTE peer (a joiner's
// entity the host snaps from the wire — never the local player). [orig: net-re §5.2b/§5.38]
EntityHandle spawn_player_entity(World &world, const PlayerSpawn &spawn, bool is_local) {
    // The infantry motor (mounted below) requires an AiSystem. Bail BEFORE allocating a pool-0
    // slot so a missing AiSystem never leaks a half-initialized player entity into the pool (the
    // per-tick spawn retry would otherwise orphan one every tick until the pool is exhausted).

    // The def hp word (itemDef+0x17C) once the traits sweep stored the Player row's (class-8
    // Player = 150; 0 a real word), else, unswept, the spawn seed's fallback. health_max is
    // stamped alongside so the §5.10 field-17 tier denominator reads the spawn value (full =>
    // tier 2 => the golden 0x28) even for a joiner spawning AFTER the mission-load sweep.
    // (D-NET-144)
    int32_t row_index = 0;
    const PlayerTemplate &row = player_spawn_row(world, spawn, row_index);
    const bool def_hp = row.has_item_def && row.item_hp.has_value();
    const int32_t hp = retail_signed_i16(
        def_hp ? *row.item_hp : static_cast<int32_t>(spawn.health));
    // A row word of 0: the class init's model init marks the body indestructible, the row's
    // two armor words -1, subType -1 and Health 1, which the ceiling-0 raises below leave
    // (D-PWR-10). [orig: PlayerClass_InitEntity @0x4B1060 - Entity_InitFromModel called
    //  @0x4B10C8: the healthMax test @0x40DC85, Flags |= 0x4000000 @0x40DC8E, def+0x190 /
    //  def+0x192 = -1 @0x40DC95 / @0x40DC9F, Health = 1 @0x40DCA6, subType @0x40DCAF]
    const bool indestructible = def_hp && hp == 0;

    // §5.2b steps 1-4: a pool-0 player-infantry entity of the bound row's type (the stock
    // class-8 desert row is 0x14B9), the row's health, placed pose. kind=Organic.
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - ItemTypeIndex @0x43C426, itemDef
    //  @0x43C429]
    Entity seed;
    seed.net_id = spawn.net_id;
    // Key the player by its net_id as the bms_id too, so the host can resolve a player avatar
    // node for it through the present pass (the player has no BMS placement of its own). A high
    // net_id (0xFFF0) won't collide with the small mission bms ids. [net-re §5.38; ADR 0012]
    seed.bms_id = static_cast<int32_t>(spawn.net_id);
    // No BMS placement — carry the explicit none/synthetic origin, matching the joiner-side
    // materializer (client_world_materializer.cpp). The Entity default 0 is indistinguishable
    // from authored record (kind 0, index 0), which made the host's wire present pass defer the
    // admitted-player row to a placed node that does not exist, leaving the avatar unbuilt.
    seed.spawn_origin = kSpawnOriginNone;
    seed.kind = EntityKind::Organic;
    seed.item_id = row.type_id;
    seed.has_item_def = row.has_item_def;
    seed.item_type_index = row_index;
    seed.has_graphic_model = row.has_graphic_model;
    seed.item_type = row.item_type;
    seed.item_attrib = row.item_attrib;
    seed.armor_impact = retail_signed_i16(row.armor_impact);
    seed.armor_kz = retail_signed_i16(row.armor_kz);
    seed.damage_reduc_pp = row.damage_reduc_pp;
    seed.damage_reduc_max = row.damage_reduc_max;
    seed.radar_sig = row.radar_sig; // AI engage caps [orig: @0x40e136]
    seed.heat_sig = row.heat_sig;
    if (indestructible) {
        seed.engine_flags |= kEntityFlagIndestructible;
        seed.armor_impact = -1;
        seed.armor_kz = -1;
        seed.sub_type = 0xFF;
    }
    seed.player_class = spawn.player_class; // entity+0x294 (host-diag 2026-07-01: was left 0)
    seed.position = spawn.position;
    seed.heading = spawn.heading; // the Yaw word (D-NET-NEW-L6-2)
    seed.pitch = spawn.pitch;
    seed.roll = spawn.roll;
    seed.team = spawn.team;
    // The wire record's item init stores the def word [orig: Entity_InitFromItemDef @0x49e550:
    // healthMax -> Health]; the authority's spawn zeroes it below.
    seed.health = hp;
    seed.critical_hp = retail_signed_i16(row.critical_hp);
    seed.health_max = hp; // the §5.10 field-17 tier denominator (D-NET-144)
    seed.equipped_adm_index = spawn.equipped_adm_index; // entity+0x2B0 spawn default (D-NET-143)
    // entity+0x374 character selector + entity+0x15C wire NetId, picked per assigned team from
    // the joiner's 0x42 join vars by the inmatch add. [orig: Server_PlayerAdd @0x51cbc0
    // @0x51d0b1 / slot+440; D-NET-146]
    seed.anim_slot = spawn.anim_slot;
    seed.minimap_net_id = spawn.minimap_net_id;
    // Players carry commandGroup 1: the SP deploy leg stamps it on every (re)spawn, and
    // shipped missions script against it (00TRa's tour dialogs are all "group 1 enters
    // area X"). [orig: the deploy leg @0x519fd0 — word entity+0x11C = 1]
    seed.group_id = 1;
    seed.alive = true;
    seed.flags = 2u | 0x100u;   // movement gate + player classifier in the live/wire mirror
    // Both local and remote player bodies carry the retail player classifier. The
    // damage trigger uses this bit—not local ownership—to bypass NPC move/group alerts.
    // [orig: OrganicClass_HandleEvent test victim Flags,100h @0x4073c8]
    seed.engine_flags |= kEntityFlagPlayer;
    // entity+0x78: the owning connection's dcb (host loopback dcb / a joiner's 0x48-ack dcb). The
    // 0x0C organic-spawn carries it so the client self-matches its own player. [orig: Server_PlayerAdd
    // @0x51cbc0 writes entity+0x78 = conn->connection_id; net-re §5.2b]
    seed.owner_connection_id = spawn.owner_connection_id;

    const EntityHandle h = world.registry.spawn_from(0, spawn.min_entity_slot, seed);
    if (!h.valid()) return h;
    Entity *ent = world.registry.get(h);

    // Publish the local-player handle ONLY for the host's own player: the net anchor + present
    // resolve it. A remote peer never becomes the local player. [ADR 0012] The class init points
    // it at the new body ahead of its health raise [orig: PlayerClass_InitEntity @0x4B1124].
    if (is_local) world.cached.local_player = h;
    // The authority's spawn zeroes the row (a joiner's own body keeps its record's def word),
    // then the class init and the spawn each raise the health to the ceiling, a signed word
    // compare: the def word in a session, out of one the local player's with the difficulty
    // word a fresh SP start's bring-up seeded from the profile (D-PWR-6). [orig:
    //  Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE, the class init called
    //  @0x43C4FE (PlayerClass_InitEntity's raise @0x4B112B), the raise @0x43C557..0x43C56B]
    const int32_t ceiling = max_health_with_difficulty(world, *ent, hp);
    if (!spawn.from_wire_record) ent->health = 0;
    if (indestructible) ent->health = 1; // the model init, ahead of the raises [orig: @0x40DCA6]
    if (retail_signed_i16(ent->health) < ceiling) ent->health = ceiling;

    // §5.2b step 5: clear the entity+36 bit-1 movement gate + back up the spawn position.
    // [orig: Entity_ResetToSpawnState @0x4B9610]
    entity_reset_to_spawn_state(*ent, ceiling);

    // Mount the infantry motor — same motor as an NPC organic. The host's own player is ordered
    // from input and never AI-think/routed; a remote peer is snapped from the wire and the motor
    // skips it once net-snapped (inf.is_local_player stays false). [orig: net-re §5.38; ADR 0012]
    const int idx = world.ai.attach(h);
    AiEntity &ae = *world.ai.at(idx);
    // The heading word the placement copied (D-NET-376).
    // [orig: Entity_FindBestSpawnPoint @0x50CF38]
    ae.heading = spawn.heading;
    ae.team = spawn.team;
    ae.net_id = spawn.net_id;
    ae.health = static_cast<int16_t>(ent->health); // a signed 16-bit value above
    ae.inf.active = true;
    ae.inf.is_local_player = is_local;
    // The shared spawn/revive body reset (fresh attach: everything is default
    // already — this pins both call sites to ONE field list).
    ae.inf.reset_for_spawn(ae.heading);
    ae.inf.max_health = static_cast<int16_t>(hp);
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C54F]
    if (spawn.berserk) ae.slot.f[AiSlot::kBehaviorFlags] |= 0x200;

    // The player class init zeroes the incoming-lock tone for the local
    // player's entity [orig: PlayerClass_InitEntity @0x4b10d0..0x4b10de].
    if (is_local && world.local_player_state != nullptr)
        world.local_player_state->radar.lock_tone = 0;
    // The registry edge's republish. An embedder that binds the body's def and
    // model after this builds the slices again once the model init stamped
    // its bound radius (MissionKernel::bind_spawned_body).
    if (world.collision != nullptr)
        world.collision->refresh_after_registry_change(world);
    return h;
}

} // namespace

const PlayerTemplate &player_item_row(const World &world, const Entity &body) {
    const std::vector<PlayerTemplate> &rows = world.tables.item_rows;
    if (body.item_type_index < 0 || static_cast<size_t>(body.item_type_index) >= rows.size())
        return world.tables.player;
    return rows[static_cast<size_t>(body.item_type_index)];
}

void rebind_player_camouflage_row(World &world, Entity &body) {
    if (world.tables.item_rows.empty() || !world.tables.class_cammo_installed) return;
    const int32_t index =
            item_index_by_type_id(world, player_cammo_type_id(world, body.player_class));
    const PlayerTemplate &row = world.tables.item_rows[static_cast<size_t>(index)];
    body.item_id = row.type_id;
    body.item_type_index = index;
    body.has_item_def = row.has_item_def;
    body.has_graphic_model = row.has_graphic_model;
    body.item_type = row.item_type;
    body.item_attrib = row.item_attrib;
    body.armor_impact = retail_signed_i16(row.armor_impact);
    body.armor_kz = retail_signed_i16(row.armor_kz);
    body.damage_reduc_pp = row.damage_reduc_pp;
    body.damage_reduc_max = row.damage_reduc_max;
    if (row.item_hp.has_value() && *row.item_hp != 0) body.health_max = *row.item_hp;
}

EntityHandle spawn_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/true);
}

EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/false);
}

} // namespace opennova::world
