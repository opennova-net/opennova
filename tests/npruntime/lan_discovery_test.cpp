// The LAN browse window (npwire/lan_discovery.h LanDiscoveryBrowser): one
// probe identity per window, the 30-second window (strictly greater) and the
// 3-second re-announce cadence, the reply filter (CI, PN/PV1 case-insensitive,
// PG bytewise, no PV2 gate), the session records (in place, replaced, expired,
// drained) with their two connect-log lines, and the list's row walk (the 32 cap,
// rows keyed by the record id).
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 (the 0x7530 gate @0x55933a,
// the 32 cap @0x5593b5, the key hit @0x5593c4); Nwu_HandleServerHello @0x626d20
// (the CI resolve @0x627533, PN @0x62758b, PG @0x627591, PV1 @0x627639);
// CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0; NapiNPSession_SendAnnouncePacket @0x61fa00]
#include <cstddef>
#include <net/protocol/protocol.h>
#include <base/gameprofile/gameprofile.h>
#include <base/version/one_build.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <net/npwire/flat_tlv.h>
#include <net/npwire/lan_discovery.h>
#include <net/napi/envelope.h>
#include <net/novaworld/proxy_rendezvous.h>
#include <net/npwire/net_ports.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

// JO's PV3 over ONE's build (D-NET-520), where retail wrote its own 1.7.5.7 and build date.
static std::string one_pv3() {
	return opennova::protocol_version3(
			kJo, opennova::one_build().wire_version(), opennova::one_build().date_text());
}

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
    const ClientHello retail = make_protocol_client_hello(kJo, 0);
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

// One frame with nothing received between its legs (LanDiscoveryBrowser::frame): `due` is
// whether its re-announce fell due.
bool run_frame(LanDiscoveryBrowser &b, double dt, bool &due) {
    due = false;
    LanDiscoveryBrowser::FrameLegs legs;
    legs.send_burst = [&due] { due = true; };
    return b.frame(dt, legs);
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
    run_frame(b, 1.0, due);
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
        CHECK(hello.pv3 == one_pv3());
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
    while (run_frame(b, 1.0 / 64.0, due)) {
        ++frames;
        if (due) ++announces;
    }
    CHECK(!b.browsing());
    CHECK(frames == 1920); // frame 1920 lands exactly on 30 s and is still inside
    // Each re-announce takes 193 frames (192 is exactly 3 s, not past it), so the
    // tenth would land at frame 1930, past the window: nine.
    // [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 — `> interval`]
    CHECK(announces == 9);
    // Once stopped, a frame keeps saying so and never re-announces.
    CHECK(!run_frame(b, 1.0, due));
    CHECK(!due);
}

void test_window_boundary_is_strictly_greater_than_thirty_seconds() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
    bool due = false;
    CHECK(run_frame(b, 30.0, due)); // exactly 30000 ms: `> 0x7530` is false, still browsing
    CHECK(b.browsing());
    CHECK(!run_frame(b, 0.001, due));
    CHECK(!b.browsing());
}

void test_announce_clock_restarts_from_zero_not_the_overshoot() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
    bool due = false;
    CHECK(run_frame(b, 2.9, due));
    CHECK(!due);
    CHECK(run_frame(b, 0.5, due)); // 3.4 s: due, clock restarts at 0 (not 0.4)
    CHECK(due);
    CHECK(run_frame(b, 2.9, due));
    CHECK(!due);
    CHECK(run_frame(b, 0.2, due)); // 3.1 s since the restart
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
        const ClientHello retail = make_protocol_client_hello(kJo, 0);
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

// A reply's sender, the dotted quad as the socket reports it.
PeerAddr sender(const std::string &dotted, int port) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(dotted.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return PeerAddr{0, 0};
    return peer_addr_from_octets({static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                  static_cast<uint8_t>(c), static_cast<uint8_t>(d)},
                                 static_cast<uint16_t>(port));
}

// A game-server 0x81 carrying `hk` as its HK (and `sf` as its SF).
std::vector<uint8_t> server_reply_hk(const std::string &name, uint32_t hk, uint32_t players = 1,
                                     uint32_t flags = 0x904u, uint32_t sf = 0) {
    const ClientHello retail = make_protocol_client_hello(kJo, 0);
    ServerHello hello;
    hello.ci = kBrowseCi;
    hello.pn = retail.pn;
    hello.pg = retail.pg;
    hello.pv1 = retail.pv1;
    hello.pv2 = retail.pv2;
    hello.sn = name;
    hello.hk = hk;
    hello.sf = sf;
    hello.p1 = 2;
    hello.p2 = flags;
    hello.np = players;
    hello.mp = 16;
    return nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
}

// The frame's walk (a frame with no time passing): the rows the frame appends.
void walk(LanDiscoveryBrowser &b) {
    bool due = false;
    run_frame(b, 0.0, due);
}

// One datagram through the browse's frame head: the manager's receive (accept_reply), then
// the protocol pump's queue pass (pump_queue), which records a queued reply.
void reply(LanDiscoveryBrowser &b, const std::vector<uint8_t> &datagram, const PeerAddr &from) {
    b.accept_reply(datagram.data(), datagram.size(), from);
    b.pump_queue();
}

