// FX-1: a blast's kill zone queues on every peer that flies the round, and the
// drain's pool-0 walk pushes a person on every peer while the damage stays the
// authority's. Retail's producers carry no authority test (the fuse head, the
// terrain / item / water impacts, the instant kill zone, the grenade's
// submerged push), only the person impact does; the walk applies the collision
// force ahead of the authority's damage read, so a joiner's own grenade throws
// the joiner's own body before the host's death reaches it.
// [orig: Projectile_UpdatePhysics @0x4E9DC6..0x4E9E03; Projectile_HandleTerrainImpact
//  @0x4E928C..0x4E92C0; Projectile_HandleEntityImpact @0x4E9866..0x4E98A2;
//  Projectile_SpawnImpactEffect @0x4E9C40..0x4E9C81; RoundData_SpawnRound
//  @0x4EC1F3..0x4EC20E; Entity_UpdateGrenadePhysics @0x444A0F;
//  Projectile_HandleTerrainImpact_0 @0x4E9AF0..0x4E9B00 (the one authority test);
//  WeaponEffect_PushExplosionQueueEntry @0x4E83C0; Projectile_ProcessExplosionQueue
//  @0x4EAD80 - the force @0x4EB1D2 ahead of AmmoDef_GetKzDamageIfAuthority @0x4EB2A0;
//  Entity_ApplyCollisionForce @0x4AF4A0 - the walk push @0x4AF56B..0x4AF602;
//  GameEvent_HandleMedicInteraction @0x4E6790 - the authority tests @0x4E67D4 /
//  @0x4E67FE]
// Core-only: an authored ammo row (the JO frag's shape: C4 kill zone, kz_physics 1).
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/destruction.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int kItemFrag = 1883;
enum : int { kAmmoNull = 0, kAmmoFrag = 1, kAmmoKz = 2, kAmmoMedic = 3, kAmmoCount = 4 };

// A flat height field at word 0.
struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    FlatField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// The retail walk push: floor((9216 * table) / 2^22) per axis, +5120 vertical on
// a live body [orig: Entity_ApplyCollisionForce @0x4AF5E0 / @0x4AF5F3 / @0x4AF602].
int32_t push(int32_t table_q22) {
    return static_cast<int32_t>((int64_t{9216} * table_q22) >> 22);
}

struct Peer {
    std::unique_ptr<World> heap = std::make_unique<World>();
    bool heap_jo = test_titles::jo_world(*heap);
    World &w = *heap;
    FlatField flat;
    EntityHandle body;

    // authority = the session host; otherwise a session joiner whose rounds fly
    // VisualOnly (its own and the host's).
    explicit Peer(bool authority) {
        w.rules.mp_session = true;
        w.rules.logic_authority = authority;
        w.rules.projectile_authority = authority;
        w.ai.is_authority = authority;
        w.env.water_z = -(1000 << 16);
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        w.tables.ammo.entries.resize(kAmmoCount);
        w.tables.ammo.entries[kAmmoNull].name = "AT_NULL";
        w.tables.ammo.entries[kAmmoNull].valid = true;
        AmmoTableEntry &frag = w.tables.ammo.entries[kAmmoFrag];
        frag.name = "grenadehe";
        frag.valid = true;
        frag.flags = kAmmoFlagUseOwnMove | kAmmoFlagForceTracer;
        frag.velocity = 30;
        frag.max_age_ticks = 248;
        frag.drag_fp16 = 0x10000;
        frag.kztype = ammo_kz::kC4;
        frag.kz_damage = 205;
        frag.kz_minradius = 6.0f;
        frag.kz_maxradius = 12.0f;
        frag.kz_physics = 1;
        frag.tracer_item_friendly = kItemFrag;
        frag.tracer_item_enemy = kItemFrag;
        AmmoTableEntry &kz = w.tables.ammo.entries[kAmmoKz];
        kz = frag;
        kz.name = "fragkillzone";
        kz.flags = kAmmoFlagInstantKillZone;
        AmmoTableEntry &medic = w.tables.ammo.entries[kAmmoMedic];
        medic.name = "medpack";
        medic.valid = true;
        medic.flags = kAmmoFlagInstantKillZone;
        medic.kztype = ammo_kz::kMedic;
        medic.kz_maxradius = 3.0f;
        w.throwables.classes.set({kItemFrag, ThrowClass::kNade, ThrowClass::kNade, 5, 0, 0, 1});
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.has_item_def = true;
        seed.item_type = 3;
        seed.item_type_index = 1;
        seed.health = 100;
        seed.health_max = 150;
        seed.alive = true;
        seed.team = 1;
        seed.flags |= kEntityFlagPlayer;
        seed.bound_radius.q16 = 0x8000;
        seed.position.set_q16({51 << 16, 50 << 16, 0});
        body = w.registry.spawn(0, seed);
        AiEntity *ae = w.ai.at(w.ai.attach(body));
        ae->inf.active = true;
        ae->health = 100;
    }
    AiEntity &ai() { return *w.ai.for_handle(body); }
    int throw_frag(RoundConsequenceMode mode) {
        RoundSpawnParams p;
        p.owner = body;
        p.shooter_handle = body.packed;
        p.origin = Vec3{50.0f, 50.0f, 1.0f};
        p.dir_pitch_bam = -0x40000000;
        p.ammo_index = kAmmoFrag;
        return w.round_sim.spawn(w, p, mode);
    }
};

