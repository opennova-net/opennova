// The reported training regressions through the actual mounted assets and host
// tick: US01 locomotion/footsteps, neutral tags, and the Little Bird pilot gun.
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"
#include <runtime/world/friendly_tags.h>
#include <runtime/audio/sound_profile.h>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The shell consumes switch requests through MissionKernel::install_weapon.
// Exercise that public boundary here as well; advancing the host alone leaves
// presentation-requested weapon definitions uninstalled in a headless rig.
static void advance(testrig::RetailMissionRig &rig, int count = 1) {
    for (int tick = 0; tick < count; ++tick) {
        // FireEvent is an embedder-drained presentation queue, not a per-tick
        // snapshot. Consume the previous frame before observing new shots.
        rig.world.round_sim.fired.clear();
        rig.tick();
        std::vector<WeaponPresentationEvent> events;
        events.swap(rig.local.weapon.events);
        for (const auto &event : events) {
            if (!event.switch_to_weapon.empty())
                CHECK(rig.install_weapon(event.switch_to_weapon, event.preserve_slot_state));
            else if (event.clear_weapon)
                local_weapon_clear(rig.local.weapon, rig.local.view);
        }
    }
}

static std::unique_ptr<testrig::RetailMissionRig> boot(const std::string &root,
        const std::string &expansion, const char *mission) {
    auto rig = std::make_unique<testrig::RetailMissionRig>();
    std::string error;
    if (!rig->open(root, mission, error, expansion) ||
            !rig->boot(testrig::BootOptions{}, error)) {
        std::printf("FAIL %s/%s: %s\n", expansion.c_str(), mission, error.c_str());
        ++failures;
        return {};
    }
    advance(*rig, 62);
    return rig;
}

static void truck_glass(testrig::RetailMissionRig &rig) {
    Entity *truck = rig.world.registry.by_net_id(11);
    CHECK(truck != nullptr);
    if (!truck) return;
    const int32_t anchor[3] = {int32_t(truck->position.x * 65536),
            int32_t(truck->position.y * 65536), int32_t(truck->position.z * 65536)};
    const auto boxes = rig.collision.debug_hitboxes(rig.world, anchor, 12 * 65536, 64, 30000);
    const auto *weapon = rig.world.tables.weapons.by_index(rig.local.player()->equipped_adm_index);
    CHECK(weapon != nullptr && weapon->ammo_index >= 0);
    if (!weapon || weapon->ammo_index < 0) return;
    // Use the posed CFAC triangles themselves, so the shot follows the moving
    // truck and does not accidentally exercise the metal frame beside a pane.
    for (const auto &box : boxes) {
        if (box.handle != truck->handle) continue;
        for (const auto &face : box.faces) {
            if (face.material != 15) continue;
            double center[3], a[3], b[3], normal[3];
            for (int axis = 0; axis < 3; ++axis) {
                center[axis] = (double(face.v[0][axis]) + face.v[1][axis] + face.v[2][axis]) / (3 * 65536.0);
                a[axis] = (double(face.v[1][axis]) - face.v[0][axis]) / 65536.0;
                b[axis] = (double(face.v[2][axis]) - face.v[0][axis]) / 65536.0;
            }
            normal[0] = a[1] * b[2] - a[2] * b[1];
            normal[1] = a[2] * b[0] - a[0] * b[2];
            normal[2] = a[0] * b[1] - a[1] * b[0];
            const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length < 0.00001) continue;
            for (double &component : normal) component /= length;
            for (const int sign : {1, -1}) {
                ProjectileTrace trace;
                trace.owner = rig.world.cached.local_player;
                trace.start = {int32_t((center[0] + sign * normal[0] * 0.25) * 65536),
                        int32_t((center[1] + sign * normal[1] * 0.25) * 65536),
                        int32_t((center[2] + sign * normal[2] * 0.25) * 65536)};
                trace.end = {int32_t((center[0] - sign * normal[0] * 0.25) * 65536),
                        int32_t((center[1] - sign * normal[1] * 0.25) * 65536),
                        int32_t((center[2] - sign * normal[2] * 0.25) * 65536)};
                const auto hit = rig.collision.trace_projectile(rig.world, trace);
                if (hit.geometry_entity != truck->handle || hit.surface_type != 15) continue;
                RoundSpawnParams shot;
                shot.owner = trace.owner;
                shot.shooter_handle = trace.owner.packed;
                shot.ammo_index = weapon->ammo_index;
                shot.origin = {trace.start.x / 65536.0f, trace.start.y / 65536.0f, trace.start.z / 65536.0f};
                shot.dir_yaw_bam = io::bam_from_radians(std::atan2(-sign * normal[1], -sign * normal[0]));
                shot.dir_pitch_bam = io::bam_from_radians(std::atan2(-sign * normal[2], std::hypot(normal[0], normal[1])));
                auto &rounds = rig.world.round_sim;
                rounds.reset();
                rounds.weapon_spread_enabled = false;
                const int index = rounds.spawn(rig.world, shot);
                CHECK(index >= 0);
                if (index < 0) return;
                rounds.tick(rig.world, nullptr, &rig.collision);
                CHECK(rounds.debug_trail_count == 1);
                CHECK(rounds.debug_trail[0].material == 15);
                CHECK(rounds.debug_trail[0].entity == truck->handle.packed);
                CHECK(rounds.rounds[index].active);
                std::printf("truck glass: material=%d round_active=%d\n",
                        rounds.debug_trail[0].material, rounds.rounds[index].active);
                return;
            }
        }
    }
    CHECK(false); // the authored truck must supply a ray-visible glass pane
}