// A connect-log hook keeping only the session-record lines (the manager's packet lines and
// NET GOT have their own case).
std::function<void(const std::string &)> session_lines(std::vector<std::string> &lines) {
    return [&lines](const std::string &text) {
        if (text.rfind("NET SESSION", 0) == 0) lines.push_back(text);
    };
}

void test_replies_are_filtered_and_listed_at_first_sighting() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    const std::vector<uint8_t> alpha = server_reply("Alpha", 3, 32);
    // Outside the browsed range, an unusable source, or a foreign packet: no record.
    reply(b, alpha, sender("192.168.1.10", 32776));
    reply(b, alpha, sender("0.0.0.0", 32768));
    const std::vector<uint8_t> junk = {1, 2, 3};
    reply(b, junk, sender("192.168.1.10", 32768));
    const std::vector<uint8_t> stale = server_reply("Stale", 1, 8, 2, kBrowseCi ^ 0x100u);
    reply(b, stale, sender("192.168.1.10", 32768));
    walk(b);
    CHECK(b.record_count() == 0);
    CHECK(b.servers().empty());
    // A first announce is a record, listed at the frame's walk.
    reply(b, alpha, sender("192.168.1.10", 32768));
    CHECK(b.record_count() == 1);
    CHECK(b.servers().empty());
    walk(b);
    CHECK(b.servers().size() == 1);
    CHECK(b.servers()[0].host_ip == "192.168.1.10");
    CHECK(b.servers()[0].port == 32768);
    CHECK(b.servers()[0].record_id == 1);
    CHECK(b.servers()[0].server.server_name == "Alpha");
    CHECK(b.servers()[0].server.current_players == 3);
    CHECK(b.servers()[0].server.max_players == 32);
    CHECK(b.servers()[0].server.gametype == 2);
    CHECK(b.servers()[0].server.expansion == "revx02");
    // The same host re-announcing with the same HK and SN: its record in place.
    reply(b, alpha, sender("192.168.1.10", 32768));
    // Live state moved (a player joined), the same HK and SN: the record is stamped in
    // place and the row keeps its first-sighting counts for the rest of the window.
    const std::vector<uint8_t> alpha4 = server_reply("Alpha", 4, 32);
    const std::vector<uint8_t> bravo = server_reply("Bravo", 0, 16, 5);
    reply(b, bravo, sender("192.168.1.11", 32768));
    reply(b, alpha4, sender("192.168.1.10", 32768));
    walk(b);
    CHECK(b.record_count() == 2);
    CHECK(b.servers().size() == 2);
    CHECK(b.servers()[0].server.server_name == "Alpha");
    CHECK(b.servers()[0].server.current_players == 3);
    CHECK(b.servers()[1].server.server_name == "Bravo");
    CHECK(b.servers()[1].server.gametype == 5);
    CHECK(b.servers()[1].record_id == 2);
    // The same host on another port is another sender.
    reply(b, alpha, sender("192.168.1.10", 32769));
    walk(b);
    CHECK(b.servers().size() == 3);
    // A stopped browser takes nothing; a new window starts empty.
    b.stop();
    reply(b, alpha, sender("192.168.1.10", 32770));
    CHECK(b.record_count() == 3);
    CHECK(b.begin(kJo, 2, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    CHECK(b.servers().empty());
    CHECK(b.record_count() == 0);
}

// The walk examines at most 32 listed sessions; nothing bounds its append past the 32-row
// table. Once 32 rows are listed, a replaced session among the first 32 frees its node, so
// the 33rd listed session enters the walk's first 32 and lists as row 33; a join from row 33
// takes the 33rd listed session (L2-s16's PERMANENT note withdrawn: V-ROLL-J).
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 - the 32 break @0x5593B5, the append
//  @0x5593FA..0x559524 with no row-count test, UIList_AddRow @0x559516;
//  UI_StartLANSessionSearch @0x558300 - memset(.., 0x23D00) @0x5583C1 (32 rows of 0x11E8);
//  UI_HandleLANGameListEvent @0x556C70 - the row index @0x556C21; UI_JoinSelectedSession
//  @0x5699D0 - the node walk @0x569A4B..0x569A76]
void test_window_lists_at_most_thirty_two_hosts() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const std::vector<uint8_t> many = server_reply("Many", 1, 8);
    for (int i = 1; i <= 40; ++i)
        reply(b, many, sender("10.0.0." + std::to_string(i), 32768));
    walk(b);
    CHECK(b.record_count() == 40);
    CHECK(b.servers().size() == kLanBrowseMaxSessions);
    CHECK(b.servers().back().host_ip == "10.0.0.32");
    // 10.0.0.1 restarts (another HK): its node goes and its new one is the list's last, so the
    // walk's first 32 now reach 10.0.0.33, which lists as row 33.
    const std::vector<uint8_t> restarted = server_reply_hk("Many", 0x99u);
    reply(b, restarted, sender("10.0.0.1", 32768));
    walk(b);
    CHECK(b.servers().size() == kLanBrowseMaxSessions + 1);
    if (b.servers().size() == kLanBrowseMaxSessions + 1)
        CHECK(b.servers().back().host_ip == "10.0.0.33");
    // Row 33's join takes the 33rd listed session: the listed sessions now run 10.0.0.2 ..
    // 10.0.0.40, then the restarted 10.0.0.1.
    LanDiscoveryRow join;
    CHECK(b.join_session(kLanBrowseMaxSessions, join) && join.host_ip == "10.0.0.34");
    CHECK(b.join_session(0, join) && join.host_ip == "10.0.0.2");
}

