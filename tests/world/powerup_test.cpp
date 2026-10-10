// The powerup family (world/powerup.h): the mission-start bind, the pickup's
// weapon / health / class-1 / per-class / allammo arms and its consume
// outcomes, the authority's refusal walks, the authority-only respawn
// countdown, the remote-player grants and the S2C 0x35 weapon fan and receive.
// [orig: sub_442D00 @0x442D00; PowerupAction_Pickup @0x4428A0;
//  WeaponSlot_RecalculateScore @0x542450; WeaponSlot_InitFromAvatarDef
//  @0x542730; Server_BroadcastWeaponOverlayUpdate @0x509FC0; sub_4E03D0
//  @0x4E03D0; Entity_TickFireTimer @0x442850; PowerupAction_Respawn @0x442B40]
#include <runtime/world/powerup.h>

#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_powerup.h>
#include <runtime/world/player_weapon.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace opennova::world;
namespace def = opennova::def;
namespace im = opennova::inmatch;

static int failures = 0;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

namespace {

constexpr int32_t kFillAmount = 0x7FFF;

// A weapon table with the built-in class ids 0..10 and three granted classes
// (11 "5.56mm" cap 120, 12 "7.62mm" cap 90, 13 "grenade" cap 4); class 1 caps
// at 6 (the entity+288 word).
WeaponTable weapons() {
    WeaponTable t;
    const char *names[] = {"null", "class1", "c2", "c3", "c4", "c5", "c6",
                           "c7", "c8", "c9", "c10", "5.56mm", "7.62mm", "grenade"};
    for (const char *n : names) t.ammo_class_names.emplace_back(n);
    t.ammo_class_caps.assign(t.ammo_class_names.size(), 0);
    t.ammo_class_caps[1] = 6;
    t.ammo_class_caps[11] = 120;
    t.ammo_class_caps[12] = 90;
    t.ammo_class_caps[13] = 4;
    // One populated weapon def at index 4: a 30-round magazine on class 11 with
    // 90 start rounds, so the `allammo` re-seed and clip redraw have a slot.
    t.entries.resize(10);
    auto weapon = [&](int index, const char *name, uint8_t category, uint8_t rank,
                      int16_t clipsize, int16_t startrounds,
                      int16_t ammo_class) -> WeaponTableEntry & {
        WeaponTableEntry &e = t.entries[static_cast<size_t>(index)];
        e.valid = true;
        e.name = name;
        e.category = category;
        e.rank = rank;
        e.clipsize = clipsize;
        e.startrounds = startrounds;
        e.ammo_class_id = ammo_class;
        e.ammo_class_count = 1;
        return e;
    };
    t.entries[0].valid = true; // the engine's "null" row 0: category 0 rank 0
    t.entries[0].name = "null";
    weapon(4, "WPN_TEST", 5, 1, 30, 90, 11);
    // The `weapon` grant's defs: a pistol, a variant that refills it (`sameas`),
    // and a launcher whose loadout_subclasses entry follows it.
    weapon(5, "WPN_PISTOL", 2, 1, 7, 21, 12);
    weapon(6, "WPN_PISTOL_B", 2, 2, 7, 14, 12).sameas = "wpn_pistol";
    weapon(8, "WPN_LAUNCHER", 4, 1, 1, 3, 13).loadout_subclasses = 1;
    weapon(9, "WPN_LAUNCHER_ALT", 4, 2, 1, 2, 13);
    return t;
}

constexpr int32_t kRifleCombo = 5 * 65 + 1;
constexpr int32_t kPistolCombo = 2 * 65 + 1;
constexpr int32_t kPistolBCombo = 2 * 65 + 2;
constexpr int32_t kLauncherCombo = 4 * 65 + 1;
constexpr int32_t kLauncherAltCombo = 4 * 65 + 2;

def::DefPowerupFile shipped_and_synthetic() {
    static const char kText[] =
            "powerup \"FULLHP\"\r\n hp -1\r\n respawn_time 30\r\n max_respawns 1\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset HEALTH_UP\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            "powerup \"FULLHP_INF\"\r\n hp -1\r\n respawn_time 30\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset HEALTH_UP\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            "powerup \"AmmoFull\"\r\n respawn_time 100\r\n max_respawns 1\r\n allammo\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset PU_AMMO\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            // Synthetic rows for the arms no shipped row reaches.
            "powerup \"HP25\"\r\n hp 25\r\n respawn_time 0\r\nend\r\n"
            "powerup \"CLASSES\"\r\n mana -1\r\n ammo 5.56mm 30\r\n ammo grenade -1\r\n"
            " ammo 7.62mm 0\r\n ammo unknown_class 9\r\nend\r\n"
            "powerup \"NULLFN\"\r\n hp -1\r\n action \"pickup\"\r\n  function null\r\n end\r\nend\r\n"
            "powerup \"WPN\"\r\n hp 10\r\n weapon all\r\nend\r\n"
            "powerup \"MEDAMMO\"\r\n hp -1\r\n allammo\r\n respawn_time 5\r\nend\r\n"
            // JOTAC's PU_* shape: a weapon, a respawn particle and a pickup
            // sound and text token.
            "powerup \"PU_PISTOL\"\r\n respawn_time 120\r\n weapon WPN_PISTOL\r\n"
            " action \"respawn\"\r\n  particle FX_Pickup_Green\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset GF_45_ST\r\n  function powerup_pickup\r\n"
            "  texttoken PU_PISTOL\r\n end\r\nend\r\n"
            "powerup \"PU_PISTOLB\"\r\n respawn_time 120\r\n weapon WPN_PISTOL_B\r\nend\r\n"
            "powerup \"PU_LAUNCHER\"\r\n respawn_time 120\r\n weapon WPN_LAUNCHER\r\nend\r\n";
    def::DefPowerupFile f;
    if (def::def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kText),
                                      sizeof(kText) - 1, &f) != 0)
        std::abort();
    return f;
}

