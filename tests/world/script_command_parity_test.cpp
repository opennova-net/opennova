// Script command parity: the WAC command bodies and the shared EntityCommands
// primitives they reach, pinned against the witnessed retail handlers (each test
// names its addresses). Scripts compile through the runtime compiler and run on
// a small world; the primitives are also driven directly through EntityCommands.
#include <cstdio>
#include <string>

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

using namespace opennova::wac;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct ScriptWorld final : World {
    ScriptWorld() {
        registry.configure_pool(0, 16);
        registry.configure_pool(1, 16);
        registry.configure_pool(2, 16);
        registry.configure_pool(3, 16);
        cached.humans = 1;
    }
};

// One VM execution per 62 logic ticks [orig: WacScript_AdvanceTick @0x4F81B1].
void run(World &w, int executions) {
    for (int i = 0; i < executions * WacSystem::kTicksPerExecution; ++i)
        w.run_logic_tick(/*is_authority=*/true);
}

// Load `source` into `sys` on `w`; the caller keeps `sys` alive while it runs.
bool load_script(World &w, WacSystem &sys, const std::string &source) {
    CompileEnv env;
    env.registry = &w.registry;
    Program program = compile_source(source, env);
    const bool ok = program.ok();
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();
    return ok;
}

// An NPC row with a controller slot: item-typed person (def+0x5C == 3).
EntityHandle spawn_npc(World &w, int32_t net_id, uint8_t group, int32_t health = 100) {
    Entity e;
    e.net_id = static_cast<uint16_t>(net_id);
    e.kind = EntityKind::Organic;
    e.item_id = 1001;
    e.item_type = 3;
    e.has_item_def = true;
    e.group_id = group;
    e.health = health;
    e.health_max = 100;
    e.alive = health > 0;
    const EntityHandle h = w.registry.spawn(0, e);
    w.ai.attach(h);
    return h;
}

} // namespace

// WAC killSSN is its own handler, not the BMS kill: the ItemTypeIndex gate, the
// cleared attacker and (for a person) the cleared staged clip, then the class
// event with a zeroed hit record, which for an NPC is the phase-1 red alert on
// the victim and its trigger group. The death transaction that follows carries
// no killer. The handler's IDB name is a misnomer.
// [orig: Entity_ResetWeaponState @0x4F1E40 — gate @0x4F1E89, lastAttacker
// @0x4F1EAD, +0x2C0 @0x4F1EB7..0x4F1EBD, callback @0x4F1EC7..0x4F1ED2;
// Entity_HandleDamageTrigger phase-1 arm @0x4073BF..0x4073EA]
static void test_wac_kill_ssn_clears_and_alerts() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 100, 4);
    const EntityHandle shooter = spawn_npc(w, 101, 5);
    Entity *victim = w.registry.get(npc);
    victim->last_attacker = shooter;          // a prior non-lethal hit
    victim->death_anim_state = 183;           // ... and its staged clip
    Entity itemless;
    itemless.net_id = 102;
    itemless.health = 50;
    const EntityHandle bare = w.registry.spawn(1, itemless);

    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then killSSN(100) store(v1) killSSN(102) store(v2) endif\n"));
    run(w, 1);

    CHECK(victim->health == 0);
    CHECK(!victim->last_attacker.valid());
    CHECK(victim->death_anim_state == 0);
    CHECK(w.ai.for_handle(npc)->slot.bytes()[AiSlot::kAlertByte] == 2);
    CHECK(w.script.relations.group(4).alert == TriggerRelations::kAlertRed);
    CHECK(w.script.relations.group(5).alert == TriggerRelations::kAlertGreen);
    CHECK(w.round_sim.deaths.size() == 1);
    if (!w.round_sim.deaths.empty()) {
        CHECK(w.round_sim.deaths[0].victim == npc);
        CHECK(!w.round_sim.deaths[0].killer.valid());
    }
    CHECK(w.script.vars.get_mission(1) == 1);
    // No ItemTypeIndex: refused, returns 0, health untouched.
    CHECK(w.script.vars.get_mission(2) == 0);
    CHECK(w.registry.get(bare)->health == 50);
}

