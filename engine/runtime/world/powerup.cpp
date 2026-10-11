#include <runtime/world/powerup.h>

#include <base/io/strutil.h>
#include <formats/def/def.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cstring>

namespace opennova::world {

namespace {

// The 62 Hz tick [orig: the `*31, *2` fold of respawn_time @0x442AD0..0x442ADD]
constexpr int32_t kTicksPerSecond = 62;
// The "fill" amount a -1 row adds before the class cap clamps it
// [orig: `push 7FFFh` @0x4429F9 / @0x442A69]
constexpr int32_t kFillAmount = 0x7FFF;
// The first ammo class the per-class table grants; classes below it are the
// fixed built-ins [orig: `mov edi, 0Bh` @0x442A4F]
constexpr int32_t kFirstGrantedClass = 11;

PowerupAction make_action(const def::DefPowerupAction &row, const char *default_function) {
    PowerupAction out;
    out.authored = row.present != 0;
    if (!out.authored) {
        // No block: the register step creates the row bound straight to the
        // default handler [orig: @0x442C43..0x442C8B / @0x442CAB..0x442CE6]
        out.handler = weapon_handler_named(default_function);
        return out;
    }
    // An authored block keeps whatever FUNCTION bound -- the placeholder for
    // none, `null` or an unknown name [orig: ActionDef_ParseScriptLine
    // @0x40296E; the miss @0x4028F7]
    out.handler = weapon_handler_named(row.function[0] != '\0' ? row.function : nullptr);
    out.soundset = row.soundset;
    out.soundsetend = row.soundsetend;
    out.particle = row.particle;
    out.texttoken = row.texttoken;
    return out;
}

// [orig: Entity_RaiseHealthToMax @0x43C290 -- `if (Health < max) Health = max`,
//  the picker's ceiling Entity_GetMaxHealthWithDifficulty @0x43B8A0
//  (max_health_with_difficulty: the local player's doubles out of a session at
//  difficulty -1, the objective Co-op mission start's word)]
void raise_health_to_max(const World &world, Entity &e) {
    const int32_t max = max_health_with_difficulty(world, e);
    if (retail_signed_i16(e.health) < max) e.health = max;
}

// Ammo class 1 is the entity's own word: entity+288, the field this model
// carries as `mana`; the cap is the class-1 carry cap [orig: WeaponSlot_AddAmmo
// @0x540A20, the slot-1 leg @0x540A28..0x540A77 -- add, clamp to
// dword_24E7DE4, store the low word]. The vehicle-mount legs (@0x540A3A..
// 0x540A56) do not arise for a body on foot.
void add_class1(Entity &e, const WeaponTable &table, int32_t amount) {
    int32_t v = amount + retail_signed_i16(e.mana);
    const int32_t cap = table.ammo_class_caps.size() > 1 ? table.ammo_class_caps[1] : 0;
    if (v > cap) v = cap;
    e.mana = static_cast<int16_t>(v);
}

void play_action_sound(World &world, const PowerupAction &action, const Entity &at) {
    // [orig: ActionSlot_PlaySound @0x4010C0 -> Entity_PlaySound3D_FullVolume
    //  @0x528E20 at the entity's position (entity+4)]
    if (action.soundset.empty()) return;
    world.out.fire_sounds.play_immediate(action.soundset.c_str(), at.position, at.bms_id,
                                         at.handle.packed);
}

// The action's particle at the entity's position, unattached and undirected,
// when the row authored one (its interned handle word +0x10). The EFFECT
// userpoint the helper looks up first is overwritten unread.
// [orig: ActionSlot_SpawnParticleAtEntity @0x442380 -- the userpoint
//  @0x442384..0x4423A4, overwritten by the handle load @0x4423AC;
//  Effect_SubmitDescriptor(0, 0, entity+4, handle) @0x4423BD]
void spawn_action_particle(World &world, const PowerupAction &action, const Entity &at) {
    if (action.particle.empty()) return;
    world.out.destruction.effects.push_back(
            DestructionEffectEvent{action.particle, at.position, Vec3{}});
}

// The tables the authority's refusal walk reads for `picker`: the local
// player's own inventory (one table here for retail's g_WeaponSlotArrayBase and
// the listen host's own slot block, D-WPN-24's collapse), else the session
// slot's copy; null when neither exists, which WeaponSlot_RecalculateScore
// treats as a pass. [orig: WeaponSlot_RecalculateScore @0x542464..0x542519 --
// Entity_ValidatePtr, else the local table for the local player, else 0]
const WeaponInventory *authority_tables(const World &world, const Entity &picker,
                                        const WeaponInventory *local, WeaponInventory &remote) {
    if (local != nullptr) return local;
    if (picker.handle == world.cached.local_player) return nullptr;
    if (world.remote_weapon_tables == nullptr) return nullptr;
    return world.remote_weapon_tables->remote_weapon_tables(world, picker.handle, remote)
            ? &remote
            : nullptr;
}

} // namespace

void powerup_sync_local_weapon_grant(World &world, LocalPlayer &lp, const Entity &picker,
                                     const WeaponAvatarGrant &landed) {
    if (!landed.reloaded) return;
    // WeaponSlot_ReloadAmmo's entry stamps the picker's 3P reload window
    // [orig: @0x54173c; world-wac-ai-re.md §14.8.5]
    if (AiEntity *body = world.ai.for_handle(picker.handle))
        if (body->inf.active) body->inf.reload_anim_ticks = 80;
    // The held weapon's FSM carries its own magazine, which the next pump
    // mirrors back into the inventory; a refill of the equipped slot reaches it
    // now, its pending-reload bit cleared as the refill clears slot+0x5A's
    // [orig: WeaponSlot_ReloadAmmo @0x5417A2]
    if (!lp.weapon.active || lp.weapon.usegun_slot_active) return;
    if (lp.inventory.equipped_combo < 0 || lp.inventory.equipped_combo != landed.combo) return;
    WeaponSlotState &slot = *active_local_weapon_slot(world, lp.weapon);
    slot.clip = weapon_inventory_loaded_rounds(world.tables.weapons, lp.inventory,
                                               lp.inventory.equipped_combo);
    slot.phase = static_cast<uint8_t>(slot.phase & ~weapon_phase::kReloadPendingBit);
}

namespace {

// [orig: PowerupAction_Pickup @0x4428A0 (action_def = the row's pickup
//  ActionDef, action_slot = the powerup entity, entity = the picker)]
void action_pickup(World &world, const PowerupDef &def, const PowerupAction &action,
                   Entity &powerup, Entity &picker, const TickContext &ctx) {
    // A consumed row awaiting its respawn refuses [orig: @0x4428CD..0x4428D4]
    if (powerup.powerup_respawn_timer > 0) return;

    const bool local = picker.handle == world.cached.local_player;
    LocalPlayer *lp = local ? world.local_player_state : nullptr;
    WeaponInventory *inv = lp != nullptr && lp->inventory_valid ? &lp->inventory : nullptr;
    const WeaponTable &table = world.tables.weapons;
    PowerupGrant grant;
    grant.picker = picker.handle;
    bool grant_needed = false;
    // Retail's non-local pool arm is the authority's per-connection table
    // (Entity_ValidatePtr @0x540AC5). Retail writes that table inline, arm by
    // arm, so a grant staged by an earlier arm survives a later arm's return.
    // A non-authority peer also runs this for a REMOTE body (the joiner's
    // wire-replica picker): there retail's class arm adds to the LOCAL pools
    // whoever picked (`g_LocalAmmoPools` @0x540B20), a leg no shipped table
    // reaches (none authors `ammo`), so it stays unported, and `allammo`
    // touches only the local player (@0x4DC3F0).
    const bool remote_pools = inv == nullptr && ctx.is_authority;
    auto publish_grant = [&]() {
        if (grant_needed) world.out.powerup_grants.push_back(std::move(grant));
        grant_needed = false;
    };

    // The `weapon` grant; `all` (-1) and an unresolved name (0) skip it
    // [orig: @0x4428DA..0x44292A]. JO:CA's powerup.def authors no `weapon`;
    // the JOTAC mod's PU_* weapon pickups do.
    if (def.weapon != -1 && def.weapon != 0) {
        const uint8_t weapon = static_cast<uint8_t>(def.weapon);
        // The authority refuses a picker already carrying the weapon full:
        // the whole pickup ends, the row unconsumed [orig: WeaponSlot_RecalculateScore
        //  @0x4428E8, the refusal @0x4428F2]
        if (ctx.is_authority) {
            WeaponInventory remote;
            const WeaponInventory *tables = authority_tables(world, picker, inv, remote);
            if (tables != nullptr && weapon_inventory_weapon_full(table, *tables, weapon))
                return;
        }
        powerup.powerup_weapon = weapon; // +0x2B0 @0x4428FB
        if (inv != nullptr) {
            // The local player's own table [orig: WeaponSlot_InitFromAvatarDef
            //  (g_WeaponSlotArrayBase, picker, row) @0x442912]
            const WeaponAvatarGrant landed = weapon_inventory_init_from_avatar_def(table, *inv,
                    powerup.powerup_weapon, picker.player_class,
                    world.rules.allow_sniper_scope_zoom);
            powerup_sync_local_weapon_grant(world, *lp, picker, landed);
        }
        // The authority's slot-table copy and S2C 0x35, for every picker
        // [orig: Server_BroadcastWeaponOverlayUpdate @0x442925]
        if (ctx.is_authority)
            world.out.powerup_weapon_grants.push_back({picker.handle, powerup.handle, weapon});
    }

    // `allammo` [orig: @0x44292D..0x44294B -- WeaponSlot_RecalculateScore
    //  (entity, -1) @0x442936: the authority refuses a picker whose every held
    //  slot already scores its startrounds (the pack touched at full ammo stays,
    //  as a full med pack does); then Entity_UpdateWeaponOverlayFrameState
    //  @0x442943: the slot pools re-seeded from the defs and every clip redrawn;
    //  the connection-side seeded-slot tail is D-PWR-4]
    if (def.allammo && ctx.is_authority) {
        WeaponInventory remote;
        const WeaponInventory *tables = authority_tables(world, picker, inv, remote);
        if (tables != nullptr && weapon_inventory_all_full(table, *tables, picker.has_item_def)) {
            publish_grant();
            return;
        }
    }
    if (def.allammo) {
        if (inv != nullptr) {
            weapon_inventory_seed_pools(table, *inv, picker.player_class);
            weapon_inventory_recalc_clips(table, *inv);
            // Retail's slot table IS the running weapon's magazine (slot+16);
            // here the held weapon's FSM carries its own copy, which the next
            // pump mirrors back into the inventory, so the redrawn clip has to
            // reach it now [orig: the same slot word @0x542375 that
            //  WeaponAction_* read; player_weapon.cpp mirrors the FSM clip].
            if (inv->equipped_combo >= 0)
                lp->weapon.slot.clip = weapon_inventory_loaded_rounds(
                        table, *inv, inv->equipped_combo);
        } else if (remote_pools) {
            grant.allammo = true;
            grant_needed = true;
        }
    }

    // `hp` [orig: @0x44294B..0x4429D4]
    if (def.hp == -1) {
        const int32_t before = retail_signed_i16(picker.health); // @0x442954
        raise_health_to_max(world, picker);                       // @0x44295C
        if (retail_signed_i16(picker.health) == before) {
            // Already full: the local player's "full" cue reads a sound id no
            // code assigns (dword_24E0994 has no writer), so the 32-entry hold
            // refuses the null id and nothing plays; the pickup ends here,
            // unconsumed [orig: @0x44296B..0x4429A4; Server_TrackEntityInTable
            // @0x527B35 on a null id]
            publish_grant();
            return;
        }
    } else if (def.hp > 0) {
        const int32_t before = retail_signed_i16(picker.health); // @0x4429B2
        raise_health_to_max(world, picker);                       // @0x4429BA
        if (before >= retail_signed_i16(picker.health)) {        // @0x4429C9 -> the same silent cue
            publish_grant();
            return;
        }
        // The raise probed the ceiling; the grant is old + hp, stored as the
        // low word with no clamp [orig: @0x4429CD..0x4429D4]
        picker.health = retail_signed_i16(static_cast<int64_t>(before) + def.hp);
    }

    // `mana` = ammo class 1 [orig: @0x4429DB..0x442A45 -- a negative word skips;
    //  -1 zeroes it then fills, else the row's amount adds]
    if (retail_signed_i16(picker.mana) >= 0) {
        int32_t amount = def.mana;
        if (def.mana == -1) {
            picker.mana = 0;
            amount = kFillAmount;
        }
        add_class1(picker, table, amount);
    }

    // The per-class table, classes 11..count-1 [orig: @0x442A48..0x442A8E --
    //  -1 fills, 0 skips, else adds; g_WeaponClassCount @0x830B8C]
    const int32_t class_count = static_cast<int32_t>(table.ammo_class_names.size());
    for (int32_t cls = kFirstGrantedClass; cls < class_count && cls < 128; ++cls) {
        const int32_t row = def.ammo[static_cast<size_t>(cls)];
        const int32_t amount = row == -1 ? kFillAmount : row;
        if (amount == 0) continue;
        if (inv != nullptr) {
            weapon_pool_add(table, *inv, cls, amount);
        } else if (remote_pools) {
            grant.ammo_adds.emplace_back(cls, amount);
            grant_needed = true;
        }
    }

    // The action's texttoken, its text resolved at the load (the mission
    // text's entry, else the game text's, else ""), becomes the local
    // player's kill-announce banner line [orig: ActionSlot_OnTextTokenCallback
    // @0x4011B0 @0x442A9A; ActionDef_ParseScriptLine @0x4028A8]: JO:CA's rows
    // author none, JOTAC's PU_* rows do; not emitted here (D-PWR-3).
    // The soundset plays at the PICKER [orig: @0x442AA6, entity+4]
    play_action_sound(world, action, picker);
    // So does the action's particle [orig: the handle test @0x442AAE,
    //  ActionSlot_SpawnParticleAtEntity(action, row, picker) @0x442AB8]: no
    //  shipped pickup row authors one.
    spawn_action_particle(world, action, picker);

    // Consume: a respawning row hides (its model pointer zeroed) and arms the
    // countdown; otherwise the row is destroyed [orig: @0x442AC0..0x442B35]
    if (def.respawn_time != 0 && powerup.powerup_respawns_left != 0) {
        powerup.powerup_respawn_timer = def.respawn_time * kTicksPerSecond; // @0x442AD0..0x442AE1
        // The model pointer zeroed: the row neither draws nor collides until
        // the respawn restores it (CollisionWorld::live_instance reads this
        // pair as the withdrawn model)                                     // +0x30 = 0 @0x442AE7
        powerup.hidden = true;
        // The row's +0x1CC emitter (its ItemDef+0x274 particlefx, which no
        // shipped Powerup item authors) stops [orig: @0x442AF1] (D-PWR-3), and
        // its +0x1B4 glow handle clears [orig: CEffectInstance_ClearByHandle
        //  @0x442B17; the store Entity_SpawnGlowEffects @0x56C92C]: the light
        // director follows the row's visibility (renderer light_scene).
    } else {
        world.commands.remove_ssn(powerup.handle); // Entity_Destroy @0x442B27
    }
    publish_grant();
}

// [orig: PowerupAction_Respawn @0x442B40 (action_slot = the powerup row,
//  entity = the entity the invoker passed)]
void action_respawn(World &world, const PowerupAction &action, Entity &powerup,
                    const Entity &at) {
    if (!powerup.has_item_def) return; // @0x442B4F
    // The model pointer returns from the ItemDef (+0xF0) [orig: @0x442B6B]
    powerup.hidden = false;
    play_action_sound(world, action, at); // @0x442B6E
    // The action's particle at the entity, the row itself from the countdown
    // [orig: the handle test @0x442B76, ActionSlot_SpawnParticleAtEntity
    //  (action, row, entity) @0x442B80]: JOTAC's PU_* respawn rows author
    //  FX_Pickup_Green.
    spawn_action_particle(world, action, at);
    // A row with an item ordinal re-spawns its model's LGHT glow records
    // [orig: @0x442B88..0x442BA0 -> Entity_SpawnGlowEffects @0x56C7C0]: the
    // light director follows the row's visibility (renderer light_scene).
}

void run_action(World &world, const PowerupDef &def, const PowerupAction &action,
                Entity &powerup, Entity &entity, const TickContext &ctx) {
    switch (action.handler) {
    case weapon_handler::kPowerupPickup:
        action_pickup(world, def, action, powerup, entity, ctx);
        break;
    case weapon_handler::kPowerupRespawn:
        action_respawn(world, action, powerup, entity);
        break;
    default:
        // The placeholder and every weapon handler run nothing for a powerup row
        // [orig: ActionSlot_ExecuteAction @0x4020A0]
        break;
    }
}

} // namespace

int32_t PowerupTable::index_of(const char *name) const {
    // [orig: PowerUpDef_FindByName @0x442660 -- stricmp over the rows in order]
    if (name == nullptr) return -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (strutil::iequals(rows[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

PowerupTable build_powerup_table(const def::DefPowerupFile &file, const WeaponTable &weapons) {
    PowerupTable table;
    table.loaded = true;
    table.rows.reserve(file.count);
    for (size_t i = 0; i < file.count; ++i) {
        const def::DefPowerupDef &row = file.entries[i];
        PowerupDef def;
        def.name = row.name;
        def.respawn_time = row.respawn_time;
        def.max_respawns = row.max_respawns;
        def.hp = row.hp;
        def.mana = row.mana;
        def.allammo = row.allammo != 0;
        if (row.weapon_all) {
            def.weapon = -1; // @0x4431DA
        } else if (row.weapon[0] != '\0') {
            // The weapon table index, the low byte of the def ordinal
            // [orig: AdmDef_GetIndexFromPtr @0x443211]; a miss leaves 0
            const int index = weapons.index_of(row.weapon);
            def.weapon = index >= 0 ? (index & 0xFF) : 0;
        }
        for (size_t a = 0; a < row.ammo_count; ++a) {
            // An unknown class is the "ammo class error" row: dropped
            // [orig: @0x44325D..0x44327D]; class 0 is the error id there too.
            const int cls = weapons.ammo_class_id_of(row.ammo[a].class_name);
            if (cls <= 0 || cls >= 128) continue;
            def.ammo[static_cast<size_t>(cls)] = row.ammo[a].count;
        }
        def.pickup = make_action(row.pickup, "powerup_pickup");
        def.respawn = make_action(row.respawn, "powerup_respawn");
        table.rows.push_back(std::move(def));
    }
    return table;
}

void powerup_bind_entities(World &world, const def::DefItemsFile &items) {
    std::vector<EntityHandle> rows;
    for (int pool = 1; pool <= 2; ++pool) {
        world.registry.for_each_in_pool(pool, [&](const Entity &e) {
            // The walk's own gates: a live item ordinal and the ItemDef's
            // Powerup bit [orig: @0x4432BF / @0x4432CC (pool 1), @0x44330B /
            // @0x443318 (pool 2)]
            if (e.item_type_index == 0 || !e.has_item_def) return;
            if ((e.item_attrib & kItemAttribPowerup) == 0) return;
            rows.push_back(e.handle);
        });
    }
    for (const EntityHandle h : rows) {
        Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        const char *name = e->item_type_index >= 0 &&
                        static_cast<size_t>(e->item_type_index) < items.count
                ? items.entries[e->item_type_index].powerup_def
                : "";
        // [orig: sub_442D00 @0x442D00 -- PowerUpDef_FindByName(itemDef+0x890)
        //  @0x442D10; the miss destroys the row @0x442E26]
        const int32_t index = world.tables.powerups.index_of(name);
        if (index < 0) {
            world.commands.remove_ssn(h);
            continue;
        }
        const PowerupDef &def = world.tables.powerups.rows[static_cast<size_t>(index)];
        e->powerup_def_index = index;                            // +0x2C0 @0x442D2A
        // max_respawns seeds max-1 remaining; 0 (unset) is unlimited (-1)
        // [orig: @0x442D42..0x442D59]
        e->powerup_respawns_left = def.max_respawns != 0 ? def.max_respawns - 1 : -1;
        e->powerup_respawn_timer = -1;                           // +0x2B4 @0x442D65
        // The EFFECT userpoint and the ItemDef+0x274 (particlefx) emitter bind
        // [orig: @0x442D60..0x442E18]: no shipped Powerup item authors
        // particlefx, and the userpoint's one reader overwrites it unread
        // (D-PWR-3).
    }
}

void powerup_pickup(World &world, EntityHandle powerup, EntityHandle picker,
                    const TickContext &ctx) {
    Entity *body = world.registry.get(picker);
    if (body == nullptr) return; // @0x4428A6
    powerup_pickup_by(world, powerup, *body, ctx);
}

void powerup_pickup_by(World &world, EntityHandle powerup, Entity &picker,
                       const TickContext &ctx) {
    Entity *row = world.registry.get(powerup);
    if (row == nullptr) return; // @0x4428B2
    // The row's +0x2B8 callback is its def's pickup action; an unbound row has
    // none [orig: Entity_InvokeCollisionCallback @0x442358..0x44236B]
    const PowerupDef *def = world.tables.powerups.by_index(row->powerup_def_index);
    if (def == nullptr) return;
    run_action(world, *def, def->pickup, *row, picker, ctx);
}

void powerup_process_contacts(World &world, const TickContext &ctx) {
    if (world.collision == nullptr) return;
    const std::vector<CollisionWorld::GameplayContact> contacts =
            world.collision->take_powerup_contacts();
    for (const CollisionWorld::GameplayContact &contact : contacts)
        powerup_pickup(world, contact.target, contact.source, ctx);
}

bool powerup_weapon_grant_received(World &world, LocalPlayer &lp, const Entity *row,
                                   bool local_dead) {
    // [orig: sub_4E03D0 @0x4E03D0 -- the local-player and null gates
    //  @0x4E03DC..0x4E03E8, the dead bit @0x4E03EE, the row's +0x155 byte
    //  @0x4E03F0 (set only on a dropped carried object and a 0x18 drop
    //  template, neither a powerup row; not carried here)]
    Entity *player = world.registry.get(world.cached.local_player);
    if (player == nullptr || row == nullptr || local_dead) return false;
    if (!lp.inventory_valid) return false;
    const WeaponTable &table = world.tables.weapons;
    const WeaponAvatarGrant landed = weapon_inventory_init_from_avatar_def(table, lp.inventory,
            row->powerup_weapon, player->player_class, world.rules.allow_sniper_scope_zoom);
    if (landed.combo < 0) return false;                          // @0x4E040B
    powerup_sync_local_weapon_grant(world, lp, *player, landed);
    // Player_MountWeaponSlot(slot) @0x4E040E
    local_player_mount_weapon_slot(world, lp.weapon, lp.inventory, lp.view, landed.combo);
    return true;
}

void powerup_tick(World &world, const TickContext &ctx) {
    if (!ctx.is_authority) return; // @0x442862
    if (world.tables.powerups.rows.empty()) return;
    std::vector<EntityHandle> rows;
    for (int pool = 1; pool <= 2; ++pool) {
        world.registry.for_each_in_pool(pool, [&](const Entity &e) {
            if (e.powerup_def_index >= 0) rows.push_back(e.handle);
        });
    }
    for (const EntityHandle h : rows) {
        Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if (e->powerup_respawns_left == 0) continue; // the +0x2C8 gate @0x442862
        const int32_t timer = e->powerup_respawn_timer;
        if (timer > 0) {
            e->powerup_respawn_timer = timer - 1; // @0x442878
            continue;
        }
        if (timer != 0) continue; // idle (-1)
        e->powerup_respawn_timer = -1; // @0x442883
        // [orig: Entity_InvokeFireCallback @0x442810 -- the +0x2BC respawn row
        //  run with the row as both entity arguments @0x442828; the +0x2C6
        //  emitter re-spawn @0x442838 has no bound emitter on a shipped row]
        const PowerupDef *def = world.tables.powerups.by_index(e->powerup_def_index);
        if (def != nullptr) run_action(world, *def, def->respawn, *e, *e, ctx);
        if (Entity *again = world.registry.get(h)) --again->powerup_respawns_left; // @0x442895
    }
}

} // namespace opennova::world
