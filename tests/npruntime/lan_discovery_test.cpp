// The LAN browse window (npruntime/lan_discovery.h LanDiscoveryBrowser): one
// probe identity per window, the 30-second window and the 3-second
// re-announce cadence, the reply filter, and the endpoint-keyed rows
// refreshed in place. [orig: UI_ProcessLANSessionStateMachine @0x558de0
// (the 0x7530 gate @0x55933a); CNapiNPConnection_PumpEnumeratorAndSend
// @0x6290c0; NapiNPSession_SendAnnouncePacket @0x61fa00]
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <net/npwire/lan_discovery.h>
#include <net/npwire/net_ports.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// A game-server 0x81 carrying the retail JO identity, as a whole datagram.
std::vector<uint8_t> server_reply(const std::string &name, uint32_t players,
                                  uint32_t max_players, uint32_t gametype = 2) {
    const ClientHello retail = make_jointoperations_client_hello(0);
    ServerHello hello;
    hello.is_game_server = true;
    hello.pn = retail.pn;
    hello.pg = retail.pg;
    hello.pv1 = retail.pv1;
    hello.pv2 = retail.pv2;
    hello.sn = name;
    hello.sus1 = "session-" + name;
    hello.sus2 = "revx02";
    hello.p1 = gametype;
    hello.np = players;
    hello.mp = max_players;
    return nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
}

void test_begin_validates_the_range_and_fixes_one_identity() {
    opennova::LanDiscoveryBrowser b;
    CHECK(!b.begin(7, 0, 100));
    CHECK(!b.begin(7, 32768, 32760));
    CHECK(!b.begin(7, 1, 70000));
    CHECK(!b.browsing());
    CHECK(b.begin(7, kRetailLanPortMin, kRetailLanPortMax));
    CHECK(b.browsing());
    CHECK(!b.probe().empty());
    const std::vector<uint8_t> first = b.probe();
    bool due = false;
    b.advance(1.0, due);
    CHECK(b.probe() == first); // the identity holds across the window
    CHECK(b.probe() == opennova::build_lan_discovery_probe(7));
}

void test_window_and_announce_cadence() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(1, kRetailLanPortMin, kRetailLanPortMax));
    bool due = false;
    int announces = 0;
    int frames = 0;
    // 60 fps frames: the window closes at 30 s, announces fall due every 3 s.
    while (b.advance(1.0 / 60.0, due)) {
        ++frames;
        if (due) ++announces;
    }
    CHECK(!b.browsing());
    CHECK(frames >= 1798 && frames <= 1800);
    CHECK(announces == 9); // 3, 6, ... 27 s (the 30 s edge closes the window instead)
    // Once stopped, advance keeps saying so and never re-announces.
    CHECK(!b.advance(1.0, due));
    CHECK(!due);
}

void test_announce_clock_restarts_from_zero_not_the_overshoot() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(1, kRetailLanPortMin, kRetailLanPortMax));
    bool due = false;
    CHECK(b.advance(2.9, due));
    CHECK(!due);
    CHECK(b.advance(0.5, due)); // 3.4 s: due, clock restarts at 0 (not 0.4)
    CHECK(due);
    CHECK(b.advance(2.9, due));
    CHECK(!due);
    CHECK(b.advance(0.2, due)); // 3.1 s since the restart
    CHECK(due);
}

void test_replies_are_filtered_and_upserted_by_endpoint() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(1, 32768, 32775));
    const std::vector<uint8_t> alpha = server_reply("Alpha", 3, 32);
    // Outside the browsed range, an unusable source, or a foreign packet: no row.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32776) == opennova::LanRowChange::kNone);
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "0.0.0.0", 32768) == opennova::LanRowChange::kNone);
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "", 32768) == opennova::LanRowChange::kNone);
    const std::vector<uint8_t> junk = {1, 2, 3};
    CHECK(b.accept_reply(junk.data(), junk.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    CHECK(b.servers().empty());
    // A first announce adds the row.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kAdded);
    CHECK(b.servers().size() == 1);
    CHECK(b.servers()[0].host_ip == "192.168.1.10");
    CHECK(b.servers()[0].port == 32768);
    CHECK(b.servers()[0].server.server_name == "Alpha");
    CHECK(b.servers()[0].server.current_players == 3);
    CHECK(b.servers()[0].server.max_players == 32);
    CHECK(b.servers()[0].server.gametype == 2);
    CHECK(b.servers()[0].server.expansion == "revx02");
    // The same host re-announcing unchanged: no change reported.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    // Live state moved (a player joined): refreshed in place, first-seen order kept.
    const std::vector<uint8_t> alpha4 = server_reply("Alpha", 4, 32);
    const std::vector<uint8_t> bravo = server_reply("Bravo", 0, 16, 5);
    CHECK(b.accept_reply(bravo.data(), bravo.size(), "192.168.1.11", 32768) == opennova::LanRowChange::kAdded);
    CHECK(b.accept_reply(alpha4.data(), alpha4.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kUpdated);
    CHECK(b.servers().size() == 2);
    CHECK(b.servers()[0].server.server_name == "Alpha");
    CHECK(b.servers()[0].server.current_players == 4);
    CHECK(b.servers()[1].server.server_name == "Bravo");
    CHECK(b.servers()[1].server.gametype == 5);
    // The same host on another port is another endpoint.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32769) == opennova::LanRowChange::kAdded);
    CHECK(b.servers().size() == 3);
    // A stopped browser accepts nothing; a new window starts empty.
    b.stop();
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    CHECK(b.begin(2, 32768, 32775));
    CHECK(b.servers().empty());
}

} // namespace

int main() {
    test_begin_validates_the_range_and_fixes_one_identity();
    test_window_and_announce_cadence();
    test_announce_clock_restarts_from_zero_not_the_overshoot();
    test_replies_are_filtered_and_upserted_by_endpoint();
    if (failures == 0) std::printf("lan_discovery_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