struct Rig {
    World world;
    std::unique_ptr<LocalPlayer> local;
    def::DefItemsFile items{};
    EntityHandle picker;

    Rig() {
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(1, 8);
        world.registry.configure_pool(2, 4);
        world.tables.weapons = weapons();
        // A presenting peer stamps its listener; without one the one-shot
        // queue readies nothing (the dedicated-host gate).
        world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
        def::DefPowerupFile file = shipped_and_synthetic();
        world.tables.powerups = build_powerup_table(file, world.tables.weapons);
        def::def_free_powerup(&file);
        // items.def rows: ordinal 1 = the med pack (FULLHP), 2 = the infinite med
        // pack, 3 = the ammo pack, 4 = HP25, 5 = CLASSES, 6 = NULLFN, 7 = an
        // unknown powerupdef name, 8 = WPN, 9 = MEDAMMO, 10..12 = the weapon
        // pickups.
        items.count = 13;
        items.entries = static_cast<def::DefItemDef *>(std::calloc(items.count, sizeof(def::DefItemDef)));
        const char *names[] = {"", "fullhp", "fullhp_Inf", "AmmoFull", "HP25", "CLASSES",
                               "NULLFN", "NOT_A_ROW", "WPN", "MEDAMMO", "PU_PISTOL",
                               "PU_PISTOLB", "PU_LAUNCHER"};
        for (size_t i = 0; i < items.count; ++i)
            std::snprintf(items.entries[i].powerup_def, sizeof(items.entries[i].powerup_def),
                          "%s", names[i]);
        picker = spawn_player(true);
    }
    ~Rig() { std::free(items.entries); }

    EntityHandle spawn_player(bool local_player) {
        Entity e;
        e.kind = EntityKind::Organic;
        e.item_id = 5305;
        e.has_item_def = true;
        e.player_class = 8;
        e.health = 60;
        e.health_max = 100;
        e.mana = 2;
        e.alive = true;
        e.flags = kEntityFlagPlayer;
        e.engine_flags = kEntityFlagPlayer;
        e.position = {5.0f, 5.0f, 0.0f};
        const EntityHandle h = world.registry.spawn(0, e);
        if (local_player) {
            world.cached.local_player = h;
            local = std::make_unique<LocalPlayer>(world);
            local->inventory.reset(world.tables.weapons);
            local->inventory.pools[11] = 10;
            local->inventory_valid = true;
            world.local_player_state = local.get();
        }
        return h;
    }

    EntityHandle spawn_powerup(int32_t item_type_index, int pool = 1) {
        Entity e;
        e.kind = EntityKind::Item;
        e.item_id = 2041;
        e.has_item_def = true;
        e.item_type_index = item_type_index;
        e.item_attrib = kItemAttribPowerup;
        e.item_type = def::DEF_ITEM_TYPE_POWERUP;
        e.alive = true;
        e.position = {5.0f, 5.0f, 0.0f};
        return world.registry.spawn(pool, e);
    }

    TickContext ctx(bool authority) {
        TickContext c;
        c.world = &world;
        c.is_authority = authority;
        c.logic_tick = 100;
        return c;
    }

    Entity *get(EntityHandle h) { return world.registry.get(h); }
};

// The table build: rows, first-wins lookup, the action bind and the
// weapon/ammo name resolution.
void test_table_build() {
    Rig rig;
    const PowerupTable &t = rig.world.tables.powerups;
    CHECK(t.loaded);
    CHECK(t.rows.size() == 11);
    CHECK(t.index_of("fullhp") == 0);
    CHECK(t.index_of("FULLHP_INF") == 1);
    CHECK(t.index_of("nope") == -1);
    CHECK(t.index_of(nullptr) == -1);
    const PowerupDef &fullhp = t.rows[0];
    CHECK(fullhp.pickup.authored && fullhp.pickup.handler == weapon_handler::kPowerupPickup);
    CHECK(fullhp.pickup.soundset == "HEALTH_UP");
    CHECK(fullhp.respawn.authored && fullhp.respawn.handler == weapon_handler::kPowerupRespawn);
    // An unauthored block binds the default handler with no sound.
    const PowerupDef &hp25 = t.rows[3];
    CHECK(!hp25.pickup.authored && hp25.pickup.handler == weapon_handler::kPowerupPickup);
    CHECK(!hp25.respawn.authored && hp25.respawn.handler == weapon_handler::kPowerupRespawn);
    CHECK(hp25.pickup.soundset.empty());
    // The class rows resolve by name; an unknown class is dropped; 0 stays 0.
    const PowerupDef &classes = t.rows[4];
    CHECK(classes.mana == -1);
    CHECK(classes.ammo[11] == 30);
    CHECK(classes.ammo[13] == -1);
    CHECK(classes.ammo[12] == 0);
    // `function null` keeps the placeholder: the row runs nothing.
    CHECK(t.rows[5].pickup.authored && t.rows[5].pickup.handler == weapon_handler::kPlaceholder);
    CHECK(t.rows[6].weapon == -1);
    // `weapon <name>` resolves to the weapon table index.
    CHECK(t.rows[8].weapon == 5 && t.rows[9].weapon == 6 && t.rows[10].weapon == 8);
}

