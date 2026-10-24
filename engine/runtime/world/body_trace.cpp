// The body readout's recording calls (an instrument, no behaviour; body_trace.h).
#include <runtime/world/body_trace.h>

#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

BodyTraceRecord body_trace_head(const World &world, BodyTraceEvent event, EntityHandle h) {
    BodyTraceRecord record;
    record.logic_tick = world.logic_tick;
    record.event = event;
    record.handle = h.valid() ? static_cast<uint16_t>(h.packed) : uint16_t{0xFFFFu};
    return record;
}

uint16_t handle_word(EntityHandle h) {
    return h.valid() ? static_cast<uint16_t>(h.packed) : uint16_t{0xFFFFu};
}

int32_t collision_model_of(const World &world, EntityHandle h) {
    return world.collision != nullptr && h.valid() ? world.collision->entity_model_id(h) : -1;
}

} // namespace

// The words a body's motion and death turn on, after the entity pass: a person's velocity
// triple is the infantry body's (+0x98 / +0x9C / +0xA0, the last its slideDecay), a hull's
// the motor's two words and its vertical word; +0x28 is the word the movement resolver's
// ground probe stores [orig: Entity_RaycastGroundHeightAndObject @0x414320 - the store
// @0x414370; Entity_MovementCollisionResolver @0x4B2BD0 - the call @0x4B3D95].
void body_trace_tick(World &world) {
    if (!world.body_trace.any_watch()) return;
    for (const uint16_t word : world.body_trace.watch) {
        if (word == 0xFFFFu) continue;
        EntityHandle h;
        h.packed = word;
        BodyTraceRecord record = body_trace_head(world, BodyTraceEvent::kTick, h);
        const Entity *e = world.registry.get(h);
        record.present = e != nullptr;
        if (e != nullptr) {
            record.pos[0] = e->position.x.q16;
            record.pos[1] = e->position.y.q16;
            record.pos[2] = e->position.z.q16;
            if (const AiEntity *body = world.ai.for_handle(h); body != nullptr && body->inf.active) {
                const InfantryState &inf = body->inf;
                record.vel[0] = inf.vel[0];
                record.vel[1] = inf.vel[1];
                record.vel[2] = inf.vel[2];
                record.eye[0] = inf.eye_offset_x;
                record.eye[1] = inf.eye_offset_y;
                record.eye[2] = inf.eye_offset_z;
                record.eye_source = inf.dbg_eye_source;
                record.anim_state = static_cast<int16_t>(inf.anim_state);
                record.anim_playing = static_cast<int16_t>(inf.body_clip_state());
                record.anim_prev = static_cast<int16_t>(inf.anim_prev);
                record.anim_blend_milli = static_cast<int16_t>(inf.anim_blend_weight * 1000.0f);
                record.clip_phase = inf.clip_phase;
                record.capsule_bottom = inf.prev_capsule_bottom;
                record.airborne = inf.airborne;
                record.resolve_push[0] = inf.dbg_res_dx;
                record.resolve_push[1] = inf.dbg_res_dy;
                record.contact_item = inf.dbg_contact_item;
            } else {
                record.vel[0] = e->veh.vel_x;
                record.vel[1] = e->veh.vel_y;
                record.vel[2] = e->veh.slide_z;
            }
            record.flags = e->flags;
            record.engine_flags = e->engine_flags;
            record.health = static_cast<int16_t>(e->health);
            record.mounted = e->mounted;
            record.mount_target = handle_word(e->mount_target);
            record.mount_seat = e->mount_seat;
            record.mount_bone = e->mount_bone;
            record.ground_target = handle_word(e->ground_target);
            record.collision_model = collision_model_of(world, h);
            record.think_clock = e->spawn_phase;
            if (const AiEntity *brain = world.ai.for_handle(h);
                    brain != nullptr && !brain->inf.active && brain->brain.f[AiBrain::kOwner] != 0) {
                record.brain_state = static_cast<int16_t>(brain->brain.f[AiBrain::kCurState]);
                record.brain_pend = static_cast<int16_t>(brain->brain.f[AiBrain::kPendState]);
            }
        }
        world.body_trace.append(record);
    }
}

EntityHandle body_trace_ground_probe(World &world, EntityHandle body, const int32_t start[3],
        int32_t ground_z, EntityHandle hit) {
    if (!world.body_trace.watches(body)) return hit;
    BodyTraceRecord record = body_trace_head(world, BodyTraceEvent::kGroundProbe, body);
    record.present = true;
    record.pos[0] = start[0];
    record.pos[1] = start[1];
    record.pos[2] = start[2];
    record.probe_z = ground_z;
    record.ground_target = handle_word(hit);
    record.collision_model = collision_model_of(world, hit);
    world.body_trace.append(record);
    return hit;
}

void body_trace_force(World &world, const Entity &target, uint8_t hit_type, uint8_t force_type,
        const FixedVec3 &source, EntityHandle attacker, const int32_t vel[3]) {
    if (!world.body_trace.watches(target.handle)) return;
    BodyTraceRecord record = body_trace_head(world, BodyTraceEvent::kForce, target.handle);
    record.present = true;
    record.vel[0] = vel[0];
    record.vel[1] = vel[1];
    record.vel[2] = vel[2];
    record.flags = target.flags;
    record.engine_flags = target.engine_flags;
    record.health = static_cast<int16_t>(target.health);
    record.other = handle_word(attacker);
    record.hit_type = hit_type;
    record.force_type = force_type;
    record.source[0] = source.x;
    record.source[1] = source.y;
    record.source[2] = source.z;
    world.body_trace.append(record);
}

void body_trace_blast(World &world, const Entity &target, int32_t kz_type, EntityHandle attacker,
        const int32_t centre[3], int32_t damage, int32_t health_before) {
    if (!world.body_trace.watches(target.handle)) return;
    BodyTraceRecord record = body_trace_head(world, BodyTraceEvent::kBlast, target.handle);
    record.present = true;
    record.pos[0] = target.position.x.q16;
    record.pos[1] = target.position.y.q16;
    record.pos[2] = target.position.z.q16;
    record.flags = target.flags;
    record.engine_flags = target.engine_flags;
    record.health = static_cast<int16_t>(target.health);
    record.health_before = static_cast<int16_t>(health_before);
    record.other = handle_word(attacker);
    record.hit_type = static_cast<uint8_t>(kz_type);
    record.source[0] = centre[0];
    record.source[1] = centre[1];
    record.source[2] = centre[2];
    record.damage = damage;
    world.body_trace.append(record);
}

namespace {

void append_brain_event(World &world, BodyTraceEvent event, EntityHandle entity, int32_t word,
        int32_t state_before, int32_t state_after) {
    if (!world.body_trace.watches(entity)) return;
    BodyTraceRecord record = body_trace_head(world, event, entity);
    record.present = world.registry.get(entity) != nullptr;
    record.ai_event = word;
    record.brain_pend = static_cast<int16_t>(state_before);
    record.brain_state = static_cast<int16_t>(state_after);
    world.body_trace.append(record);
}

} // namespace

void body_trace_ai_event(World &world, EntityHandle entity, int32_t type, int32_t state_before,
        int32_t state_after) {
    append_brain_event(world, BodyTraceEvent::kAiEvent, entity, type, state_before, state_after);
}

void body_trace_class_event(World &world, EntityHandle entity, int32_t phase,
        int32_t state_before, int32_t state_after) {
    append_brain_event(world, BodyTraceEvent::kClassEvent, entity, phase, state_before,
            state_after);
}

} // namespace opennova::world
