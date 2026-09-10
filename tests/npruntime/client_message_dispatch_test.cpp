// Drive the encrypted receive path: reducer-only tests cannot detect a
// connection that silently drops a message before the reducer sees it.
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_world_materializer.h>
#include <runtime/world/world.h>
#include <runtime/world/player_spawn.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (false)

struct FramedClient {
    inmatch::ClientRuntime client{"ControlJoiner"};
    SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
    uint64_t now = 1;
    FramedClient() {
        client.seed_session(0x10203040u, 1u, "CONTROL-CLIENT-SCRK", "CONTROL-SERVER-SCRK",
                1, 0, 2, world::kPlayerInfantryTypeId, 0, 0x00100000u, true);
    }
    bool deliver(const std::vector<ProtocolMessage> &messages) {
        std::vector<uint8_t> body;
        if (!frame_session_packet(server_tx,
                SessionCrypto{"CONTROL-SERVER-SCRK", {}, 1u}, messages, body)) return false;
        const auto datagram = nw_encode_outbound(
                SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
        client.receive(datagram.data(), datagram.size());
        (void)client.Client_ProcessNetworkFrame(now++);
        return true;
    }
};

ProtocolMessage text_command(const std::string &command) {
    std::vector<uint8_t> body(command.begin(), command.end());
    body.push_back(0);
    return make_protocol_message(s2c::TEXT_COMMAND, std::move(body));
}

// The 0x0F waypoint section is conditional on the game type at that exact
// wire position. A later 0x08 must not make an earlier body undecodable.
bool session_config_does_not_reinterpret_earlier_messages() {
    FramedClient wire;
    std::vector<uint8_t> world_load(539, 0);
    world_load[22] = 8; // game_flags: ceasefire
    world_load[535] = 1; // waypoint count, ignored outside the waypoint family
    WorldStateLoad decoded;
    CHECK(decode_world_state_load(world_load.data(), world_load.size(), decoded, false));
    CHECK(!decode_world_state_load(world_load.data(), world_load.size(), decoded, true));
    std::vector<uint8_t> config(51, 0);
    config[12] = 0x20; // fields[3] = Co-op (0x10020)
    config[14] = 1;
    const auto malformed = wire.client.view().malformed_bodies();
    CHECK(wire.deliver({make_protocol_message(s2c::WORLD_STATE_LOAD, world_load),
            make_protocol_message(s2c::SESSION_CONFIG, config)}));
    CHECK(wire.client.state().cease_fire);
    CHECK(wire.client.view().malformed_bodies() == malformed);
    CHECK(wire.client.view().game_type() == 0x10020u);
    return true;
}

bool ceasefire_and_chat_reach_client_in_wire_order() {
    FramedClient wire;
    CHECK(wire.deliver({text_command("SETCEASEFIRE 1")}));
    CHECK(wire.client.state().cease_fire);
    ChatBroadcast chat;
    chat.channel = 2;
    chat.sender_slot = 3;
    chat.text = "Vehicle ready";
    CHECK(wire.deliver({text_command("SETCEASEFIRE 0"),
            make_protocol_message(s2c::CHAT_BROADCAST, encode_chat_broadcast(chat)),
            text_command("SETCEASEFIRE 1")}));
    CHECK(wire.client.state().cease_fire);
    auto lines = wire.client.view().drain_chat_lines();
    CHECK(lines.size() == 1);
    CHECK(lines[0].channel == 2 && lines[0].sender_slot == 3);
    CHECK(lines[0].text == "Vehicle ready");
    CHECK(wire.client.view().drain_chat_lines().empty());
    CHECK(wire.deliver({text_command("SETCEASEFIRE 0")}));
    CHECK(!wire.client.state().cease_fire);
    return true;
}

// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780]
// A repair initializes the selected slot even if its type is unchanged;
// item_type == 0 destroys it. Exercise the native materializer as well.
bool full_entity_repair_replaces_the_native_lifetime() {
    FramedClient wire;
    world::World world;
    world.registry.configure_pool(1, 64);
    replication::ClientWorldMaterializer materializer;
    FullEntitySpawnRecord spawn;
    spawn.slot_id = 0x1007;
    spawn.item_type_id = 1291;
    spawn.item_type = 1;
    spawn.team = 2;
    spawn.entity_flags = 0x76543210u;
    spawn.player_class = 7;
    spawn.minimap_flags = 0x40;
    spawn.entity_name = "repair buggy";
    spawn.pos_x = 0x120000;
    spawn.pos_y = -0x30000;
    spawn.pos_z = 0x80000;
    spawn.heading_hi = 0x4567;
    spawn.pitch_hi = 0xFE12;
    spawn.seat_mask = 1;
    spawn.mount_handles[0] = 0x0002;
    spawn.mount_handle_8 = 0xFFFF;
    spawn.mount_handle_9 = 0xFFFF;
    CHECK(wire.deliver({make_protocol_message(
            s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(spawn))}));
    const auto *row = wire.client.state().find(spawn.slot_id);
    CHECK(row != nullptr);
    CHECK(row->type_id == 1291 && row->team == 2);
    CHECK(row->x == spawn.pos_x && row->y == spawn.pos_y && row->z == spawn.pos_z);
    CHECK(static_cast<uint32_t>(row->heading_bam) == 0x45670000u);
    CHECK(static_cast<uint32_t>(row->pitch_bam) == 0xFE120000u);
    CHECK(row->spawn_mount_handles[0] == 2);
    CHECK(materializer.sync(wire.client.state(), world).spawned.size() == 1);
    auto *native = world.registry.get(world::EntityHandle{spawn.slot_id});
    CHECK(native != nullptr && native->item_id == 1291);
    CHECK(native->owner_connection_id == 0x76543210u && native->player_class == 7);
    const uint64_t first_lifetime = native->registry_spawn_id;
    native->health = 1;
    spawn.pos_x += 0x10000;
    CHECK(wire.deliver({make_protocol_message(
            s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(spawn))}));
    const auto replacement = materializer.sync(wire.client.state(), world);
    CHECK(replacement.retired.size() == 1 && replacement.spawned.size() == 1);
    native = world.registry.get(world::EntityHandle{spawn.slot_id});
    CHECK(native != nullptr && native->registry_spawn_id != first_lifetime);
    CHECK(native->position.x == 19.0f);
    spawn.item_type = 0;
    CHECK(wire.deliver({make_protocol_message(
            s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(spawn))}));
    CHECK(wire.client.state().find(spawn.slot_id) == nullptr);
    CHECK(materializer.sync(wire.client.state(), world).retired.size() == 1);
    CHECK(world.registry.get(world::EntityHandle{spawn.slot_id}) == nullptr);
    return true;
}
// Deletion and reconstruction can arrive in one receive pump. The native
// materializer still owns the first lifetime until the pump finishes.
bool repair_after_empty_slot_has_a_new_generation() {
    FramedClient wire;
    world::World world;
    world.registry.configure_pool(1, 64);
    replication::ClientWorldMaterializer materializer;
    FullEntitySpawnRecord spawn;
    spawn.slot_id = 0x1007;
    spawn.item_type_id = 1291;
    spawn.item_type = 1;
    auto repair = [&] { return make_protocol_message(
            s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(spawn)); };
    CHECK(wire.deliver({repair()}));
    CHECK(materializer.sync(wire.client.state(), world).spawned.size() == 1);
    const auto first = world.registry.get(world::EntityHandle{spawn.slot_id})->registry_spawn_id;
    spawn.item_type = 0;
    const auto empty = repair();
    spawn.item_type = 1;
    CHECK(wire.deliver({empty, repair()}));
    const auto replaced = materializer.sync(wire.client.state(), world);
    CHECK(replaced.retired.size() == 1 && replaced.spawned.size() == 1);
    CHECK(world.registry.get(world::EntityHandle{spawn.slot_id})->registry_spawn_id != first);
    EntityDeathRecord death;
    death.entity_handle = spawn.slot_id;
    CHECK(wire.deliver({make_protocol_message(s2c::ENTITY_DEATH, std::vector<uint8_t>{7, 0x10, 0, 0}), repair()}));
    CHECK(wire.client.drain_entity_deaths().empty());
    // A death after the replacement still belongs to the new lifetime.
    CHECK(wire.deliver({repair(), make_protocol_message(s2c::ENTITY_DEATH, std::vector<uint8_t>{7, 0x10, 0, 0})}));
    CHECK(wire.client.drain_entity_deaths().size() == 1);
    return true;
}

bool listen_client_sound_gates_and_consumption() {
    replication::LoopbackChannel loop;
    inmatch::ClientRuntime client(loop);
    world::World world;
    PlaySoundCommand command;
    command.sound_name = "MP_COMMAND1";
    auto send = [&] {
        loop.host_send(s2c::PLAY_SOUND, encode_play_sound(command));
        client.Client_ProcessNetworkFrame(1);
        client.apply_received_sounds(world);
    };
    send();
    CHECK(world.out.script_sounds.empty()); // retail's multiplayer-session gate
    client.view().set_mp_session(true);
    send();
    CHECK(world.out.script_sounds.size() == 1);
    CHECK(world.out.script_sounds[0].kind == world::ScriptSoundEvent::Kind::Interface);
    client.apply_received_sounds(world);
    CHECK(world.out.script_sounds.size() == 1);
    command.flag = 2;
    send();
    CHECK(world.out.script_sounds.size() == 1 && world.out.slot_sounds.empty());
    loop.host_send(s2c::PLAY_SOUND, {0, 'x'}); // missing terminator
    client.Client_ProcessNetworkFrame(2);
    client.apply_received_sounds(world);
    CHECK(world.out.script_sounds.size() == 1);
    return true;
}

} // namespace

int main() {
    bool ok = session_config_does_not_reinterpret_earlier_messages();
    ok = ceasefire_and_chat_reach_client_in_wire_order() && ok;
    ok = full_entity_repair_replaces_the_native_lifetime() && ok;
    ok = repair_after_empty_slot_has_a_new_generation() && ok;
    ok = listen_client_sound_gates_and_consumption() && ok;
    return ok ? 0 : 1;
}
