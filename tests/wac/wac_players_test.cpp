#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <net/npwire/session_hello.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::wac;
namespace nm = opennova::inmatch;
namespace repl = opennova::replication;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Fixture() {
        world.registry.configure_pool(0, 16);
        world.match.configure({});
    }
    EntityHandle spawn(uint16_t ssn, uint8_t slot, bool registered = true) {
        Entity entity;
        entity.net_id = ssn;
        entity.item_id = 1;
        entity.has_item_def = true;
        entity.health = 100;
        const auto handle = world.registry.spawn(0, entity);
        if (registered) world.match.upsert_player({handle, slot, "Player", {}});
        return handle;
    }
    void run(const std::string &source) {
        const auto program = compile_source(source, {});
        CHECK(program.ok());
        WacVm vm;
        vm.load(program);
        vm.execute(world);
    }
    std::string select(EntityHandle handle) const {
        return "item=" + std::to_string(handle.packed) + "\n";
    }
    int32_t value(int index) const { return world.script.vars.get_mission(index); }
    int32_t stat(EntityHandle handle, size_t field) const {
        const MatchPlayer *player = world.match.player(handle);
        CHECK(player != nullptr);
        return player ? player->stats[field] : 0;
    }
};

static void test_registered_slot_predicates_and_byte_bank() {
    Fixture f;
    const auto first = f.spawn(10, 0);
    const auto second = f.spawn(11, 1);
    const auto npc = f.spawn(12, 2, false);
    f.world.cached.local_player = first;
    // These handlers do not require ItemDef, the Player flag or positive HP.
    f.world.registry.get(first)->has_item_def = false;
    f.world.registry.get(first)->item_id = 0;
    f.world.registry.get(first)->health = 0;
    f.world.match.player(first)->stats[MatchStats::kEnemyKills] = 3;
    f.world.registry.get(npc)->flags |= kEntityFlagPlayer;
    f.run("piskills(3) store(v1)\npiskills(4) store(v2)\n"
          "pisvar(0) store(v3)\npsetvar(0) store(v4)\n"
          "psetvar(16) store(v5)\npisvar(16) store(v6)\n"
          "psetvar(17) store(v7)\npsetvar(-1) store(v8)\n"
          "pisvar(-1) store(v9)\n" +
          f.select(second) + "pisvar(0) store(v10)\npsetvar(8)\n" +
          f.select(npc) + "psetvar(0) store(v11)\npiskills(-1) store(v12)\n");
    CHECK(f.value(1) == 1 && f.value(2) == 0 && f.value(3) == 0);
    CHECK(f.value(4) == 1 && f.value(5) == 1 && f.value(6) == 1);
    CHECK(f.value(7) == 0 && f.value(8) == 0 && f.value(9) == 0);
    CHECK(f.value(10) == 0 && f.value(11) == 0 && f.value(12) == 0);
    CHECK(f.world.match.player(first)->script_vars[8] == 0);
    CHECK(f.world.match.player(second)->script_vars[8] == 1);
    const auto baseline = f.world.snapshot();
    f.run("psetvar(8)\n");
    CHECK(f.world.match.player(first)->script_vars[8] == 1);
    f.world.restore(baseline);
    CHECK(f.world.match.player(first)->script_vars[8] == 0);
    CHECK(f.world.match.player(first)->script_vars[16] == 1);
    f.world.match.upsert_player({first, 0, "Renamed", {}});
    CHECK(f.world.match.player(first)->script_vars[16] == 1);
    f.world.match.upsert_player({npc, 0, "Replacement", {}});
    CHECK(f.world.match.player(first) == nullptr);
    CHECK(f.world.match.player(npc)->script_vars[16] == 0);
    f.world.match.player(npc)->stats[MatchStats::kEnemyKills] = -3;
    f.run(f.select(npc) + "piskills(-3) store(v13)\npiskills(-2) store(v14)\n"
          "item=65535\npsetvar(0) store(v15)\npiskills(-2147483648) store(v16)\n");
    CHECK(f.value(13) == 1 && f.value(14) == 0);
    CHECK(f.value(15) == 0 && f.value(16) == 0);
    f.run(f.select(npc) + "pisgold() store(v17)\n" +
          f.select(second) + "pisgold() store(v18)\nitem=65535\npisgold() store(v19)\n");
    CHECK(f.value(17) == 0 && f.value(18) == 0 && f.value(19) == 0);
    CHECK(f.world.diagnostics.empty());
}