// The browse admits a session only when its P2 flag word allows the browser's network
// connect type: 0x400 (a NovaWorld-transport host) needs type 1, 0x100 (LAN) type 2,
// 0x200 (the internet type) type 3, and a word with none of the three passes any. The
// LAN screen browses as type 2, so a NovaWorld-registered host never lists there, and a
// filtered session takes no slot in the walk.
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
    reply(lan, nw_host, sender("10.0.0.1", 32768));
    reply(lan, net_host, sender("10.0.0.2", 32768));
    reply(lan, lan_host, sender("10.0.0.3", 32768));
    reply(lan, bare_host, sender("10.0.0.4", 32768));
    walk(lan);
    CHECK(lan.servers().size() == 2);
    CHECK(lan.servers()[0].host_ip == "10.0.0.3" && lan.servers()[1].host_ip == "10.0.0.4");
    opennova::LanDiscoveryBrowser nw;
    CHECK(nw.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeNovaWorld));
    reply(nw, lan_host, sender("10.0.0.3", 32768));
    reply(nw, nw_host, sender("10.0.0.1", 32768));
    walk(nw);
    CHECK(nw.servers().size() == 1 && nw.servers()[0].host_ip == "10.0.0.1");
    // A filtered session takes no slot: 40 NovaWorld hosts on the LAN screen leave room.
    opennova::LanDiscoveryBrowser capped;
    CHECK(capped.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    for (int i = 1; i <= 40; ++i)
        reply(capped, nw_host, sender("10.1.0." + std::to_string(i), 32768));
    reply(capped, lan_host, sender("10.2.0.1", 32768));
    walk(capped);
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
    reply(b, sf_less, sender("192.168.1.20", 32768));
    walk(b);
    CHECK(b.servers().size() == 1);
    // P2 still gates it: an SF-less NovaWorld-transport host is not listed on the LAN screen.
    const std::vector<uint8_t> nw_sf_less =
            without_field(server_reply("NwNoFlags", 1, 8, 2, kBrowseCi, 0x404u), "SF");
    reply(b, nw_sf_less, sender("192.168.1.21", 32768));
    walk(b);
    CHECK(b.servers().size() == 1);
}