// The SM-brained item's class event is its brain machine's event 1: the queued
// type-1 AIEvent with the hit record's (cleared) owner word.
// [orig: Entity_ResetWeaponState @0x4F1ED2 -> EntityAI_ProcessVehicleStateMachine
// @0x4583C0 event-1 arm, queue @0x45851a]
static void test_wac_kill_ssn_queues_brain_event() {
    ScriptWorld w;
    Entity vehicle;
    vehicle.net_id = 300;
    vehicle.item_id = 2001;
    vehicle.has_item_def = true;
    vehicle.is_ai_capable = true;
    vehicle.health = 400;
    vehicle.alive = true;
    const EntityHandle h = w.registry.spawn(1, vehicle);
    w.ai.attach(h);
    CHECK(w.ai.events.count() == 0);
    CHECK(w.commands.wac_kill_ssn(h));
    CHECK(w.registry.get(h)->health == 0);
    CHECK(w.ai.events.count() == 1);
    if (w.ai.events.count() == 1) {
        CHECK(w.ai.events.at(0).type() == 1);
        CHECK(w.ai.events.at(0).f[3] == 0);
    }
    CHECK(w.round_sim.deaths.empty()); // the brain machine owns the item's death
}

// BMS KillSingle keeps its own shape: pool 0 loses the attacker and staged clip
// too, but there is no ItemTypeIndex gate. [orig: Entity_KillByNetId @0x43DBD0
// — pool 0 @0x43DC0E..0x43DC31]
static void test_bms_kill_single_pool0() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 110, 6);
    const EntityHandle shooter = spawn_npc(w, 111, 7);
    Entity *victim = w.registry.get(npc);
    victim->last_attacker = shooter;
    victim->death_anim_state = 181;
    victim->item_id = 0; // no gate on this path
    CHECK(w.commands.kill_ssn(uint16_t(110)));
    CHECK(victim->health == 0);
    CHECK(!victim->last_attacker.valid());
    CHECK(victim->death_anim_state == 0);
    CHECK(w.script.relations.group(6).alert == TriggerRelations::kAlertRed);
    CHECK(w.round_sim.deaths.size() == 1);
}

// WAC kill / BMS KillGroup: pools 2, 0, 1 (a pool-3 member is skipped), every
// member including the dead, the attacker kept, group 0 a no-op, and the WAC
// command returns 0. [orig: Entity_KillAllByNetId @0x43C8E0 — group-0 exit
// @0x43C8F2, pools @0x43C8F8/@0x43C946/@0x43C996, per row @0x43C917..0x43C93F;
// WacCmd_Kill `xor eax,eax` @0x4EDC9D]
static void test_kill_group_walk_and_return() {
    ScriptWorld w;
    const EntityHandle a = spawn_npc(w, 120, 9);
    const EntityHandle b = spawn_npc(w, 121, 9);
    const EntityHandle shooter = spawn_npc(w, 122, 10);
    w.registry.get(a)->last_attacker = shooter;
    Entity marker;
    marker.net_id = 123;
    marker.kind = EntityKind::Marker;
    marker.group_id = 9;
    marker.health = 20;
    const EntityHandle m = w.registry.spawn(3, marker);
    CHECK(w.commands.kill_group(0) == 0);

    WacSystem sys;
    CHECK(load_script(w, sys, "if never() then set(v3,7) kill(9) store(v3) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(3) == 0);
    CHECK(w.registry.get(a)->health == 0 && w.registry.get(b)->health == 0);
    CHECK(w.registry.get(m)->health == 20);
    CHECK(w.script.relations.group(9).alert == TriggerRelations::kAlertRed);
    CHECK(w.round_sim.deaths.size() == 2);
    if (w.round_sim.deaths.size() == 2) {
        CHECK(w.round_sim.deaths[0].victim == a);
        CHECK(w.round_sim.deaths[0].killer == shooter); // kept, unlike KillSingle
    }
    // A second kill visits the dead rows again but raises no second death.
    w.round_sim.deaths.clear();
    CHECK(w.commands.kill_group(9) == 2);
    CHECK(w.round_sim.deaths.empty());
}

// Gkill runs the killSSN body on each handle of the script group.
// [orig: WacCmd_GroupKill @0x4F1F40 -> @0x4F1F5E]
static void test_gkill_uses_kill_ssn_body() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 130, 11);
    const EntityHandle shooter = spawn_npc(w, 131, 12);
    w.registry.get(npc)->last_attacker = shooter;
    w.registry.set_script_group_members(w.registry.intern_group("squad"), {npc});
    WacSystem sys;
    CHECK(load_script(w, sys, "if never() then Gkill(G_squad) endif\n"));
    run(w, 1);
    CHECK(w.registry.get(npc)->health == 0);
    CHECK(!w.registry.get(npc)->last_attacker.valid());
    CHECK(w.round_sim.deaths.size() == 1);
}