// The bind: each Powerup row takes its def, the respawn seeds, unknown names
// destroy the row, non-Powerup rows and rows without an ordinal are skipped.
void test_bind() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    const EntityHandle med_inf = rig.spawn_powerup(2, 2);
    const EntityHandle ammo = rig.spawn_powerup(3);
    const EntityHandle unknown = rig.spawn_powerup(7);
    const EntityHandle no_ordinal = rig.spawn_powerup(0);
    Entity crate;
    crate.kind = EntityKind::Item;
    crate.has_item_def = true;
    crate.item_type_index = 1;
    crate.item_attrib = 0;
    const EntityHandle plain = rig.world.registry.spawn(1, crate);

    powerup_bind_entities(rig.world, rig.items);

    const Entity *m = rig.get(med);
    CHECK(m != nullptr && m->powerup_def_index == 0);
    CHECK(m != nullptr && m->powerup_respawns_left == 0); // max_respawns 1 -> one-shot
    CHECK(m != nullptr && m->powerup_respawn_timer == -1);
    const Entity *mi = rig.get(med_inf);
    CHECK(mi != nullptr && mi->powerup_def_index == 1);
    CHECK(mi != nullptr && mi->powerup_respawns_left == -1); // unset -> unlimited
    const Entity *a = rig.get(ammo);
    CHECK(a != nullptr && a->powerup_def_index == 2 && a->powerup_respawns_left == 0);
    CHECK(rig.get(unknown) == nullptr); // Entity_Destroy on the lookup miss
    const Entity *n = rig.get(no_ordinal);
    CHECK(n != nullptr && n->powerup_def_index == -1);
    const Entity *p = rig.get(plain);
    CHECK(p != nullptr && p->powerup_def_index == -1);
}

// The bind with no table loaded destroys every Powerup row (retail's
// "Unable to load powerup.def" outcome).
void test_bind_without_table() {
    Rig rig;
    rig.world.tables.powerups = PowerupTable{};
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    CHECK(rig.get(med) == nullptr);
}

// FULLHP on a hurt local player: health to the ceiling, the sound at the
// picker, the one-shot row destroyed.
void test_med_pack_one_shot() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    const Entity *p = rig.get(rig.picker);
    CHECK(p != nullptr && p->health == 100);
    CHECK(rig.get(med) == nullptr);
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(std::string(sounds[0].set_name) == "HEALTH_UP");
    CHECK(rig.world.out.powerup_grants.empty());
}

// A client's copy of a REMOTE player's pickup (the joiner's wire-replica
// body, a transient picker with no registry entity): the pack's sound plays
// at the remote body and this peer's copy of the row is consumed; the local
// player's inventory and health are untouched and no grant is staged.
// [orig: PowerupAction_Pickup @0x4428A0 (the soundset at the picker
//  @0x442AA6, the consume @0x442AC0..0x442B35), reached from a client's
//  resolver for a remote body]
void test_remote_body_pickup_on_a_client() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    Entity remote;
    remote.has_item_def = true;
    remote.health_max = 100;
    remote.health = 87; // a tier-2 class byte's midpoint
    remote.position = {9.0f, 5.0f, 0.0f};
    const int32_t local_pool = rig.local->inventory.pools[11];
    powerup_pickup_by(rig.world, med, remote, rig.ctx(false));
    CHECK(rig.get(med) == nullptr); // FULLHP is a one-shot: the copy is destroyed
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) {
        CHECK(std::string(sounds[0].set_name) == "HEALTH_UP");
        CHECK(sounds[0].pos.x == 9.0f);
    }
    CHECK(rig.get(rig.picker)->health == 60);
    CHECK(rig.local->inventory.pools[11] == local_pool);
    CHECK(rig.world.out.powerup_grants.empty());
}

// FULLHP at full health: nothing happens, the row stays, no sound and no
// particle (the refusal returns ahead of both [orig: @0x4429A4, ahead of
// @0x442AA6 / @0x442AB8]).
void test_med_pack_refuses_when_full() {
    Rig rig;
    rig.get(rig.picker)->health = 100;
    rig.world.tables.powerups.rows[0].pickup.particle = "FX_Pickup_Spark";
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    const Entity *m = rig.get(med);
    CHECK(m != nullptr && !m->hidden && m->powerup_respawn_timer == -1);
    CHECK(rig.get(rig.picker)->health == 100);
    CHECK(rig.world.out.fire_sounds.drain().empty());
    CHECK(rig.world.out.destruction.effects.empty());
}

