#include <runtime/world/teammate_operations.h>
#include <runtime/world/world.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_part_anim.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opennova::world {
namespace {
EntityLifetime lifetime(const Entity *entity) {
    return entity ? EntityLifetime{entity->handle, entity->registry_spawn_id} : EntityLifetime{};
}
std::array<int32_t, 3> position(const World &world, const Entity &entity) {
    if (const AiEntity *ai = world.ai.for_handle(entity.handle))
        return {ai->pos[0], ai->pos[1], ai->pos[2]};
    return {int32_t(entity.position.x * 65536.0f), int32_t(entity.position.y * 65536.0f),
            int32_t(entity.position.z * 65536.0f)};
}
int32_t heading(const World &world, const Entity &entity) {
    const AiEntity *ai = world.ai.for_handle(entity.handle);
    return ai ? ai->heading : bam_heading_from_mission_yaw_deg(entity.yaw);
}
int32_t trig(int32_t angle, bool sine) {
    const double rad = angle / io::kBamPerRadian;
    return int32_t((sine ? std::sin(rad) : std::cos(rad)) * 4194304.0);
}
int32_t mul22(int32_t a, int32_t b) {
    return int32_t((int64_t(a) * b) >> 22);
}
double distance(const World &world, const Entity &a, const Entity &b) {
    const auto p = position(world, a), q = position(world, b);
    const double dx = io::bam_sub(p[0], q[0]), dy = io::bam_sub(p[1], q[1]),
                 dz = io::bam_sub(p[2], q[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
int32_t ftol_distance(const World &world, const Entity &a, const Entity &b) {
    const double d = distance(world, a, b);
    return d < 2147483648.0 ? int32_t(d) : std::numeric_limits<int32_t>::min();
}
void flag40(Entity *entity, bool set) {
    if (!entity) return;
    if (set) { entity->flags |= 0x40u; entity->engine_flags |= 0x40u; }
    else { entity->flags &= ~0x40u; entity->engine_flags &= ~0x40u; }
}
bool dead(const Entity &entity) {
    return ((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0;
}
void order(World &world, Entity *source, const Entity *target) {
    if (!source || !target) return;
    if (AiEntity *ai = world.ai.for_handle(source->handle)) {
        ai->slot.f[37] = 125;
        ai->slot.f[38] = target->net_id;
        ai->slot.f[36] = int32_t(target->handle.packed) + 1;
        ai->slot.bytes()[AiSlot::kAlertByte] = 2;
    }
}
void queue(World &world, const AiEntity &ai, int32_t type, int32_t parameter) {
    AiEventEntry event{};
    event.f[0] = type;
    event.f[1] = int32_t(uint32_t(world.ai.index_for_handle(ai.handle)) << 16);
    event.f[3] = parameter;
    event.set_timer(0.0f);
    world.ai.events.queue(event);
}
void waypoint(AiEntity &ai, const std::array<int32_t, 3> &goal) {
    ai.brain.f[AiBrain::kWpType] = 3;
    for (int i = 0; i < 3; ++i) ai.brain.f[17 + i] = goal[i];
    ai.brain.f[20] = 1310720;
}
// Both the approach and flyover arms write the patient's aim and torso after
// the entity update's previous frame. [orig: @0x451829..0x451947, @0x451C4B..0x451D28]
void animate_patient(World &world, const Entity *patient) {
    if (!patient) return;
    AiEntity *ai = world.ai.for_handle(patient->handle);
    if (!ai) return;
    const int32_t fast = int32_t(world.logic_tick << 26);
    const int32_t slow = int32_t(world.logic_tick << 22);
    int32_t pitch = mul22(mul22(262470208, trig(fast, false)), trig(slow, false));
    const int32_t roll = mul22(mul22(262470208, trig(fast, true)), trig(slow, true));
    if (std::abs(int64_t(pitch)) > 178956960) pitch = 0;
    ai->inf.aim_pitch = pitch >> 3;
    ai->inf.torso_roll = io::bam_sub(pitch, roll) >> 2;
}
void diagnostic(World &world, const char *reason, int32_t subtype, int32_t patient) {
    world.diagnostics.record({RuntimeGapKind::BmsAction, 39, subtype, -1, -1, reason},
            world.logic_tick, {patient, 0, 0, 0});
}
void retire(World &world, EntityLifetime &owner) {
    if (const Entity *entity = world.registry.get(owner)) {
        // Use the bound handle: several concurrent operations intentionally
        // share 11000/12000/12001 identities.
        world.commands.remove_ssn(entity->handle);
    }
    owner = {};
}
EntityLifetime spawn(World &world, uint16_t type, uint16_t ssn,
        const std::array<int32_t, 3> &pos, int32_t angle, bool helicopter = false) {
    TeammateSpawn request;
    request.item_type = type; request.ssn = ssn; request.heading = angle;
    request.helicopter = helicopter;
    std::copy(pos.begin(), pos.end(), request.pos);
    return lifetime(world.registry.get(world.teammate_spawner->spawn_teammate(request)));
}
// [orig: Entity_FindNearestTeammate @0x4520B0] The 40-unit gate is a box
// in X/Y only. Z participates in ranking, not in the eligibility test.
void announce_pickup(World &world) {
    const Entity *local = world.registry.get(world.cached.local_player);
    if (!local) return;
    EntityHandle best;
    int32_t best_distance = 2147418112;
    const auto origin = position(world, *local);
    world.registry.for_each_in_pool(0, [&](const Entity &entity) {
        if (entity.item_id == 0 || dead(entity) || entity.team != 1 || entity.handle == local->handle)
            return;
        const auto p = position(world, entity);
        if (std::abs(int64_t(io::bam_sub(p[0], origin[0]))) > 2621440 ||
                std::abs(int64_t(io::bam_sub(p[1], origin[1]))) > 2621440) return;
        const int32_t d = int32_t(std::min(distance(world, *local, entity), 2147418112.0));
        if (d < best_distance) { best = entity.handle; best_distance = d; }
    });
    if (best.valid()) world.script.voice.ssn_wave(world, best, "DltB086C.wav", 100, false);
}
} // namespace

void TeammateOperations::reset(World &world) {
    // [orig: EventTrigger_ResetAllSlots @0x4513B0]
    slots_ = {};
    count_ = 0;
    world.script.heli_lift_active_count = 0;
}

bool TeammateOperations::start(World &world, int32_t subtype, int32_t patient_ssn,
        int32_t marker_number) {
    // Action 39 has only these two retail arms; subtype 3 is a no-op.
    // [orig: EventAction_Dispatch @0x454A41]
    if (subtype != 1 && subtype != 2) return false;
    if (count_ == kCapacity) return false;
    EntityLifetime patient, marker;
    world.registry.for_each_in_pool(0, [&](const Entity &entity) {
        if (!patient.valid() && entity.net_id == patient_ssn) patient = lifetime(&entity);
    });
    world.registry.for_each_in_pool(3, [&](const Entity &entity) {
        if (!marker.valid() && entity.has_item_def && entity.item_id == 6088 &&
                entity.wp_number == marker_number) marker = lifetime(&entity);
    });
    if (!patient.valid() || !marker.valid()) return false;
    if (!world.teammate_spawner) {
        diagnostic(world, "Teammate spawn assets are unavailable", subtype, patient_ssn);
        return false;
    }
    Slot slot;
    slot.patient = patient;
    const Entity &point = *world.registry.get(marker);
    auto pos = position(world, point);
    int32_t angle = heading(world, point);
    slot.goal = pos;
    if (subtype == 2) {
        // [orig: HeliLift_SpawnFlyover @0x452730]
        pos[0] = io::bam_add(pos[0], 39321600);
        pos[2] = io::bam_add(pos[2], 5242880);
        angle = 2147483520;
        slot.helicopter = spawn(world, 1281, 11000, pos, angle, true);
        Entity *heli = world.registry.get(slot.helicopter);
        if (!heli) {
            diagnostic(world, "Teammate helicopter allocation failed", subtype, patient_ssn);
            return false;
        }
        heli->veh.part_spin.speed = kRotorSpeedMax;
        pos = position(world, *heli);
        angle = heading(world, *heli);
        pos[0] = io::bam_sub(io::bam_add(pos[0], mul22(trig(angle, false), 16384)), 8192);
        pos[1] = io::bam_sub(io::bam_add(pos[1], mul22(trig(angle, true), 16384)), 8192);
        pos[2] = io::bam_sub(pos[2], 98304);
        angle = io::bam_sub(angle, 1073741760);
        slot.state = State::FlyToHover;
    } else {
        // [orig: HeliLift_SpawnPickup @0x4525E0] No helicopter is allocated.
        slot.state = State::ApproachPatient;
    }
    slot.first = spawn(world, 4529, 12000, pos, angle);
    if (subtype == 2) {
        pos[0] = io::bam_add(pos[0], 16384);
        pos[1] = io::bam_add(pos[1], 16384);
    }
    slot.second = spawn(world, 4520, 12001, pos, angle);
    Entity *first = world.registry.get(slot.first), *second = world.registry.get(slot.second);
    if (!first || !second) {
        // Retail dereferences failed pool allocations. Abort this operation
        // without removing its patient or another allocation at a reused handle.
        retire(world, slot.first); retire(world, slot.second); retire(world, slot.helicopter);
        diagnostic(world, "Teammate medic allocation failed", subtype, patient_ssn);
        return false;
    }
    if (AiEntity *ai = world.ai.for_handle(first->handle)) ai->slot.f[15] = 0;
    if (subtype == 2) {
        if (AiEntity *ai = world.ai.for_handle(slot.helicopter.handle)) {
            auto hover = slot.goal; hover[2] = io::bam_add(hover[2], 2293760);
            waypoint(*ai, hover);
            queue(world, *ai, 7, 5);
            queue(world, *ai, 11, 120);
        }
        first->ground_target = second->ground_target = slot.helicopter.handle;
    } else {
        order(world, first, world.registry.get(patient));
        order(world, second, first);
        announce_pickup(world);
    }
    world.registry.get(patient)->corpse_timer = subtype == 2 ? 30000 : 10000;
    slots_[count_++] = slot;
    world.script.heli_lift_active_count = int32_t(count_);
    return true;
}

void TeammateOperations::tick(World &world) {
    // [orig: HeliLift_UpdateAll @0x451FA0] Finished rows remain active until
    // the NEXT pass; compaction revisits the shifted row in this pass.
    for (size_t i = 0; i < count_;) {
        if (slots_[i].state == State::Finished) {
            for (size_t j = i; j + 1 < kCapacity; ++j) slots_[j] = slots_[j + 1];
            slots_.back() = {};
            --count_;
        } else {
            update(world, slots_[i]);
            ++i;
        }
    }
    world.script.heli_lift_active_count = int32_t(count_);
}

void TeammateOperations::update(World &world, Slot &slot) {
    // [orig: HeliLift_UpdateSlotState @0x451730]
    Entity *patient = world.registry.get(slot.patient);
    Entity *first = world.registry.get(slot.first), *second = world.registry.get(slot.second);
    Entity *heli = world.registry.get(slot.helicopter);
    AiEntity *air = heli ? world.ai.for_handle(heli->handle) : nullptr;
    const auto orient = [&]() { if (air) air->brain.f[AiBrain::kWorkHeading] = slot.heading; };
    switch (slot.state) {
    case State::ApproachPatient:
        if (!patient || !first) {
            diagnostic(world, "Teammate approach lost an entity", 1, patient ? patient->net_id : -1);
            slot.state = State::Finished; break;
        }
        animate_patient(world, patient);
        slot.assisting = 1;
        orient();
        if (dead(*first)) slot.state = State::Treat;
        if (ftol_distance(world, *patient, *first) < 73728) {
            slot.state = State::Treat; slot.medic = 1;
        }
        if (slot.state == State::Treat) slot.deadline = 0;
        break;
    case State::Treat:
        if (slot.deadline == 0) {
            slot.deadline = int32_t(world.logic_tick + 310);
            flag40(first, true);
        }
        orient();
        if (slot.deadline < int32_t(world.logic_tick)) {
            if (!heli) {
                // A fresh pickup has no slot[3] producer anywhere in retail.
                // Its @0x451DCD dereference would fault; terminate at that edge.
                flag40(first, false); flag40(second, false);
                diagnostic(world, "Teammate pickup has no helicopter", 1, patient ? patient->net_id : -1);
                slot.state = State::Finished; break;
            }
            Entity *medic = slot.medic == 1 ? first : slot.medic == 2 ? second : nullptr;
            if (medic && patient) {
                medic->dragger = medic->handle; medic->dragger_spawn_id = medic->registry_spawn_id;
                patient->dragger = medic->handle; patient->dragger_spawn_id = medic->registry_spawn_id;
            }
            order(world, first, heli); order(world, second, heli);
            flag40(first, false); flag40(second, false);
            slot.state = State::Return;
        }
        break;
    case State::Return:
        if (!heli) { slot.state = State::Boarded; break; }
        orient();
        if (!first || !second) {
            diagnostic(world, "Teammate return lost a medic", 2, patient ? patient->net_id : -1);
            slot.state = State::Boarded; break;
        }
        if (ftol_distance(world, *heli, *first) < 131072 ||
                (ftol_distance(world, *heli, *second) < 131072 && dead(*first)) ||
                (dead(*first) && dead(*second))) slot.state = State::Boarded;
        break;
    case State::Boarded:
        slot.state = State::Ascend;
        break;
    case State::FlyToHover:
        if (air && air->brain.f[AiBrain::kWpDistance] != 0 &&
                air->brain.f[AiBrain::kWpDistance] < 2621440) {
            queue(world, *air, 11, 20);
            slot.state = State::Descend;
        }
        [[fallthrough]];
    case State::Descend:
        if (air && air->brain.f[AiBrain::kWpDistance] != 0 &&
                air->brain.f[AiBrain::kWpDistance] < 327680) {
            waypoint(*air, slot.goal);
            queue(world, *air, 11, 0);
            slot.state = State::Land;
        }
        animate_patient(world, patient);
        break;
    case State::Land:
        if (air && patient) {
            const auto p = position(world, *patient), h = position(world, *heli);
            const double bearing = std::atan2(double(io::bam_sub(p[1], h[1])),
                    double(io::bam_sub(p[0], h[0]))) * io::kBamPerRadian;
            slot.heading = io::bam_sub(1107296256, int32_t(int64_t(bearing)));
            orient();
            air->brain.f[17] = io::bam_add(slot.goal[0], mul22(trig(slot.heading, false), 1966080));
            air->brain.f[18] = io::bam_add(slot.goal[1], mul22(trig(slot.heading, true), 1966080));
            if (h[2] == heli->saved_live_pos[2]) {
                order(world, first, patient); order(world, second, first);
                slot.state = State::ApproachPatient; slot.medic = 0;
            }
        }
        break;
    case State::Ascend:
        if (!air) { slot.state = State::Depart; break; }
        air->brain.f[AiBrain::kWpType] = 3;
        air->brain.f[20] = 1310720;
        if (air->pos[2] < io::bam_add(slot.goal[2], 2293760)) {
            air->brain.f[19] = io::bam_add(slot.goal[2], 3604480);
        } else {
            air->brain.f[17] = io::bam_add(slot.goal[0], 39321600);
            air->brain.f[19] = io::bam_add(slot.goal[2], 5242880);
            queue(world, *air, 11, 120);
            air->brain.f[AiBrain::kWpDistance] = 0;
            slot.state = State::Depart;
        }
        break;
    case State::Depart:
        if (!air || (air->brain.f[AiBrain::kWpDistance] != 0 &&
                air->brain.f[AiBrain::kWpDistance] < 1966080)) {
            retire(world, slot.patient); retire(world, slot.first); retire(world, slot.second);
            if (air) retire(world, slot.helicopter); // retail preserves a brainless helicopter
            slot.state = State::Finished;
        }
        break;
    case State::Finished:
        break;
    }
}
} // namespace opennova::world