// hideSSN/unhideSSN write Flags bit 0, the bit the area/location tests (and the
// AI target scan, zone capture, traces) key on; a row without an ItemTypeIndex
// is refused. [orig: WacCmd_HideSsn @0x4F7750 — gate @0x4F7797, `or Flags,1`
// @0x4F779D; WacCmd_UnhideSsn `and Flags,~1` @0x4F77FD; WacCmd_SsnArea's
// hidden test @0x4F1081]
static void test_hide_ssn_flag_bit() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 140, 13);
    w.registry.get(npc)->position = {5.0f, 5.0f, 0.0f};
    const Aabb zone{{0.0f, 0.0f, -10.0f}, {10.0f, 10.0f, 10.0f}};
    w.registry.register_area("zone", zone, true, 51, zone);
    Entity itemless;
    itemless.net_id = 141;
    const EntityHandle bare = w.registry.spawn(1, itemless);
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then hideSSN(140) v1=SSNarea(140,51) unhideSSN(140) "
            "v2=SSNarea(140,51) hideSSN(141) store(v3) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 0); // hidden: out of every area test
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK((w.registry.get(npc)->engine_flags & kEntityFlagCarried) == 0);
    CHECK(!w.registry.get(npc)->hidden);
    CHECK(w.script.vars.get_mission(3) == 0);
    CHECK((w.registry.get(bare)->engine_flags & kEntityFlagCarried) == 0);
    // A respawn-hidden row (bit 0 on both views) is lifted by unhideSSN too.
    Entity *e = w.registry.get(npc);
    e->flags |= kEntityFlagCarried;
    e->engine_flags |= kEntityFlagCarried;
    e->hidden = true;
    CHECK(w.commands.set_ssn_hidden(npc, false));
    CHECK(((e->flags | e->engine_flags) & kEntityFlagCarried) == 0 && !e->hidden);
    CHECK(w.commands.set_ssn_hidden(npc, true));
    CHECK((e->flags & kEntityFlagCarried) != 0 && (e->engine_flags & kEntityFlagCarried) != 0);
}

// disableSSN/enableSSN write Flags bit 28, the bit the vehicle motors test with
// the dead bit (10000002h) before reading their driver.
// [orig: WacCmd_DisableSsn @0x4F7690 — gate @0x4F76D7, `or Flags,10000000h`
// @0x4F76DD; WacCmd_EnableSsn `and Flags,0EFFFFFFFh` @0x4F773D;
// Entity_UpdateVehiclePhysics @0x48B980]
static void test_disable_ssn_flag_bit() {
    ScriptWorld w;
    Entity vehicle;
    vehicle.net_id = 150;
    vehicle.item_id = 2001;
    vehicle.has_item_def = true;
    const EntityHandle h = w.registry.spawn(1, vehicle);
    WacSystem sys;
    CHECK(load_script(w, sys, "if never() then disableSSN(150) store(v1) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    const Entity *e = w.registry.get(h);
    CHECK((e->flags & kEntityFlagScriptDisabled) != 0);
    CHECK((e->engine_flags & kEntityFlagScriptDisabled) != 0);
    CHECK(w.commands.set_ssn_disabled(h, false));
    CHECK(((e->flags | e->engine_flags) & kEntityFlagScriptDisabled) == 0);
}

int main() {
    test_wac_kill_ssn_clears_and_alerts();
    test_wac_kill_ssn_queues_brain_event();
    test_bms_kill_single_pool0();
    test_kill_group_walk_and_return();
    test_gkill_uses_kill_ssn_body();
    test_hide_ssn_flag_bit();
    test_disable_ssn_flag_bit();
    if (failures) {
        std::printf("SCRIPT COMMAND PARITY TESTS FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("script command parity tests passed\n");
    return 0;
}