// FULLHP_INF: the row hides and arms 30 s; the authority's countdown respawns
// it and spends nothing of an unlimited count; a non-authority never counts.
void test_infinite_med_pack_respawns() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(2);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(false));
    Entity *m = rig.get(med);
    CHECK(m != nullptr && m->hidden && m->powerup_respawn_timer == 30 * 62);
    CHECK(rig.get(rig.picker)->health == 100);
    (void)rig.world.out.fire_sounds.drain();
    // A second touch while consumed refuses.
    rig.get(rig.picker)->health = 10;
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(false));
    CHECK(rig.get(rig.picker)->health == 10);
    CHECK(rig.world.out.fire_sounds.drain().empty());
    // No countdown off the authority.
    powerup_tick(rig.world, rig.ctx(false));
    CHECK(rig.get(med)->powerup_respawn_timer == 30 * 62);
    for (int i = 0; i < 30 * 62; ++i) powerup_tick(rig.world, rig.ctx(true));
    m = rig.get(med);
    CHECK(m != nullptr && m->hidden && m->powerup_respawn_timer == 0);
    powerup_tick(rig.world, rig.ctx(true)); // zero fires the respawn
    m = rig.get(med);
    CHECK(m != nullptr && !m->hidden && m->powerup_respawn_timer == -1);
    CHECK(m != nullptr && m->powerup_respawns_left == -2); // unlimited counts down past -1
    CHECK(rig.world.out.fire_sounds.drain().empty()); // the shipped respawn row has no sound
    // Idle rows stay idle.
    powerup_tick(rig.world, rig.ctx(true));
    CHECK(rig.get(med)->powerup_respawn_timer == -1);
    // And it can be taken again.
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 100);
}

// A counted respawn: max_respawns 2 -> one respawn, then the next pickup destroys.
void test_counted_respawns() {
    Rig rig;
    rig.world.tables.powerups.rows[0].max_respawns = 2;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    CHECK(rig.get(med)->powerup_respawns_left == 1);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(med) != nullptr && rig.get(med)->hidden);
    for (int i = 0; i <= 30 * 62; ++i) powerup_tick(rig.world, rig.ctx(true));
    CHECK(rig.get(med) != nullptr && !rig.get(med)->hidden);
    CHECK(rig.get(med)->powerup_respawns_left == 0);
    rig.get(rig.picker)->health = 1;
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(med) == nullptr);
}

// hp 25: the raise probes the ceiling, the grant is old + 25 with no clamp.
void test_hp_add() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(4);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 90;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 115);
    CHECK(rig.get(row) == nullptr); // respawn_time 0 destroys
    // At the ceiling the add refuses.
    const EntityHandle again = rig.spawn_powerup(4);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 100;
    powerup_pickup(rig.world, again, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 100);
    CHECK(rig.get(again) != nullptr);
}

// The class arms on the local inventory: mana -1 zeroes then fills class 1 to
// its cap; 5.56mm adds 30 onto the pool of 10; grenade -1 fills to the cap;
// a 0 row leaves the class alone.
void test_class_grants_local() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->mana = 2;
    rig.local->inventory.pools[12] = 7;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->mana == 6);
    CHECK(rig.local->inventory.pools[11] == 40);
    CHECK(rig.local->inventory.pools[13] == 4);
    CHECK(rig.local->inventory.pools[12] == 7);
    CHECK(rig.world.out.powerup_grants.empty());
    // A negative class-1 word skips the mana arm.
    const EntityHandle again = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->mana = -1;
    powerup_pickup(rig.world, again, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->mana == -1);
}

// A remote player on the authority: health lands on the entity, the class
// adds and the allammo re-seed go out as one grant.
void test_remote_player_grant() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    const EntityHandle classes = rig.spawn_powerup(5);
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, classes, remote, rig.ctx(true));
    CHECK(rig.get(remote)->mana == 6);
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (rig.world.out.powerup_grants.size() == 1) {
        const PowerupGrant &g = rig.world.out.powerup_grants[0];
        CHECK(g.picker == remote);
        CHECK(!g.allammo);
        CHECK(g.ammo_adds.size() == 2);
        if (g.ammo_adds.size() == 2) {
            CHECK(g.ammo_adds[0].first == 11 && g.ammo_adds[0].second == 30);
            CHECK(g.ammo_adds[1].first == 13 && g.ammo_adds[1].second == kFillAmount);
        }
    }
    rig.world.out.powerup_grants.clear();
    powerup_pickup(rig.world, ammo, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (rig.world.out.powerup_grants.size() == 1)
        CHECK(rig.world.out.powerup_grants[0].allammo);
    CHECK(rig.get(ammo) == nullptr); // one-shot
    // The local pools are untouched by a remote pickup.
    CHECK(rig.local->inventory.pools[11] == 10);
}

// `allammo` on the local player re-seeds the pools from the defs and keeps the
// health arm out of it. The authority's walk refuses a player whose every held
// slot already scores its startrounds -- an empty kit included, since the walk
// finds no slot to disagree -- so the pack stays; a joiner's own copy takes it.
// [orig: WeaponSlot_RecalculateScore @0x54253D..0x5425A4 -> the refusal @0x442940]
void test_allammo_local() {
    {
        Rig rig;
        const EntityHandle ammo = rig.spawn_powerup(3);
        powerup_bind_entities(rig.world, rig.items);
        rig.get(rig.picker)->health = 30;
        powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
        CHECK(rig.get(ammo) != nullptr && !rig.get(ammo)->hidden);
        CHECK(rig.world.out.fire_sounds.drain().empty());
    }
    Rig rig;
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 30;
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(false));
    CHECK(rig.get(rig.picker)->health == 30);
    CHECK(rig.get(ammo) == nullptr);
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(std::string(sounds[0].set_name) == "PU_AMMO");
}