// D-NET-956 (b): the browse's session records and their two connect-log lines. A
// sender's next 0x81 with the same HK and SN (its first 63 characters) is its record in
// place, no line; another HK or SN replaces it: the old record's REMOVED line (its SN),
// then the new record's DISCOVERED line. The new record takes the next id, so the walk
// lists it again beside the old row, which stays. A session the channel filter drops
// writes no DISCOVERED line, but its record's removal writes REMOVED.
// [orig: Nwu_HandleServerHello @0x626D20 - FindSessionByAddress (the call @0x627667),
//  the HK test @0x627917, the SN's String_ExactMatch @0x627928, the in-place stamp
//  @0x627E34, NapiNPTimer_Destroy (the call @0x627941) -> NapiNPSession_Create (the call
//  @0x627947); NapiNPSession_Create @0x624540 - the id @0x624704..0x624710;
//  CNapiNetwork_OnSessionDiscovered @0x4C8470 - the channel test @0x4C84A3..0x4C84BA, the
//  line @0x4C8663; CNapiNetwork_OnSessionRemoved @0x4C68C0 - the line @0x4C6975;
//  UI_ProcessLANSessionStateMachine @0x558DE0 - the key compare @0x5593C1..0x5593E3]
void test_records_replace_in_place_and_write_their_lines() {
    std::vector<std::string> lines;
    opennova::LanDiscoveryBrowser b;
    b.set_connect_log(session_lines(lines));
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32775, kLanConnectTypeLan));
    CHECK(lines.empty());
    const PeerAddr host = sender("192.168.1.20", 32768);
    const std::vector<uint8_t> first = server_reply_hk("Alpha", 0x11u);
    reply(b, first, host);
    walk(b);
    CHECK(lines.size() == 1);
    CHECK(!lines.empty() && lines[0] == "NET SESSION DISCOVERED AT 192.168.1.20:32768 [Alpha]");
    CHECK(b.servers().size() == 1 && b.servers()[0].record_id == 1);
    // The same HK and SN, another count: in place, no line.
    const std::vector<uint8_t> again = server_reply_hk("Alpha", 0x11u, 5);
    reply(b, again, host);
    CHECK(lines.size() == 1);
    CHECK(b.record_count() == 1);
    // Another HK (the host restarted): removed, then discovered again; the walk appends
    // the new record's row beside the old one.
    const std::vector<uint8_t> restarted = server_reply_hk("Alpha", 0x22u, 2);
    reply(b, restarted, host);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[1] == "NET SESSION AT 192.168.1.20:32768 [Alpha] HAS BEEN REMOVED.");
        CHECK(lines[2] == "NET SESSION DISCOVERED AT 192.168.1.20:32768 [Alpha]");
    }
    CHECK(b.record_count() == 1);
    walk(b);
    CHECK(b.servers().size() == 2);
    if (b.servers().size() == 2) {
        CHECK(b.servers()[0].record_id == 1 && b.servers()[0].server.current_players == 1);
        CHECK(b.servers()[1].record_id == 2 && b.servers()[1].server.current_players == 2);
        CHECK(b.servers()[1].host_ip == "192.168.1.20" && b.servers()[1].port == 32768);
    }
    // Another SN under the same HK replaces it too; the REMOVED line names the old SN.
    const std::vector<uint8_t> renamed = server_reply_hk("Bravo", 0x22u);
    reply(b, renamed, host);
    CHECK(lines.size() == 5);
    if (lines.size() == 5) {
        CHECK(lines[3] == "NET SESSION AT 192.168.1.20:32768 [Alpha] HAS BEEN REMOVED.");
        CHECK(lines[4] == "NET SESSION DISCOVERED AT 192.168.1.20:32768 [Bravo]");
    }
    walk(b);
    CHECK(b.servers().size() == 3 && b.servers().back().server.server_name == "Bravo");
    // The SN is the record's 63-character copy: a name differing only past it is the same.
    const std::string long_a = std::string(63, 'x') + "A";
    const std::string long_b = std::string(63, 'x') + "B";
    const std::vector<uint8_t> la = server_reply_hk(long_a, 0x33u);
    const std::vector<uint8_t> lb = server_reply_hk(long_b, 0x33u);
    const PeerAddr other = sender("192.168.1.21", 32768);
    reply(b, la, other);
    reply(b, lb, other);
    CHECK(lines.size() == 6);
    if (lines.size() == 6)
        CHECK(lines[5] == "NET SESSION DISCOVERED AT 192.168.1.21:32768 [" + std::string(63, 'x') + "]");
    walk(b);
    CHECK(b.servers().back().server.server_name == std::string(63, 'x'));

    // A session the channel filter drops: no DISCOVERED line and no row, but its
    // record's replacement writes REMOVED.
    lines.clear();
    const PeerAddr nw_host = sender("192.168.1.30", 32768);
    const std::vector<uint8_t> nw = server_reply_hk("Nw", 0x44u, 1, 0x404u);
    const std::vector<uint8_t> nw_restarted = server_reply_hk("Nw", 0x45u, 1, 0x404u);
    const size_t rows = b.servers().size();
    reply(b, nw, nw_host);
    CHECK(lines.empty());
    reply(b, nw_restarted, nw_host);
    CHECK(lines.size() == 1);
    CHECK(!lines.empty() && lines[0] == "NET SESSION AT 192.168.1.30:32768 [Nw] HAS BEEN REMOVED.");
    walk(b);
    CHECK(b.servers().size() == rows);
}

// A record no 0x81 stamps for more than 30000 ms goes while the enumerator runs, writing
// REMOVED; its row stays, and the host's next 0x81 is a new record and a new row. The
// window's end only stops the enumerator: the records stay and expire no more.
// [orig: CNapiGameSession_InitTransportConnection @0x4C9E10 - the lifetime 0x7530
//  @0x4C9FB4; CNapiNPConnection_PumpStateMachine @0x6292E0 - state 2 @0x629414, the
//  enumerator's +0x2C @0x62941E, `now - stamp > lifetime` @0x62945E..0x629468, the destroy
//  @0x62946F; UI_ProcessLANSessionStateMachine @0x558DE0 - the window's end
//  CNapiNetwork_DrainPendingDataTransfers (the call @0x559341)]
void test_records_expire_after_thirty_seconds_while_browsing() {
    std::vector<std::string> lines;
    opennova::LanDiscoveryBrowser b;
    b.set_connect_log(session_lines(lines));
    b.set_windowed(false);
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const PeerAddr alpha_host = sender("10.0.0.1", 32768);
    const PeerAddr bravo_host = sender("10.0.0.2", 32768);
    const std::vector<uint8_t> alpha = server_reply_hk("Alpha", 1u);
    const std::vector<uint8_t> bravo = server_reply_hk("Bravo", 2u);
    reply(b, alpha, alpha_host); // stamped at 0
    reply(b, bravo, bravo_host);
    bool due = false;
    CHECK(run_frame(b, 20.0, due));
    reply(b, bravo, bravo_host); // stamped again at 20 s
    CHECK(run_frame(b, 10.0, due)); // 30 s since Alpha's stamp: not past it
    CHECK(b.record_count() == 2);
    CHECK(lines.size() == 2);
    CHECK(run_frame(b, 0.001, due)); // past it: Alpha goes, Bravo stays
    CHECK(b.record_count() == 1);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) CHECK(lines[2] == "NET SESSION AT 10.0.0.1:32768 [Alpha] HAS BEEN REMOVED.");
    CHECK(b.servers().size() == 2); // Alpha's row stays
    // Alpha answers again: a new record, discovered again, and a new row beside its old one.
    reply(b, alpha, alpha_host);
    walk(b);
    CHECK(lines.size() == 4);
    CHECK(b.servers().size() == 3);
    if (b.servers().size() == 3) {
        CHECK(b.servers()[2].host_ip == "10.0.0.1" && b.servers()[2].record_id == 3);
    }

    // A windowed browse: its end stops the enumerator, and its records stay unexpired.
    lines.clear();
    opennova::LanDiscoveryBrowser w;
    w.set_connect_log(session_lines(lines));
    CHECK(w.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    CHECK(run_frame(w, 1.0, due));
    reply(w, alpha, alpha_host); // stamped at 1 s
    CHECK(!run_frame(w, 29.5, due)); // the window ends at 30.5 s
    CHECK(w.servers().size() == 1); // the last frame's walk still lists it
    CHECK(!run_frame(w, 60.0, due));
    CHECK(w.record_count() == 1);
    CHECK(lines.size() == 1); // DISCOVERED only: no expiry once stopped
}

