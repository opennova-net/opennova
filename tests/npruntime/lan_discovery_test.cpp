// The LAN browse window (npwire/lan_discovery.h LanDiscoveryBrowser): one
// probe identity per window, the 30-second window (strictly greater) and the
// 3-second re-announce cadence, the reply filter (CI, PN/PV1 case-insensitive,
// PG bytewise, no PV2 gate), the 32-row cap and the first-sighting rows.
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 (the 0x7530 gate @0x55933a,
// the 32 cap @0x5593b5, the key hit @0x5593c4); Nwu_HandleServerHello @0x626d20
// (the CI resolve @0x627533, PN @0x62758b, PG @0x627591, PV1 @0x627639);
// CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0; NapiNPSession_SendAnnouncePacket @0x61fa00]
#include <cstddef>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <net/npwire/flat_tlv.h>
#include <net/npwire/lan_discovery.h>
#include <net/novaworld/proxy_rendezvous.h>
#include <net/npwire/net_ports.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr uint32_t kBrowseCi = 0x00C0FFEEu;
// The LAN browse's enumerate address, the broadcast (PeerAddr's packing of 255.255.255.255).
constexpr uint32_t kBroadcast = 0xFFFFFFFFu;

// A game-server 0x81 carrying the retail JO identity, as a whole datagram.
std::vector<uint8_t> server_reply(const std::string &name, uint32_t players,
                                  uint32_t max_players, uint32_t gametype = 2,
                                  uint32_t ci = kBrowseCi, uint32_t flags = 0x904u) {
    const ClientHello retail = make_title_client_hello(kJo, 0);
    ServerHello hello;
    hello.ci = ci;
    hello.pn = retail.pn;
    hello.pg = retail.pg;
    hello.pv1 = retail.pv1;
    hello.pv2 = retail.pv2;
    hello.sn = name;
    hello.sus1 = "session-" + name;
    hello.sus2 = "revx02";
    hello.p1 = gametype;
    hello.p2 = flags;
    hello.np = players;
    hello.mp = max_players;
    return nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
}

// The datagram re-framed with every flat-TLV field named `unwanted` left out of its body.
std::vector<uint8_t> without_field(const std::vector<uint8_t> &datagram, std::string_view unwanted) {
    uint8_t opcode = 0;
    std::vector<uint8_t> body;
    if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, body)) return {};
    std::vector<uint8_t> kept;
    std::size_t pos = 0;
    while (pos < body.size()) {
        FlatTlvField field;
        const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
        if (next == kFlatTlvEnd) break;
        if (field.name != unwanted)
            kept.insert(kept.end(), body.begin() + static_cast<std::ptrdiff_t>(pos),
                        body.begin() + static_cast<std::ptrdiff_t>(next));
        pos = next;
    }
    return nw_encode_outbound(opcode, kept);
}

