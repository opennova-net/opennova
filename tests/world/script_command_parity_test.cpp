// Script command parity: the WAC command bodies and the shared EntityCommands
// primitives they reach, pinned against the witnessed retail handlers (each test
// names its addresses). Scripts compile through the runtime compiler and run on
// a small world; the primitives are also driven directly through EntityCommands.
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

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
    e.item_type_index = 7; // the def row's ordinal, the ItemTypeIndex (+0x1C) gates read
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
// [orig: WacCmd_KillSsn @0x4F1E40 — gate @0x4F1E89, lastAttacker
// @0x4F1EAD, +0x2C0 @0x4F1EB7..0x4F1EBD, callback @0x4F1EC7..0x4F1ED2;
// OrganicClass_HandleEvent phase-1 arm @0x4073BF..0x4073EA]
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
// [orig: WacCmd_KillSsn @0x4F1ED2 -> EntityAI_ProcessGroundStateMachine
// @0x4583C0 event-1 arm, queue @0x45851a]
static void test_wac_kill_ssn_queues_brain_event() {
    ScriptWorld w;
    Entity vehicle;
    vehicle.net_id = 300;
    vehicle.item_id = 2001;
    vehicle.has_item_def = true;
    vehicle.item_type_index = 7;
    vehicle.is_ai_capable = true;
    vehicle.health = 400;
    vehicle.alive = true;
    const EntityHandle h = w.registry.spawn(1, vehicle);
    w.ai.attach(h);
    // An ai_function cveh row routes the class event to the vehicle machine;
    // without a brain-class row the class event runs no machine
    // [orig: g_EntityClassEventCallbackTable cveh @0x813378].
    VehicleTraits ground;
    ground.brain_class = VehicleBrainClass::Ground;
    w.vehicles.traits.set(2001, ground);
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
    victim->item_type_index = 0; // no ItemTypeIndex gate on this path
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
    vehicle.item_type_index = 7;
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

// groupdead/groupalive read the trigger group's live count, the 62-tick
// rescan's word (group 0 forced to zero), not an instant scan of the members.
// [orig: WacCmd_GroupDead @0x4ED1A0 `setle` @0x4ED1B2; WacCmd_GroupAlive
// @0x4ED1C0 `setnle` @0x4ED1D2; EntityPool_RecountLiveByGroup @0x40E8D0]
static void test_group_dead_alive_read_live_count() {
    ScriptWorld w;
    const EntityHandle a = spawn_npc(w, 160, 20);
    spawn_npc(w, 161, 20);
    spawn_npc(w, 162, 0);
    // Before any rescan the count is zero: dead, although two members live.
    CHECK(w.commands.group_dead(20) && !w.commands.group_alive(20));
    w.recount_group_live();
    CHECK(w.commands.group_alive(20) && !w.commands.group_dead(20));
    CHECK(w.commands.group_dead(0) && !w.commands.group_alive(0)); // live[0] = 0
    CHECK(!w.commands.group_dead(64) && !w.commands.group_alive(64));
    // A kill between rescans leaves the count, and the answer, unchanged.
    CHECK(w.commands.wac_kill_ssn(a));
    CHECK(w.commands.group_alive(20));
    WacSystem sys;
    CHECK(load_script(w, sys, "if never() then v1=groupalive(20) v2=groupdead(0) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1);
}

// SSNdead/SSNalive read the Flags dead bit behind the ItemTypeIndex gate, not
// health: a same-execution kill is not dead yet, a raised corpse stays dead.
// SSNexists is the same gate. [orig: WacCmd_SsnDead @0x4F1AC0 — gate @0x4F1B07,
// `and eax,2` @0x4F1B11; WacCmd_SsnAlive @0x4F1B20 — @0x4F1B6D..0x4F1B75;
// WacCmd_SsnExists @0x4F1AB9]
static void test_ssn_dead_alive_read_the_flag() {
    ScriptWorld w;
    const EntityHandle zeroed = spawn_npc(w, 170, 21, 0);  // health 0, not yet flagged
    const EntityHandle raised = spawn_npc(w, 171, 21);     // dead bit, health raised
    Entity *r = w.registry.get(raised);
    r->engine_flags |= kEntityFlagDead;
    r->alive = true;
    Entity itemless;
    itemless.net_id = 172;
    w.registry.spawn(1, itemless);
    (void)zeroed;
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then v1=SSNdead(170) v2=SSNalive(170) v3=SSNdead(171) "
            "v4=SSNalive(171) v5=SSNalive(172) v6=SSNexists(172) v7=SSNexists(171) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(4) == 0);
    CHECK(w.script.vars.get_mission(5) == 0);
    CHECK(w.script.vars.get_mission(6) == 0);
    CHECK(w.script.vars.get_mission(7) == 1);
}

// SSNwounded compares the SIGNED health word with the signed healthMax word
// halved. [orig: WacCmd_SsnWounded @0x4F1B80 — `sar cx,1` @0x4F1BD9,
// `cmp [eax+11Eh],cx` @0x4F1BDC, `setle dl` @0x4F1BE3]
static void test_ssn_wounded_signed_compare() {
    ScriptWorld w;
    const EntityHandle h = spawn_npc(w, 180, 22);
    Entity *e = w.registry.get(h);
    e->health = -5;
    CHECK(w.commands.ssn_wounded(h));       // unsigned would read 65531
    e->health = 40000;                      // the word reads -25536
    CHECK(w.commands.ssn_wounded(h));
    e->health = 51;
    CHECK(!w.commands.ssn_wounded(h));
    e->health = 50;
    CHECK(w.commands.ssn_wounded(h));
}

// ssnguard, ssncspd and ssnrelease gate on the ItemTypeIndex (+0x1C, item_type_index);
// ssnrelease and ssn2ssn detach only on the authority.
// [orig: WacCmd_SsnGuard @0x4F7207; WacCmd_SsnCspd @0x4F74FD;
// WacCmd_SsnRelease @0x4F7465, Entity_DetachFromVehicleIfServer call @0x4F7475;
// WacCmd_SsnToSsn @0x4F73DC]
static void test_ssn_item_gates_and_authority_detach() {
    ScriptWorld w;
    Entity bare;
    bare.net_id = 190;
    const EntityHandle b = w.registry.spawn(0, bare);
    w.ai.attach(b);
    CHECK(!w.commands.set_ssn_guard(b, true));
    CHECK((w.registry.get(b)->engine_flags & kEntityFlagMounted) == 0);
    WacSystem sys;
    CHECK(load_script(w, sys, "if never() then ssncspd(190,20) store(v1) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 0);
    // A non-person item (def type 0) with an ItemTypeIndex is released.
    const EntityHandle rider = spawn_npc(w, 191, 23);
    Entity *occ = w.registry.get(rider);
    occ->item_type = 0;
    occ->mounted = true;
    w.ai.for_handle(rider)->slot.f[37] = 125;
    w.ai.is_authority = false;
    CHECK(w.commands.release_boarding_command(rider));
    CHECK(occ->mounted);                      // a client never detaches
    CHECK(w.ai.for_handle(rider)->slot.f[37] == 0);
}

// SSNHP stores the health word (a 16-bit wrap) and clears the attacker, with no
// gate, and returns 1. A write that leaves a living organic at or below zero
// crosses the death edge once (with the cleared attacker) and keeps the word.
// [orig: WacCmd_SsnHp @0x4F2100 — `mov [eax+11Eh],dx` @0x4F214C,
// `mov [eax+178h],0` @0x4F2153, return 1 @0x4F215D]
static void test_ssn_hp_word_and_attacker() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 200, 30);
    const EntityHandle shooter = spawn_npc(w, 201, 31);
    Entity *e = w.registry.get(npc);
    e->last_attacker = shooter;
    CHECK(w.commands.set_ssn_hp(npc, 40000));
    CHECK(e->health == -25536);             // the word, not 40000
    CHECK(!e->last_attacker.valid());
    CHECK(w.round_sim.deaths.size() == 1);
    if (w.round_sim.deaths.size() == 1) CHECK(!w.round_sim.deaths[0].killer.valid());
    // A later positive write lifts the word only: no alive write, no second death.
    e->engine_flags |= kEntityFlagDead;     // the edge claimed it
    CHECK(w.commands.set_ssn_hp(npc, 50));
    CHECK(e->health == 50 && !e->alive);
    CHECK(w.commands.set_ssn_hp(npc, -7));
    CHECK(e->health == -7);
    CHECK(w.round_sim.deaths.size() == 1);
    // No ItemTypeIndex or ItemDef gate.
    Entity itemless;
    itemless.net_id = 202;
    itemless.health = 5;
    const EntityHandle bare = w.registry.spawn(1, itemless);
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then set(v1,7) SSNHP(202,9000) store(v1) endif\n"));
    run(w, 1);
    CHECK(w.registry.get(bare)->health == 9000);
    CHECK(w.script.vars.get_mission(1) == 1);
}

// SSNADDHP: the ItemDef pointer gates it (not the ItemTypeIndex); the 16-bit
// add floors at 0 and caps at the def healthMax, each returning 1 with the
// attacker kept; a plain add clears the attacker and returns 0.
// [orig: WacCmd_SsnAddHp @0x4F2170 — gate @0x4F21B8..0x4F21BD, `add
// [eax+11Eh],cx` @0x4F21C4, floor @0x4F21D7..0x4F21E5, cap @0x4F21E6..0x4F21FE,
// `mov [eax+178h],0` @0x4F21FF, return 0 @0x4F2209]
static void test_ssn_add_hp_gate_clamp_return() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 210, 32, 50);
    const EntityHandle shooter = spawn_npc(w, 211, 33);
    Entity *e = w.registry.get(npc);
    e->last_attacker = shooter;
    CHECK(w.commands.add_ssn_hp(npc, 20) == 0);
    CHECK(e->health == 70 && !e->last_attacker.valid());
    e->last_attacker = shooter;
    CHECK(w.commands.add_ssn_hp(npc, 45) == 1);   // capped at healthMax
    CHECK(e->health == 100 && e->last_attacker == shooter);
    // No ItemDef: refused and untouched; a missing ItemTypeIndex is no gate.
    e->has_item_def = false;
    CHECK(w.commands.add_ssn_hp(npc, -5) == 0);
    CHECK(e->health == 100);
    e->has_item_def = true;
    e->item_id = 0;
    CHECK(w.commands.add_ssn_hp(npc, -5) == 0);
    CHECK(e->health == 95);
    // Below zero: floored, the attacker kept, the edge crossed once.
    e->last_attacker = shooter;
    CHECK(w.commands.add_ssn_hp(npc, -150) == 1);
    CHECK(e->health == 0 && e->last_attacker == shooter);
    CHECK(w.round_sim.deaths.size() == 1);
    if (w.round_sim.deaths.size() == 1) CHECK(w.round_sim.deaths[0].killer == shooter);
    // The add is 16-bit: 32000 + 1000 reads negative and floors.
    const EntityHandle big = spawn_npc(w, 212, 32, 32000);
    w.registry.get(big)->health_max = 32767;
    CHECK(w.commands.add_ssn_hp(big, 1000) == 1);
    CHECK(w.registry.get(big)->health == 0);
    // The script reads the handler's return.
    spawn_npc(w, 213, 32, 90);
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then SSNADDHP(213,500) store(v1) set(v2,7) SSNADDHP(213,-1) "
            "store(v2) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 0);
}

// GroupHP walks pools 0, 1, 2 (a pool-3 member is skipped) and stores the
// health word of every matching row, touching nothing else (no attacker, no
// alive write), and returns 1. A zeroing write keeps the word; only a living
// organic crosses the death edge. The handler's IDB name is a misnomer.
// [orig: WacCmd_GroupHp @0x4F7B30 — pools @0x4F7B30/@0x4F7B6D/
// @0x4F7B9D, the signed group match @0x4F7B57, the word @0x4F7B64, return 1
// @0x4F7BD0]
static void test_group_hp_pools_word_and_return() {
    ScriptWorld w;
    const EntityHandle a = spawn_npc(w, 220, 34);
    const EntityHandle shooter = spawn_npc(w, 221, 35);
    w.registry.get(a)->last_attacker = shooter;
    Entity vehicle;
    vehicle.net_id = 222;
    vehicle.group_id = 34;
    vehicle.item_id = 2001;
    vehicle.health = 300;
    const EntityHandle v = w.registry.spawn(1, vehicle);
    Entity marker;
    marker.net_id = 223;
    marker.kind = EntityKind::Marker;
    marker.group_id = 34;
    marker.health = 20;
    const EntityHandle m = w.registry.spawn(3, marker);
    const EntityHandle corpse = spawn_npc(w, 224, 34, 0);
    Entity *c = w.registry.get(corpse);
    c->engine_flags |= kEntityFlagDead;
    c->alive = false;
    CHECK(w.commands.set_group_hp(34, 90) == 3);
    CHECK(w.registry.get(a)->health == 90 && w.registry.get(a)->last_attacker == shooter);
    CHECK(w.registry.get(v)->health == 90);
    CHECK(w.registry.get(m)->health == 20);
    CHECK(c->health == 90 && !c->alive);    // a corpse keeps its latch
    CHECK(w.round_sim.deaths.empty());
    CHECK(w.commands.set_group_hp(34, -5) == 3);
    CHECK(w.registry.get(a)->health == -5 && c->health == -5 && w.registry.get(v)->health == -5);
    CHECK(w.round_sim.deaths.size() == 1);
    if (w.round_sim.deaths.size() == 1) CHECK(w.round_sim.deaths[0].victim == a);
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then set(v1,7) GroupHP(34,60) store(v1) set(v2,7) GroupHP(77,60) "
            "store(v2) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1); // no member: still 1
}

// Handlers that return 1 whatever they did; the value folds into the
// accumulator `store` reads.
// [orig: WacAction_Win @0x4ED4AD; WacCmd_Tod @0x4EDC7F; WacCmd_Quake @0x4ED4CE;
// Env_TriggerLightningFlashA @0x4ED50A; Env_TriggerLightningFlashB @0x4ED51A;
// Chat_AddSystemMessage @0x4EDB64; Chat_AddFormattedIntMessage @0x4EDBC0;
// Wac_ConsolDebugMessage @0x4EDBF4; WacCmd_ConsolNumber @0x4EDC50;
// WacCmd_GroupToWaypoint (GtoWP) @0x4ED3E4; WacCmd_GroupSetAccuracy
// @0x4F7C43]
static void test_wac_handler_returns() {
    ScriptWorld w;
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then "
            "set(v1,7) TOD(600) store(v1) "
            "set(v2,7) quake(1) store(v2) "
            "set(v3,7) flash() store(v3) "
            "set(v4,7) farflash() store(v4) "
            "set(v5,7) text(a_text) store(v5) "
            "set(v6,7) consol(a_debug) store(v6) "
            "set(v7,7) text#(n_text,3) store(v7) "
            "set(v8,7) consol#(n_debug,4) store(v8) "
            "set(v9,7) GtoWP(40,1) store(v9) "
            "set(v10,7) Gsetaccuracy(40,50,50) store(v10) "
            "set(v11,7) ptext(p_text) store(v11) "
            "set(v12,7) pconsol(p_debug) store(v12) "
            "set(v13,7) win(0) store(v13) "
            "endif\n"));
    run(w, 1);
    for (int i = 1; i <= 13; ++i) {
        if (w.script.vars.get_mission(i) != 1)
            std::printf("  v%d = %d\n", i, static_cast<int>(w.script.vars.get_mission(i)));
        CHECK(w.script.vars.get_mission(i) == 1);
    }
}

