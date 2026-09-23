#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <cstdio>
#include <memory>

using namespace opennova::world;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// Full original dispatchers, including their original reset/enter/exit callbacks,
// the no-target idle latch (ammo dwords, the profile's resolved weapon ammo bytes,
// the gunner guard) and the alert edge's entity+0x170 occupant gate.
// [orig: EntityAI_ProcessAirStateMachine @ 0x4581B0;
// EntityAI_ProcessGroundStateMachine @ 0x4583C0]
void brain_dispatch_vectors() {
    static const int32_t cases[][20] = {
#include "fixtures/brain_dispatch_vectors.inc"
    };
    auto world = std::make_unique<World>();
    auto &w = *world;
    w.registry.configure_pool(0, 1);
    w.registry.configure_pool(1, 1);
    const auto handle = w.registry.spawn(1, Entity{});
    const auto crew = w.registry.spawn(0, Entity{});
    const int index = w.ai.attach(handle);
    for (const auto &v : cases) {
        auto &e = *w.ai.at(index);
        e = AiEntity{};
        e.handle = handle;
        e.brain.f[AiBrain::kOwner] = 1;
        e.brain.f[AiBrain::kCurState] = v[3];
        e.brain.f[AiBrain::kPendState] = v[4];
        e.brain.f[AiBrain::kStep] = 31;
        e.brain.f[AiBrain::kAlert] = 3;
        e.brain.f[AiBrain::kPrevAlert] = 5;
        e.brain.f[AiBrain::kAmmoA] = v[5];
        e.brain.f[AiBrain::kAmmoB] = v[6];
        e.profile.fire_a.ammo_index = v[7]; // the resolved row behind profile+0x94
        e.profile.fire_b.ammo_index = v[8]; // ... and profile+0xB4
        e.brain.f[AiBrain::kGuard] = v[9];
        // The occupant at entity+0x170: none, an NPC, or a Player (Flags 0x100).
        w.registry.get(handle)->primary_occupant = v[10] != 0 ? crew : EntityHandle{};
        w.registry.get(crew)->flags = v[10] == 2 ? kEntityFlagPlayer : 0u;
        e.profile.flags96 = static_cast<uint8_t>(v[11]);
        w.registry.get(handle)->spawn_phase = 37;
        w.ai.is_authority = v[1] != 0;
        if (v[0]) w.ai.process_air_state_machine(e, w, v[2]);
        else w.ai.process_ground_state_machine(e, w, v[2]);
        const int32_t actual[] = {e.brain.f[AiBrain::kCurState], e.brain.f[AiBrain::kPendState],
            e.brain.f[AiBrain::kStep], e.brain.f[AiBrain::kTick], e.brain.f[AiBrain::kAlert],
            e.brain.f[AiBrain::kPrevAlert], w.registry.get(handle)->spawn_phase,
            e.brain.f[AiBrain::kNoTargetIdle]};
        for (int i = 0; i < 8; ++i) if (actual[i] != v[12 + i]) {
            std::printf("brain air=%d authority=%d event=%d state=%d pending=%d ammo=%d/%d "
                "weap=%d/%d guard=%d occupant=%d flags96=%d field=%d: %d != %d\n", v[0], v[1],
                v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], i, actual[i],
                v[12 + i]);
            ++failures;
            break;
        }
    }
}

// Exercise the public motor: its command registers survive the contact solve.
// [orig: Entity_UpdateVehiclePhysics @ 0x48B847..0x48C095]
void player_motor_vectors() {
    static const int32_t cases[][19] = {
#include "fixtures/vehicle_input_vectors.inc"
    };
    auto world = std::make_unique<World>();
    auto &w = *world;
    w.registry.configure_pool(0, 1);
    w.registry.configure_pool(1, 1);
    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.has_item_def = true;
    vehicle.health = vehicle.health_max = 3000;
    vehicle.alive = true;
    vehicle.position.z = 10.0f;
    Seat seat;
    seat.type = SeatType::Controller;
    seat.bone_index = 1;
    vehicle.seats.push_back(seat);
    const auto vh = w.registry.spawn(1, vehicle);
    Entity driver;
    driver.kind = EntityKind::Organic;
    driver.health = driver.health_max = 100;
    driver.alive = true;
    driver.player_class = 8;
    driver.position.z = 10.0f;
    const auto dh = w.registry.spawn(0, driver);
    CHECK(w.vehicles.process_attach(dh, vh, 1));
    const int di = w.ai.attach(dh);
    auto &body = *w.ai.at(di);
    body.inf.active = true;
    VehicleTraits traits;
    traits.family = VehicleFamily::Ground;
    traits.physics = 1;
    traits.player_control = true;
    traits.player_speed = 27542;
    traits.acceleration = 60;
    traits.deceleration = 280;
    traits.hand_brake = 1;
    w.ai.is_authority = true;
    w.env.water_z = -100 * 65536;
    for (const auto &row : cases) {
        const auto *v = row + 1;
        traits.family = row[0] == 0 ? VehicleFamily::Ground : VehicleFamily::Bike;
        auto &veh = *w.registry.get(vh);
        auto &drv = *w.registry.get(dh);
        veh.veh = Entity::VehicleMotorState{};
        veh.flags = veh.engine_flags = 0;
        veh.position = {0, 0, 10};
        veh.veh.yaw_seeded = true;
        veh.veh.yaw_bam = v[5];
        veh.veh.steer_target_bam = v[6];
        veh.veh.steer_ramp_bam = v[7];
        veh.veh.handbrake_latched = static_cast<uint8_t>(v[8]);
        veh.veh.handbrake_direction = v[9];
        veh.veh.bike_ground_contact_ticks = 1;
        drv.net_move_input = static_cast<uint8_t>(v[0]);
        drv.net_stance_bits = static_cast<uint8_t>(v[0] >> 8);
        drv.net_analog_x = static_cast<int8_t>(v[1]);
        drv.net_analog_y = static_cast<int8_t>(v[2]);
        drv.net_analog_z = static_cast<int8_t>(v[3]);
        drv.yaw = 64; // rounded mirror; the body owns the full input angle
        body.heading = body.inf.target_heading = v[4];
        w.vehicles.tick_motor(veh, traits);
        const int32_t actual[] = {veh.veh.cmd_speed, veh.veh.steer_target_bam,
            veh.veh.steer_ramp_bam, body.heading,
            int32_t(drv.net_move_input) | (int32_t(drv.net_stance_bits) << 8),
            int32_t(veh.flags & (0x80u | 0x20u | 0x8u)), veh.veh.handbrake_latched, veh.veh.handbrake_direction};
        for (int i = 0; i < 8; ++i) if (actual[i] != v[10 + i]) {
            std::printf("motor family=%d order=%x axes=%d,%d,%d brake=%d prior_dir=%d field=%d: %d != %d\n",
                row[0], v[0], v[1], v[2], v[3], v[8], v[9], i, actual[i], v[10 + i]);
            ++failures;
            break;
        }
    }
}