// The authority refuses `allammo` to a full kit (every held slot at its def's
// startrounds) and takes it once one slot is short.
void test_allammo_refuses_full_kit() {
    Rig rig;
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kRifleCombo].adm_index = 4;
    inv.slots[kRifleCombo].clip = 30;
    inv.pools[11] = 60; // 60 + 30 = the rifle's 90 startrounds
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
    CHECK(rig.get(ammo) != nullptr && !rig.get(ammo)->hidden);
    inv.pools[11] = 59;
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
    CHECK(rig.get(ammo) == nullptr);
    // seed 90, the 30 loaded rounds return (120, the cap), a clip is drawn (90)
    CHECK(inv.pools[11] == 90 && inv.slots[kRifleCombo].clip == 30);
}

// `allammo` on a populated local slot: the class pool re-seeds from the def,
// the magazine returns and redraws, and the held weapon's FSM takes the redrawn
// clip now (the next pump mirrors the FSM clip back into the inventory).
void test_allammo_refills_populated_slot() {
    Rig rig;
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kRifleCombo].adm_index = 4;
    inv.slots[kRifleCombo].clip = 3;
    inv.equipped_combo = kRifleCombo;
    inv.pools[11] = 0;
    rig.local->weapon.slot.clip = 3;
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
    // seed 90, the 3 loaded rounds return (93), a 30-round clip is drawn (63)
    CHECK(inv.pools[11] == 63);
    CHECK(inv.slots[kRifleCombo].clip == 30);
    CHECK(rig.local->weapon.slot.clip == 30);
    CHECK(rig.world.out.powerup_grants.empty());
}

// A row that refills AND heals, touched at full health by a remote player: the
// refill ran before the health arm (retail's arm order), so its grant survives
// the silent unconsumed return.
void test_full_health_keeps_staged_grant() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    rig.get(remote)->health = 100;
    const EntityHandle row = rig.spawn_powerup(9);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (!rig.world.out.powerup_grants.empty())
        CHECK(rig.world.out.powerup_grants[0].allammo);
    const Entity *r = rig.get(row);
    CHECK(r != nullptr && !r->hidden && r->powerup_respawn_timer == -1);
    CHECK(rig.world.out.fire_sounds.drain().empty());
}

// The host drains a remote player's grants onto ITS connection: the seed and
// clip redraw over the connection's slot rows, the class adds onto its pools;
// another player's connection and an unowned grant are left alone.
void test_server_applies_remote_grants() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    const EntityHandle other = rig.spawn_player(false);
    const EntityHandle ammo = rig.spawn_powerup(3);
    const EntityHandle classes = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);

    im::NapiNPServerCtx server;
    server.np_protocol.connection_list.emplace_back();
    server.np_protocol.connection_list.emplace_back();
    im::NapiNPConnection &conn = server.np_protocol.connection_list[0];
    conn.phase = im::ConnectionPhase::PlayerAdded;
    conn.link.owned_entity = remote;
    conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].adm_index = 4;
    conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].clip = 3;
    conn.reply.ammo_pools[11] = 0;
    im::NapiNPConnection &bystander = server.np_protocol.connection_list[1];
    bystander.phase = im::ConnectionPhase::PlayerAdded;
    bystander.link.owned_entity = other;
    bystander.reply.ammo_pools[11] = 7;

    powerup_pickup(rig.world, ammo, remote, rig.ctx(true));
    powerup_pickup(rig.world, classes, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 2);
    // A grant no connection owns is dropped with the rest of the drain.
    PowerupGrant orphan;
    orphan.picker = EntityHandle::make(0, 3);
    orphan.ammo_adds.emplace_back(11, 5);
    rig.world.out.powerup_grants.push_back(orphan);

    im::Server_ApplyPowerupGrants(server.np_protocol.connection_list, rig.world);
    CHECK(rig.world.out.powerup_grants.empty());
    // allammo: seed 90 + the 3 returned rounds - the 30-round redraw = 63,
    // then the class grant adds 30 (cap 120)
    CHECK(conn.reply.ammo_pools[11] == 93);
    CHECK(conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].clip == 30);
    CHECK(conn.reply.ammo_pools[13] == 4);
    CHECK(bystander.reply.ammo_pools[11] == 7);
    // The listen host's own player never routes through a grant.
    CHECK(rig.local->inventory.pools[11] == 10);
}

// A `function null` pickup row runs nothing: no health, no consume.
void test_placeholder_row_runs_nothing() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(6);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 60);
    CHECK(rig.get(row) != nullptr && !rig.get(row)->hidden);
}

// `weapon all` (-1) skips the arm; the rest of the row still runs
// [orig: `cmp eax, -1` @0x4428DD].
void test_weapon_all_skips_the_arm() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(8);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 70);
    CHECK(rig.world.out.powerup_weapon_grants.empty());
}