void test_begin_validates_the_range_and_fixes_one_identity() {
    opennova::LanDiscoveryBrowser b;
    CHECK(!b.begin(kJo, 7, kBroadcast, 0, 100, kLanConnectTypeLan));
    CHECK(!b.begin(kJo, 7, kBroadcast, 32768, 32760, kLanConnectTypeLan));
    CHECK(!b.begin(kJo, 7, kBroadcast, 1, 70000, kLanConnectTypeLan));
    CHECK(!b.begin(kJo, 0, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan)); // a zero CI is omitted on the wire
    CHECK(!b.browsing());
    CHECK(b.begin(kJo, 7, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
    CHECK(b.browsing());
    CHECK(b.client_index() == 7);
    CHECK(!b.probe(kRetailLanPortMin).empty());
    const std::vector<uint8_t> first = b.probe(kRetailLanPortMin);
    bool due = false;
    b.advance(1.0, due);
    CHECK(b.probe(kRetailLanPortMin) == first); // the identity holds across the window
    CHECK(b.probe(kRetailLanPortMin) ==
          opennova::build_lan_discovery_probe(kJo, 7, kBroadcast, kRetailLanPortMin));
}

// D-NET-520: one announce per port, each naming its own destination: the enumerate address
// as EIP and the port as EPN, with the JO build string as PV3. Only the EPN differs between
// two ports' announces. A zero enumerate address writes no EIP.
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the port walk
//  @0x629241..0x629268; NapiNPSession_SendAnnouncePacket @0x61fa00 - PV3 @0x61fc68..0x61fc9a,
//  EIP @0x61fcf5..0x61fd19, EPN @0x61fd23..0x61fd47; CNapiGameSession_InitTransportConnection
//  @0x4c9eab (-1 on a LAN browse)]
void test_each_port_gets_an_announce_naming_its_destination() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 9, kBroadcast, 32768, 32770, kLanConnectTypeLan));
    CHECK(b.enumerate_ip() == 0xFFFFFFFFu);
    for (int port = 32768; port <= 32770; ++port) {
        uint8_t opcode = 0;
        std::vector<uint8_t> body;
        const std::vector<uint8_t> probe = b.probe(port);
        ClientHello hello;
        CHECK(nw_decode_inbound(probe.data(), probe.size(), opcode, body));
        CHECK(opcode == SESSION_OPCODE_CLIENT_HELLO);
        CHECK(parse_client_hello(body.data(), body.size(), hello));
        CHECK(hello.ci == 9u);
        CHECK(hello.eip == 0xFFFFFFFFu);
        CHECK(hello.epn == static_cast<uint32_t>(port));
        CHECK(hello.pv3 == "CONSUMER RELEASE 1.7.5.7 [8165168.8165140] Jul 21 2009 18:54:42");
    }
    CHECK(b.probe(32768) != b.probe(32769));

    // A direct browse announces to the resolved address (127.0.0.1 in PeerAddr's packing).
    opennova::LanDiscoveryBrowser direct;
    CHECK(direct.begin(kJo, 9, 0x0100007Fu, 32786, 32786, kLanConnectTypeLan));
    ClientHello to_host;
    {
        uint8_t opcode = 0;
        std::vector<uint8_t> body;
        const std::vector<uint8_t> probe = direct.probe(32786);
        CHECK(nw_decode_inbound(probe.data(), probe.size(), opcode, body) &&
              parse_client_hello(body.data(), body.size(), to_host));
    }
    CHECK(to_host.eip == 0x0100007Fu && to_host.epn == 32786u);

    // No enumerate address: the announce carries EPN alone.
    opennova::LanDiscoveryBrowser unresolved;
    CHECK(unresolved.begin(kJo, 9, 0u, 32768, 32768, kLanConnectTypeLan));
    uint8_t opcode = 0;
    std::vector<uint8_t> body;
    const std::vector<uint8_t> probe = unresolved.probe(32768);
    CHECK(nw_decode_inbound(probe.data(), probe.size(), opcode, body));
    bool eip = false;
    bool epn = false;
    for (std::size_t pos = 0; pos < body.size();) {
        FlatTlvField field;
        const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
        if (next == kFlatTlvEnd) break;
        if (field.name == "EIP") eip = true;
        if (field.name == "EPN") epn = true;
        pos = next;
    }
    CHECK(!eip && epn);
}

void test_window_and_announce_cadence() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
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
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
    bool due = false;
    CHECK(b.advance(30.0, due)); // exactly 30000 ms: `> 0x7530` is false, still browsing
    CHECK(b.browsing());
    CHECK(!b.advance(0.001, due));
    CHECK(!b.browsing());
}