// TOD stores minute * 0x44444 raw; the weather tick wraps the advanced clock
// into the day on the SIGNED word, so a negative minute lands before midnight.
// [orig: WacCmd_Tod @0x4EDC70 — `imul eax,44444h` @0x4EDC74, the store
// @0x4EDC7A; Environment_ComputeTimeOfDayColors @0x57DE40 — the signed wrap
// @0x57DE51..0x57DE78, the store @0x57DE84]
static void test_tod_raw_store_and_signed_day_wrap() {
    ScriptWorld w;
    WeatherTickEvents events;
    w.commands.set_time_of_day_minutes(-60);
    CHECK(w.weather.tod_fixed24 == 0xFF000010u);          // -60 * 0x44444, raw
    w.weather.tick_sim(nullptr, events);
    CHECK(w.weather.tod_fixed24 == 0x17000010u);          // 23:00, not 15:00
    w.commands.set_time_of_day_minutes(1500);            // past a day
    CHECK(w.weather.tod_fixed24 == 1500u * 0x44444u);
    w.weather.tick_sim(nullptr, events);
    CHECK(w.weather.tod_fixed24 == 1500u * 0x44444u - WeatherState::kTodDayFixed24);
}

// holdSSN/unholdSSN set and clear bit 0x2000 of the entity+0x2C dword (our
// cause_flags) behind the ItemTypeIndex gate and leave its other bits; the
// org1 think reads it. [orig: WacCmd_HoldSsn @0x4F7810 — gate @0x4F7857,
// `or [eax+2Ch],2000h` @0x4F785D; WacCmd_UnholdSsn @0x4F7870 — gate
// @0x4F78B7, `and [eax+2Ch],0FFFFDFFFh` @0x4F78BD]
static void test_hold_ssn_cause_bit() {
    ScriptWorld w;
    const EntityHandle npc = spawn_npc(w, 230, 36);
    Entity itemless;
    itemless.net_id = 231;
    const EntityHandle bare = w.registry.spawn(1, itemless);
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then holdSSN(230) store(v1) holdSSN(231) store(v2) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK((w.registry.get(npc)->cause_flags & kCauseFlagScriptHold) != 0);
    CHECK(w.script.vars.get_mission(2) == 0);                // no ItemTypeIndex
    CHECK(w.registry.get(bare)->cause_flags == 0);
    Entity *e = w.registry.get(npc);
    e->cause_flags = 0x800u | kCauseFlagScriptHold;           // a latched critical hit
    CHECK(w.commands.set_ssn_held(npc, false));
    CHECK(e->cause_flags == 0x800u);
    CHECK(w.commands.set_ssn_held(npc, true));
    CHECK(e->cause_flags == (0x800u | kCauseFlagScriptHold));
    CHECK(!w.commands.set_ssn_held(bare, true));
}