// Every drain writes each record's REMOVED line, from the enumerator list's head, whether
// the session listed or not: a search's begin (which then empties the rows) and the LAN
// screen's or the enumerate-and-join's drain (the rows stay).
// [orig: UI_StartLANSessionSearch @0x558300 - CNapiNetwork_DrainProtocolTimers (the call
//  @0x55830F), the row table cleared @0x5583C1 / @0x5583C9; UI_InitLANMultiplayerScreen
//  @0x556930 - the call @0x5569C7; UI_EnumerateAndJoinSession @0x569F50 - the call
//  @0x56A08C; NapiNPEnumerator_DrainTimerList @0x622140 - the head destroyed @0x62215E]
void test_every_drain_writes_each_records_removed_line() {
    std::vector<std::string> lines;
    opennova::LanDiscoveryBrowser b;
    b.set_connect_log(session_lines(lines));
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const std::vector<uint8_t> lan = server_reply_hk("Lan", 1u);
    const std::vector<uint8_t> nw = server_reply_hk("Nw", 2u, 1, 0x404u);
    reply(b, lan, sender("10.0.0.1", 32768));
    reply(b, nw, sender("10.0.0.2", 32768));
    walk(b);
    CHECK(b.servers().size() == 1);
    lines.clear();
    // The next search drains, then empties its rows.
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        CHECK(lines[0] == "NET SESSION AT 10.0.0.1:32768 [Lan] HAS BEEN REMOVED.");
        CHECK(lines[1] == "NET SESSION AT 10.0.0.2:32768 [Nw] HAS BEEN REMOVED.");
    }
    CHECK(b.record_count() == 0 && b.servers().empty());
    // A drain alone (the screen's activation, a join's enumerate) keeps the rows; a drain
    // with no records writes nothing.
    reply(b, lan, sender("10.0.0.1", 32768));
    walk(b);
    b.stop();
    lines.clear();
    b.drain_records();
    CHECK(lines.size() == 1);
    CHECK(b.record_count() == 0 && b.servers().size() == 1);
    b.drain_records();
    CHECK(lines.size() == 1);
}

// The frame's order: the protocol pump's re-announce runs ahead of the window's gate, so a
// re-announce due on the window's last frame is still reported.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - CNapiNetwork_PumpTransportAndProtocol
//  (the call @0x559324) ahead of the gate @0x55933A; CNapiNPConnection_PumpFlags @0x629780 -
//  the announce @0x6297B7]
void test_a_reannounce_due_on_the_last_frame_still_goes_out() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, 1, kBroadcast, kRetailLanPortMin, kRetailLanPortMax, kLanConnectTypeLan));
    bool due = false;
    CHECK(run_frame(b, 27.5, due) && due); // re-announced at 27.5 s
    CHECK(!run_frame(b, 3.5, due));        // 31 s: the window ends this frame
    CHECK(due);                         // 3.5 s since the last announce
    CHECK(!b.browsing());
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
    // The manager's send envelopes it: 52 bytes on the wire, the 48-byte body inside (the LN S1b
    // retail joiner's 52 B datagrams to NI / NP; D-NET-1249).
    // [orig: CNapiNPConnection_SendPingPacket @0x61F8C0 - flags 1 @0x61F9B1, CNapiNPManager_SendTo
    //  called @0x61F9DA; NapiSocket_SendTo @0x62CFA0 - NapiSocket_EncodePacket called @0x62CFFF]
    CHECK(burst[0].bytes.size() == 52);
    std::vector<uint8_t> r(burst[0].bytes.size());
    std::size_t decoded = 0;
    CHECK(opennova::napi_envelope_decode(burst[0].bytes.data(), burst[0].bytes.size(), r.data(),
            r.size(), &decoded) == 0);
    r.resize(decoded);
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
    CHECK(run_frame(b, 3.001, due) && due);
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
        CHECK(run_frame(b, 0.1, due));
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