void test_announce_clock_restarts_from_zero_not_the_overshoot() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
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
    CHECK(parse_lan_discovery_reply(kJo, ok.data(), ok.size(), kBrowseCi, out));
    CHECK(out.server_name == "Alpha");
    // A reply for another enumerator's CI (or no CI at all) is not ours.
    const std::vector<uint8_t> foreign_ci = server_reply("Alpha", 3, 32, 2, kBrowseCi ^ 1u);
    CHECK(!parse_lan_discovery_reply(kJo, foreign_ci.data(), foreign_ci.size(), kBrowseCi, out));
    const std::vector<uint8_t> absent_ci = server_reply("Alpha", 3, 32, 2, 0);
    CHECK(!parse_lan_discovery_reply(kJo, absent_ci.data(), absent_ci.size(), kBrowseCi, out));
    CHECK(!parse_lan_discovery_reply(kJo, ok.data(), ok.size(), 0, out));
    // PN and PV1 compare case-insensitively; PV2 is not a browse gate.
    {
        const ClientHello retail = make_title_client_hello(kJo, 0);
        ServerHello hello;
        hello.ci = kBrowseCi;
        hello.pn = "jointoperations";
        hello.pg = retail.pg;
        hello.pv1 = "0.0.0 1/12/2004 em";
        hello.pv2 = "17";
        hello.sn = "Mixed";
        const std::vector<uint8_t> mixed =
                nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
        CHECK(parse_lan_discovery_reply(kJo, mixed.data(), mixed.size(), kBrowseCi, out));
        CHECK(out.server_name == "Mixed");
        // The 16-byte GUID is compared bytewise.
        hello.pg[3] ^= 1u;
        const std::vector<uint8_t> bad_pg =
                nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
        CHECK(!parse_lan_discovery_reply(kJo, bad_pg.data(), bad_pg.size(), kBrowseCi, out));
    }
}

void test_replies_are_filtered_and_listed_at_first_sighting() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeLan));
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
    CHECK(b.begin(kJo, 2, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    CHECK(b.servers().empty());
}

void test_window_lists_at_most_thirty_two_hosts() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const std::vector<uint8_t> reply = server_reply("Many", 1, 8);
    for (int i = 1; i <= 40; ++i) {
        const std::string ip = "10.0.0." + std::to_string(i);
        const LanRowChange change = b.accept_reply(reply.data(), reply.size(), ip, 32768);
        CHECK(change == (i <= 32 ? LanRowChange::kAdded : LanRowChange::kNone));
    }
    CHECK(b.servers().size() == kLanBrowseMaxSessions);
}