namespace {

// The packed handles of the S2C 0x12 removals queued for the joiners, in order.
std::vector<uint16_t> queued_removals(const World &w) {
    std::vector<uint16_t> out;
    for (const auto &event : w.out.entity_events)
        if (const auto *removal = std::get_if<EntityRemoveEvent>(&event))
            out.push_back(removal->handle);
    return out;
}

// A pool-1 row with the PlacedDevice record a placed charge carries.
EntityHandle place_device(World &w, EntityHandle owner, int32_t ammo_index,
                          uint32_t attrib = 0, int32_t item_type_index = 9) {
    Entity row;
    row.item_id = 2101;
    row.has_item_def = true;
    row.item_type_index = item_type_index;
    row.item_attrib = attrib;
    row.health = 10;
    const EntityHandle h = w.registry.spawn(1, row);
    PlacedDevice device;
    device.active = true;
    device.entity = h;
    device.entity_spawn_id = w.registry.get(h)->registry_spawn_id;
    device.owner = owner;
    device.owner_spawn_id = w.registry.get(owner)->registry_spawn_id;
    device.ammo_index = ammo_index;
    w.throwables.devices.push_back(device);
    return h;
}

} // namespace

// WAC removeSSN and Gremove remove through Server_RemoveEntityAndNotify: each
// row queues its S2C 0x12 removal for the joiners, then the shared destroy.
// removeSSN returns 1 for a resolved SSN, Gremove 0.
// [orig: WacCmd_RemoveSsn @0x4F1EE0 (the Server_RemoveEntityAndNotify call
//  @0x4F1F28), return 1 @0x4F1F30; WacCmd_GroupRemove @0x4F1F80 (the call
//  @0x4F1FF2), return 0 @0x4F2006; Server_RemoveEntityAndNotify @0x50A270 —
//  the 0x12 send @0x50A2AC]
static void test_wac_removals_notify_the_joiners() {
    ScriptWorld w;
    const EntityHandle single = spawn_npc(w, 240, 37);
    const EntityHandle first = spawn_npc(w, 241, 37);
    const EntityHandle second = spawn_npc(w, 242, 37);
    const EntityHandle keep = spawn_npc(w, 243, 37);
    w.registry.set_script_group_members(w.registry.intern_group("gone"), {first, second});
    w.out.entity_events.clear();
    WacSystem sys;
    CHECK(load_script(w, sys,
            "if never() then removeSSN(240) store(v1) set(v2,7) Gremove(G_gone) "
            "store(v2) endif\n"));
    run(w, 1);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 0);
    CHECK(w.registry.get(single) == nullptr);
    CHECK(w.registry.get(first) == nullptr && w.registry.get(second) == nullptr);
    CHECK(w.registry.get(keep) != nullptr);
    CHECK(queued_removals(w) ==
          (std::vector<uint16_t>{single.packed, first.packed, second.packed}));
}

