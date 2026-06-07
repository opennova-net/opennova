#include "world/world.h"

#include <vector>

namespace opennova::world {

// ----------------------------------------------------------------------------
// EntityCommands — the shared Entity_* primitive layer.
// In this foundational core, commands mutate the clean Entity model directly.
// (Replication routing through World::net is the deferred MP seam; LocalSink
// makes single-player run everything locally.)
// ----------------------------------------------------------------------------

bool EntityCommands::kill_ssn(uint16_t ssn) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->alive = false;
    e->health = 0;
    return true;
}

bool EntityCommands::remove_ssn(uint16_t ssn) {
    EntityHandle h = world_.registry.find_by_net_id(ssn);
    if (!world_.registry.get(h)) return false;
    world_.registry.despawn(h);
    return true;
}

bool EntityCommands::set_ssn_hp(uint16_t ssn, int32_t hp) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    return true;
}

bool EntityCommands::add_ssn_hp(uint16_t ssn, int32_t delta) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->health += delta;
    if (e->health < 0) e->health = 0;
    e->alive = e->health > 0;
    return true;
}

bool EntityCommands::set_ssn_waypoint(uint16_t ssn, int32_t wp) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->waypoint_id = static_cast<uint8_t>(wp);
    e->wp_number = 0;
    return true;
}

bool EntityCommands::set_ssn_alert(uint16_t ssn, int32_t state) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->alert_state = static_cast<uint8_t>(state);
    return true;
}

bool EntityCommands::set_ssn_target(uint16_t ssn, uint16_t target) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->ai_target = target;
    return true;
}

bool EntityCommands::set_ssn_move_speed(uint16_t ssn, int32_t kph) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->move_speed_kph = kph;
    return true;
}

bool EntityCommands::set_ssn_engage_min(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->engage_min = v;
    return true;
}

bool EntityCommands::set_ssn_engage_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->engage_max = v;
    return true;
}

bool EntityCommands::set_ssn_attack_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->attack_max = v;
    return true;
}

bool EntityCommands::set_ssn_anim(uint16_t ssn, int32_t anim_slot) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->anim_slot = anim_slot;
    return true;
}

bool EntityCommands::set_ssn_hidden(uint16_t ssn, bool hidden) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->hidden = hidden;
    return true;
}

bool EntityCommands::set_ssn_held(uint16_t ssn, bool held) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->held = held;
    return true;
}

bool EntityCommands::set_ssn_disabled(uint16_t ssn, bool disabled) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->disabled = disabled;
    return true;
}

bool EntityCommands::ssn_exists(uint16_t ssn) const {
    return world_.registry.get(world_.registry.find_by_net_id(ssn)) != nullptr;
}

bool EntityCommands::ssn_alive(uint16_t ssn) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    return e != nullptr && e->alive;
}

bool EntityCommands::ssn_dead(uint16_t ssn) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    return e != nullptr && !e->alive;
}

bool EntityCommands::ssn_in_area(uint16_t ssn, int area_id) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    const Area *a = world_.registry.area(area_id);
    if (!e || !a) return false;
    return a->bounds.contains(e->position);
}

int EntityCommands::kill_group(int group) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->alive = false; e->health = 0; ++n; }
    }
    return n;
}

int EntityCommands::group_to_waypoint(int group, int32_t wp) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->waypoint_id = static_cast<uint8_t>(wp); e->wp_number = 0; ++n; }
    }
    return n;
}

int EntityCommands::set_group_hp(int group, int32_t hp) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->health = hp; e->alive = hp > 0; ++n; }
    }
    return n;
}

int EntityCommands::set_group_engage_min(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->engage_min = v; ++n; }
    }
    return n;
}

int EntityCommands::set_group_engage_max(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->engage_max = v; ++n; }
    }
    return n;
}

int EntityCommands::set_group_attack_max(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->attack_max = v; ++n; }
    }
    return n;
}

bool EntityCommands::group_alive(int group) const {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    for (EntityHandle h : members) {
        const Entity *e = world_.registry.get(h);
        if (e && e->alive) return true;
    }
    return false;
}

bool EntityCommands::group_dead(int group) const {
    return !group_alive(group);
}

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

void World::load_systems() {
    for (ISystem *s : systems_) s->on_load(*this);
}

void World::run_logic_tick(bool is_authority, bool pre_mission) {
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.pre_mission = pre_mission;
    // Scripting + sim run only on the authoritative host (faithful: WAC/BMS live
    // inside Server_TickUpdate). Non-authority peers only apply replicated state.
    if (is_authority) {
        for (ISystem *s : systems_) s->tick(*this, ctx);
    }
    ++logic_tick; // [orig: dword_C6EAD8 increments after execution, sub_4F81A0]
}

World::Snapshot World::snapshot() const {
    Snapshot s;
    s.registry = registry;
    s.vars = vars;
    s.env = env;
    s.logic_tick = logic_tick;
    return s;
}

void World::restore(const Snapshot &s) {
    registry = s.registry;
    vars = s.vars;
    env = s.env;
    logic_tick = s.logic_tick;
    effects.clear();
    load_systems(); // systems re-init their per-mission state
}

// ----------------------------------------------------------------------------
// TickService
// ----------------------------------------------------------------------------

bool TickService::advance_frame(World &world, bool is_authority) {
    if (paused) return false;
    if (++frame_accum_ >= kFramesPerLogicTick) {
        frame_accum_ = 0;
        world.run_logic_tick(is_authority);
        return true;
    }
    return false;
}

} // namespace opennova::world