// A JOTAC-shaped PU_* row on the authority's own player: the weapon lands in
// its slot with a drawn clip and its class pool at startrounds less the clip,
// the row keeps the weapon byte, consumes into its countdown, and the
// authority records the grant for its S2C 0x35 (the listen host's own pickup
// does not mount the weapon).
// [orig: PowerupAction_Pickup @0x4428DA..0x44292A; WeaponSlot_InitFromAvatarDef
//  @0x542730]
void test_weapon_grant_lands_local() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(10);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kRifleCombo].adm_index = 4;
    inv.equipped_combo = kRifleCombo;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(inv.slots[kPistolCombo].adm_index == 5);
    CHECK(inv.slots[kPistolCombo].clip == 7);
    CHECK(inv.pools[12] == 14);
    CHECK(inv.equipped_combo == kRifleCombo);
    const Entity *r = rig.get(row);
    CHECK(r != nullptr && r->hidden && r->powerup_respawn_timer == 120 * 62);
    CHECK(r != nullptr && r->powerup_weapon == 5);
    CHECK(rig.world.out.powerup_weapon_grants.size() == 1);
    if (rig.world.out.powerup_weapon_grants.size() == 1) {
        const PowerupWeaponGrant &g = rig.world.out.powerup_weapon_grants[0];
        CHECK(g.picker == rig.picker && g.powerup == row && g.weapon == 5);
    }
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(std::string(sounds[0].set_name) == "GF_45_ST");
}

// The action particle: the pickup row's at the PICKER, the respawn row's at
// the row the countdown respawns, each unattached and undirected; a row that
// authors none spawns nothing. No shipped pickup row authors one, so the
// pickup leg runs on a row given one here.
// [orig: PowerupAction_Pickup @0x442AAE..0x442ABD; PowerupAction_Respawn
//  @0x442B76..0x442B80; ActionSlot_SpawnParticleAtEntity @0x442380 ->
//  Effect_SubmitDescriptor(0, 0, entity+4, handle) @0x4423BD]
void test_action_particles() {
    Rig rig;
    const int32_t pistol = rig.world.tables.powerups.index_of("PU_PISTOL");
    CHECK(pistol >= 0);
    if (pistol < 0) return;
    PowerupDef &def = rig.world.tables.powerups.rows[static_cast<size_t>(pistol)];
    CHECK(def.respawn.particle == "FX_Pickup_Green" && def.pickup.particle.empty());
    def.pickup.particle = "FX_Pickup_Spark";
    const EntityHandle row = rig.spawn_powerup(10);
    rig.get(row)->position = {7.0f, 3.0f, 1.0f};
    rig.get(rig.picker)->position = {6.0f, 2.0f, 0.5f};
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    const auto &effects = rig.world.out.destruction.effects;
    CHECK(effects.size() == 1);
    if (effects.size() == 1) {
        CHECK(effects[0].effect == "FX_Pickup_Spark");
        CHECK(effects[0].pos.x == 6.0f && effects[0].pos.y == 2.0f && effects[0].pos.z == 0.5f);
        CHECK(effects[0].dir.x == 0.0f && effects[0].dir.y == 0.0f && effects[0].dir.z == 0.0f);
        CHECK(effects[0].attach_net_id == 0 && effects[0].family == 0);
    }
    rig.world.out.destruction.effects.clear();
    // The countdown's respawn runs the respawn row with the row as the entity.
    Entity *r = rig.get(row);
    CHECK(r != nullptr && r->hidden);
    if (r != nullptr) r->powerup_respawn_timer = 0;
    powerup_tick(rig.world, rig.ctx(true));
    CHECK(rig.get(row) != nullptr && !rig.get(row)->hidden);
    CHECK(effects.size() == 1);
    if (effects.size() == 1) {
        CHECK(effects[0].effect == "FX_Pickup_Green");
        CHECK(effects[0].pos.x == 7.0f && effects[0].pos.y == 3.0f && effects[0].pos.z == 1.0f);
    }
    // The med pack's rows author no particle.
    rig.world.out.destruction.effects.clear();
    const EntityHandle med = rig.spawn_powerup(2);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 10;
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 100 && effects.empty());
    rig.get(med)->powerup_respawn_timer = 0;
    powerup_tick(rig.world, rig.ctx(true));
    CHECK(!rig.get(med)->hidden && effects.empty());
}

// The authority refuses a picker already carrying the weapon full: the whole
// pickup ends, the row unconsumed and no grant recorded. A joiner's own copy
// never refuses (the walk returns 0 off the authority) and refills it.
// [orig: WeaponSlot_RecalculateScore @0x54249E..0x54253C, the refusal @0x4428F2]
void test_weapon_grant_refused_when_full() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(10);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kPistolCombo].adm_index = 5;
    inv.slots[kPistolCombo].clip = 7;
    inv.pools[12] = 14; // 14 + 7 = 21 startrounds
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    const Entity *r = rig.get(row);
    CHECK(r != nullptr && !r->hidden && r->powerup_respawn_timer == -1);
    CHECK(r != nullptr && r->powerup_weapon == 0);
    CHECK(rig.world.out.powerup_weapon_grants.empty());
    CHECK(rig.world.out.fire_sounds.drain().empty());
    inv.slots[kPistolCombo].clip = 1;
    inv.pools[12] = 20;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(false));
    CHECK(rig.get(row) != nullptr && rig.get(row)->hidden);
    CHECK(inv.slots[kPistolCombo].clip == 7 && inv.pools[12] == 14);
    CHECK(rig.world.out.powerup_weapon_grants.empty());
}