// A removed PLAYER's placed devices go before its destroy, each through the
// same notifying removal: the player's 0x12, then every device's. Only pool-1
// rows with an ItemTypeIndex that this player placed, whose def is neither
// PlayerControl nor Eweap and whose placed ammo is the satchel, the claymore
// or the AV mine go; a non-player's removal sweeps nothing.
// [orig: Server_RemoveEntityAndNotify @0x50A270 — the Player test @0x50A2B1,
//  the sweep call @0x50A2BB; Entity_RemovePlacedDevicesByOwner @0x546E00 —
//  the +0x1C gate @0x546E2D, the owner compare @0x546E37, the attrib skips
//  @0x546E46 / @0x546E50, the ammo compares @0x546E68 / @0x546E8B / @0x546EAE]
static void test_player_removal_sweeps_placed_devices() {
    ScriptWorld w;
    w.tables.ammo.entries.resize(4);
    const char *names[] = {"satchel", "claymore", "AV_Mine", "grenadehe"};
    for (int i = 0; i < 4; ++i) {
        w.tables.ammo.entries[static_cast<size_t>(i)].name = names[i];
        w.tables.ammo.entries[static_cast<size_t>(i)].valid = true;
    }
    Entity body;
    body.net_id = 250;
    body.item_id = 1001;
    body.item_type_index = 5;
    body.has_item_def = true;
    body.health = 100;
    body.engine_flags = kEntityFlagPlayer;
    const EntityHandle player = w.registry.spawn(0, body);
    const EntityHandle npc = spawn_npc(w, 251, 38);
    const EntityHandle satchel = place_device(w, player, 0);
    const EntityHandle claymore = place_device(w, player, 1);
    const EntityHandle mine = place_device(w, player, 2);
    const EntityHandle grenade = place_device(w, player, 3);
    const EntityHandle eweap = place_device(w, player, 0, kItemAttribEweap);
    const EntityHandle control = place_device(w, player, 1, kItemAttribPlayerControl);
    const EntityHandle ungated = place_device(w, player, 2, 0, 0);
    const EntityHandle npc_charge = place_device(w, npc, 0);
    w.out.entity_events.clear();
    CHECK(w.commands.server_remove_and_notify(player));
    CHECK(queued_removals(w) == (std::vector<uint16_t>{player.packed, satchel.packed,
                                                        claymore.packed, mine.packed}));
    CHECK(w.registry.get(player) == nullptr);
    CHECK(w.registry.get(satchel) == nullptr && w.registry.get(claymore) == nullptr &&
          w.registry.get(mine) == nullptr);
    CHECK(w.registry.get(grenade) != nullptr && w.registry.get(eweap) != nullptr &&
          w.registry.get(control) != nullptr && w.registry.get(ungated) != nullptr);
    w.out.entity_events.clear();
    CHECK(w.commands.server_remove_and_notify(npc));
    CHECK(queued_removals(w) == (std::vector<uint16_t>{npc.packed}));
    CHECK(w.registry.get(npc_charge) != nullptr);
}

