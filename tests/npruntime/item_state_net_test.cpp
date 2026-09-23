// Class-state and expiry notifications through the actual server tick and fan.
// [orig: Server_SendEntityStatePacket @0x509D70; Server_RemoveEntityAndNotify @0x50A270]
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/client_runtime.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_message_id.h>
#include <memory>
#include <cstdio>
#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

std::vector<std::vector<uint8_t>> take(ns::LoopbackChannel &channel, uint8_t tag) {
    std::vector<std::vector<uint8_t>> bodies;
    ns::Datagram row;
    while (channel.client_recv(row)) if (row.tag == tag) bodies.push_back(std::move(row.body));
    return bodies;
}

int main() {
    auto heap = std::make_unique<w::World>();
    auto &world = *heap;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);
    world.rules.mp_session = true;
    w::Entity person;
    person.kind = w::EntityKind::Organic;
    person.flags = person.engine_flags = w::kEntityFlagPlayer;
    const auto alive = world.registry.spawn(0, person);
    person.health = 0;
    person.alive = false;
    person.damage_state = -1;
    person.flags |= w::kEntityFlagDead;
    person.engine_flags |= w::kEntityFlagDead;
    const auto dead = world.registry.spawn(0, person);
    const auto local = world.registry.spawn(0, person);
    ns::LoopbackChannel alive_wire, dead_wire, local_wire, waiting_wire;
    inmatch::NapiNPServerCtx ctx;
    ctx.world = &world;
    ctx.is_authority = ctx.is_in_session = 1;
    auto &connections = ctx.np_protocol.connection_list;
    connections.push_back(conn_fixture::make_conn(1, 1, &alive_wire, ns::TransportMode::Client, alive, true));
    connections.push_back(conn_fixture::make_conn(2, 1, &dead_wire, ns::TransportMode::Client, dead, true));
    connections.push_back(conn_fixture::make_conn(3, 2, &local_wire, ns::TransportMode::Loopback, local, true));
    connections.push_back(conn_fixture::make_conn(4, 1, &waiting_wire, ns::TransportMode::Client, alive, false));
    for (size_t i = 0; i < connections.size(); ++i) {
        connections[i].reply.player_slot = static_cast<uint8_t>(i);
        connections[i].admission_stage = inmatch::GameAdmissionStage::Complete;
        connections[i].link.last_deploy_tick_valid = true;
    }
    w::Entity item;
    item.kind = w::EntityKind::Item;
    item.has_item_def = true;
    item.item_type_index = 7; // the def row's ordinal the +0x1C kill gate reads
    item.item_id = 10;
    item.health = 0;
    const auto handle = world.registry.spawn(1, item);
    w::ItemDeathTraits traits;
    traits.death_class = w::ItemDeathClass::kGnrl;
    world.tables.item_death_traits.set(10, traits);
    inmatch::Server_TickUpdate(ctx);
    const std::vector<uint8_t> expected = {uint8_t(handle.packed), uint8_t(handle.packed >> 8), 0, 0};
    CHECK(take(alive_wire, s2c::KILL_SYNC) == std::vector<std::vector<uint8_t>>{expected});
    CHECK(take(dead_wire, s2c::KILL_SYNC) == std::vector<std::vector<uint8_t>>{expected});
    CHECK(take(local_wire, s2c::KILL_SYNC).empty());
    CHECK(take(waiting_wire, s2c::KILL_SYNC).empty());
    CHECK(world.out.entity_events.empty());
    // Palm/tower completion uses signed section -1 in this same wire envelope.
    w::emit_item_state(world, *world.registry.get(handle), -1);
    inmatch::Server_TickUpdate(ctx);
    auto states = take(dead_wire, s2c::KILL_SYNC);
    CHECK(states.size() == 1 && states[0][2] == 255 && states[0][3] == 255);
    ns::ClientReplicaPipeline replica;
    replica.apply(s2c::KILL_SYNC, states[0]);
    const auto deaths = replica.drain_effect_commands();
    CHECK(deaths.size() == 1 && std::get<ns::EntityDeathEvent>(deaths[0]).item_state && std::get<ns::EntityDeathEvent>(deaths[0]).hit_section == -1);
    CHECK(std::get<ns::EntityDeathEvent>(deaths[0]).death_anim_state_id == 0);
    item.item_id = 11;
    const auto barrel = world.registry.spawn(1, item);
    traits.death_class = w::ItemDeathClass::kBarrel;
    world.tables.item_death_traits.set(11, traits);
    for (int tick = 0; tick <= 10; ++tick) inmatch::Server_TickUpdate(ctx);
    CHECK(world.registry.get(barrel) == nullptr);
    const std::vector<uint8_t> removed = {uint8_t(barrel.packed), uint8_t(barrel.packed >> 8)};
    std::vector<ns::Datagram> packets;
    ns::Datagram packet;
    while (dead_wire.client_recv(packet))
        if (packet.tag == s2c::EXPLOSION_EFFECT || packet.tag == s2c::ENTITY_REMOVE)
            packets.push_back(std::move(packet));
    CHECK(packets.size() == 2);
    CHECK(packets[0].tag == s2c::EXPLOSION_EFFECT && packets[1].tag == s2c::ENTITY_REMOVE);
    CHECK(packets[1].body == removed);
    const std::vector<uint8_t> explosion = {0,10,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,64};
    CHECK(packets[0].body == explosion);
    ExplosionEffectRecord decoded;
    CHECK(decode_explosion_effect(explosion.data(),explosion.size(),decoded));
    CHECK(encode_explosion_effect(decoded) == explosion);
    const uint8_t short_body[] = {0,60,0x34};
    CHECK(decode_explosion_effect(short_body,sizeof(short_body),decoded));
    CHECK(decoded.count == 60 && decoded.source == 0 && decoded.x == 0 && decoded.heading == 0);
    CHECK(decode_explosion_effect(nullptr,0,decoded));
    CHECK(decoded.type == 0 && decoded.source == 0);
    inmatch::ClientRuntime joiner("effects");
    auto joiner_world = std::make_unique<w::World>();
    joiner_world->rules.logic_authority = false;
    joiner_world->rules.mp_session = true;
    joiner_world->registry.configure_pool(1,8);
    item.item_id = 11;
    item.health = 100;
    const auto receiver = joiner_world->registry.spawn(1,item);
    traits.death_class = w::ItemDeathClass::kGnrl;
    traits.particledeath = "ClassDeath";
    joiner_world->tables.item_death_traits.set(11,traits);
    joiner.view().apply(s2c::KILL_SYNC,{uint8_t(receiver.packed),uint8_t(receiver.packed>>8),0,0});
    joiner.view().apply(s2c::EXPLOSION_EFFECT,explosion);
    joiner.apply_received_effects(*joiner_world);
    CHECK(joiner_world->out.destruction.effects.size() == 2);
    CHECK(joiner_world->out.destruction.effects[0].effect == "ClassDeath");
    CHECK(joiner_world->out.destruction.effects[1].effect == "Effect_AirExp");
    CHECK(joiner_world->out.entity_events.empty());
    CHECK(take(local_wire, s2c::ENTITY_REMOVE).empty());
    CHECK(take(waiting_wire, s2c::ENTITY_REMOVE).empty());
    CHECK(world.out.entity_events.empty());
    return 0;
}