// The browse admits a session only when its P2 flag word allows the browser's network
// connect type: 0x400 (a NovaWorld-transport host) needs type 1, 0x100 (LAN) type 2,
// 0x200 (the internet type) type 3, and a word with none of the three passes any. The
// LAN screen browses as type 2, so a NovaWorld-registered host never lists there, and a
// filtered session takes no slot under the 32 cap.
// [orig: NetPacket_ValidateChannelType @0x4c49b0, the gate in
//  CNapiNetwork_OnSessionDiscovered @0x4c84b0; the LAN screen's type 2 @0x5569e1;
//  CNapiServerConfig_BuildFlags @0x4c4de4/@0x4c4df1/@0x4c4dfe]
void test_browse_admits_only_its_network_type() {
    const std::vector<uint8_t> lan_host = server_reply("Lan", 1, 8, 2, kBrowseCi, 0x904u);
    const std::vector<uint8_t> nw_host = server_reply("Nw", 1, 8, 2, kBrowseCi, 0x404u);
    const std::vector<uint8_t> net_host = server_reply("Net", 1, 8, 2, kBrowseCi, 0xA04u);
    const std::vector<uint8_t> bare_host = server_reply("Bare", 1, 8, 2, kBrowseCi, 0x0u);
    opennova::LanDiscoveryBrowser lan;
    CHECK(lan.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    CHECK(lan.accept_reply(nw_host.data(), nw_host.size(), "10.0.0.1", 32768) == LanRowChange::kNone);
    CHECK(lan.accept_reply(net_host.data(), net_host.size(), "10.0.0.2", 32768) == LanRowChange::kNone);
    CHECK(lan.accept_reply(lan_host.data(), lan_host.size(), "10.0.0.3", 32768) == LanRowChange::kAdded);
    CHECK(lan.accept_reply(bare_host.data(), bare_host.size(), "10.0.0.4", 32768) == LanRowChange::kAdded);
    CHECK(lan.servers().size() == 2);
    opennova::LanDiscoveryBrowser nw;
    CHECK(nw.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeNovaWorld));
    CHECK(nw.accept_reply(lan_host.data(), lan_host.size(), "10.0.0.3", 32768) == LanRowChange::kNone);
    CHECK(nw.accept_reply(nw_host.data(), nw_host.size(), "10.0.0.1", 32768) == LanRowChange::kAdded);
    // A filtered session takes no slot: 32 NovaWorld hosts on the LAN screen leave room.
    opennova::LanDiscoveryBrowser capped;
    CHECK(capped.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    for (int i = 1; i <= 40; ++i) {
        capped.accept_reply(nw_host.data(), nw_host.size(), "10.1.0." + std::to_string(i), 32768);
    }
    CHECK(capped.accept_reply(lan_host.data(), lan_host.size(), "10.2.0.1", 32768) == LanRowChange::kAdded);
    CHECK(capped.servers().size() == 1);
}

// D-NET-468: the browse lists a JO 0x81 whatever fields it carries. An SF-less reply reads SF
// as 0, so its row carries no password flag; only P2's channel type gates the session.
// [orig: Nwu_HandleServerHello @0x626d20 - SF zeroed @0x626eb7, loaded @0x62721a, kept in the
//  session record @0x6277c2; CNapiNetwork_OnSessionDiscovered @0x4c8470 - the P2 gate
//  @0x4c84a3..0x4c84ba, SF bit 0 the row's password flag @0x4c84f1;
//  UI_ProcessLANSessionStateMachine @0x558de0 lists every session node]
void test_reply_without_sf_is_listed() {
    const std::vector<uint8_t> sf_less = without_field(server_reply("NoFlags", 2, 16), "SF");
    CHECK(!sf_less.empty());
    LanDiscoveryServer out;
    CHECK(parse_lan_discovery_reply(kJo, sf_less.data(), sf_less.size(), kBrowseCi, out));
    CHECK(out.server_name == "NoFlags");
    CHECK(out.current_players == 2 && out.max_players == 16);
    CHECK(!out.password_required);
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    CHECK(b.accept_reply(sf_less.data(), sf_less.size(), "192.168.1.20", 32768) == LanRowChange::kAdded);
    CHECK(b.servers().size() == 1);
    // P2 still gates it: an SF-less NovaWorld-transport host is not listed on the LAN screen.
    const std::vector<uint8_t> nw_sf_less =
            without_field(server_reply("NwNoFlags", 1, 8, 2, kBrowseCi, 0x404u), "SF");
    CHECK(b.accept_reply(nw_sf_less.data(), nw_sf_less.size(), "192.168.1.21", 32768) == LanRowChange::kNone);
    CHECK(b.servers().size() == 1);
}

} // namespace

