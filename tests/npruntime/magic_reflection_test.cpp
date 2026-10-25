// The NovaWorld host's magic-packet reflection (D-NET-NEW-L1-20).
//
// Retail's session create arms the game protocol's reflection (+0x52C = 1) and draws its key
// (+0x530) for a NovaWorld authority in a session, as NapiNPTimer_GenerateRandomId draws it:
// (GetTickCount() + rand()) % 900000 + 100000, the rand after the round init's (the slot table's
// seed and draw, the listing's GameServerBaffleKey); any other create zeroes both. The NAPI
// manager hands a magic packet (unpacked words 0x1071 and '0', 48 bytes or more) to
// NapiNP_HandleMagicPacket, which answers the address and port the packet names at +4 / +8 with
// the four bytes 72 10 04 00 through the socket's envelope when the packet's dword at +12 is an
// armed protocol's key, and answers nothing otherwise.
//
// Pinned here, core-only (synthetic packets, a recording socket, no retail data):
//   - a NovaWorld authority's create arms the key from the stream's draw after the round init's,
//     a LAN host's and a single player's arm nothing and spend no draw;
//   - a matching magic packet is answered at the named address, a mismatched key or an unarmed
//     host answers nothing, and no handler takes the packet;
//   - the session owner sends the answer from inside its receive, to the named address.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 (@0x4C9B05..0x4C9B3F, the draw @0x4C9B26);
//  NapiNPTimer_GenerateRandomId @0x61E510; CNapiGameSession_BuildAndCreateSession @0x5694D0 -
//  the round init's call @0x5695B3 ahead of the create's @0x56997D; NapiNPManager_HandlePacket
//  @0x622F10 (the magic test @0x622F1B..0x622F36, the NapiNP_HandleMagicPacket call @0x622F62);
//  NapiNP_HandleMagicPacket @0x622E70 (@0x622EA6..0x622EB8, CNapiNPManager_SendTo(.., 4, 1)
//  @0x622EF8);
//  NapiSocket_SendTo @0x62CFA0 (NapiSocket_EncodePacket @0x62C8F0)]
#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_rand.h>
#include <net/napi/envelope.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace opennova;

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

constexpr uint32_t kStartTick = 100000u;
constexpr uint32_t kSeed = 654321u;

// A session create on a context with a World (the stream's draws), its network type given.
struct Created {
	std::unique_ptr<world::World> world = std::make_unique<world::World>(); // heap: 1 MB stack
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
};

std::unique_ptr<Created> create(inmatch::ConnectionMode mode, inmatch::SocketMode socket,
		inmatch::NetworkType network) {
	auto c = std::make_unique<Created>();
	c->ctx->world = c->world.get();
	c->ctx->transport_mode = network;
	inmatch::set_connection_mode(*c->ctx, mode);
	inmatch::set_transport_mode(*c->ctx, socket);
	inmatch::GameConfig settings;
	settings.server_name = "Magic";
	settings.max_players = 8;
	inmatch::SessionStartup startup;
	startup.title = &kJo;
	startup.host_key = 0x12345678u;
	startup.host_start_tick = kStartTick;
	startup.session_seed_id = kSeed;
	inmatch::create_session(*c->ctx, settings, startup, nullptr);
	return c;
}

// The stream after the create's seed and `draws` draws, and the draw at that index.
uint32_t draw_at(int index, io::CrtRand &stream) {
	stream.seed(kSeed);
	uint32_t d = 0;
	for (int i = 0; i <= index; ++i) d = stream.next();
	return d;
}

void test_create_arms_a_novaworld_authority() {
	const auto nw = create(inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::NovaWorldSocket,
			inmatch::NetworkType::NovaWorld);
	// The stream: the slot table's draw (0), the listing key's (1), the cookie-key ring's init (2,
	// D-NET-NEW-L9-13: CSessionIdRing_Init's one rand @0x4DBC45 after the key @0x51CB6D), the magic key's (3).
	io::CrtRand stream;
	const uint32_t draw = draw_at(3, stream);
	CHECK(nw->ctx->np_protocol.magic_reflect == 1);
	CHECK(nw->ctx->np_protocol.magic_key == (kStartTick + draw) % 0xDBBA0u + 0x186A0u);
	CHECK(nw->ctx->np_protocol.magic_key >= 100000u && nw->ctx->np_protocol.magic_key <= 999999u);
	CHECK(nw->world->crt_rand.state == stream.state);
	// The listing's key is the round init's draw, ahead of the create's.
	io::CrtRand listing;
	CHECK(nw->ctx->game_server_baffle_key ==
			(kStartTick + draw_at(1, listing)) % 0xDBBA0u + 0x186A0u);

	// A LAN host and a single player arm nothing and spend no draw past the round init's two (the
	// listing key and the ring's init).
	const std::unique_ptr<Created> others[2] = {
			create(inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
					inmatch::NetworkType::Lan),
			create(inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
					inmatch::NetworkType::Lan)};
	for (const std::unique_ptr<Created> &other : others) {
		io::CrtRand after_init;
		(void)draw_at(2, after_init);
		CHECK(other->ctx->np_protocol.magic_reflect == 0);
		CHECK(other->ctx->np_protocol.magic_key == 0);
		CHECK(other->world->crt_rand.state == after_init.state);
	}
}