// A held, short weapon is refilled in place: the loaded rounds zeroed, the pool
// SET to startrounds and one clip drawn, and the held weapon's FSM magazine
// follows when it is the equipped slot.
void test_weapon_grant_refills_held() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(10);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kPistolCombo].adm_index = 5;
    inv.slots[kPistolCombo].clip = 2;
    inv.pools[12] = 3;
    inv.equipped_combo = kPistolCombo;
    rig.local->weapon.active = true;
    rig.local->weapon.slot.clip = 2;
    rig.local->weapon.slot.phase = weapon_phase::kReloadPendingBit;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(inv.slots[kPistolCombo].clip == 7);
    CHECK(inv.pools[12] == 14);
    CHECK(rig.local->weapon.slot.clip == 7);
    CHECK((rig.local->weapon.slot.phase & weapon_phase::kReloadPendingBit) == 0);
}

// `sameas`: a variant whose named weapon is held refills that weapon's slot
// (with that weapon's own startrounds) and lands nothing of its own.
// [orig: WeaponSlot_InitFromAvatarDef @0x542779..0x5427C8]
void test_weapon_grant_sameas() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(11);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kPistolCombo].adm_index = 5;
    inv.slots[kPistolCombo].clip = 3;
    inv.pools[12] = 0;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(inv.slots[kPistolBCombo].adm_index == -1);
    CHECK(inv.slots[kPistolCombo].adm_index == 5);
    CHECK(inv.slots[kPistolCombo].clip == 7 && inv.pools[12] == 14);
    // Without the pistol the variant lands in its own slot.
    Rig fresh;
    const EntityHandle row2 = fresh.spawn_powerup(11);
    powerup_bind_entities(fresh.world, fresh.items);
    powerup_pickup(fresh.world, row2, fresh.picker, fresh.ctx(true));
    CHECK(fresh.local->inventory.slots[kPistolBCombo].adm_index == 6);
    CHECK(fresh.local->inventory.slots[kPistolBCombo].clip == 7);
    CHECK(fresh.local->inventory.pools[12] == 7);
}

// An empty slot takes the def's loadout_subclasses entries first (each
// initialized, its loaded rounds zeroed, its class pool set to ITS
// startrounds; their reload runs on the still-empty main slot), then the def,
// whose refill sets the shared pool to the def's startrounds and draws a clip.
// [orig: WeaponSlot_InitFromAvatarDef @0x542800..0x542909]
void test_weapon_grant_subvariants() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(12);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(inv.slots[kLauncherAltCombo].adm_index == 9);
    CHECK(inv.slots[kLauncherAltCombo].clip == 0);
    CHECK(inv.slots[kLauncherCombo].adm_index == 8);
    CHECK(inv.slots[kLauncherCombo].clip == 1);
    CHECK(inv.pools[13] == 2);
}

// The slot grant itself: the 0xFF byte, the "null" row 0 (category 0 rank 0,
// no `sameas`) and an unused index name no slot; the full predicates.
void test_avatar_grant_names_no_slot() {
    const WeaponTable table = weapons();
    WeaponInventory inv;
    inv.reset(table);
    CHECK(weapon_inventory_init_from_avatar_def(table, inv, 0xFF, 8, false).combo == -1);
    CHECK(weapon_inventory_init_from_avatar_def(table, inv, 0, 8, false).combo == -1);
    CHECK(weapon_inventory_init_from_avatar_def(table, inv, 7, 8, false).combo == -1);
    const WeaponAvatarGrant landed = weapon_inventory_init_from_avatar_def(table, inv, 5, 8, false);
    CHECK(landed.combo == kPistolCombo && landed.reloaded);
    CHECK(!weapon_inventory_weapon_full(table, inv, 9));
    CHECK(weapon_inventory_weapon_full(table, inv, 5)); // 14 + 7 = 21
    CHECK(weapon_inventory_weapon_full(table, inv, 6)); // the variant reads the pistol slot
    CHECK(weapon_inventory_all_full(table, inv, true));
    CHECK(!weapon_inventory_all_full(table, inv, false)); // no item def scores 0
}