static void test_experience_raw_scoring_validation_and_retry() {
    Fixture f;
    const auto actor = f.spawn(10, 0);
    const auto npc = f.spawn(11, 1, false);
    const auto no_def = f.spawn(12, 2);
    f.world.registry.get(actor)->health = 0; // dead slots retain statistics
    f.world.registry.get(actor)->item_id = 0; // pointer validity, not numeric ItemDef id
    f.world.registry.get(no_def)->has_item_def = false;
    f.run("AddExp(10,7) store(v1)\nAddExp(10,0) store(v2)\n"
          "AddExp(11,100) store(v3)\nAddExp(12,100) store(v4)\n"
          "AddExp(999,100) store(v5)\n");
    CHECK(f.value(1) == 1 && f.value(2) == 0 && f.value(3) == 0);
    CHECK(f.value(4) == 0 && f.value(5) == 0);
    CHECK(f.stat(actor, MatchStats::kPoints) == 7);
    CHECK(f.stat(no_def, MatchStats::kPoints) == 0);
    CHECK(f.world.match.player(npc) == nullptr);
    for (uint8_t team = 0; team < 5; ++team)
        CHECK(f.world.match.team_stats(team)[MatchStats::kPoints] == 0);
    const auto baseline = f.world.snapshot();
    f.run("AddExp(10,-10) store(v6)\n");
    CHECK(f.value(6) == 1 && f.stat(actor, MatchStats::kPoints) == -3);
    f.world.restore(baseline);
    CHECK(f.stat(actor, MatchStats::kPoints) == 7);
    f.world.match.player(actor)->stats[MatchStats::kPoints] =
            std::numeric_limits<int32_t>::max();
    f.run("AddExp(10,1) store(v7)\n");
    CHECK(f.value(7) == 1);
    CHECK(f.stat(actor, MatchStats::kPoints) == std::numeric_limits<int32_t>::min());
    CHECK(f.world.diagnostics.empty());
}

static void test_experience_both_occupant_links_and_zero_shares() {
    Fixture f;
    const auto first = f.spawn(10, 0);
    const auto middle = f.spawn(11, 1);
    const auto last = f.spawn(12, 2);
    f.world.registry.get(first)->primary_occupant = middle;
    f.world.registry.get(middle)->primary_occupant = last;
    // Linked recipients need only a registered slot; AddExp's ItemDef gate
    // applies only to its explicitly addressed source.
    f.world.registry.get(middle)->has_item_def = false;
    f.world.registry.get(last)->has_item_def = false;
    f.run("AddExp(10,64)\n");
    CHECK(f.stat(first, MatchStats::kPoints) == 64);
    CHECK(f.stat(middle, MatchStats::kPoints) == 32);
    CHECK(f.stat(last, MatchStats::kPoints) == 32);
    CHECK(f.stat(middle, MatchStats::kSharedPointAwards) == 1);
    CHECK(f.stat(last, MatchStats::kSharedPointAwards) == 2);
    f.run("AddExp(10,1)\n");
    CHECK(f.stat(first, MatchStats::kPoints) == 65);
    CHECK(f.stat(middle, MatchStats::kPoints) == 32);
    CHECK(f.stat(last, MatchStats::kPoints) == 32);
    CHECK(f.stat(middle, MatchStats::kSharedPointAwards) == 2);
    CHECK(f.stat(last, MatchStats::kSharedPointAwards) == 3);
    f.run("AddExp(10,-4)\n");
    CHECK(f.stat(first, MatchStats::kPoints) == 61);
    CHECK(f.stat(middle, MatchStats::kSharedPointAwards) == 2);
    f.world.match.remove_player(f.world, middle);
    f.run("AddExp(10,8)\n"); // unregistered first link does not hide the second
    CHECK(f.stat(last, MatchStats::kPoints) == 34);
    CHECK(f.stat(last, MatchStats::kSharedPointAwards) == 4);
    CHECK(f.world.diagnostics.empty());
}

