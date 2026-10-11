// Retail data stays outside the repository; boot the two shipped lndm missions.
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"
#include <base/io/fixed.h>
#include <runtime/world/entity_spawn.h>
#include <cstdio>
#include <memory>

using namespace opennova;

int main() {
    const std::string root = retail::install();
    if (root.empty()) return retail::skip("packed JO install is not configured");
    for (const auto &mission : {std::pair<const char *, int>{"00TRd.bms", 98},
                               {"CP09.bms", 74}}) {
        auto rig = std::make_unique<testrig::RetailMissionRig>();
        std::string error;
        if (!rig->open(root, mission.first, error)) {
            std::fprintf(stderr, "FAIL open %s: %s\n", mission.first, error.c_str());
            return 1;
        }
        testrig::BootOptions options;
        options.wac = false;
        if (!rig->boot(options, error)) {
            std::fprintf(stderr, "FAIL boot %s: %s\n", mission.first, error.c_str());
            return 1;
        }
        int count = 0, small = 0, large = 0;
        world::EntityHandle target;
        rig->world.registry.for_each([&](const world::Entity &e) {
            if (!e.minefield.think) return;
            ++count;
            if (!target.valid()) target = e.handle;
            for (uint8_t type : e.minefield.types) {
                if (type == 2) ++small;
                if (type == 4) ++large;
            }
        });
        if (count != mission.second || small != count * 9 || large != count * 5) {
            std::fprintf(stderr, "FAIL %s: %d fields, %d small, %d large\n",
                    mission.first, count, small, large);
            return 1;
        }
        auto *field = rig->world.registry.get(target);
        const auto seed = field->minefield;
        const auto random = rig->world.prng16_b_state;
        const int small_id = rig->world.tables.ammo.index_of("SMALLLANDMINE");
        const int large_id = rig->world.tables.ammo.index_of("LARGELANDMINE");
        if (small_id <= 0 || large_id <= 0 ||
                seed.ammo_small != static_cast<uint32_t>(small_id) ||
                seed.ammo_large != static_cast<uint32_t>(large_id)) return 1;
        rig->tick(160); // approach an armed field, after its initial stagger
        field = rig->world.registry.get(target);
        const auto point = rig->world.minefields.point(*field, 0);
        if (field->section_mask & 1u) return 1;
        const auto *approaching = rig->world.registry.get(rig->world.cached.local_player);
        if (!approaching || approaching->health <= 0) return 1;
        // CP09 is an objective Co-op mission: offline its player starts at the doubled ceiling
        // (D-PWR-2 [orig: Game_StartMission @0x525CC2..0x525D12]), which one small mine does not
        // empty. Pin that start, then bring the player to its def hp so the kill below is the
        // mine's alone.
        if (rig->world.rules.difficulty == -1) {
            if (approaching->health != world::max_health_with_difficulty(rig->world, *approaching) ||
                    approaching->health != 2 * approaching->health_max) {
                std::fprintf(stderr, "FAIL %s: the Co-op start is not the doubled ceiling (%d)\n",
                        mission.first, approaching->health);
                return 1;
            }
            rig->world.registry.get(rig->world.cached.local_player)->health = approaching->health_max;
        }
        rig->local.teleport_local_player({point.x * io::kInvFp16One,
                point.y * io::kInvFp16One, point.z * io::kInvFp16One + 1.0f}, 0, 0);
        const auto before = rig->world.out.rounds.last_stat();
        const auto mine_fired = [&] {
            for (const auto &round : rig->world.out.rounds.records)
                if (round.stat > before && round.shooter_handle == 0xFFFF &&
                        round.adm_index == small_id && round.origin_x == point.x &&
                        round.origin_y == point.y && round.origin_z == point.z) return true;
            return false;
        };
        for (int i = 0; i < 160 && !mine_fired(); ++i) rig->tick();
        field = rig->world.registry.get(target);
        if (!mine_fired() || !field || !(field->section_mask & 1u)) {
            std::fprintf(stderr, "FAIL %s: no unowned mine round or spent field\n", mission.first);
            return 1;
        }
        rig->tick(); // pool-2 contacts queue after this tick's explosion drain
        const auto *player = rig->world.registry.get(rig->world.cached.local_player);
        if (!player || player->health > 0) {
            std::fprintf(stderr, "FAIL %s: retail mine did not kill player (%d)\n",
                    mission.first, player ? player->health : -1);
            return 1;
        }
        if (!rig->restore_baseline()) return 1;
        field = rig->world.registry.get(target);
        if (!field || field->section_mask || !field->minefield.initialized ||
                rig->world.prng16_b_state != random ||
                std::memcmp(field->minefield.offsets, seed.offsets, sizeof(seed.offsets)) ||
                std::memcmp(field->minefield.rotations, seed.rotations, sizeof(seed.rotations))) {
            std::fprintf(stderr, "FAIL %s: baseline failed to restore field layout and RNG\n",
                    mission.first);
            return 1;
        }
        std::printf("PASS %s: %d fields (%d small/%d large), trigger, damage, reset\n",
                mission.first, count, small, large);
    }
    return 0;
}
