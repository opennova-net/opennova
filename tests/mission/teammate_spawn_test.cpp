#include "common/retail_mission_files.h"
#include "common/retail_paths.h"
#include <runtime/world/teammate_operations.h>
#include <runtime/world/vehicle_motor.h>
#include <formats/aip/aip.h>
#include <formats/def/def.h>
#include <runtime/mission/mission_kernel.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace opennova;
namespace w = opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
// The helicopter's class init copies a weapon block's ammo count into the
// brain only when the block resolved a nonzero ammo byte: the null first ammo
// row resolves to byte 0 and seeds nothing.
// [orig: Entity_InitHelicopterAIFromDef @0x468555..0x46858D (`cmp byte ptr
//  [eax+94h],0`, `cmp byte ptr [eax+0B4h],0`); AmmoDef_LookupByName @0x409870]
void test_helicopter_ammo_seed_needs_a_resolved_byte() {
    std::array<def::DefItemDef, 1> rows{};
    rows[0].id = mission::kItemIdOffset + 1281;
    rows[0].type = 1;
    rows[0].attrib = w::kItemAttribAIData;
    rows[0].hp = 100;
    std::strcpy(rows[0].ai_function, "CHel");
    def::DefItemsFile items{rows.data(), rows.size()};
    auto kernel = std::make_unique<mission::MissionKernel>();
    kernel->set_items_table(&items);
    kernel->world.registry.configure_pool(0, 8);
    kernel->world.registry.configure_pool(1, 8);
    kernel->world.tables.ammo.entries.resize(2);
    kernel->world.tables.ammo.entries[0].name = "AT_NULL";
    kernel->world.tables.ammo.entries[0].valid = true;
    kernel->world.tables.ammo.entries[1].name = "50CAL";
    kernel->world.tables.ammo.entries[1].valid = true;
    static const char kProfile[] =
            "type HELO\nprimary_weap 50CAL\nprimary_ammo 5\n"
            "secondary_weap AT_NULL\nsecondary_ammo 7\n";
    kernel->ai_profiles.push_back({"h_bhawkn",
            aip::parse_profile(reinterpret_cast<const uint8_t *>(kProfile),
                    sizeof(kProfile) - 1)});
    w::TeammateSpawn request;
    request.item_type = 1281;
    request.ssn = 11000;
    request.helicopter = true;
    const w::EntityHandle heli = kernel->spawn_teammate(request);
    CHECK(heli.valid());
    const w::AiEntity *ai = kernel->world.ai.for_handle(heli);
    CHECK(ai != nullptr);
    if (ai == nullptr) return;
    CHECK(ai->brain.f[w::AiBrain::kAmmoA] == 5);
    CHECK(ai->brain.f[w::AiBrain::kAmmoB] == 0);
}
// The flyover helicopter takes its spawn transform verbatim: heading
// 0x7FFFFF80, zero pitch and roll. Its first AI flight pass must read that
// heading, not the degree mirror's 270 rounded back to 0x80000000.
// [orig: HeliLift_SpawnFlyover @0x4527E7 (the heading); Entity_SpawnHelicopter
//  @0x452209..0x45224C (x/y/z/yaw from spawnPos), @0x452253/@0x45225A (pitch/roll 0)]
void test_flyover_helicopter_keeps_its_spawn_heading() {
    std::array<def::DefItemDef, 1> rows{};
    rows[0].id = mission::kItemIdOffset + 1281;
    rows[0].type = 1;
    rows[0].attrib = w::kItemAttribAIData;
    rows[0].hp = 100;
    std::strcpy(rows[0].ai_function, "CHel");
    def::DefItemsFile items{rows.data(), rows.size()};
    auto kernel = std::make_unique<mission::MissionKernel>();
    kernel->set_items_table(&items);
    kernel->world.registry.configure_pool(0, 8);
    kernel->world.registry.configure_pool(1, 8);
    static const char kProfile[] = "type HELO\n";
    kernel->ai_profiles.push_back({"h_bhawkn",
            aip::parse_profile(reinterpret_cast<const uint8_t *>(kProfile),
                    sizeof(kProfile) - 1)});
    w::TeammateSpawn request;
    request.item_type = 1281;
    request.ssn = 11000;
    request.heading = 2147483520; // 0x7FFFFF80
    request.helicopter = true;
    const w::EntityHandle heli = kernel->spawn_teammate(request);
    w::Entity *hull = kernel->world.registry.get(heli);
    CHECK(hull != nullptr);
    if (hull == nullptr) return;
    int32_t pos[3], yaw = 0, pitch = 1, roll = 1;
    w::carrier_pose_fixed(*hull, pos, yaw, pitch, roll);
    CHECK(yaw == 2147483520 && pitch == 0 && roll == 0);
    kernel->world.ai.chel_ai_drive(kernel->world, *hull, nullptr, w::VehicleTraits{});
    CHECK(hull->veh.yaw_bam == 2147483520);
    CHECK(hull->veh.air_pitch_bam == 0 && hull->veh.air_roll_bam == 0);
}
}
int main() {
    test_helicopter_ammo_seed_needs_a_resolved_byte();
    test_flyover_helicopter_keeps_its_spawn_heading();
    if (failures) {
        std::printf("retail teammate factory: FAIL\n");
        return 1;
    }
    RETAIL_REQUIRE_OR_SKIP(install, retail::install(), "OPENNOVA_JO_DIR (CP01 and the teammate DEF/AIP assets)");
    auto owned = std::make_unique<testrig::RetailMissionRig>();
    auto &rig = *owned;
    std::string error;
    if (!rig.open(install, "CP01.bms", error)) return retail::skip(error.c_str());
    if (rig.mission.organics.empty()) return 1;
    bms::Entity patient = rig.mission.organics.front();
    patient.id = 22000; patient.team = 1; patient.waypoint_id = 0;
    patient.type_id = 4520; patient.group_id = 0;
    rig.mission.organics.push_back(patient);
    bms::Entity point{}; point.type = bms::ItemType::Marker;
    point.type_id = 6088; point.id = 22001; point.wp_number = 981;
    point.x = patient.x; point.y = patient.y; point.z = patient.z; point.yaw = 90;
    rig.mission.markers.push_back(point);
    bms::Event event{}; event.flags = bms::EventFlags::PreMission;
    event.action_index = 0; event.action_count = 1;
    bms::Action action{}; action.action_type = bms::ActionType::Teammates;
    action.action_sub_type = 2; action.param1 = patient.id; action.param2 = point.wp_number;
    rig.mission.events = {event}; rig.mission.actions = {action}; rig.mission.triggers.clear();
    testrig::BootOptions options; options.listen_server = false; options.wac = false;
    if (!rig.boot(options, error)) { std::printf("%s\n", error.c_str()); return 1; }
    const bool medic_assets = rig.index.has_file("DeltaMED.3di")
            && rig.index.has_file("Medic01.3di") && rig.index.has_file("Medic01.adm")
            && rig.index.has_file("Medic02.adm");
    if (!medic_assets)
        std::printf("UNEXERCISED: retail medic ADM/3DI assets absent; authored scene coverage is separate\n");
    CHECK(rig.assets().model("Fblkhawm").get() != nullptr);
    CHECK(rig.world.teammates.count() == 1); // pre-mission dispatch sees initialized marker DEFs
    if (rig.world.teammates.count() != 1) return 1;
    rig.capture_baseline();
    const size_t baseline_count = rig.world.registry.live_count();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto slot = *rig.world.teammates.at(0);
        const auto *first = rig.world.ai.for_handle(slot.first.handle);
        const auto *second = rig.world.ai.for_handle(slot.second.handle);
        const auto *heli = rig.world.ai.for_handle(slot.helicopter.handle);
        CHECK(first && second && heli);
        if (!first || !second || !heli) return 1;
        CHECK(first->inf.active && second->inf.active && !heli->inf.active);
        CHECK(!first->has_occupant && !second->has_occupant && !heli->has_occupant);
        CHECK(heli->brain.f[w::AiBrain::kCurState] == 0);
        if (medic_assets) {
            CHECK(first->inf.adm_id >= 0 && second->inf.adm_id >= 0);
            CHECK(first->inf.adm_id != second->inf.adm_id);
            CHECK(rig.root_motion.has_clip(first->inf.adm_id, 137));
            CHECK(rig.root_motion.has_clip(first->inf.adm_id, 138));
            CHECK(rig.root_motion.has_clip(second->inf.adm_id, 139));
            int32_t head[3], hand[3];
            CHECK(rig.resolve_skeletal_anchor(rig.world, slot.first.handle, w::SkeletalAnchor::HeldWeapon, hand));
            CHECK(rig.resolve_skeletal_anchor(rig.world, slot.second.handle, w::SkeletalAnchor::Head, head));
        }
        CHECK(heli->profile.type == 1);
        CHECK(rig.world.registry.get(slot.first)->item_id == 4529);
        CHECK(rig.world.registry.get(slot.second)->item_id == 4520);
        CHECK(rig.world.registry.get(slot.helicopter)->item_id == 1281);
        CHECK(rig.world.registry.get(slot.first)->npc_respawns == 10);
        CHECK(rig.world.registry.get(slot.first)->spawn_phase == 63);
        CHECK(rig.world.registry.get(slot.helicopter)->spawn_origin == w::kSpawnOriginNone);
        CHECK(rig.world.vehicles.traits.get(1281) != nullptr);
        const int32_t initial_x = heli->pos[0];
        rig.tick(125);
        heli = rig.world.ai.for_handle(slot.helicopter.handle);
        // The authored helicopter has no PlayerControl: its initial queued
        // FOLLOWWP (7) survives the no-pilot mover. Baseline restoration drops
        // transient AI events, so that pass reaches the class idle state (14).
        // [orig: Entity_UpdateAircraftPhysics @0x490310, gate @0x490ef6]
        const auto *traits = rig.world.vehicles.traits.get(1281);
        CHECK(traits && !traits->player_control);
        const int expected_state = attempt == 0 ? 7 : 14;
        CHECK(heli && heli->brain.f[w::AiBrain::kCurState] == expected_state);
        // The two medics stand on the helicopter (their ground link), so the
        // entity update's pool-0 walk wakes its contact solve every fourth
        // tick. [orig: HeliLift_SpawnFlyover @0x452980/@0x4529BD (the +0x28
        //  stores); Entity_UpdateAllEntities -- the Entity_FindChildByDefType
        //  call @0x4C2484, `test tick,3` @0x4C25CE -> Entity_WakeContactSolve @0x459290]
        const w::Entity *hull = rig.world.registry.get(slot.helicopter);
        CHECK(hull && ((hull->flags | hull->engine_flags) & 0x40u) != 0);
        CHECK(hull && hull->veh.contact_wake_tick == ((rig.world.logic_tick - 1u) & ~3u));
        CHECK(rig.world.teammates.at(0)->state == w::TeammateOperations::State::FlyToHover);
        w::EntityHandle existing;
        for (int i = 0; i < rig.world.ai.count(); ++i) {
            auto *ai = rig.world.ai.at(i);
            if (ai && ai->inf.active && ai->profile.clip_size > 1) {
                existing = ai->handle; ai->inf.magazine = 1; break;
            }
        }
        CHECK(existing.valid());
        rig.events.dispatch_action_for_test(rig.world, action);
        CHECK(rig.world.teammates.count() == 2);
        if (existing.valid()) CHECK(rig.world.ai.for_handle(existing)->inf.magazine == 1);
        CHECK(rig.restore_baseline());
        CHECK(rig.world.teammates.count() == 1);
        CHECK(rig.world.registry.live_count() == baseline_count);
        CHECK(rig.world.script.heli_lift_active_count == 1);
        CHECK(rig.world.ai.for_handle(slot.helicopter.handle)->pos[0] == initial_x);
    }
    for (const auto &gap : rig.world.diagnostics.gaps())
        CHECK(gap.origin.kind != w::RuntimeGapKind::BmsAction || gap.origin.code != 39);
    std::printf("retail teammate factory: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