// L2-s16 FOUND 2: a join from the list's row takes the listed session at the row's
// index (the discovered callback's nodes in their discovery order), not the row's own
// record. A replaced session's node goes and its new one is appended, while its old row
// stays: the old row's index then names the next listed session, and the last row's
// index names none, a join the join machine refuses (CVUNABLETOCREATE) before anything
// is sent. A session the channel filter drops has no node and takes no index. The join
// dials the record's address with its HK and its SF, which the in-place leg keeps
// current while the row keeps the node's copy.
// [orig: UI_HandleLANGameListEvent @0x556C70 - the selected row's index @0x556BF4..0x556C21;
//  UI_EnumerateAndJoinSession @0x569F50 - the count gate @0x56A196, the node walk
//  @0x56A1AE..0x56A1D6, CVUNABLETOCREATE @0x56A1A2 / @0x56A1D8; UI_JoinSelectedSession
//  @0x5699D0 - the walk @0x569A4B..0x569A76; CNapiNetwork_StartClientConnection @0x4CA160 -
//  the record by the node's id (the call @0x4CA1EE); CNapiNPConnection_InitFromSession
//  @0x626320 - HK @0x62639C, SF @0x6263A8; Nwu_HandleServerHello @0x626D20 - the in-place
//  SF @0x627D12; NapiSession_AllocPlayerNode @0x4C7710 - the tail append @0x4C780E..0x4C783B]
void test_a_row_joins_the_listed_session_at_its_index() {
    opennova::LanDiscoveryBrowser b;
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const PeerAddr alpha_host = sender("10.0.0.1", 32768);
    const PeerAddr nw_host = sender("10.0.0.2", 32768);
    const PeerAddr bravo_host = sender("10.0.0.3", 32768);
    const std::vector<uint8_t> alpha = server_reply_hk("Alpha", 0x11u);
    const std::vector<uint8_t> nw = server_reply_hk("Nw", 0x22u, 1, 0x404u);
    const std::vector<uint8_t> bravo = server_reply_hk("Bravo", 0x33u);
    reply(b, alpha, alpha_host);
    reply(b, nw, nw_host);
    reply(b, bravo, bravo_host);
    walk(b);
    CHECK(b.servers().size() == 2);
    LanDiscoveryRow join;
    CHECK(b.join_session(0, join));
    CHECK(join.host_ip == "10.0.0.1" && join.port == 32768 && join.server.host_key == 0x11u);
    CHECK(join.record_id == 1);
    // The filtered session takes no index: row 1 is Bravo's.
    CHECK(b.join_session(1, join));
    CHECK(join.host_ip == "10.0.0.3" && join.server.host_key == 0x33u && join.record_id == 3);
    CHECK(!b.join_session(2, join));

    // Alpha restarts (another HK): its record and node are replaced, the new node at the
    // list's tail, and the walk appends its row beside the old one.
    const std::vector<uint8_t> alpha_restarted = server_reply_hk("Alpha", 0x44u, 2);
    reply(b, alpha_restarted, alpha_host);
    walk(b);
    CHECK(b.servers().size() == 3);
    if (b.servers().size() == 3) {
        CHECK(b.servers()[0].record_id == 1 && b.servers()[0].server.host_key == 0x11u);
        CHECK(b.servers()[2].record_id == 4 && b.servers()[2].server.host_key == 0x44u);
    }
    // Alpha's old row (row 0) now joins Bravo, the first listed session; row 1 (Bravo's)
    // joins the restarted Alpha; row 2 names none.
    CHECK(b.join_session(0, join));
    CHECK(join.host_ip == "10.0.0.3" && join.server.server_name == "Bravo" && join.record_id == 3);
    CHECK(b.join_session(1, join));
    CHECK(join.host_ip == "10.0.0.1" && join.server.host_key == 0x44u && join.record_id == 4);
    CHECK(join.server.current_players == 2);
    CHECK(!b.join_session(2, join));

    // Bravo's next 0x81 sets its SF under the same HK and SN: the record in place takes it
    // (the join's PW gate), the row keeps the node's first copy.
    const std::vector<uint8_t> bravo_locked = server_reply_hk("Bravo", 0x33u, 1, 0x904u, 1u);
    reply(b, bravo_locked, bravo_host);
    walk(b);
    CHECK(b.servers().size() == 3);
    if (b.servers().size() == 3) CHECK(!b.servers()[1].server.password_required);
    CHECK(b.join_session(0, join));
    CHECK(join.record_id == 3 && join.server.password_required);

    // The records stay past the window's end, and with them the joins; a drain leaves the
    // rows and no listed session, so every row's join is refused.
    b.stop();
    CHECK(b.join_session(1, join) && join.record_id == 4);
    b.drain_records();
    CHECK(b.servers().size() == 3);
    CHECK(!b.join_session(0, join));
}

