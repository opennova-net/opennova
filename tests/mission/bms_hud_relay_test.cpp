// The BMS HUD relays: the objective notification BMS actions 35/36 show, the
// SubGoalWon/SubGoalLost announcement relay, and the host fan that carries
// both to the joiners as S2C 0x3F in the order the sim produced them.
// [orig: HUD_ShowObjectiveNotification @0x5ba2e0; GameMsg_AddChatLineAndRelay
//  @0x5ba170; Server_BroadcastEntityActionPacket @0x5080d0]
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/world/world.h>

using namespace opennova;
using world::World;
namespace nm = opennova::inmatch;
namespace repl = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void configure_pools(World &w) {
    for (int pool = 0; pool <= 3; ++pool) w.registry.configure_pool(pool, 8);
}

static world::EntityHandle spawn_row(World &w, int pool, uint16_t ssn) {
    world::Entity e{};
    e.net_id = ssn;
    e.item_id = 1213;
    e.health = 100;
    return w.registry.spawn(pool, e);
}

static bms::Action action(bms::ActionType type, int32_t p1, int32_t p2 = 0) {
    bms::Action a{};
    a.action_type = type;
    a.param1 = p1;
    a.param2 = p2;
    return a;
}

// ShowWinSubgoal/ShowLoseSubgoal: an active notice posts the "objective" chat
// lines on a client or outside a session, the authority queues the S2C 0x3F
// kind-0 relay, and a shown win objective plays NEW_GOAL at the local player.
// A hidden objective (param2 0) only clears its mask bit.
// [orig: EventAction_Dispatch case 35 @0x4546af / case 36 @0x454724;
//  HUD_ShowObjectiveNotification @0x5ba2e0 — @0x5ba2f3, @0x5ba382/@0x5ba38b,
//  @0x5ba3ca; NEW_GOAL @0x4546e7..0x45470c]
static void test_objective_notification() {
    auto box = std::make_unique<World>();
    World &w = *box;
    w.cached.humans = 1;
    configure_pools(w);
    const world::EntityHandle player = spawn_row(w, 0, 1);
    w.cached.local_player = player;
    w.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    w.script.subgoals.win_text_ids[2] = 21;
    w.script.subgoals.lose_text_ids[3] = 33;
    const auto count_new_goal = [&]() {
        int n = 0;
        for (const auto &ready : w.out.fire_sounds.drain())
            if (ready.set_name == "NEW_GOAL") ++n;
        return n;
    };
    const auto &relays = w.out.hud_relays;

    sys.dispatch_action_for_test(w, action(bms::ActionType::ShowWinSubgoal, 2, 1));
    CHECK((w.script.subgoals.show_win & (1u << 2)) != 0);
    CHECK(w.out.effects.count("objective") == 1);
    CHECK(relays.size() == 1 && relays[0].kind == 0 && relays[0].slot == 2 &&
          relays[0].is_win == 1 && relays[0].is_active == 1 && relays[0].flag == 1);
    CHECK(count_new_goal() == 1);

    sys.dispatch_action_for_test(w, action(bms::ActionType::ShowLoseSubgoal, 3, 1));
    CHECK(w.out.effects.count("objective") == 2);
    CHECK(relays.size() == 2 && relays[1].flag == 0 && relays[1].is_win == 0);
    CHECK(count_new_goal() == 0);

    // Hiding posts, relays and plays nothing.
    sys.dispatch_action_for_test(w, action(bms::ActionType::ShowWinSubgoal, 2, 0));
    CHECK((w.script.subgoals.show_win & (1u << 2)) == 0);
    CHECK(w.out.effects.count("objective") == 2);
    CHECK(relays.size() == 2);
    CHECK(count_new_goal() == 0);

    // A dedicated host (in session, no client) relays but posts nothing.
    w.rules.mp_session = true;
    w.rules.mp_session_peer = false;
    sys.dispatch_action_for_test(w, action(bms::ActionType::ShowLoseSubgoal, 3, 1));
    CHECK(w.out.effects.count("objective") == 2);
    CHECK(relays.size() == 3);

    // A joiner (in session, a client, not the authority) posts and relays
    // nothing further.
    w.rules.mp_session_peer = true;
    w.rules.logic_authority = false;
    w.show_objective_notification(3, 0, 1, 0);
    CHECK(w.out.effects.count("objective") == 3);
    CHECK(relays.size() == 3);
}