struct HostFixture : Fixture {
    nm::NapiNPServerCtx ctx;
    repl::UdpSessionTransport transport{repl::UdpSessionTransport::Role::Host};
    EntityHandle actor;
    explicit HostFixture(uint8_t type = nm::NapiNPConnection::kTypeServerSide) {
        nm::set_connection_mode(ctx, nm::ConnectionMode::HostOnly);
        nm::GameConfig config;
        config.max_players = 4;
        nm::create_session(ctx, config, {}, nullptr);
        ctx.world = &world;
        actor = spawn(10, 0);
        nm::NapiNPConnection conn;
        conn.type = type;
        conn.phase = nm::ConnectionPhase::PlayerAdded;
        conn.burst.spawned = true;
        conn.link.mode = type == nm::NapiNPConnection::kTypeServerSide
                ? repl::TransportMode::Client : repl::TransportMode::Loopback;
        conn.link.transport = &transport;
        conn.link.owned_entity = actor;
        conn.link.owned_entity_spawn_id = world.registry.get(actor)->registry_spawn_id;
        ctx.np_protocol.connection_list.push_back(std::move(conn));
    }
    int descriptions(uint8_t reason) {
        int count = 0;
        repl::Datagram datagram;
        while (transport.pop_outbound(datagram)) {
            if (datagram.tag != 3 || datagram.protocol_flags_raw != 0xA0) continue;
            ++count;
            CHECK(datagram.reliable);
            opennova::DisconnectEvent event;
            CHECK(opennova::parse_disconnect_event(
                    datagram.body.data(), datagram.body.size(), event));
            CHECK(event.ds == 1 && event.dc == 2);
            CHECK(event.dpc == reason && event.dstr.empty() && event.ddstr == "wac punt");
        }
        return count;
    }
};

static void test_punt_command_reaches_connection_and_first_reason_wins() {
    for (const bool kill : {false, true}) {
        HostFixture f;
        const std::string first = kill ? "pkillpunt" : "ppunt";
        const std::string second = kill ? "ppunt" : "pkillpunt";
        f.run(f.select(f.actor) + first + "() store(v1)\n" +
              second + "() store(v2)\n");
        CHECK(f.value(1) == 1 && f.value(2) == 1);
        CHECK(f.world.registry.get(f.actor)->health == 100);
        nm::Server_TickUpdate(f.ctx);
        CHECK(f.descriptions(kill ? 49 : 33) == 1);
        CHECK(f.ctx.np_protocol.connection_list.front().host_disconnect_sent);
        CHECK(f.world.registry.get(f.actor)->health == 100); // pkillpunt does not deal damage
        f.run(f.select(f.actor) + second + "() store(v3)\n");
        nm::Server_TickUpdate(f.ctx);
        CHECK(f.value(3) == 1 && f.descriptions(kill ? 49 : 33) == 0);
        CHECK(f.world.diagnostics.empty());
    }
}

static void test_punt_retry_local_and_stale_slot_guards() {
    HostFixture f;
    const auto baseline = f.world.snapshot();
    f.run(f.select(f.actor) + "ppunt()\n");
    f.world.restore(baseline);
    nm::Server_TickUpdate(f.ctx);
    CHECK(f.descriptions(33) == 0);
    f.run(f.select(f.actor) + "ppunt()\n");
    f.world.registry.despawn(f.actor);
    const auto replacement = f.spawn(11, 0);
    CHECK(replacement == f.actor);
    nm::Server_TickUpdate(f.ctx);
    CHECK(f.descriptions(33) == 0);
    CHECK(!f.ctx.np_protocol.connection_list.front().host_disconnect_sent);
    HostFixture local(nm::NapiNPConnection::kTypeClientSide);
    local.run(local.select(local.actor) + "ppunt() store(v1)\n");
    nm::Server_TickUpdate(local.ctx);
    CHECK(local.value(1) == 1 && local.descriptions(33) == 0);
    CHECK(local.world.match.drain_player_punts().empty());
    const auto npc = local.spawn(12, 1, false);
    local.run(local.select(npc) + "ppunt() store(v2)\npkillpunt() store(v3)\n"
              "item=65535\nppunt() store(v4)\n");
    CHECK(local.value(2) == 0 && local.value(3) == 0 && local.value(4) == 0);
    local.run(local.select(local.actor) + "ppunt()\n");
    local.world.match.configure({});
    CHECK(local.world.match.drain_player_punts().empty());
}

int main() {
    test_registered_slot_predicates_and_byte_bank();
    test_experience_raw_scoring_validation_and_retry();
    test_experience_both_occupant_links_and_zero_shares();
    test_punt_command_reaches_connection_and_first_reason_wins();
    test_punt_retry_local_and_stale_slot_guards();
    std::printf("wac_players: %d failures\n", failures);
    return failures ? 1 : 0;
}