// D-NET-956 (a): the browse's packet lines. Each datagram the browse's socket hands over
// writes the manager's receive lines: RAW, then TOSSED [3] for a datagram that does not
// unpack, else COOKED and the dispatch's line, PROTOCOL (the game protocol's name) for a 0x81
// the hello handler queues, TOSSED [4] for anything it does not take (a foreign CI, another
// opcode, any reply once the enumerator stopped). The protocol pump's queue pass then writes
// each queued reply's NET GOT (its length with the opcode) ahead of its record's DISCOVERED
// line; nothing is recorded before that pass.
// [orig: NapiNPManager_PumpReceive @0x623010 - cb_on_raw @0x623121, cb_on_error(3) @0x623229,
//  the cooked callback @0x623254; NapiNPManager_HandlePacket @0x622F10 - cb_on_dispatch
//  @0x622FCD, cb_on_error(4) @0x622FF9; Nwu_HandleServerHello @0x626D20 - the CI @0x627541,
//  the enumerator running @0x627559, the queue @0x627F45; NapiNPProtocol_PumpRecvQueues
//  @0x6266A0 - NapiLog_OnRecv (proto+0x26C) @0x6268AA ahead of the handler @0x6268D0]
void test_each_datagram_writes_the_managers_lines_and_net_got() {
    std::vector<std::string> lines;
    opennova::LanDiscoveryBrowser b;
    b.set_connect_log([&lines](const std::string &text) { lines.push_back(text); });
    // Before a browse began there is no protocol: nothing is written.
    const std::vector<uint8_t> alpha = server_reply_hk("Alpha", 0x11u);
    const PeerAddr host = sender("10.0.0.1", 32768);
    b.accept_reply(alpha.data(), alpha.size(), host);
    CHECK(lines.empty());
    CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const std::vector<uint8_t> foreign = server_reply("Foreign", 1, 8, 2, kBrowseCi ^ 0x100u);
    const std::vector<uint8_t> junk = {1, 2, 3};
    const std::vector<uint8_t> probe = b.probe(32768); // a 0x41: no client handler takes it
    const auto cooked = [](const std::vector<uint8_t> &datagram) {
        std::vector<uint8_t> unpacked;
        return nw_unpack_inbound(datagram.data(), datagram.size(), unpacked)
                ? std::to_string(unpacked.size())
                : std::string("?");
    };
    const std::string at = " BYTES FROM 10.0.0.1:32768";
    b.accept_reply(alpha.data(), alpha.size(), host);
    b.accept_reply(foreign.data(), foreign.size(), host);
    b.accept_reply(junk.data(), junk.size(), host);
    b.accept_reply(probe.data(), probe.size(), host);
    CHECK(b.record_count() == 0); // queued, not yet recorded
    const std::vector<std::string> received = {
        "NAPI GOT RAW " + std::to_string(alpha.size()) + at,
        "NAPI GOT COOKED " + cooked(alpha) + at,
        "NAPI GOT PROTOCOL " + cooked(alpha) + at + " [" + std::string(kJo.pn) + "]",
        "NAPI GOT RAW " + std::to_string(foreign.size()) + at,
        "NAPI GOT COOKED " + cooked(foreign) + at,
        "NAPI TOSSED " + cooked(foreign) + at + " [4]",
        "NAPI GOT RAW 3" + at,
        "NAPI TOSSED 3" + at + " [3]",
        "NAPI GOT RAW " + std::to_string(probe.size()) + at,
        "NAPI GOT COOKED " + cooked(probe) + at,
        "NAPI TOSSED " + cooked(probe) + at + " [4]",
    };
    CHECK(lines == received);
    lines.clear();
    b.pump_queue();
    CHECK(b.record_count() == 1);
    const std::vector<std::string> pumped = {
        "NET GOT " + cooked(alpha) + at + " [0x81]",
        "NET SESSION DISCOVERED AT 10.0.0.1:32768 [Alpha]",
    };
    CHECK(lines == pumped);
    // The queue is empty after its pass.
    lines.clear();
    b.pump_queue();
    CHECK(lines.empty());
    // Once the enumerator stopped, the hello handler takes nothing: TOSSED [4], no queue.
    b.stop();
    b.accept_reply(alpha.data(), alpha.size(), host);
    b.pump_queue();
    CHECK(lines.size() == 3);
    if (lines.size() == 3) CHECK(lines[2] == "NAPI TOSSED " + cooked(alpha) + at + " [4]");
}