// SubGoalWon/SubGoalLost announce through GameMsg_AddChatLineAndRelay: the
// authority in a session relays the STRWINMSG%03i key with team 1 and the
// STRLOSEMSG%03i key with team 0 (S2C 0x3F kind 1) while the round runs.
// Outside a session, off the authority, after the round, or for an already
// won slot, nothing relays.
// [orig: EventAction_Dispatch case 14 @0x454500 — the gate @0x45453a, the call
//  @0x454578; case 15 @0x4545e0 — the gate @0x4545f0, the call @0x454632;
//  GameMsg_AddChatLineAndRelay @0x5ba170 — @0x5ba19f/@0x5ba1a8]
static void test_subgoal_announcements_relay_their_keys() {
    auto box = std::make_unique<World>();
    World &w = *box;
    w.cached.humans = 1;
    configure_pools(w);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    w.script.subgoals.win_text_ids[2] = 21;
    w.script.subgoals.lose_text_ids[3] = 33;
    const auto &relays = w.out.hud_relays;

    // Outside a session the lines stay local.
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalLost, 3));
    CHECK(w.out.effects.count("subgoal_lost") == 1);
    CHECK(relays.empty());

    w.rules.mp_session = true;
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalWon, 2));
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalLost, 3));
    CHECK(relays.size() == 2);
    CHECK(relays.size() == 2 && relays[0].kind == 1 && relays[0].team == 1 &&
          relays[0].key == "STRWINMSG021");
    CHECK(relays.size() == 2 && relays[1].kind == 1 && relays[1].team == 0 &&
          relays[1].key == "STRLOSEMSG033");

    // An already won slot skips the whole case.
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalWon, 2));
    CHECK(w.out.effects.count("subgoal_won") == 1);
    CHECK(relays.size() == 2);

    // Off the authority nothing relays.
    w.rules.logic_authority = false;
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalLost, 3));
    CHECK(relays.size() == 2);
    w.rules.logic_authority = true;

    // Once the round is over the announcement neither posts nor relays.
    w.process_round_end(1);
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalLost, 3));
    CHECK(relays.size() == 2);
}

// The host fan sends every queued relay to each active remote connection as
// S2C 0x3F, reliable, in the order the sim queued them, never to the listen
// host's loopback, and releases the queue.
// [orig: Server_BroadcastEntityActionPacket @0x5080D0 — send_mask 90h
//  @0x50818f, the NapiNPServer_SendFiltered call @0x508199;
//  NapiNPServer_SendFiltered @0x4C87E0 — the 0x90 host exclusion @0x4c88f6]
static void test_host_fan_sends_the_relays_in_order() {
    auto box = std::make_unique<World>();
    World &w = *box;
    w.registry.configure_pool(0, 16);
    w.match.configure({});
    nm::NapiNPServerCtx ctx;
    nm::set_connection_mode(ctx, nm::ConnectionMode::HostOnly);
    nm::GameConfig config;
    config.max_players = 4;
    nm::create_session(ctx, config, {}, nullptr);
    ctx.world = &w;
    repl::UdpSessionTransport remote{repl::UdpSessionTransport::Role::Host};
    repl::LoopbackChannel loopback;
    const auto add_connection = [&](repl::ISessionTransport *transport,
                                    repl::TransportMode mode, uint16_t ssn) {
        const world::EntityHandle owned = spawn_row(w, 0, ssn);
        nm::NapiNPConnection conn;
        conn.type = mode == repl::TransportMode::Loopback
                ? nm::NapiNPConnection::kTypeClientSide : nm::NapiNPConnection::kTypeServerSide;
        conn.phase = nm::ConnectionPhase::PlayerAdded;
        conn.burst.spawned = true;
        conn.link.mode = mode;
        conn.link.transport = transport;
        conn.link.owned_entity = owned;
        conn.link.owned_entity_spawn_id = w.registry.get(owned)->registry_spawn_id;
        ctx.np_protocol.connection_list.push_back(std::move(conn));
    };
    add_connection(&remote, repl::TransportMode::Client, 11);
    add_connection(&loopback, repl::TransportMode::Loopback, 12);

    w.rules.mp_session = true;
    w.relay_mission_text_chat(1, "STRWINMSG021");
    w.show_objective_notification(2, 1, 1, 1);
    CHECK(w.out.hud_relays.size() == 2);
    nm::Server_TickUpdate(ctx);
    CHECK(w.out.hud_relays.empty());

    std::vector<ObjectiveNotification> sent;
    repl::Datagram datagram;
    while (remote.pop_outbound(datagram)) {
        if (datagram.tag != s2c::OBJECTIVE_NOTIFICATION) continue;
        CHECK(datagram.reliable);
        ObjectiveNotification notice;
        size_t consumed = 0;
        CHECK(decode_objective_notification(
                datagram.body.data(), datagram.body.size(), notice, consumed));
        CHECK(consumed == datagram.body.size());
        sent.push_back(notice);
    }
    CHECK(sent.size() == 2);
    CHECK(sent.size() == 2 && sent[0].kind == 1 && sent[0].team == 1 &&
          sent[0].key == "STRWINMSG021");
    CHECK(sent.size() == 2 && sent[1].kind == 0 && sent[1].slot == 2 &&
          sent[1].is_win == 1 && sent[1].is_active == 1 && sent[1].flag == 1);
    int to_listen_host = 0;
    while (loopback.client_recv(datagram))
        if (datagram.tag == s2c::OBJECTIVE_NOTIFICATION) ++to_listen_host;
    CHECK(to_listen_host == 0);
}

int main() {
    test_objective_notification();
    test_subgoal_announcements_relay_their_keys();
    test_host_fan_sends_the_relays_in_order();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bms_hud_relay: all passed\n");
    return 0;
}
