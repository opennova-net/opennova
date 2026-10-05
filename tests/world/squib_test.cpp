#include <runtime/world/world.h>
#include <runtime/world/collision.h>
#include <runtime/mission/item_traits.h>
#include <formats/def/def.h>
#include <memory>
#include <cstdio>
using namespace opennova::world;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n",__LINE__,#c); return 1; } } while(0)
// The move-function row selects only the motor; the +0x2C8 spread (and the
// +0x160/+0x26C ammo words) come from the ai_function class row's init slot.
// [orig: sub_448CE0 @0x448CE0 via def+0x148; the move table row @0x82AC88]
static int test_squib_init_keys_on_ai_function() {
    const char text[]="begin AiOnly\r\n id 106034\r\n type marker\r\n ai_function squib\r\n"
        " move_function nade\r\n sqb_error 20\r\n end\r\n"
        "begin MoveOnly\r\n id 106035\r\n type marker\r\n ai_function null\r\n"
        " move_function squib\r\n sqb_error 20\r\n end\r\n";
    opennova::def::DefItemsFile definitions{};
    CHECK(opennova::def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(text),
            sizeof(text)-1,&definitions)==0);
    CHECK(definitions.count==2);
    auto heap=std::make_unique<World>();
    auto &w=*heap;
    w.registry.configure_pool(3,4);
    Entity marker; marker.kind=EntityKind::Marker;
    marker.item_id=6034;
    const auto ai_only=w.registry.spawn(3,marker);
    marker.item_id=6035;
    const auto move_only=w.registry.spawn(3,marker);
    opennova::mission::resolve_item_traits(w,definitions,{});
    CHECK(w.registry.get(ai_only)->has_item_def && w.registry.get(move_only)->has_item_def);
    CHECK(!w.registry.get(ai_only)->squib.motor);
    CHECK(w.registry.get(ai_only)->squib.spread_q16==20*65536);
    CHECK(w.registry.get(move_only)->squib.motor);
    CHECK(w.registry.get(move_only)->squib.spread_q16==0);
    opennova::def::def_free_items(&definitions);
    return 0;
}
int main() {
    if (test_squib_init_keys_on_ai_function()!=0) return 1;
    const char text[]="begin Squib\r\n id 106031\r\n type marker\r\n ai_function squib\r\n"
        " move_function squib\r\n sqb_rate 6\r\n sqb_distance 1.25\r\n sqb_error 20\r\n"
        " ammo_marker3 BALL\r\n end\r\n";
    opennova::def::DefItemsFile definitions{};
    CHECK(opennova::def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(text),
            sizeof(text)-1,&definitions)==0);
    CHECK(definitions.count==1);
    CHECK(definitions.entries[0].deathtime_ticks==10);
    CHECK(definitions.entries[0].clipsize==81920);
    CHECK(definitions.entries[0].door_type==20*65536);
    opennova::def::def_free_items(&definitions);
    auto heap=std::make_unique<World>();
    auto &w=*heap;
    w.registry.configure_pool(0,4); w.registry.configure_pool(1,4); w.registry.configure_pool(3,4);
    w.tables.ammo.entries.resize(2);
    auto &ammo=w.tables.ammo.entries[1]; ammo.valid=true; ammo.name="BALL";
    Entity source; source.kind=EntityKind::Marker; source.item_id=106031; source.net_id=10;
    source.has_item_def=true; source.script_next_ssn=11; source.deathtime_ticks=2;
    source.squib.motor=true; source.equipped_adm_index=0;
    source.position={0,0,5};
    ItemDeathTraits traits; traits.death_class=ItemDeathClass::kSquib;
    traits.squib_distance_q16=65536; traits.squib_ammo="BALL";
    w.tables.item_death_traits.set(source.item_id,traits);
    const auto h=w.registry.spawn(3,source);
    Entity end=source; end.item_id=106030; end.net_id=11; end.position={2,0,5};
    end.script_next_ssn=12; end.squib.motor=false;
    const auto target=w.registry.spawn(3,end);
    ItemDeathTraits null; null.death_class=ItemDeathClass::kNull;
    w.tables.item_death_traits.set(end.item_id,null);
    end.net_id=12;
    const auto terminal=w.registry.spawn(3,end);
    w.logic_tick=1;
    CHECK(w.commands.kill_ssn(uint16_t(10))); // The real pool-3 phase-4 trigger.
    CHECK(w.registry.get(h)->class_think_ticks==0x7FFFFFF);
    const auto clone=EntityHandle::make(1,0);
    auto *s=w.registry.get(clone);
    CHECK(s && s->death_tick==1 && s->squib.remaining==2);
    CHECK(s->squib.next_ssn==12 && s->squib.step.x==65536);
    CHECK(s->squib.origin.x==0 && s->squib.origin.z==5*65536);
    CHECK(s->squib.direction.z < -65530); // Nearly vertical, with retail BAM constant.
    CHECK(s->squib.center.z > 8*65536); // No overlap with origin X.
    const auto seed=w.throwables.fan_prng_state;
    w.logic_tick=4; tick_squib(w,*s);
    CHECK(s->squib.remaining==1 && s->squib.last_tick==3 && s->squib.origin.x==65536);
    w.logic_tick=5; tick_squib(w,*s);
    CHECK(w.registry.get(clone) && s->squib.remaining==0);
    w.logic_tick=7; tick_squib(w,*s); // N+1 sweep; then kill next_ssn.
    CHECK(!w.registry.get(clone) && w.registry.get(terminal)->health==0);
    CHECK(w.registry.get(target)->health>0 && w.throwables.fan_prng_state==seed);
    CHECK(w.registry.get(h)->squib.origin.x==0); // Source stays parked.
    w.registry.get(h)->script_next_ssn=999;
    CHECK(w.commands.kill_ssn(uint16_t(10)) && !w.registry.get(clone));
    // Whole-person sphere query, no posed bones, and same-team near-source exemption.
    CollisionWorld collision;
    Entity person; person.kind=EntityKind::Organic; person.item_id=1; person.item_type=3;
    person.has_item_def=true; person.bound_radius=1; person.position={2,0,5}; person.team=0;
    const auto near=w.registry.spawn(0,person);
    auto hit=collision.trace_squib(w,h,{0,0,5*65536},{10*65536,0,5*65536});
    CHECK(!hit.hit());
    w.registry.get(near)->position.x=3;
    hit=collision.trace_squib(w,h,{0,0,5*65536},{10*65536,0,5*65536});
    CHECK(hit.hit_class==ProjectileHitClass::Person && hit.distance_q16==3*65536);
    CHECK(hit.surface_type==0 && hit.bone_index==0 && hit.hit_zone==0);
    // The separate damage-ammo register, not the presentation override, owns damage.
    ammo.weight_in_grains=100; ammo.velocity=620; ammo.penetration_impact=10;
    w.registry.get(near)->health=100; w.registry.get(near)->health_max=100;
    LiveRound round; round.ammo_index=1; round.pos={0,0,5};
    FixedVec3 velocity{10*65536,0,0};
    w.round_sim.process_damage_hit(w,round,hit,velocity);
    CHECK(w.registry.get(near)->health<100 && !w.round_sim.hits.empty());
    std::puts("squib PASS"); return 0;
}