// D-NET-712: the proxy-assisted NovaWorld join's rendezvous rides the browse. With all six
// proxy fields installed, every burst opens with the 48-byte rendezvous to the game node and
// then one announce per port; a field missing installs nothing. The layout is the witnessed
// SendPingPacket body: '@' at 4, the relay port at 6, the relay address at 8, the cookie at 12.
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the six-field gate
//  @0x629122..0x62914f, CNapiNPConnection_SendPingPacket @0x61F8C0 called @0x629159 ahead of
//  the announce walk @0x6291c6..0x629268; CNapiNPConnection_SendPingPacket @0x61f8c0 - '@'
//  @0x61f9ca, the relay port @0x61f9d1, the relay addr @0x61f9d6, the cookie @0x61f9bb]
void test_proxy_rendezvous_leads_each_burst() {
    opennova::LanDiscoveryBrowser b;
    opennova::ProxyRendezvousConfig proxy;
    proxy.node_addr = proxy_inet_addr("10.1.2.3");
    proxy.node_port = 4000;
    proxy.cookie = 77;
    proxy.relay_addr = proxy_inet_addr("192.168.0.9");
    proxy.relay_port = 32768;
    b.set_proxy(proxy);
    CHECK(b.begin(kJo, 9, 0x0900A8C0u, 32768, 32769, kLanConnectTypeNovaWorld));
    const std::vector<LanBurstDatagram> burst = b.burst();
    CHECK(burst.size() == 3);
    if (burst.size() != 3) return;
    CHECK(burst[0].rendezvous);
    CHECK(burst[0].node == peer_addr_from_octets({10, 1, 2, 3}, 4000));
    const std::vector<uint8_t> &r = burst[0].bytes;
    CHECK(r.size() == 48);
    if (r.size() != 48) return;
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0);
    CHECK(r[4] == '@' && r[5] == 0);
    CHECK(r[6] == 0x00 && r[7] == 0x80);
    CHECK(r[8] == 192 && r[9] == 168 && r[10] == 0 && r[11] == 9);
    CHECK(r[12] == 77 && r[13] == 0 && r[14] == 0 && r[15] == 0);
    for (std::size_t i = 16; i < r.size(); ++i) CHECK(r[i] == 0);
    CHECK(!burst[1].rendezvous && burst[1].port == 32768 && burst[1].bytes == b.probe(32768));
    CHECK(!burst[2].rendezvous && burst[2].port == 32769 && burst[2].bytes == b.probe(32769));
    // The re-announce burst repeats the same order.
    bool due = false;
    CHECK(b.advance(3.001, due) && due);
    CHECK(b.burst().size() == 3 && b.burst()[0].rendezvous);

    // Any field missing: announces alone.
    opennova::LanDiscoveryBrowser plain;
    proxy.cookie = 0;
    plain.set_proxy(proxy);
    CHECK(plain.begin(kJo, 9, kBroadcast, 32768, 32769, kLanConnectTypeLan));
    const std::vector<LanBurstDatagram> announces = plain.burst();
    CHECK(announces.size() == 2);
    for (const LanBurstDatagram &d : announces) CHECK(!d.rendezvous);
}

// D-NET-703: the enumerate-and-join's browse has no window: it keeps re-announcing every
// > 3000 ms past the LAN screen's 30 s until a session answers or the player aborts. And every
// enumerate creates its connection twice, so the browse's CI is the counter's second step.
// [orig: UI_EnumerateAndJoinSession @0x569f50 - the loop @0x56a0c1..0x56a0f9;
//  CreateServerConnection @0x56a011 then InitTransportConnection @0x56a097 (its create @0x4c9e16);
//  the LAN search's create @0x55837e then @0x5583b0]
void test_enumerate_and_join_browse_has_no_window() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.windowed());
    b.set_windowed(false);
    CHECK(b.begin(kJo, 9, 0x0100007Fu, 32768, 32768, kLanConnectTypeNovaWorld));
    int bursts = 0;
    bool due = false;
    for (int i = 0; i < 1000; ++i) { // 100 s at 0.1 s frames
        CHECK(b.advance(0.1, due));
        if (due) ++bursts;
    }
    CHECK(b.browsing());
    CHECK(bursts >= 30 && bursts <= 33);
    b.stop();
    CHECK(!b.browsing() && !b.windowed()); // the mode outlives the window

    uint32_t counter = next_connection_index();
    const uint32_t browse_ci = next_browse_connection_index();
    advance_connection_index(counter);
    advance_connection_index(counter);
    CHECK(browse_ci == counter);
}

int main() {
    test_begin_validates_the_range_and_fixes_one_identity();
    test_each_port_gets_an_announce_naming_its_destination();
    test_window_and_announce_cadence();
    test_window_boundary_is_strictly_greater_than_thirty_seconds();
    test_announce_clock_restarts_from_zero_not_the_overshoot();
    test_reply_filter_matches_retail_admission();
    test_replies_are_filtered_and_listed_at_first_sighting();
    test_window_lists_at_most_thirty_two_hosts();
    test_browse_admits_only_its_network_type();
    test_reply_without_sf_is_listed();
    test_proxy_rendezvous_leads_each_burst();
    test_enumerate_and_join_browse_has_no_window();
    if (failures == 0) std::printf("lan_discovery_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