// Destroying a non-person row that holds a refNum destroys every other member
// of that refNum group whose def carries EWeap, each through the same destroy
// and without a notify of its own (the joiners' own destroy of the carrier
// takes them): an addeweap child never outlives its carrier's row, even when
// the carrier's def is EWeap too. A member without EWeap, a row of another
// refNum, and every member of a person's or a def-less row's group stay.
// [orig: Entity_Destroy @0x43E810 — the list removal @0x43E840..0x43E858, the
//  def / def type != 3 / refNum gates @0x43E9B6..0x43E9CA, the call @0x43E9CD;
//  CStreamingMem_Destroy @0x546F30 — member tests @0x546F8A..0x546FA0,
//  Entity_Destroy @0x546FA3]
static void test_destroy_takes_the_eweap_refnum_group() {
    ScriptWorld w;
    auto row = [&](int pool, uint8_t ref, uint32_t attrib, uint8_t type, bool def = true) {
        Entity e;
        e.item_id = 2102;
        e.has_item_def = def;
        e.item_type = type;
        e.item_type_index = 9;
        e.item_attrib = attrib;
        e.ref_num = ref;
        e.health = 10;
        if (type == 3) e.kind = EntityKind::Organic;
        return w.registry.spawn(pool, e);
    };
    const EntityHandle carrier = row(1, 9, kItemAttribPlayerControl | kItemAttribEweap, 1);
    const EntityHandle turret = row(1, 9, kItemAttribEweap, 5);
    const EntityHandle gun = row(1, 9, kItemAttribEweap, 5);
    const EntityHandle plain = row(1, 9, 0, 5);
    const EntityHandle stranger = row(1, 10, kItemAttribEweap, 5);
    Entity rider;
    rider.kind = EntityKind::Organic;
    rider.item_type = 3;
    rider.has_item_def = true;
    rider.health = 100;
    const EntityHandle rider_h = w.registry.spawn(0, rider);
    Entity *gun_row = w.registry.get(gun);
    Seat seat;
    seat.type = SeatType::Gunner;
    seat.occupant = rider_h;
    gun_row->seats.push_back(seat);
    Entity *rider_row = w.registry.get(rider_h);
    rider_row->mounted = true;
    rider_row->mount_target = gun;
    rider_row->mount_seat = 0;
    rider_row->mount_type = SeatType::Gunner;
    w.out.entity_events.clear();
    CHECK(w.commands.server_remove_and_notify(carrier));
    CHECK(queued_removals(w) == (std::vector<uint16_t>{carrier.packed}));
    CHECK(w.registry.get(carrier) == nullptr);
    CHECK(w.registry.get(turret) == nullptr && w.registry.get(gun) == nullptr);
    CHECK(w.registry.get(plain) != nullptr && w.registry.get(stranger) != nullptr);
    CHECK(w.registry.get(plain)->ref_num == 9);
    rider_row = w.registry.get(rider_h);
    CHECK(rider_row != nullptr && !rider_row->mounted);

    const EntityHandle person = row(0, 11, 0, 3);
    const EntityHandle person_gun = row(1, 11, kItemAttribEweap, 5);
    const EntityHandle bare = row(1, 12, 0, 1, /*def=*/false);
    const EntityHandle bare_gun = row(1, 12, kItemAttribEweap, 5);
    CHECK(w.commands.remove_ssn(person));
    CHECK(w.commands.remove_ssn(bare));
    CHECK(w.registry.get(person) == nullptr && w.registry.get(bare) == nullptr);
    CHECK(w.registry.get(person_gun) != nullptr && w.registry.get(bare_gun) != nullptr);
}