// A magic packet as the socket carries it: the unpacked words 0x1071 and 0x30, the answer's
// address at +4 and port at +8, the key at +12, 48 bytes, in the socket's envelope.
std::vector<uint8_t> magic_datagram(const PeerAddr &answer_to, uint32_t key) {
	std::vector<uint8_t> unpacked(0x30, 0);
	unpacked[0] = 0x71;
	unpacked[1] = 0x10;
	unpacked[2] = 0x30;
	unpacked[3] = 0x00;
	for (int i = 0; i < 4; ++i) unpacked[4 + i] = static_cast<uint8_t>(answer_to.ip >> (8 * i));
	unpacked[8] = static_cast<uint8_t>(answer_to.port & 0xFFu);
	unpacked[9] = static_cast<uint8_t>(answer_to.port >> 8);
	for (int i = 0; i < 4; ++i) unpacked[12 + i] = static_cast<uint8_t>(key >> (8 * i));
	std::vector<uint8_t> raw(unpacked.size() + 4);
	std::size_t n = 0;
	(void)napi_envelope_encode(unpacked.data(), unpacked.size(), raw.data(), raw.size(), &n);
	raw.resize(n);
	return raw;
}

// The answer, out of its envelope.
std::vector<uint8_t> unwrap(const std::vector<uint8_t> &datagram) {
	std::vector<uint8_t> out(datagram.size());
	std::size_t n = 0;
	if (napi_envelope_decode(datagram.data(), datagram.size(), out.data(), out.size(), &n) != 0)
		return {};
	out.resize(n);
	return out;
}

const PeerAddr kSender = peer_addr_from_octets({203, 0, 113, 7}, 1201);
const PeerAddr kNamed = peer_addr_from_octets({198, 51, 100, 23}, 32123);

void test_a_matching_packet_is_answered_at_the_named_address() {
	const auto nw = create(inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::NovaWorldSocket,
			inmatch::NetworkType::NovaWorld);
	const uint32_t key = nw->ctx->np_protocol.magic_key;
	const std::vector<uint8_t> raw = magic_datagram(kNamed, key);
	inmatch::HandleResult r =
			inmatch::handle_server_datagram(*nw->ctx, kSender, raw.data(), raw.size(), 0);
	CHECK(r.addressed_outbound.size() == 1);
	if (r.addressed_outbound.size() == 1) {
		CHECK(r.addressed_outbound[0].to == kNamed);
		CHECK(unwrap(r.addressed_outbound[0].datagram) ==
				(std::vector<uint8_t>{0x72, 0x10, 0x04, 0x00}));
		CHECK(r.addressed_outbound[0].datagram.size() == 8); // the envelope's four bytes ahead
	}
	CHECK(r.outbound.empty() && r.immediate_outbound.empty());
	CHECK(!r.taken); // the magic packet goes to no opcode handler

	// Another key answers nothing.
	const std::vector<uint8_t> wrong = magic_datagram(kNamed, key + 1);
	r = inmatch::handle_server_datagram(*nw->ctx, kSender, wrong.data(), wrong.size(), 0);
	CHECK(r.addressed_outbound.empty());

	// An unarmed host answers nothing, its zero key included.
	const auto lan = create(inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			inmatch::NetworkType::Lan);
	const std::vector<uint8_t> zero = magic_datagram(kNamed, 0);
	r = inmatch::handle_server_datagram(*lan->ctx, kSender, zero.data(), zero.size(), 0);
	CHECK(r.addressed_outbound.empty());
}

struct RecordingSocket final : opennova::IDatagramSocket {
	std::deque<std::vector<uint8_t>> inbound;
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
		const std::vector<uint8_t> d = std::move(inbound.front());
		inbound.pop_front();
		const std::size_t n = d.size() < cap ? d.size() : cap;
		for (std::size_t i = 0; i < n; ++i) buf[i] = d[i];
		from = kSender;
		return static_cast<int>(n);
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

// The session owner's receive sends the answer to the address the packet named.
void test_the_owner_sends_the_answer_to_the_named_address() {
	auto world = std::make_unique<world::World>();
	auto owner = std::make_unique<inmatch::HostOwner>();
	owner->ctx.world = world.get();
	inmatch::HostConfig cfg;
	cfg.title = &kJo;
	cfg.socket_mode = inmatch::SocketMode::NovaWorldSocket;
	cfg.network_type = inmatch::NetworkType::NovaWorld;
	cfg.config.max_players = 8;
	cfg.serve_and_play = false;
	cfg.host_key = 0x12345678u;
	cfg.host_start_tick = kStartTick;
	cfg.session_seed_id = kSeed;
	(void)inmatch::start_host_session(*owner, cfg);
	// The session onto its World (the create reads none): its rules, the own add.
	inmatch::bind_host_session_world(*owner, cfg);
	CHECK(owner->ctx.np_protocol.magic_reflect == 1);
	RecordingSocket socket;
	socket.inbound.push_back(magic_datagram(kNamed, owner->ctx.np_protocol.magic_key));
	inmatch::host_session_load_pump(*owner, socket);
	CHECK(socket.sent.size() == 1);
	if (socket.sent.size() == 1) {
		CHECK(socket.sent[0].first == kNamed);
		CHECK(unwrap(socket.sent[0].second) == (std::vector<uint8_t>{0x72, 0x10, 0x04, 0x00}));
	}
}

} // namespace

int main() {
	test_create_arms_a_novaworld_authority();
	test_a_matching_packet_is_answered_at_the_named_address();
	test_the_owner_sends_the_answer_to_the_named_address();
	if (failures != 0) {
		std::printf("npruntime_magic_reflection: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_magic_reflection: ok\n");
	return 0;
}
