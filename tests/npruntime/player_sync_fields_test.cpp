// D-NET-127 — the S2C 0x46 per-field values the host's player-slot table
// carries. Field 0x1000 is 1 for an active slot whose spectator latch
// (slot+100567) is set and whose loading byte (slot+100579) is clear, else 0;
// the client lands it as the roster slot's spectator byte.
// [orig: NetPacket_SerializePlayerSync0x46 @0x505E80 (@0x506197..0x5061cc);
//  NapiNPClientMsg_PlayerSync @0x431370; NapiNPServerMsg_0x022 @0x514C90]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

void test_spectator_field_follows_the_slot() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	ns::LoopbackChannel a, b, c, d;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(conn_fixture::make_conn(1, 1, &a, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // the requester, a player
	roster.push_back(conn_fixture::make_conn(2, 1, &b, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // a spectator in the match
	roster.push_back(conn_fixture::make_conn(3, 1, &c, ns::TransportMode::Client,
			world.registry.spawn(0, person), false)); // a spectator still loading
	roster.push_back(conn_fixture::make_conn(4, 1, &d, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // a player in the match
	for (size_t i = 0; i < roster.size(); ++i) {
		roster[i].reply.player_slot = static_cast<uint8_t>(i);
		roster[i].phase = inmatch::ConnectionPhase::PlayerAdded;
	}
	roster[1].link.spectator = true;
	roster[2].link.spectator = true;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const auto field_1000 = [&](uint8_t slot) -> int {
		const uint16_t fields = kPlayerSyncHasLateJoinFlag;
		for (const ProtocolMessage &m : inmatch::dispatch_session_replies(ctx.config, roster[0],
				{make_protocol_message(c2s::PLAYER_SYNC_REQUEST,
						{slot, uint8_t(fields), uint8_t(fields >> 8)})},
				100, roster, &world, inputs)) {
			if (m.tag != s2c::PLAYER_SYNC) continue;
			PlayerSync sync;
			if (!decode_player_sync(m.payload.data(), m.payload.size(), sync)) return -2;
			if (sync.removal || (sync.field_bitmask & kPlayerSyncHasLateJoinFlag) == 0) return -3;
			return sync.field_1000;
		}
		return -1;
	};
	CHECK(field_1000(0) == 0);
	CHECK(field_1000(1) == 1);
	CHECK(field_1000(2) == 0); // the loading byte is still set
	CHECK(field_1000(3) == 0);
}

// D-NET-295: on a NovaWorld host the admission JOIN's CD cookie loads the
// joiner's PCID and squad id, which its 0x7A, its slot's 0x46 fields 0x0010 /
// 0x0800 and its PUBJOINTICKET lookup read; a LAN host leaves them "" / 0.
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0; NetPacket_WritePCID
//  @0x5076e0; NetPacket_SerializePlayerSync0x46 @0x506070 / @0x506257]
std::vector<uint8_t> join_with_cookie(const std::string &key) {
	std::vector<uint8_t> squad = {0x44, 0x33, 0x22, 0x11, 'S', 'q', 0, 'T', 0};
	const std::string pcid("00000009", 9);
	std::vector<uint8_t> cd;
	const auto pair = [&](const std::string &name, const std::string &value) {
		cd.insert(cd.end(), name.begin(), name.end());
		cd.push_back(0);
		cd.insert(cd.end(), value.begin(), value.end());
		cd.push_back(0);
	};
	const auto pub = [&key](const std::vector<uint8_t> &plain) {
		std::string out;
		CHECK(encode_pub_value(plain, key, out));
		return out;
	};
	pair("PUBPCID", pub(std::vector<uint8_t>(pcid.begin(), pcid.end())));
	pair("PUBSQUADINFO", pub(squad));
	pair("PUBJOINTICKET", "TICKET");
	std::vector<uint8_t> body;
	const uint8_t crc[] = {'0', 0};
	append_flat_tlv(body, "VERSIONCRCSTRING", crc, 2);
	append_flat_tlv(body, "CD", cd.data(), static_cast<uint16_t>(cd.size()));
	return body;
}

struct AdmittedJoin {
	std::string pcid_7a;
	PlayerSync sync;
	bool synced = false;
	std::string account_pcid;
	uint32_t account_squad = 0;
};

AdmittedJoin admit_and_sync(inmatch::NetworkType transport) {
	AdmittedJoin out;
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	ns::LoopbackChannel a, b;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	ctx.transport_mode = transport;
	ctx.config.expansion.clear(); // the JOIN below carries no EXP
	ctx.config.max_players = 4;
	ctx.cookie_key_table.keys = {0x1000001u, 0x1000002u, 0, 0, 0, 0};
	ctx.cookie_key_table.count = 2;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(conn_fixture::make_conn(1, 1, &a, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // the requester
	roster.push_back(conn_fixture::make_conn(2, 1, &b, ns::TransportMode::Client,
			w::EntityHandle{}, false)); // the joiner
	roster[0].reply.player_slot = 0;
	roster[0].phase = inmatch::ConnectionPhase::PlayerAdded;
	inmatch::NapiNPConnection &joiner = roster[1];
	joiner.admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
	joiner.join_environment = {0, 2, 1, 0, 20042002, 180};
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, joiner,
			{make_protocol_message(0x00, join_with_cookie(std::to_string(0x1000002)))}, 1, roster,
			&world, inputs);
	(void)inmatch::dispatch_session_replies(ctx.config, joiner,
			{make_protocol_message(0x01, {0x00})}, 2, roster, &world, inputs);
	std::vector<uint8_t> echo(256, 0);
	for (int i = 0; i < 4; ++i) {
		echo[i] = static_cast<uint8_t>(joiner.admission_padding_x >> (8 * i));
		echo[4 + i] = static_cast<uint8_t>(joiner.admission_padding_y >> (8 * i));
	}
	for (const ProtocolMessage &m : inmatch::dispatch_session_replies(ctx.config, joiner,
			{make_protocol_message(0x02, echo)}, 3, roster, &world, inputs)) {
		if (m.tag == s2c::PLAYER_NAME && !m.payload.empty())
			out.pcid_7a.assign(reinterpret_cast<const char *>(m.payload.data()), m.payload.size() - 1);
	}
	out.account_pcid = joiner.account.pcid;
	out.account_squad = joiner.account.squad_id;
	joiner.link.owned_entity = world.registry.spawn(0, person);
	joiner.reply.player_slot = 1;
	joiner.phase = inmatch::ConnectionPhase::PlayerAdded;
	const uint16_t fields = kPlayerSyncHasPcid | kPlayerSyncHasAccountId;
	for (const ProtocolMessage &m : inmatch::dispatch_session_replies(ctx.config, roster[0],
			{make_protocol_message(c2s::PLAYER_SYNC_REQUEST,
					{1, uint8_t(fields), uint8_t(fields >> 8)})},
			100, roster, &world, inputs)) {
		if (m.tag == s2c::PLAYER_SYNC)
			out.synced = decode_player_sync(m.payload.data(), m.payload.size(), out.sync);
	}
	return out;
}

void test_novaworld_account_fields_follow_the_join_cookie() {
	const AdmittedJoin nw = admit_and_sync(inmatch::NetworkType::NovaWorld);
	CHECK(nw.account_pcid == "00000009");
	CHECK(nw.account_squad == 0x11223344u);
	CHECK(nw.pcid_7a == "00000009");
	CHECK(nw.synced);
	CHECK(nw.sync.id_label == "00000009");
	CHECK(nw.sync.account_id == 0x11223344u);
	const AdmittedJoin lan = admit_and_sync(inmatch::NetworkType::Lan);
	CHECK(lan.account_pcid.empty() && lan.account_squad == 0 && lan.pcid_7a.empty());
	CHECK(lan.synced && lan.sync.id_label.empty() && lan.sync.account_id == 0);
}

void test_join_ticket_rides_the_admission_join() {
	auto heap = std::make_unique<w::World>();
	ns::LoopbackChannel a;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = heap.get();
	ctx.is_authority = ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	ctx.config.expansion.clear();
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(conn_fixture::make_conn(2, 1, &a, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	roster[0].admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
	roster[0].join_environment = {0, 2, 1, 0, 20042002, 180};
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, roster[0],
			{make_protocol_message(0x00, join_with_cookie("1"))}, 1, roster, heap.get(), inputs);
	bool ticket = false;
	for (const auto &pair : roster[0].join_identity_pairs)
		ticket = ticket || (pair.first == "PUBJOINTICKET" && pair.second == "TICKET");
	CHECK(ticket); // the remote joiner's CD cookie is kept at its admission JOIN
}

} // namespace

int main() {
	test_spectator_field_follows_the_slot();
	test_novaworld_account_fields_follow_the_join_cookie();
	test_join_ticket_rides_the_admission_join();
	std::printf("npruntime_player_sync_fields: %d failures\n", failures);
	return failures ? 1 : 0;
}