// A joiner's own grenade: the fuse head queues the blast on the peer, and the
// walk pushes the joiner's own body with no damage.
void test_peer_fuse_queues_and_pushes() {
    Peer peer(false);
    const int slot = peer.throw_frag(RoundConsequenceMode::VisualOnly);
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &round = peer.w.round_sim.rounds[static_cast<size_t>(slot)];
    // The fused round at the thrower's side, one unit off along -x.
    round.pos.set_q16({50 << 16, 50 << 16, 0});
    round.det_at_expiry = true;
    peer.w.round_sim.tick(peer.w, &peer.flat.field, nullptr);
    CHECK(peer.w.explosions.queue.size() == 1);
    peer.w.explosions.process(peer.w, nullptr, &peer.flat.field, peer.w.out.destruction);
    // The source lies along -x: the bearing from the source is 0, the table's
    // first row (sin 0, cos the 256th row).
    int32_t cos22 = 0, sin22 = 0;
    quantized_dir(0, cos22, sin22);
    CHECK(peer.ai().inf.vel[0] == push(cos22));
    CHECK(peer.ai().inf.vel[1] == push(sin22));
    CHECK(peer.ai().inf.vel[2] == 5120);
    CHECK(peer.w.registry.get(peer.body)->health == 100); // the peer's damage read is 0
}

// The authority's own grenade queues and damages as before.
void test_authority_fuse_damages() {
    Peer host(true);
    const int slot = host.throw_frag(RoundConsequenceMode::Authoritative);
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &round = host.w.round_sim.rounds[static_cast<size_t>(slot)];
    round.pos.set_q16({50 << 16, 50 << 16, 0});
    round.det_at_expiry = true;
    host.w.round_sim.tick(host.w, &host.flat.field, nullptr);
    CHECK(host.w.explosions.queue.size() == 1);
    host.w.explosions.process(host.w, nullptr, &host.flat.field, host.w.out.destruction);
    CHECK(host.ai().inf.vel[2] == 5120);
    CHECK(host.w.registry.get(host.body)->health <= 0); // 205 inside kz_minradius
}

// A peer's instant kill zone queues too; a peer's medic kit heals nothing.
void test_peer_instant_kill_zones() {
    Peer peer(false);
    RoundSpawnParams p;
    p.owner = peer.body;
    p.shooter_handle = peer.body.packed;
    p.origin = Vec3{50.0f, 50.0f, 0.0f};
    p.ammo_index = kAmmoKz;
    peer.w.round_sim.spawn(peer.w, p, RoundConsequenceMode::VisualOnly);
    CHECK(peer.w.explosions.queue.size() == 1);
    peer.w.explosions.queue.clear();

    Entity mate = *peer.w.registry.get(peer.body);
    mate.position.set_q16({50 << 16, 51 << 16, 0});
    mate.health = 40;
    const EntityHandle mate_h = peer.w.registry.spawn(0, mate);
    (void)mate_h;
    // The healer's class carries the Medic attribute [orig: @0x4EADEA..0x4EADF4].
    peer.w.registry.get(peer.body)->player_class = 5;
    peer.w.tables.class_attribute_flags[(5u - 1u) & 0xFu] = MissionTables::kCharAttrMedic;
    p.ammo_index = kAmmoMedic;
    peer.w.round_sim.spawn(peer.w, p, RoundConsequenceMode::VisualOnly);
    CHECK(peer.w.explosions.queue.size() == 1);
    peer.w.explosions.process(peer.w, nullptr, &peer.flat.field, peer.w.out.destruction);
    CHECK(peer.w.round_sim.medic_interactions.empty());
}

} // namespace

int main() {
    test_peer_fuse_queues_and_pushes();
    test_authority_fuse_damages();
    test_peer_instant_kill_zones();
    if (failures == 0) std::printf("explosion_push: ok\n");
    return failures == 0 ? 0 : 1;
}
