#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <cstdio>
#include <utility>

using namespace opennova;

namespace {
bool expect(bool condition, const char *message) {
	if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

// Exercise the real auth/framing seam: ServerAuth.MI is the connection identity,
// distinct from the roster index, entity handle, net ID and displayed callsign.
// [orig: Player_FindLocalPlayerEntity @0x4E0090; NapiNPClientMsg_0x00C @0x42E730]
bool run_identity() {
	inmatch::JoinerConnection joiner("SharedCallsign");
	const std::string server_scrk = "IDENTITY-SERVER-SCRK";
	constexpr uint32_t connection_id = 0x12345678u;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	auto datagram = joiner.start();
	if (!expect(nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			parse_client_hello(body.data(), body.size(), hello), "decode ClientHello")) return false;
	auto server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(server_hello));
	auto result = joiner.handle_datagram(datagram.data(), datagram.size());
	ClientAuth auth;
	if (!expect(result.outbound.size() == 1 &&
			nw_decode_inbound(result.outbound[0].data(), result.outbound[0].size(), opcode, body) &&
			parse_client_auth(body.data(), body.size(), auth), "decode ClientAuth")) return false;
	auto server_auth = build_server_auth(auth, 0x7F000001u, 32769, 0x11223344u,
			server_scrk, "", "", "", false);
	server_auth.mi = connection_id;
	datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	joiner.handle_datagram(datagram.data(), datagram.size());
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto receive = [&](uint8_t tag, std::vector<uint8_t> payload) {
		std::vector<uint8_t> frame;
		frame_session_packet(seq, SessionCrypto{server_scrk, {}, auth.ck},
				{make_protocol_message(tag, std::move(payload))}, frame);
		auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(frame));
		return joiner.handle_datagram(packet.data(), packet.size());
	};
	auto spawn = [&](uint16_t handle, uint32_t owner, uint16_t flags, const char *name,
			uint8_t def_type = 3) { // ItemDefType person; 0 = empty spawn
		OrganicSpawnRecord record;
		record.slot_id = handle;
		record.def_type = def_type;
		record.item_type_id = 0x14B9;
		record.owner_connection_id = owner;
		record.minimap_flags = flags;
		record.entity_name = name;
		record.pos_x = 123 << 16;
		OrganicSpawnBatch batch;
		batch.entity_count = 1;
		batch.records.push_back(record);
		return receive(s2c::ENTITY_SPAWN_BATCH, encode_organic_spawn_batch(batch));
	};
	spawn(7, connection_id + 1, 0x100, "SharedCallsign");
	if (!expect(!joiner.has_self_handle(), "same callsign with a different owner is remote")) return false;
	spawn(8, connection_id, 0, "SharedCallsign");
	if (!expect(!joiner.has_self_handle(), "owned non-player is not the local player")) return false;
	spawn(0x1009, connection_id, 0x100, "SharedCallsign");
	if (!expect(!joiner.has_self_handle(), "only pool zero supplies the local player")) return false;
	spawn(10, connection_id, 0x100, "ServerRenamed");
	if (!expect(joiner.has_self_handle() && joiner.self_handle() == 10 && !joiner.in_match(),
			"numeric identity binds despite a renamed callsign, without bypassing deployment")) return false;
	// The full deployment exchange is covered by npruntime_client_runtime. Seed
	// that completed phase here to exercise the same receive path after release.
	joiner.seed_in_match(server_auth.sk, auth.ck, auth.scrk, server_scrk,
			1, 0, 10, 0x14B9, 0);
	seq = inmatch::make_jo_game_session_sequencing();
	spawn(11, connection_id + 2, 0x100, "SharedCallsign");
	if (!expect(joiner.self_handle() == 10, "late duplicate callsign cannot steal identity")) return false;
	spawn(10, connection_id + 3, 0x100, "Replacement");
	if (!expect(!joiner.has_self_handle(), "slot reuse by another connection retires self identity")) return false;
	spawn(12, connection_id, 0x100, "ServerRenamedAgain");
	if (!expect(joiner.has_self_handle() && joiner.self_handle() == 12,
			"local player can rebind after slot reuse")) return false;
	receive(s2c::EMPTY_SLOT_SWEEP, {12, 0});
	if (!expect(!joiner.has_self_handle(), "empty-slot sweep retires self identity")) return false;
	spawn(13, connection_id, 0x100, "SharedCallsign");
	spawn(13, 0, 0, "", 0);
	if (!expect(!joiner.has_self_handle(), "empty organic record retires self identity")) return false;
	bool lifecycle_ok = true;
	spawn(14, connection_id, 0x100, "Local");
	receive(s2c::ENTITY_REMOVE, {14, 0});
	lifecycle_ok = expect(!joiner.has_self_handle(),
			"single-entity removal retires the local binding") && lifecycle_ok;

