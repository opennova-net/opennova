// The LAN browse window (npwire/lan_discovery.h LanDiscoveryBrowser): one
// probe identity per window, the 30-second window (strictly greater) and the
// 3-second re-announce cadence, the reply filter (CI, PN/PV1 case-insensitive,
// PG bytewise, no PV2 gate), the 32-row cap and the first-sighting rows.
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 (the 0x7530 gate @0x55933a,
// the 32 cap @0x5593b5, the key hit @0x5593c4); Nwu_HandleServerHello @0x626d20
// (the CI resolve @0x627533, PN @0x62758b, PG @0x627591, PV1 @0x627639);
// CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0; NapiNPSession_SendAnnouncePacket @0x61fa00]
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

constexpr uint32_t kBrowseCi = 0x00C0FFEEu;

// A game-server 0x81 carrying the retail JO identity, as a whole datagram.
std::vector<uint8_t> server_reply(const std::string &name, uint32_t players,
                                  uint32_t max_players, uint32_t gametype = 2,
                                  uint32_t ci = kBrowseCi) {
    const ClientHello retail = make_jointoperations_client_hello(0);
    ServerHello hello;
    hello.is_game_server = true;
    hello.ci = ci;
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
    CHECK(!b.begin(0, kRetailLanPortMin, kRetailLanPortMax)); // a zero CI is omitted on the wire
    CHECK(!b.browsing());
    CHECK(b.begin(7, kRetailLanPortMin, kRetailLanPortMax));
    CHECK(b.browsing());
    CHECK(b.client_index() == 7);
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
    // 64 fps frames (1/64 s is exact in binary, so the sums are too): the window
    // closes once elapsed EXCEEDS 30 s, a re-announce falls due strictly AFTER 3 s.
    while (b.advance(1.0 / 64.0, due)) {
        ++frames;
        if (due) ++announces;
    }
    CHECK(!b.browsing());
    CHECK(frames == 1920); // frame 1920 lands exactly on 30 s and is still inside
    // Each re-announce takes 193 frames (192 is exactly 3 s, not past it), so the
    // tenth would land at frame 1930, past the window: nine.
    // [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 — `> interval`]
    CHECK(announces == 9);
    // Once stopped, advance keeps saying so and never re-announces.
    CHECK(!b.advance(1.0, due));
    CHECK(!due);
}

void test_window_boundary_is_strictly_greater_than_thirty_seconds() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(1, kRetailLanPortMin, kRetailLanPortMax));
    bool due = false;
    CHECK(b.advance(30.0, due)); // exactly 30000 ms: `> 0x7530` is false, still browsing
    CHECK(b.browsing());
    CHECK(!b.advance(0.001, due));
    CHECK(!b.browsing());
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

void test_reply_filter_matches_retail_admission() {
    LanDiscoveryServer out;
    const std::vector<uint8_t> ok = server_reply("Alpha", 3, 32);
    CHECK(parse_lan_discovery_reply(ok.data(), ok.size(), kBrowseCi, out));
    CHECK(out.server_name == "Alpha");
    // A reply for another enumerator's CI (or no CI at all) is not ours.
    const std::vector<uint8_t> foreign_ci = server_reply("Alpha", 3, 32, 2, kBrowseCi ^ 1u);
    CHECK(!parse_lan_discovery_reply(foreign_ci.data(), foreign_ci.size(), kBrowseCi, out));
    const std::vector<uint8_t> absent_ci = server_reply("Alpha", 3, 32, 2, 0);
    CHECK(!parse_lan_discovery_reply(absent_ci.data(), absent_ci.size(), kBrowseCi, out));
    CHECK(!parse_lan_discovery_reply(ok.data(), ok.size(), 0, out));
    // PN and PV1 compare case-insensitively; PV2 is not a browse gate.
    {
        const ClientHello retail = make_jointoperations_client_hello(0);
        ServerHello hello;
        hello.is_game_server = true;
        hello.ci = kBrowseCi;
        hello.pn = "jointoperations";
        hello.pg = retail.pg;
        hello.pv1 = "0.0.0 1/12/2004 em";
        hello.pv2 = "17";
        hello.sn = "Mixed";
        const std::vector<uint8_t> mixed =
                nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
        CHECK(parse_lan_discovery_reply(mixed.data(), mixed.size(), kBrowseCi, out));
        CHECK(out.server_name == "Mixed");
        // The 16-byte GUID is compared bytewise.
        hello.pg[3] ^= 1u;
        const std::vector<uint8_t> bad_pg =
                nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
        CHECK(!parse_lan_discovery_reply(bad_pg.data(), bad_pg.size(), kBrowseCi, out));
    }
}

void test_replies_are_filtered_and_listed_at_first_sighting() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kBrowseCi, 32768, 32775));
    const std::vector<uint8_t> alpha = server_reply("Alpha", 3, 32);
    // Outside the browsed range, an unusable source, or a foreign packet: no row.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32776) == opennova::LanRowChange::kNone);
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "0.0.0.0", 32768) == opennova::LanRowChange::kNone);
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "", 32768) == opennova::LanRowChange::kNone);
    const std::vector<uint8_t> junk = {1, 2, 3};
    CHECK(b.accept_reply(junk.data(), junk.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    const std::vector<uint8_t> stale = server_reply("Stale", 1, 8, 2, kBrowseCi ^ 0x100u);
    CHECK(b.accept_reply(stale.data(), stale.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
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
    // The same host re-announcing unchanged: nothing to do.
    CHECK(b.accept_reply(alpha.data(), alpha.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    // Live state moved (a player joined): the row keeps its first-sighting counts —
    // retail's key hit adds and updates nothing for the rest of the window.
    const std::vector<uint8_t> alpha4 = server_reply("Alpha", 4, 32);
    const std::vector<uint8_t> bravo = server_reply("Bravo", 0, 16, 5);
    CHECK(b.accept_reply(bravo.data(), bravo.size(), "192.168.1.11", 32768) == opennova::LanRowChange::kAdded);
    CHECK(b.accept_reply(alpha4.data(), alpha4.size(), "192.168.1.10", 32768) == opennova::LanRowChange::kNone);
    CHECK(b.servers().size() == 2);
    CHECK(b.servers()[0].server.server_name == "Alpha");
    CHECK(b.servers()[0].server.current_players == 3);
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

void test_window_lists_at_most_thirty_two_hosts() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kBrowseCi, 32768, 32768));
    const std::vector<uint8_t> reply = server_reply("Many", 1, 8);
    for (int i = 1; i <= 40; ++i) {
        const std::string ip = "10.0.0." + std::to_string(i);
        const LanRowChange change = b.accept_reply(reply.data(), reply.size(), ip, 32768);
        CHECK(change == (i <= 32 ? LanRowChange::kAdded : LanRowChange::kNone));
    }
    CHECK(b.servers().size() == kLanBrowseMaxSessions);
}

} // namespace

int main() {
    test_begin_validates_the_range_and_fixes_one_identity();
    test_window_and_announce_cadence();
    test_window_boundary_is_strictly_greater_than_thirty_seconds();
    test_announce_clock_restarts_from_zero_not_the_overshoot();
    test_reply_filter_matches_retail_admission();
    test_replies_are_filtered_and_listed_at_first_sighting();
    test_window_lists_at_most_thirty_two_hosts();
    if (failures == 0) std::printf("lan_discovery_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