// A remote picker on the host: the refusal reads its connection's tables
// through the session seam; the grant lands on that connection's slot rows and
// pools, and S2C 0x35 [u16 picker][u16 powerup] goes to every in-match slot but
// the listen host's own -- for the host's own pickup too.
// [orig: Server_BroadcastWeaponOverlayUpdate @0x509FC0]
void test_server_weapon_grant_fan() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    const EntityHandle row = rig.spawn_powerup(10);
    const EntityHandle row2 = rig.spawn_powerup(10);
    powerup_bind_entities(rig.world, rig.items);

    im::NapiNPServerCtx server;
    opennova::replication::LoopbackChannel remote_wire;
    opennova::replication::LoopbackChannel host_wire;
    server.np_protocol.connection_list.emplace_back();
    server.np_protocol.connection_list.emplace_back();
    im::NapiNPConnection &conn = server.np_protocol.connection_list[0];
    conn.phase = im::ConnectionPhase::PlayerAdded;
    conn.burst.spawned = true;
    conn.link.mode = opennova::replication::TransportMode::Client;
    conn.link.transport = &remote_wire;
    conn.link.owned_entity = remote;
    im::NapiNPConnection &host = server.np_protocol.connection_list[1];
    host.phase = im::ConnectionPhase::PlayerAdded;
    host.burst.spawned = true;
    host.link.mode = opennova::replication::TransportMode::Loopback;
    host.link.transport = &host_wire;
    host.link.owned_entity = rig.picker;

    im::ServerRemoteWeaponTables tables(server.np_protocol.connection_list);
    rig.world.remote_weapon_tables = &tables;
    // Full on its connection: refused.
    conn.weapon_slots[static_cast<uint16_t>(kPistolCombo)].adm_index = 5;
    conn.weapon_slots[static_cast<uint16_t>(kPistolCombo)].clip = 7;
    conn.reply.ammo_pools[12] = 14;
    powerup_pickup(rig.world, row, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_weapon_grants.empty());
    CHECK(rig.get(row) != nullptr && !rig.get(row)->hidden);
    // Short: granted, routed onto the connection.
    conn.weapon_slots.clear();
    conn.reply.ammo_pools[12] = 0;
    powerup_pickup(rig.world, row, remote, rig.ctx(true));
    powerup_pickup(rig.world, row2, rig.picker, rig.ctx(true));
    rig.world.remote_weapon_tables = nullptr;
    CHECK(rig.world.out.powerup_weapon_grants.size() == 2);
    im::Server_BroadcastWeaponOverlayUpdates(server, rig.world);
    CHECK(rig.world.out.powerup_weapon_grants.empty());
    const auto landed = conn.weapon_slots.find(static_cast<uint16_t>(kPistolCombo));
    CHECK(landed != conn.weapon_slots.end());
    if (landed != conn.weapon_slots.end())
        CHECK(landed->second.adm_index == 5 && landed->second.clip == 7);
    CHECK(conn.reply.ammo_pools[12] == 14);
    // The listen host's own pickup wrote its live inventory, never a connection.
    CHECK(host.weapon_slots.empty());
    CHECK(rig.local->inventory.slots[kPistolCombo].adm_index == 5);
    std::vector<opennova::WeaponPickupNotice> sent;
    opennova::replication::Datagram datagram;
    while (remote_wire.client_recv(datagram)) {
        CHECK(datagram.tag == opennova::s2c::WEAPON_PICKUP && datagram.reliable);
        CHECK(datagram.body.size() == 4);
        opennova::WeaponPickupNotice n;
        size_t consumed = 0;
        if (opennova::decode_weapon_pickup(datagram.body.data(), datagram.body.size(), n, consumed))
            sent.push_back(n);
    }
    CHECK(sent.size() == 2);
    if (sent.size() == 2) {
        CHECK(sent[0].picker_handle == remote.packed && sent[0].powerup_handle == row.packed);
        CHECK(sent[1].picker_handle == rig.picker.packed &&
              sent[1].powerup_handle == row2.packed);
    }
    CHECK(!host_wire.client_recv(datagram));
}

// A client's S2C 0x35 for its own player: the weapon its copy of the row names
// lands and is mounted -- the slot becomes the pending one and the held
// weapon queues its switch-out. A dead player or a missing row refuse; with
// no equipped weapon the weapon lands and nothing mounts, as retail's gates do.
// [orig: sub_4E03D0 @0x4E03D0; Player_MountWeaponSlot @0x4DFA40]
void test_weapon_pickup_received() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(10);
    Entity *r = rig.get(row);
    r->powerup_weapon = 5;
    LocalPlayer &lp = *rig.local;
    CHECK(!powerup_weapon_grant_received(rig.world, lp, r, /*local_dead=*/true));
    CHECK(!powerup_weapon_grant_received(rig.world, lp, nullptr, false));
    CHECK(lp.inventory.slots[kPistolCombo].adm_index == -1);
    CHECK(powerup_weapon_grant_received(rig.world, lp, r, false));
    CHECK(lp.inventory.slots[kPistolCombo].adm_index == 5);
    CHECK(lp.inventory.pending_combo == -1);
    lp.inventory.slots[kRifleCombo].adm_index = 4;
    lp.inventory.equipped_combo = kRifleCombo;
    CHECK(powerup_weapon_grant_received(rig.world, lp, r, false));
    CHECK(lp.inventory.pending_combo == kPistolCombo);
    // A row naming the "null" byte lands nothing.
    r->powerup_weapon = 0;
    CHECK(!powerup_weapon_grant_received(rig.world, lp, r, false));
}

// The contact drain runs the pickups in resolver order and nothing without a
// collision world.
void test_contact_drain_without_collision_world() {
    Rig rig;
    rig.world.collision = nullptr;
    powerup_process_contacts(rig.world, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 60);
}

} // namespace

int main() {
    test_table_build();
    test_bind();
    test_bind_without_table();
    test_med_pack_one_shot();
    test_med_pack_refuses_when_full();
    test_remote_body_pickup_on_a_client();
    test_infinite_med_pack_respawns();
    test_counted_respawns();
    test_hp_add();
    test_class_grants_local();
    test_remote_player_grant();
    test_allammo_local();
    test_allammo_refuses_full_kit();
    test_allammo_refills_populated_slot();
    test_full_health_keeps_staged_grant();
    test_server_applies_remote_grants();
    test_placeholder_row_runs_nothing();
    test_weapon_all_skips_the_arm();
    test_weapon_grant_lands_local();
    test_weapon_grant_refused_when_full();
    test_action_particles();
    test_weapon_grant_refills_held();
    test_weapon_grant_sameas();
    test_weapon_grant_subvariants();
    test_avatar_grant_names_no_slot();
    test_server_weapon_grant_fan();
    test_weapon_pickup_received();
    test_contact_drain_without_collision_world();
    if (failures == 0) std::printf("powerup: OK\n");
    return failures == 0 ? 0 : 1;
}