// D-NET-956 (b): the process has one enumerator. The browse a join went on from hands its
// records to the process's (the LAN screen's) silently, keeping none, and they stay there,
// frozen, until a drain: the join's session reset, or the next search or LAN screen, each
// record writing its REMOVED line in list order.
// [orig: CNapiNetwork_StartClientConnection @0x4CA160 - the ctx's one client connection
//  @0x4CA1C2, NapiTimer_Stop @0x4CA1D8; CNapiNPConnection_Destroy @0x62A4B0 -
//  NapiNPEnumerator_DrainTimerList (the call @0x62A554)]
void test_a_join_hands_its_browse_records_to_the_process_enumerator() {
    std::vector<std::string> process_lines;
    std::vector<std::string> probe_lines;
    opennova::LanDiscoveryBrowser process;
    process.set_connect_log(session_lines(process_lines));
    opennova::LanDiscoveryBrowser probe;
    probe.set_connect_log(session_lines(probe_lines));
    CHECK(probe.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
    const std::vector<uint8_t> alpha = server_reply_hk("Alpha", 1u);
    const std::vector<uint8_t> bravo = server_reply_hk("Bravo", 2u);
    reply(probe, alpha, sender("10.0.0.1", 32768));
    reply(probe, bravo, sender("10.0.0.2", 32768));
    CHECK(probe.record_count() == 2);
    probe.stop();
    probe_lines.clear();
    process.adopt_records(probe);
    CHECK(probe.record_count() == 0 && process.record_count() == 2);
    CHECK(probe_lines.empty() && process_lines.empty());
    process.drain_records();
    const std::vector<std::string> removed = {
        "NET SESSION AT 10.0.0.1:32768 [Alpha] HAS BEEN REMOVED.",
        "NET SESSION AT 10.0.0.2:32768 [Bravo] HAS BEEN REMOVED.",
    };
    CHECK(process_lines == removed);
    CHECK(probe_lines.empty());
}

// The LAN screen's search frame pumps the browse connection's legs at its top: a due
// re-announce leaves ahead of the frame's receive and the held server's walk. The
// enumerate-and-join's loop (the unwindowed browse) reaches them in its Pump(-1) walk, after
// the receive, the queue pass and the held server's nodes.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - CNapiNetwork_PumpClientProtocolSend
//  called @0x558E0E ahead of CNapiNetwork_PumpTransportAndProtocol called @0x559324;
//  CNapiNetwork_PumpClientProtocolSend @0x4C5000 - `push 2E2h` @0x4C500B;
//  CNapiNPConnection_PumpFlags @0x629780 - the type test @0x629788..0x62978F, the 0x20 leg
//  @0x6297B7; UI_EnumerateAndJoinSession @0x569F50 - the loop's
//  CNapiNetwork_PumpTransportAndProtocol alone @0x56A0D5]
void test_the_search_frame_announces_ahead_of_its_receive() {
    for (const bool windowed : {true, false}) {
        LanDiscoveryBrowser b;
        b.set_windowed(windowed);
        CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
        std::vector<std::string> order;
        LanDiscoveryBrowser::FrameLegs legs;
        legs.send_burst = [&order] { order.push_back("announce"); };
        legs.receive = [&order] { order.push_back("receive"); };
        legs.held_walk = [&order] { order.push_back("held"); };
        // 1 s in: no re-announce yet.
        CHECK(b.frame(1.0, legs));
        CHECK((order == std::vector<std::string>{"receive", "held"}));
        order.clear();
        // 3.5 s in: the re-announce falls due.
        CHECK(b.frame(2.5, legs));
        const std::vector<std::string> expected = windowed
                ? std::vector<std::string>{"announce", "receive", "held"}
                : std::vector<std::string>{"receive", "held", "announce"};
        CHECK(order == expected);
    }
}

// The search frame's record expiry runs in the same top pump (the connection's state
// machine, the 0x40 leg after the 0x20 leg), so a record whose lifetime passes goes ahead of
// the reply the same frame reads from its host, which then makes a new record: REMOVED, then
// DISCOVERED. The enumerate-and-join's frame reads the reply first and refreshes the record
// in place, and the expiry finds it fresh.
// [orig: CNapiNPConnection_PumpFlags @0x629780 - CNapiNPConnection_PumpStateMachine (the call
//  @0x6297C3) after the 0x20 leg @0x6297B7; CNapiNPConnection_PumpStateMachine @0x6292E0 -
//  state 2 @0x629414, the running gate @0x62941E; Nwu_HandleServerHello @0x626D20 - the
//  in-place copy @0x627BFA..0x627E24, the stamp @0x627E34]
void test_a_search_frame_expires_a_record_ahead_of_its_reply() {
    for (const bool windowed : {true, false}) {
        std::vector<std::string> lines;
        LanDiscoveryBrowser b;
        b.set_connect_log(session_lines(lines));
        b.set_windowed(windowed);
        CHECK(b.begin(kJo, kBrowseCi, kBroadcast, 32768, 32768, kLanConnectTypeLan));
        const PeerAddr host = sender("10.0.0.1", 32768);
        const std::vector<uint8_t> alpha = server_reply_hk("Alpha", 1u);
        reply(b, alpha, host); // stamped at 0
        LanDiscoveryBrowser::FrameLegs legs;
        legs.receive = [&b, &alpha, &host] {
            b.accept_reply(alpha.data(), alpha.size(), host);
        };
        // 30.001 s since the stamp: past the lifetime this frame.
        b.frame(30.001, legs);
        const std::string discovered = "NET SESSION DISCOVERED AT 10.0.0.1:32768 [Alpha]";
        const std::string removed = "NET SESSION AT 10.0.0.1:32768 [Alpha] HAS BEEN REMOVED.";
        const std::vector<std::string> expected = windowed
                ? std::vector<std::string>{discovered, removed, discovered}
                : std::vector<std::string>{discovered};
        CHECK(lines == expected);
        CHECK(b.record_count() == 1);
    }
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
    test_records_replace_in_place_and_write_their_lines();
    test_records_expire_after_thirty_seconds_while_browsing();
    test_every_drain_writes_each_records_removed_line();
    test_a_reannounce_due_on_the_last_frame_still_goes_out();
    test_proxy_rendezvous_leads_each_burst();
    test_enumerate_and_join_browse_has_no_window();
    test_a_row_joins_the_listed_session_at_its_index();
    test_each_datagram_writes_the_managers_lines_and_net_got();
    test_a_join_hands_its_browse_records_to_the_process_enumerator();
    test_the_search_frame_announces_ahead_of_its_receive();
    test_a_search_frame_expires_a_record_ahead_of_its_reply();
    if (failures == 0) std::printf("lan_discovery_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