static void locomotion(const std::string &root, const std::string &expansion) {
    auto rig = boot(root, expansion, "00TRa.bms");
    if (!rig) return;
    const Vec3 start = rig->local.player_position();
    rig->local.input.forward = true;
    int footstep_ticks = 0;
    int footstep_sounds = 0;
    for (int tick = 0; tick < 62; ++tick) {
        advance(*rig);
        if ((rig->local.player_ai()->inf.last_events & 3u) != 0) {
            ++footstep_ticks;
            for (const auto &sound : rig->world.out.slot_sounds)
                if (sound.source_handle == rig->world.cached.local_player.packed &&
                        sound.slot >= audio::kSlotFootLGround && sound.slot <= audio::kSlotFootWater)
                    ++footstep_sounds;
        }
    }
    const float distance = testrig::planar_distance(start, rig->local.player_position());
    std::printf("%s locomotion: distance=%.3f footstep_ticks=%d\n",
            expansion.c_str(), distance, footstep_ticks);
    CHECK(distance > 5.0f);
    CHECK(footstep_ticks > 0);
    CHECK(footstep_sounds > 0);
    truck_glass(*rig);
}

static void tags_and_pilot(const std::string &root, const std::string &expansion) {
    auto rig = boot(root, expansion, "03TR.bms");
    if (!rig) return;
    World &world = rig->world;
    const EntityHandle player_handle = world.cached.local_player;
    Entity *player = world.registry.get(player_handle);
    FriendlyTagPassContext ctx;
    ctx.game_type = 0x10020u; // the SP/training game type
    std::vector<FriendlyTagSource> tags;
    collect_friendly_tags(world, *player, tags, ctx);
    CHECK(!tags.empty());
    for (const auto &tag : tags)
        CHECK(world.registry.get(tag.entity)->team == player->team);
    // 03TR must author a labelable neutral organic for this leg to pin the
    // drawer's unequal-team rejection.
    CHECK(player->team != 0);
    int neutral_organics = 0;
    world.registry.for_each([&](const Entity &e) {
        if (e.kind == EntityKind::Organic && (e.flags & kEntityFlagPlayer) == 0 &&
                e.has_item_def && e.team == 0 && (e.flags & kEntityFlagCarried) == 0)
            ++neutral_organics;
    });
    std::printf("%s 03TR tags: labels=%zu neutral_organics=%d\n",
            expansion.c_str(), tags.size(), neutral_organics);
    CHECK(neutral_organics > 0);
    // A round passing a person parks past its entity+0 boundRadius, so every
    // mission person must carry one. [orig: Projectile_UpdatePhysics @0x4EA7BE]
    int unsized_people = 0;
    world.registry.for_each([&](const Entity &e) {
        if (e.kind == EntityKind::Organic && e.has_item_def && e.bound_radius <= 0.0f)
            ++unsized_people;
    });
    CHECK(unsized_people == 0);

    // 03TR SSN 41 is the authored minigun Little Bird, with a free ctrlx seat.
    Entity *heli = world.registry.by_net_id(41);
    CHECK(heli != nullptr);
    if (!heli) return;
    CHECK((heli->item_attrib & kItemAttribEweap) != 0);
    const Seat *control_seat = nullptr;
    for (const Seat &seat : heli->seats)
        if (seat.type == SeatType::Controller) control_seat = &seat;
    CHECK(control_seat != nullptr);
    if (control_seat == nullptr) return;
    const EntityHandle carrier = heli->handle;
    const uint8_t personal_adm = player->equipped_adm_index;
    const WeaponInventory inventory = rig->local.inventory;
    CHECK(world.vehicles.process_attach(player_handle, carrier, control_seat->bone_index));
    // The ctrlx borrow goes through Player_MountWeaponSlot like UseGun: the
    // outgoing slot's switch action commits the carrier gun, never the attach
    // itself (the Emplaced minigun shortens that action to one tick).
    // [orig: @0x494838 -> WeaponAction_SwitchFrom @0x543475]
    CHECK(!rig->local.weapon.usegun_slot_active);
    int commit_ticks = 0;
    while (!rig->local.weapon.usegun_slot_active && commit_ticks < 124) {
        advance(*rig);
        ++commit_ticks;
    }
    std::printf("%s Little Bird: carrier gun committed after %d ticks\n",
            expansion.c_str(), commit_ticks);
    CHECK(commit_ticks >= 1 && commit_ticks < 124);
    advance(*rig, 124 - commit_ticks);
    CHECK(player->mount_type == SeatType::Controller);
    CHECK(heli->primary_weapon_slot_adm != kAdmSlotNone);
    CHECK(player->equipped_adm_index == heli->primary_weapon_slot_adm);
    CHECK(heli->primary_weapon_owner == player_handle);
    CHECK(rig->local.weapon.active);
    CHECK(local_weapon_input_block(world, rig->local.weapon) == LocalWeaponInputBlock::kNone);
    const int32_t clip_before = heli->primary_weapon_slot.clip;
    int shots = 0;
    for (int tick = 0; tick < 62; ++tick) {
        rig->local.set_weapon_input(true, tick == 0, false);
        advance(*rig);
        for (const FireEvent &shot : world.round_sim.fired) {
            if (shot.shooter != player_handle) continue;
            ++shots;
            const auto *weapon = world.tables.weapons.by_index(heli->primary_weapon_slot_adm);
            CHECK(weapon != nullptr && shot.ammo_index == weapon->ammo_index);
            CHECK(testrig::distance(shot.origin, heli->position) < heli->bound_radius * 2.0f);
        }
    }
    std::printf("%s Little Bird: shots=%d carrier_adm=%d clip=%d->%d\n",
            expansion.c_str(), shots, heli->primary_weapon_slot_adm, clip_before,
            heli->primary_weapon_slot.clip);
    CHECK(shots > 0);
    // The pilot spends the carrier's persistent clip, not a personal one.
    CHECK(heli->primary_weapon_slot.clip < clip_before);
    rig->local.set_weapon_input(false, false, false);
    advance(*rig, 62); // allow the accepted fire action to finish
    int shots_after_release = 0;
    for (int tick = 0; tick < 62; ++tick) {
        advance(*rig);
        for (const FireEvent &shot : world.round_sim.fired)
            if (shot.shooter == player_handle) ++shots_after_release;
    }
    CHECK(shots_after_release == 0);
    CHECK(world.vehicles.detach(player_handle));
    // The detach restore is queued the same way, so the personal weapon plays
    // its draw before it can fire. [orig: Entity_DetachFromVehicle
    // @0x43562A..0x43565F -> Player_MountWeaponSlot]
    bool personal_draw = false;
    for (int tick = 0; tick < 124; ++tick) {
        advance(*rig);
        if (!rig->local.weapon.usegun_slot_active &&
                rig->local.weapon.slot.current == weapon_action::kSwitchTo)
            personal_draw = true;
    }
    CHECK(personal_draw);
    CHECK(player->equipped_adm_index == personal_adm);
    CHECK(!heli->primary_weapon_owner.valid());
    CHECK(rig->local.inventory.equipped_combo == inventory.equipped_combo);
    CHECK(rig->local.inventory.pools == inventory.pools);
    CHECK(rig->local.inventory.shared_clips == inventory.shared_clips);
    for (size_t i = 0; i < inventory.slots.size(); ++i) {
        CHECK(rig->local.inventory.slots[i].adm_index == inventory.slots[i].adm_index);
        CHECK(rig->local.inventory.slots[i].clip == inventory.slots[i].clip);
    }
}

int main() {
    const std::string root = retail::install();
    if (root.empty()) return retail::skip("OPENNOVA_JO_DIR for training gameplay");
    std::vector<std::string> mounts{std::string()};
    for (const auto &expansion : retail::expansions())
        if (retail::lower_ascii(expansion) == "revx02") mounts.push_back(expansion);
    for (const auto &mount : mounts) {
        locomotion(root, mount);
        tags_and_pilot(root, mount);
    }
    return failures == 0 ? 0 : 1;
}
