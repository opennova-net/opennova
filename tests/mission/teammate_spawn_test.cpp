#include "common/retail_mission_files.h"
#include "common/retail_paths.h"
#include <runtime/world/teammate_operations.h>

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace opennova;
namespace w = opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
}
int main() {
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
        CHECK(!first->has_physics && !second->has_physics && !heli->has_physics);
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
        //  call @0x4C2484, `test tick,3` @0x4C25CE -> sub_459290 @0x459290]
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