// BMS SingleTeleport's target walk keeps the ItemTypeIndex gate inside: a
// gated row carrying the SSN is passed over for the next match, in the same
// pool or the next, and only gated rows mean no teleport.
// [orig: EventAction_TeleportEntityToSpawn @0x43DFC0 — the gates @0x43E02D /
//  @0x43E0DD / @0x43E180 ahead of the DcbId compares @0x43E033 / @0x43E0E3 /
//  @0x43E186]
static void test_teleport_walk_gates_inside() {
    ScriptWorld w;
    Entity marker;
    marker.item_id = kParticleEffectMarkerTypeId;
    marker.has_item_def = true;
    marker.wp_number = 5;
    marker.position = {10.0f, 20.0f, 30.0f};
    w.registry.spawn(3, marker);
    Entity gated;
    gated.item_id = 1001;
    gated.has_item_def = true;
    gated.item_type_index = 0;
    gated.position = {1.0f, 1.0f, 1.0f};
    gated.net_id = 260;
    const EntityHandle gated_first = w.registry.spawn(0, gated);
    Entity live = gated;
    live.item_type_index = 4;
    const EntityHandle live_second = w.registry.spawn(0, live);
    CHECK(w.commands.teleport_ssn_to_marker(260, 5));
    CHECK(w.registry.get(gated_first)->position.x == 1.0f);
    CHECK(w.registry.get(live_second)->position.x == 10.0f);
    gated.net_id = 261;
    const EntityHandle gated_pool0 = w.registry.spawn(0, gated);
    live.net_id = 261;
    const EntityHandle live_pool1 = w.registry.spawn(1, live);
    CHECK(w.commands.teleport_ssn_to_marker(261, 5));
    CHECK(w.registry.get(gated_pool0)->position.x == 1.0f);
    CHECK(w.registry.get(live_pool1)->position.x == 10.0f);
    gated.net_id = 262;
    const EntityHandle only_gated = w.registry.spawn(2, gated);
    CHECK(!w.commands.teleport_ssn_to_marker(262, 5));
    CHECK(w.registry.get(only_gated)->position.x == 1.0f);
}

int main() {
    test_wac_kill_ssn_clears_and_alerts();
    test_wac_kill_ssn_queues_brain_event();
    test_bms_kill_single_pool0();
    test_kill_group_walk_and_return();
    test_gkill_uses_kill_ssn_body();
    test_hide_ssn_flag_bit();
    test_disable_ssn_flag_bit();
    test_group_dead_alive_read_live_count();
    test_ssn_dead_alive_read_the_flag();
    test_ssn_wounded_signed_compare();
    test_ssn_item_gates_and_authority_detach();
    test_ssn_hp_word_and_attacker();
    test_ssn_add_hp_gate_clamp_return();
    test_group_hp_pools_word_and_return();
    test_wac_handler_returns();
    test_tod_raw_store_and_signed_day_wrap();
    test_hold_ssn_cause_bit();
    test_wac_removals_notify_the_joiners();
    test_player_removal_sweeps_placed_devices();
    test_destroy_takes_the_eweap_refnum_group();
    test_teleport_walk_gates_inside();
    if (failures) {
        std::printf("SCRIPT COMMAND PARITY TESTS FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("script command parity tests passed\n");
    return 0;
}