// Controller table row 4 uses controller+16 as this aircraft's phase, including
// the strict >496 boundary. Exercise dispatch, not only the translated leaf.
// [orig: AI_BeginUpdate @ 0x457B40; g_AIMoveStepFnTable @ 0x8153B8]
void aircraft_recovery_vectors() {
    static const int32_t cases[][12] = {
#include "fixtures/aircraft_recovery_vectors.inc"
    };
    auto world = std::make_unique<World>();
    auto &w = *world;
    const int first = w.ai.attach(EntityHandle::make(1, 0));
    const int second = w.ai.attach(EntityHandle::make(1, 1));
    for (const auto &v : cases) {
        auto &e = *w.ai.at(first);
        e.aircraft_controller = 4;
        e.aircraft_phase = v[0];
        e.heading = 123;
        e.brain.f[AiBrain::kPendState] = 0;
        e.brain.f[AiBrain::kFallback] = 17;
        e.brain.f[AiBrain::kStep] = v[1];
        e.brain.f[AiBrain::kSpeedA] = 7;
        e.brain.f[51] = 1000;
        e.profile.flags100 = v[2];
        e.profile.field216 = 2000;
        e.profile.field220 = 3000;
        w.ai.at(second)->aircraft_phase = 999;
        const int32_t result = w.ai.aircraft_movement(e, w);
        const int32_t actual[] = {result, e.aircraft_phase, e.brain.f[AiBrain::kPendState],
            e.brain.f[128], e.brain.f[131], e.brain.f[132], e.brain.f[133], e.brain.f[134], e.brain.f[138]};
        for (int i = 0; i < 9; ++i) CHECK(actual[i] == v[3 + i]);
        CHECK(w.ai.at(second)->aircraft_phase == 999);
    }
}

// The pool-1 callback is admitted by each entity's countdown, with no global
// budget or preliminary rewrite of its working movement registers.
// [orig: Entity_UpdatePool1Slot @ 0x4B8E1B..0x4B8E53]
void crowded_brain_tick() {
    for (bool authority : {false, true}) {
        auto world = std::make_unique<World>();
        auto &w = *world;
        constexpr int count = 80;
        w.registry.configure_pool(1, count);
        w.ai.is_authority = authority;
        // cveh class rows (their event callback is the vehicle machine) with the
        // +0x1C4 mover suspended, so the visit runs the think alone.
        VehicleTraits traits;
        traits.brain_class = VehicleBrainClass::Ground;
        w.vehicles.traits.set(77, traits);
        for (int i = 0; i < count; ++i) {
            Entity seed;
            seed.item_id = 77;
            seed.health = 100;
            seed.motor_suspended = true;
            const auto h = w.registry.spawn(1, seed);
            auto &e = *w.ai.at(w.ai.attach(h));
            e.health = 100;
            e.brain.f[AiBrain::kCurState] = kAiGroundPretty;
            e.brain.f[AiBrain::kPendState] = kAiGroundPretty;
            e.brain.f[AiBrain::kStep] = 64;
            e.brain.f[AiBrain::kOutSpeed] = 1234;
            e.brain.f[AiBrain::kWorkHeading] = 5678;
        }
        TickContext ctx{};
        ctx.world = &w;
        ctx.is_authority = authority;
        w.update_all_entities(ctx);
        for (int i = 0; i < count; ++i) {
            const auto &e = *w.ai.at(i);
            CHECK(e.brain.f[AiBrain::kTick] == 1);
            CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundPretty);
            CHECK(e.brain.f[AiBrain::kPendState] == kAiGroundPretty);
            CHECK(e.brain.f[AiBrain::kOutSpeed] == 1234);
            CHECK(e.brain.f[AiBrain::kWorkHeading] == 5678);
            CHECK(w.registry.get(e.handle)->spawn_phase == 63);
        }
    }
}
} // namespace

int main() {
    brain_dispatch_vectors();
    player_motor_vectors();
    crowded_brain_tick();
    aircraft_recovery_vectors();
    std::printf("movement_brain_parity: %d failures\n", failures);
    return failures ? 1 : 0;
}
