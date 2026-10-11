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

namespace {

// The shared faithful §5.2b spawn: alloc a pool-0 0x14B9 entity, init health, place the pose,
// clear the movement gate, and mount the infantry motor. `is_local` selects the host's OWN
// player (input-ordered, publishes World::cached.local_player) vs a REMOTE peer (a joiner's
// entity the host snaps from the wire — never the local player). [orig: net-re §5.2b/§5.38]
EntityHandle spawn_player_entity(World &world, const PlayerSpawn &spawn, bool is_local) {
    // The infantry motor (mounted below) requires an AiSystem. Bail BEFORE allocating a pool-0
    // slot so a missing AiSystem never leaks a half-initialized 0x14B9 entity into the pool (the
    // per-tick spawn retry would otherwise orphan one every tick until the pool is exhausted).

    // The def hp word (itemDef+0x17C) once the traits sweep stored the Player row's (class-8
    // Player = 150; 0 a real word), else, unswept, the spawn seed's fallback. health_max is
    // stamped alongside so the §5.10 field-17 tier denominator reads the spawn value (full =>
    // tier 2 => the golden 0x28) even for a joiner spawning AFTER the mission-load sweep.
    // (D-NET-144)
    const bool def_hp = world.tables.player.has_item_def && world.tables.player.item_hp.has_value();
    const int32_t hp = retail_signed_i16(
        def_hp ? *world.tables.player.item_hp : static_cast<int32_t>(spawn.health));
    // A row word of 0: the class init's model init marks the body indestructible, the row's
    // two armor words -1, subType -1 and Health 1, which the ceiling-0 raises below leave
    // (D-PWR-10). [orig: PlayerClass_InitEntity @0x4B1060 - Entity_InitFromModel called
    //  @0x4B10C8: the healthMax test @0x40DC85, Flags |= 0x4000000 @0x40DC8E, def+0x190 /
    //  def+0x192 = -1 @0x40DC95 / @0x40DC9F, Health = 1 @0x40DCA6, subType @0x40DCAF]
    const bool indestructible = def_hp && hp == 0;

    // §5.2b steps 1-4: a pool-0 player-infantry entity (type 0x14B9), item-template health,
    // placed pose. kind=Organic so entity_wire_bridge::entity_class_of resolves it as Player.
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
    seed.item_id = kPlayerInfantryTypeId;
    seed.has_item_def = world.tables.player.has_item_def;
    seed.item_type_index = world.tables.player.item_type_index;
    seed.has_graphic_model = world.tables.player.has_graphic_model;
    seed.item_type = world.tables.player.item_type;
    seed.item_attrib = world.tables.player.item_attrib;
    seed.armor_impact = retail_signed_i16(world.tables.player.armor_impact);
    seed.armor_kz = retail_signed_i16(world.tables.player.armor_kz);
    seed.damage_reduc_pp = world.tables.player.damage_reduc_pp;
    seed.damage_reduc_max = world.tables.player.damage_reduc_max;
    seed.radar_sig = world.tables.player.radar_sig; // AI engage caps [orig: @0x40e136]
    seed.heat_sig = world.tables.player.heat_sig;
    if (indestructible) {
        seed.engine_flags |= kEntityFlagIndestructible;
        seed.armor_impact = -1;
        seed.armor_kz = -1;
        seed.sub_type = 0xFF;
    }
    seed.player_class = spawn.player_class; // entity+0x294 (host-diag 2026-07-01: was left 0)
    seed.position = spawn.position;
    seed.yaw = spawn.yaw;
    seed.pitch = spawn.pitch;
    seed.roll = spawn.roll;
    seed.team = spawn.team;
    // The wire record's item init stores the def word [orig: Entity_InitFromItemDef @0x49e550:
    // healthMax -> Health]; the authority's spawn zeroes it below.
    seed.health = hp;
    seed.critical_hp = retail_signed_i16(world.tables.player.critical_hp);
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
    ae.pos[0] = to_fixed(spawn.position.x);
    ae.pos[1] = to_fixed(spawn.position.y);
    ae.pos[2] = to_fixed(spawn.position.z);
    // The heading word the placement copied (D-NET-376), else the (90 - yaw)
    // heading of a spawn with no source pose.
    // [orig: Entity_FindBestSpawnPoint @0x50CF38]
    ae.heading = player_spawn_heading(spawn);
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
    if (world.collision != nullptr)
        world.collision->refresh_after_registry_change(world);
    return h;
}

} // namespace

EntityHandle spawn_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/true);
}

EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/false);
}

} // namespace opennova::world