	FullEntitySpawnRecord repair;
	repair.slot_id = 15;
	repair.item_type = 3;
	repair.item_type_id = 0x14B9;
	repair.entity_flags = connection_id;
	repair.minimap_flags = 0x100;
	repair.entity_name = "RepairedLocal";
	receive(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(repair));
	lifecycle_ok = expect(joiner.has_self_handle() && joiner.self_handle() == 15,
			"full repair binds an owned player without a new organic page") && lifecycle_ok;
	spawn(15, connection_id, 0x100, "Local");
	repair.entity_flags = connection_id + 1;
	receive(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(repair));
	lifecycle_ok = expect(!joiner.has_self_handle(),
			"full repair replacing the owner retires the binding") && lifecycle_ok;
	spawn(15, connection_id, 0x100, "Local");
	repair.item_type = 0;
	receive(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(repair));
	lifecycle_ok = expect(!joiner.has_self_handle(),
			"empty full repair retires the binding") && lifecycle_ok;

	OrganicSpawnBatch partial;
	OrganicSpawnRecord first;
	first.slot_id = 16;
	first.def_type = 3; // ItemDefType person
	first.item_type_id = 0x14B9;
	first.owner_connection_id = connection_id;
	first.minimap_flags = 0x100;
	partial.records = {first, first};
	partial.records.back().slot_id = 17;
	partial.entity_count = 2;
	auto truncated = encode_organic_spawn_batch(partial);
	truncated.pop_back();
	receive(s2c::ENTITY_SPAWN_BATCH, std::move(truncated));
	lifecycle_ok = expect(joiner.has_self_handle() && joiner.self_handle() == 16,
			"complete organic prefix updates identity but incomplete tail cannot") && lifecycle_ok;
	spawn(0x0FFEu, connection_id, 0x100, "OutOfPoolBounds");
	lifecycle_ok = expect(joiner.has_self_handle() && joiner.self_handle() == 16,
			"out-of-capacity player handles cannot replace identity") && lifecycle_ok;
	partial.records = {first, first};
	partial.records.front().slot_id = 0x0FFEu;
	partial.records.back().slot_id = 17;
	receive(s2c::ENTITY_SPAWN_BATCH, encode_organic_spawn_batch(partial));
	lifecycle_ok = expect(joiner.self_handle() == 16,
			"an out-of-capacity slot stops identity processing for the rest of the page") && lifecycle_ok;
	joiner.start();
	lifecycle_ok = expect(!joiner.has_self_handle() && !joiner.in_match(),
			"new session resets identity") && lifecycle_ok;
	joiner.seed_in_match(server_auth.sk, auth.ck, auth.scrk, server_scrk,
			1, 0, 18, 0x14B9, 0);
	seq = inmatch::make_jo_game_session_sequencing();
	spawn(19, 0, 0x100, "UnknownOwner");
	spawn(18, 0, 0x100, "CapturedLocal");
	return expect(joiner.has_self_handle() && joiner.self_handle() == 18,
			"an unauthenticated replay retains its explicit handle, never infers owner zero") &&
			lifecycle_ok;
}
} // namespace

int main() { return run_identity() ? 0 : 1; }
