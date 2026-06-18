#include <novaworld/game_session.h>

#include <novaworld/ingame_decode.h>
#include <novaworld/retail_blobs.h>
#include <novaworld/retail_loading_blobs.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace opennova {

namespace {

constexpr int kTag10IntervalMs = 250;
constexpr int kTag0aIntervalMs = 300;
constexpr int kTag57IntervalMs = 1000;

void append_u16_le(std::vector<uint8_t> &out, uint16_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void write_u32_le(std::vector<uint8_t> &out, size_t off, uint32_t v) {
	out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
	out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
	out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
	out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t read_u32_le(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) |
			(static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) |
			(static_cast<uint32_t>(p[3]) << 24);
}

void append_cstr(std::vector<uint8_t> &out, const std::string &s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

void append_cstr(std::vector<uint8_t> &out, const char *s) {
	while (*s) {
		out.push_back(static_cast<uint8_t>(*s++));
	}
	out.push_back(0);
}

void append_kv(std::vector<uint8_t> &out,
               const char *name,
               const void *data,
               uint32_t len) {
	const size_t nlen = std::strlen(name);
	out.insert(out.end(), name, name + nlen);
	out.push_back(0);
	append_u32_le(out, len);
	const auto *bytes = static_cast<const uint8_t *>(data);
	out.insert(out.end(), bytes, bytes + len);
}

void append_string_kv(std::vector<uint8_t> &out,
                      const char *name,
                      const std::string &value) {
	append_kv(out, name, value.c_str(), static_cast<uint32_t>(value.size() + 1));
}

void add_reply(GameSessionDispatchResult &result,
               uint8_t tag,
               std::vector<uint8_t> payload = {}) {
	result.replies.push_back(make_protocol_message(tag, std::move(payload)));
}

void queue_reply(GameSessionState &state,
                 uint8_t tag,
                 std::vector<uint8_t> payload = {}) {
	state.queued_replies.push_back(make_protocol_message(tag, std::move(payload)));
}

void queue_custom_reply(GameSessionState &state,
                        uint8_t flags,
                        uint8_t tag,
                        std::vector<uint8_t> payload = {}) {
	state.queued_replies.push_back(make_protocol_message(tag, std::move(payload), flags));
}

void queue_blob_reply(GameSessionState &state,
                      uint8_t tag,
                      const retail_loading_blobs::RetailBlobRef &blob) {
	queue_reply(state, tag, std::vector<uint8_t>(blob.data, blob.data + blob.size));
}

void add_blob_reply(GameSessionDispatchResult &result,
                    uint8_t tag,
                    const retail_loading_blobs::RetailBlobRef &blob) {
	add_reply(result, tag, std::vector<uint8_t>(blob.data, blob.data + blob.size));
}

size_t flush_queued_replies(GameSessionState &state,
                            GameSessionDispatchResult &result,
                            size_t max_replies) {
	const size_t n = std::min(max_replies, state.queued_replies.size());
	for (size_t i = 0; i < n; ++i) {
		if (state.queued_replies[i].tag == 0x11) {
			state.mission_disconnect_ack_sent = true;
		}
		result.replies.push_back(std::move(state.queued_replies[i]));
	}
	state.queued_replies.erase(state.queued_replies.begin(), state.queued_replies.begin() + static_cast<std::ptrdiff_t>(n));
	return n;
}

bool queued_replies_have_tag(const GameSessionState &state, uint8_t tag) {
	return std::any_of(state.queued_replies.begin(), state.queued_replies.end(),
			[tag](const ProtocolMessage &msg) {
				return msg.tag == tag;
			});
}

void add_custom_reply(GameSessionDispatchResult &result,
                      uint8_t flags,
                      uint8_t tag,
                      std::vector<uint8_t> payload) {
	result.replies.push_back(make_protocol_message(tag, std::move(payload), flags));
}

size_t count_replies_with_tag(const GameSessionDispatchResult &result, uint8_t tag) {
	size_t count = 0;
	for (const ProtocolMessage &reply : result.replies) {
		if (reply.tag == tag) {
			++count;
		}
	}
	return count;
}

// tag=0x02 client-handler witnessed at `NapiNPClientMsg_0x002 @ 0x42E0F0`
// (Jointops). The IDA decompile expects 12 bytes (3×u32 read as spawn X/Y/Z),
// then the client replies via `NetPacket_WritePositionWithPadding(buf, 4096, X,
// Y, z, Z)` queued as another tag 0x02 with `CNapiNetwork_QueueReliableMessage`.
// **MISALIGNMENT (tracked in notes/ida_witness_matrix.md):** we send 512 bytes
// here, only the first 12 of which IDA reads — the rest are silently ignored.
// Phase D.0.26 captured-from-observation pad. Should be reduced to 12 bytes
// `[u32 X][u32 Y][u32 Z]` once we wire spawn coords through.
std::vector<uint8_t> build_tag02_push(uint32_t now_tick) {
	std::vector<uint8_t> payload(512, 0);
	write_u32_le(payload, 0, now_tick);
	payload[4] = 0x02; // dword_C86FCC observed value.
	payload[8] = 0x00; // requested client reply size = 256.
	payload[9] = 0x01;
	return payload;
}

// tag=0x00 client-handler is a 1-instruction no-op (`MEMORY[0xA86C24]=1`) at
// `NapiNPClientMsg_0x000 @ 0x42E0E0`. Server-side handler at
// `NapiNPServerMsg_0x000 @ 0x512AA0` is a 0x424-byte JOIN handler — that's
// the one to mirror when our SERVER receives tag=0x00. The flags=0xA0 + custom
// reply payload below was Phase D.0.26.9 captured-from-observation; client
// silently absorbs whatever bytes we send.
void add_tag00_handshake(GameSessionDispatchResult &result, uint8_t idx) {
	add_custom_reply(result, 0xA0, 0x00, {
			idx,
			0x08, 0x00, 0x00, 0x00,
			0x0C, 0x00, 0x00, 0x00,
	});
}

// tag=0x7B PLAYER-PROFILE — `NapiNPClientMsg_0x07B @ 0x429BB0` (Jointops).
// **MISALIGNMENT:** IDA expects 7 cstrings + 1 u32 (in order: cstr1..cstr5,
// u32, cstr6 (512B), cstr7 (512B)). Our format is 6 cstrings + u32 + u8 +
// cstring — off by one cstring with a stray u8. The stray 0x00 makes IDA read
// v37 as empty and our "expansion" as v38 (info field), losing one slot.
std::vector<uint8_t> build_tag7b_session_summary(const GameSessionConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.player_name);
	append_cstr(payload, "DEV-A02-0001");
	append_cstr(payload, cfg.server_name);
	append_cstr(payload, cfg.mission_name);
	append_cstr(payload, cfg.mission_file);
	append_u32_le(payload, cfg.gametype);
	payload.push_back(0);
	append_cstr(payload, cfg.expansion);
	return payload;
}

std::vector<uint8_t> build_tag7a_player_name(const GameSessionConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.player_name);
	return payload;
}

// tag=0x60 chunked KV-pair transfer — handler at
// `NapiNPClientMsg_HandleFileTransferChunk @ 0x432350` (Jointops). Expects
// `[u32 chk=1][u32 total_size][u32 offset][chunk_data]`; client appends to
// CDataStream until offset+chunk_size == total_size, then external code parses
// KV pairs from the assembled buffer. Our 12-byte prefix matches the chunk
// header; we send a single chunk with offset=0.
std::vector<uint8_t> build_tag60_server_info(const GameSessionConfig &cfg) {
	std::vector<uint8_t> info_body;
	append_string_kv(info_body, "SERVERNAME", cfg.server_name);
	append_string_kv(info_body, "MISSIONNAME", cfg.mission_name);
	uint8_t gametype_le[4] = {
			static_cast<uint8_t>(cfg.gametype & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 8) & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 16) & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 24) & 0xFFu),
	};
	append_kv(info_body, "GAMETYPE", gametype_le, 4);
	const std::string custom_text = "OpenNova dev server.";
	append_string_kv(info_body, "CUSTOMTEXT", custom_text);
	append_string_kv(info_body, "MISSIONFILENAME", cfg.mission_file);
	const uint8_t exp_fanfare[] = {0, 0};
	append_kv(info_body, "EXP_FANFARE", exp_fanfare, 2);

	std::vector<uint8_t> reply(12, 0);
	reply[0] = 0x01; // offset=1.
	write_u32_le(reply, 4, static_cast<uint32_t>(info_body.size()));
	reply.insert(reply.end(), info_body.begin(), info_body.end());
	return reply;
}

// tag=0x64 chunked mission-metadata transfer — handler at
// `NapiNPClientMsg_0x064 @ 0x432410` (Jointops). Same chunk-transfer shape as
// tag 0x60, but writes to `&unk_24D1E08 + offset`; on completion, client
// extracts mission strings at fixed offsets +0x34 (server_name), +0x54
// (mission_file), +0x74 (mission_file). Our 180-byte mission_blob places
// strings at exactly those offsets via `write_str(52..)`, `write_str(84..)`,
// `write_str(116..)`. HASH_PREFIX/SUFFIX bytes are observed-from-capture
// filler that IDA doesn't read.
std::vector<uint8_t> build_tag64_mission_metadata(const GameSessionConfig &cfg) {
	std::vector<uint8_t> mission_blob(180, 0);
	auto write_str = [&](size_t off, const std::string &s) {
		const size_t n = std::min<size_t>(s.size(), 31);
		std::memcpy(mission_blob.data() + off, s.data(), n);
	};

	static const uint8_t HASH_PREFIX[32] = {
		0x79, 0x09, 0x05, 0x38, 0xf6, 0x29, 0x3d, 0x87,
		0xd1, 0xc8, 0xda, 0x39, 0xa0, 0x1d, 0xa8, 0xae,
		0xa9, 0x84, 0x1f, 0xa2, 0xc6, 0xf9, 0x73, 0x96,
		0xc7, 0x39, 0x03, 0xe5, 0xb2, 0xf9, 0x83, 0x61,
	};
	static const uint8_t HASH_SUFFIX[32] = {
		0xdd, 0xa8, 0xe1, 0x6b, 0x7c, 0x7a, 0x34, 0x5c,
		0x0c, 0x07, 0x7d, 0x26, 0x8e, 0x93, 0x53, 0x19,
		0xbd, 0x0e, 0x16, 0xff, 0x69, 0x0a, 0x9b, 0x37,
		0x46, 0x4f, 0xb3, 0xbe, 0x95, 0x3a, 0x02, 0x3b,
	};
	std::memcpy(mission_blob.data() + 0, HASH_PREFIX, 32);
	write_u32_le(mission_blob, 32, 5705);
	write_u32_le(mission_blob, 36, 2);
	mission_blob[40] = 0x10;
	mission_blob[42] = 0x01;
	write_u32_le(mission_blob, 44, cfg.mpattrib);
	write_u32_le(mission_blob, 48, 1);
	write_str(52, cfg.server_name);
	write_str(84, cfg.mission_file);
	write_str(116, cfg.mission_file);
	std::memcpy(mission_blob.data() + 148, HASH_SUFFIX, 32);

	std::vector<uint8_t> reply(12 + mission_blob.size(), 0);
	reply[0] = 0x01;
	write_u32_le(reply, 4, static_cast<uint32_t>(mission_blob.size()));
	std::memcpy(reply.data() + 12, mission_blob.data(), mission_blob.size());
	return reply;
}

std::vector<uint8_t> build_tag2c_chat_entry(const GameSessionConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.server_name);
	append_cstr(payload, cfg.mission_file);
	return payload;
}

std::vector<uint8_t> build_tag2a_chat_history_retail_fixture() {
	return {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};
}

// tag=0x08 client-handler at `NapiNPClientMsg_0x008 @ 0x4281D0` (Jointops).
// (Earlier code cited `@ 0x42C180 (Client_ProcessNetworkFrame)` — that's an
// adjacent function, not the dispatcher entry; corrected here.) The 51-byte
// fixture is captured-from-observation; verify each field against IDA before
// relying on individual values.
std::vector<uint8_t> build_tag08_game_state_snapshot() {
	return {
			0x1e, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00,
			0x64, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
			0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
			0x04, 0x00, 0x00,
	};
}

// tag=0x0C FULL-ENTITY-SPAWN — handler `NapiNPClientMsg_0x00C @ 0x42E730`
// (Jointops). (Earlier code cited `@ 0x433400` which is the **tag 0x10**
// ENTITY-BATCH handler — wrong tag handler entirely; the wire structure
// produced here matches tag 0x0C: count + slot_id + full-data flag + type +
// MI + name + flags + XYZ + tail with 0xFFFF parent-slot terminator.) IDA
// confirms this layout against `NapiNPClientMsg_0x00C` decompile.
std::vector<uint8_t> build_tag0c_local_player_entity(const GameSessionConfig &cfg) {
	std::vector<uint8_t> spawn;
	append_u16_le(spawn, 1);      // count.
	append_u16_le(spawn, 0);      // slot_id: pool 0, slot 0.
	spawn.push_back(1);           // full data.
	append_u16_le(spawn, 0x14B9); // player item type.
	append_u32_le(spawn, 0x3CDE); // ServerAuth MI.
	append_cstr(spawn, cfg.player_name);
	append_u16_le(spawn, 0x0100); // PLAYER flag.
	append_u32_le(spawn, cfg.spawn_x);
	append_u32_le(spawn, cfg.spawn_y);
	append_u32_le(spawn, cfg.spawn_z);
	append_u32_le(spawn, 0x283a0000u);
	const uint8_t tail[] = {
			0x01,       // team 1.
			0x00,
			0x01,
			0x00, 0x02,
			0x08,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0xff, 0xff,
	};
	spawn.insert(spawn.end(), tail, tail + sizeof(tail));
	return spawn;
}

// tag=0x42 client-handler `NapiNPClientMsg_0x042 @ 0x4281A0` (Jointops).
// 2-byte fixture captured-from-observation; semantic role per IDA pending —
// kong has no descriptive name, just the index suffix.
std::vector<uint8_t> build_tag42_spawn_gate_retail_fixture() {
	return {0x00, 0x00};
}

// tag=0x4D SPAWN-SLOT notification — `NapiNPClientMsg_HandleSpawnSlot @
// 0x4317B0` (Jointops). 1-byte payload = player slot index. Handler clears
// player slot type/subtype; for non-local-player slots also queues tag=0x22
// (3 bytes, magic=0x1CF7) and tag=0x23 (0 bytes) reliable replies. Currently
// hardcoded to slot=1; should be parameterized as `build_tag4d_spawn_slot(uint8_t)`.
std::vector<uint8_t> build_tag4d_retail_fixture() {
	return {0x01};
}

// tag=0x61 SESSION-KEY delivery — `NapiNPClientMsg_HandleSessionKey @
// 0x4297C0` (Jointops). 4-byte u32 session key; client stores in
// dword_A8229C / dword_A822A0 and disables _connectlog after first receipt.
// Currently hardcoded to 0x00630000 (a specific dvxi5 capture value); should
// be parameterized as `build_tag61_session_key(uint32_t)`.
std::vector<uint8_t> build_tag61_dvxi5_retail_fixture() {
	return {0x00, 0x00, 0x63, 0x00};
}

bool is_consumed_per_frame_tag(uint8_t tag) {
	return tag == 0x06 || tag == 0x04 || tag == 0x03 ||
			tag == 0x48 || tag == 0x4E || tag == 0x4C;
}

bool is_spawn_confirm_tag(uint8_t tag) {
	return tag == 0x22 || tag == 0x23 || tag == 0x28 ||
			tag == 0x29 || tag == 0x2D || tag == 0x32 ||
			tag == 0x25;
}

void add_roster_sync(GameSessionDispatchResult &result,
                     const PlayerReplicationState &ctx) {
	add_reply(result, 0x46, build_tag_46_player_sync(ctx));
	add_reply(result, 0x16, build_tag_16_player_list(ctx));
}

void queue_roster_sync(GameSessionState &state,
                       const PlayerReplicationState &ctx) {
	queue_reply(state, 0x46, build_tag_46_player_sync(ctx));
	queue_reply(state, 0x16, build_tag_16_player_list(ctx));
}

std::vector<uint8_t> build_tag1a_tick(uint32_t now_tick) {
	return {
			static_cast<uint8_t>(now_tick & 0xFFu),
			static_cast<uint8_t>((now_tick >> 8) & 0xFFu),
			static_cast<uint8_t>((now_tick >> 16) & 0xFFu),
			static_cast<uint8_t>((now_tick >> 24) & 0xFFu),
	};
}

// tag=0x25 RESET_AND_START — handler `NapiNPClientMsg_0x025 @ 0x422800`.
// On the client (is_authority == 0), the handler:
//   sub_497990(0);                       // input reset
//   ++dword_24C116C;                     // round counter
//   dword_C8D820 = 0;                    // round-end-time = 0 (NOT MAX)
//   ... clears 7 other state flags ...
//   dword_24C1928 = 1;                   // ← spawn-gate close
//   dword_24C195C = 1;                   // companion gate
//   return 1;
//
// 2026-04-26 live test history: first tried emitting tag=0x1D here. It
// did unblock the spawn (`ghw.txt` showed "Mission loading complete" for
// the first time), but ALSO wrote `dword_C8D820 = 0x7FFFFFFF` →
// client UI immediately said "game has ended" + sent ClientGoodBye
// disconnect. Tag=0x1D is ROUND_END, not ROUND_START.
//
// Tag=0x25 is the OPPOSITE: it explicitly clears dword_C8D820 to 0 (=
// "round just started, elapsed time = 0") in addition to setting the
// gate. This is the true RESET_AND_START signal. The capture7 note
// originally dismissed it because retail captures show C2S 0x25 (mid-
// game weapon reload, completely different handler at NapiNPServerMsg_0x025
// @ 0x514df0); the S2C direction is structurally separate.
//
// Payload: empty. The C2S handler (NapiNPServerMsg_0x025) reads 4 bytes
// for the weapon-reload slot reference, but the S2C handler
// (NapiNPClientMsg_0x025) reads zero bytes — all the state changes
// happen unconditionally on entry.
std::vector<uint8_t> build_tag_25_reset_and_start() {
	return {};
}

void add_initial_sync_player_state(GameSessionDispatchResult &result,
                                   const PlayerReplicationState &ctx,
                                   const GameSessionConfig &cfg,
                                   uint32_t now_tick) {
	add_roster_sync(result, ctx);
	add_reply(result, 0x19, {0x00, 0x00, 0x00, 0x00});
	add_reply(result, 0x1A, build_tag1a_tick(now_tick));
	add_reply(result, 0x0C, build_tag0c_local_player_entity(cfg));
	// Reverted: previously added tag=0x0D LOCAL_PLAYER_SPAWN here. Live
	// 2026-04-25 test crashed JO_CLIENT at NapiNPClientMsg_0x00D + 0x730
	// (NULL deref). type_id 0x14B9 is AI-flagged in ItemDef[+84] & 0x100000,
	// which forces the receiver into the AI branch that requires a
	// flags & 0x800 trailer we don't send. See
	// `notes/dispatcher_findings.md` "tag=0x0D — verified wrong" section.
}

void queue_initial_sync_player_state(GameSessionState &state,
                                     const PlayerReplicationState &ctx,
                                     const GameSessionConfig &cfg,
                                     uint32_t now_tick) {
	queue_roster_sync(state, ctx);
	queue_reply(state, 0x19, {0x00, 0x00, 0x00, 0x00});
	queue_reply(state, 0x1A, build_tag1a_tick(now_tick));
	queue_reply(state, 0x0C, build_tag0c_local_player_entity(cfg));
}

void queue_state4_loading_gate(GameSessionState &state,
                               const PlayerReplicationState &ctx,
                               uint32_t now_tick) {
	size_t tag0d_index = 0;
	for (const retail_loading_blobs::RetailBlobRef &blob :
			retail_loading_blobs::kState4Tag0dSequence) {
		if (tag0d_index == 3) {
			queue_reply(state, 0x16, build_tag_16_player_list(ctx));
		}
		queue_blob_reply(state, 0x0D, blob);
		++tag0d_index;
	}
	queue_blob_reply(state, 0x0C, retail_loading_blobs::kState4Tag0cPreGate);
	for (const retail_loading_blobs::RetailBlobRef &blob :
			retail_loading_blobs::kState4Tag20Sequence) {
		queue_blob_reply(state, 0x20, blob);
	}
	queue_blob_reply(state, 0x45, retail_loading_blobs::kState4Tag45First);
	queue_reply(state, 0x16, build_tag_16_player_list(ctx));
	queue_blob_reply(state, 0x45, retail_loading_blobs::kState4Tag45Second);
	queue_reply(state, 0x7E, {0x00, 0x00});
	queue_reply(state, 0x1A, build_tag1a_tick(now_tick));
	state.state4_loading_gate_queued = true;
}

void queue_mission_bootstrap(GameSessionState &state,
                             const GameSessionConfig &cfg) {
	const size_t start_size = state.queued_replies.size();
	queue_reply(state, 0x2C, build_tag2c_chat_entry(cfg));
	queue_reply(state, 0x08, build_tag08_game_state_snapshot());
	for (int i = 0; i < 6; ++i) {
		queue_reply(state, 0x2A, build_tag2a_chat_history_retail_fixture());
	}
	queue_custom_reply(state, 0x00, 0x1C);
	queue_reply(state, 0x0B, std::vector<uint8_t>(
			retail_blobs::kTag0bState,
			retail_blobs::kTag0bState + sizeof(retail_blobs::kTag0bState)));
	queue_reply(state, 0x66, {0x00});
	queue_reply(state, 0x76, {0xFF, 0x03});
	queue_reply(state, 0x11);
	queue_reply(state, 0x19, {0x00, 0x00, 0x00, 0x00});
	state.ida_mission_replies_pending = state.queued_replies.size() - start_size;
}

void add_game_start_bundle(GameSessionDispatchResult &result,
                           const PlayerReplicationState &ctx,
                           size_t loadout_replies_already_present = 0) {
	// Mirrors retail's cap7 frame 743 + 745 game-start sequence in order.
	// Frame 743: 0x5A, 0x5A, 0x42, 0x0A.
	for (size_t i = loadout_replies_already_present; i < 2; ++i) {
		add_reply(result, 0x5A, build_tag_5a_weapon_loadout());
	}
	add_reply(result, 0x42, build_tag42_spawn_gate_retail_fixture());
	add_reply(result, 0x0A, build_tag_0a_world_reference(ctx));

	// tag=0x25 RESET_AND_START — closes the WaitForGameStart loading
	// gate (`dword_24C1928 = 1`). MUST come BEFORE tag=0x0F because
	// tag=0x25's handler also sets `dword_24C195C = 1` as a side effect,
	// and tag=0x25 alone makes Input_HandleActionBinding silently
	// consume the spawn-menu action IDs (100, 101, 109-111) at
	// instruction `0x49b9ad: cmp dword_24C195C, 0; jnz default-case`.
	// Tag=0x0F's handler at instr 0x42e396 explicitly clears
	// `dword_24C195C = 0`, so emitting it AFTER tag=0x25 in the bundle
	// re-enables the menu input. See `notes/spawn_gate_24C1928.md`
	// 2026-04-26 addendum for the disasm citations and live-test
	// history (run 3: tag=0x25 at end → menu blocked, freeze).
	add_reply(result, 0x25, build_tag_25_reset_and_start());

	// Frame 745: 0x0F, 0x4D, 0x61, 0x3E, then the cap7 game-start trailer
	// (0x40 ×4, 0x6F ×4, 0x6E, second 0x0A, 0x57). The trailer was missing
	// from our prior bundle — without it the spawn-select menu stayed open
	// even after tag=0x29→tag=0x51 spawn confirm completed (verified
	// 2026-04-25 21:02 live test: player input frames flowing but UI
	// overlay never closed). Bytes are SCRK-decrypted from cap7 frame 745
	// and live in `retail_loading_blobs::kRetailGameStart*` constants.
	add_reply(result, 0x0F, build_tag_0f_game_start(ctx));
	add_reply(result, 0x4D, build_tag4d_retail_fixture());
	add_reply(result, 0x61, build_tag61_dvxi5_retail_fixture());
	add_custom_reply(result, 0x00, 0x3E, {});
	for (const retail_loading_blobs::RetailBlobRef &blob :
			retail_loading_blobs::kGameStartTag40Sequence) {
		add_blob_reply(result, 0x40, blob);
	}
	for (const retail_loading_blobs::RetailBlobRef &blob :
			retail_loading_blobs::kGameStartTag6FSequence) {
		add_blob_reply(result, 0x6F, blob);
	}
	add_blob_reply(result, 0x6E, retail_loading_blobs::kGameStartTag6E);
	add_blob_reply(result, 0x0A, retail_loading_blobs::kGameStartTag0ASecond);
	add_blob_reply(result, 0x57, retail_loading_blobs::kGameStartTag57);

	// cap7 frame 749 — packet retail emits ~2ms after frame 745. Including
	// the contents in our bundle ensures we cover everything retail sends
	// before the player is "alive" from the menu's perspective. Per cap7,
	// retail's player is sending tag=0x0C input frames immediately after
	// frame 745 (frame 747 = first input), so frame 749 timing isn't strict.
	add_blob_reply(result, 0x40, retail_loading_blobs::kFrame749Tag40);
	add_blob_reply(result, 0x4E, retail_loading_blobs::kFrame749Tag4E);
	add_blob_reply(result, 0x58, retail_loading_blobs::kFrame749Tag58);
	add_custom_reply(result, 0x00, 0x5D, {});
	add_blob_reply(result, 0x46, retail_loading_blobs::kFrame749Tag46);
	add_blob_reply(result, 0x4C, retail_loading_blobs::kFrame749Tag4C);
	add_blob_reply(result, 0x57, retail_loading_blobs::kFrame749Tag57);
	add_blob_reply(result, 0x0A, retail_loading_blobs::kFrame749Tag0A);
}

void mark_game_start_bundle_sent(GameSessionState &state) {
	state.spawn_acceptance_sent = true;
	state.game_start_bundle_sent = true;
	state.spawned = true;
	state.phase = GameSessionPhase::Spawned;
	state.ida_initial_state = 5;
	state.ida_initial_subphase = 0;
	state.initial_sync_complete = true;
	state.ms_since_tag10 = 0;
	state.ms_since_tag0a = 0;
	state.ms_since_tag57 = 0;
	state.entity_batch_cursor = 0;
}

bool can_stream_world(GameSessionPhase phase) {
	return phase == GameSessionPhase::WorldStreaming ||
			phase == GameSessionPhase::SpawnRequested ||
			phase == GameSessionPhase::Spawned;
}

void reset_stream_timers(GameSessionState &state) {
	state.ms_since_tag10 = 0;
	state.ms_since_tag0a = 0;
	state.ms_since_tag57 = 0;
}

void enter_ida_state4_world_streaming(GameSessionState &state) {
	if (state.phase != GameSessionPhase::SpawnRequested &&
			state.phase != GameSessionPhase::Spawned) {
		state.phase = GameSessionPhase::WorldStreaming;
		state.spawned = false;
	}
	state.world_streaming_armed = true;
	state.ida_initial_state = 4;
	state.ida_initial_subphase = 0;
	state.initial_sync_complete = true;
	reset_stream_timers(state);
}

void begin_initial_sync(GameSessionState &state) {
	if (can_stream_world(state.phase)) {
		enter_ida_state4_world_streaming(state);
		return;
	}
	if (state.phase != GameSessionPhase::Spawned) {
		state.phase = GameSessionPhase::InitialSync;
		state.spawned = false;
	}
	state.world_streaming_armed = false;
	state.ida_initial_state = 4;
	state.ida_initial_subphase = 0;
	state.initial_sync_complete = false;
	reset_stream_timers(state);
}

void arm_world_streaming(GameSessionState &state) {
	if (state.phase == GameSessionPhase::InitialSync) {
		state.phase = GameSessionPhase::WorldStreaming;
		state.initial_sync_complete = true;
	}
	if (state.phase == GameSessionPhase::WorldStreaming &&
			!state.world_streaming_armed) {
		state.world_streaming_armed = true;
		++state.world_streaming_ack_count;
	}
}

bool is_ready_for_late_spawn_acceptance(const GameSessionState &state,
                                        const GameSessionConfig &config) {
	return can_stream_world(state.phase) &&
			!state.spawned &&
			!state.spawn_acceptance_sent &&
			state.loadout_synced &&
			state.mission_status_received &&
			state.initial_sync_complete &&
			state.entity_batch_count > 0 &&
			(!config.emit_spawn_point_entities ||
					config.spawn_points.empty() ||
					state.spawn_points_synced);
}

struct InboundTagContext {
	GameSessionState &state;
	GameSessionDispatchResult &result;
	const PlayerReplicationState &player;
	const GameSessionConfig &config;
	uint32_t now_tick = 0;
	bool &started_initial_sync;
};

void reset_for_mission_request(GameSessionState &state) {
	state.phase = GameSessionPhase::MissionReady;
	state.spawned = false;
	state.loadout_synced = false;
	state.mission_status_received = false;
	state.spawn_points_synced = false;
	state.spawn_acceptance_sent = false;
	state.world_streaming_armed = false;
	state.ida_initial_state = 2;
	state.ida_initial_subphase = 0;
	state.ida_mission_replies_pending = 0;
	state.initial_sync_complete = false;
	state.game_start_bundle_sent = false;
	state.mission_disconnect_ack_sent = false;
	state.spawn_query_count = 0;
	state.loadout_sync_count = 0;
	state.world_streaming_ack_count = 0;
	state.queued_replies.clear();
	state.ms_since_tag10 = 0;
	state.ms_since_tag0a = 0;
	state.ms_since_tag57 = 0;
	state.entity_batch_cursor = 0;
	state.entity_batch_count = 0;
	state.state4_player_list_sent = false;
	state.state4_player_sync_requested = false;
	state.state4_player_sync_sent = false;
	state.state4_loading_gate_queued = false;
	state.player_spawn_confirmed = false;
}

void emit_reference_game_start_bundle(InboundTagContext &ctx,
                                      const char *label) {
	ctx.state.phase = GameSessionPhase::SpawnRequested;
	ctx.state.queued_replies.clear();
	add_game_start_bundle(ctx.result, ctx.player,
			std::min<size_t>(2, count_replies_with_tag(ctx.result, 0x5A)));
	mark_game_start_bundle_sent(ctx.state);
	ctx.result.label = label;
}

// Retail server tag 0x00 = `NapiNPServerMsg_0x000 @ 0x512AA0` — full JOIN
// handler that parses TLV pairs (CDKIID, CDKIIDEXP1, EXP, VERSIONCRCSTRING,
// CD, SQUADINFO) and runs `Server_ValidatePlayerJoinRequest`. Our simplified
// version just acks; sufficient for the simple jodemo-style join flow but
// not for the FORM_POST/GLB_JOIN retail path (per project memory).
void handle_tag_00_init(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x00);
	ctx.result.label = "in-game init ack (tag=0x00)";
}

// Retail server tag 0x01 = `NapiNPServerMsg_0x001 @ 0x512ED0`. Sequence
// captured from observation: client tag=0x01 → server replies tag=0x02 with
// 512-byte pad. The actual retail behavior should be re-decompiled; our
// build_tag02_push has audit notes about its 12-byte vs 512-byte mismatch.
void handle_tag_01_handshake_push(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x02, build_tag02_push(ctx.now_tick));
	ctx.result.label = "in-game 0x01->0x02 handshake push";
}

// Retail server tag 0x02 = `NapiNPServerMsg_0x002 @ 0x512FD0`. Per project
// memory, this is GLB_JOIN territory (game-lobby join). Our reply burst
// (tag=0x00 ack ×2, tag=0x01, tag=0x7A label, tag=0x7B summary, tag=0x03,
// tag=0x05, tag=0x04 server-config) is captured-from-observation and likely
// corresponds to the IDA flow that follows GLB_JOIN. Re-decompile 0x512FD0
// for full verification.
void handle_tag_02_post_handshake(InboundTagContext &ctx) {
	add_tag00_handshake(ctx.result, 0);
	add_tag00_handshake(ctx.result, 1);
	add_reply(ctx.result, 0x01, {0x01, 0x00, 0x00, 0x00});
	add_reply(ctx.result, 0x7A, build_tag7a_player_name(ctx.config));
	add_reply(ctx.result, 0x7B, build_tag7b_session_summary(ctx.config));
	add_reply(ctx.result, 0x03, {0x01, 0x01, 0x00, 0x01, 0x00});
	add_reply(ctx.result, 0x05, {0x01});
	add_reply(ctx.result, 0x04, {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
	});
	ctx.result.label = "in-game 0x02->post-handshake burst";
}

// Tag 0x47 not in the standard server msginfo table — likely captured-from-
// observation. Verify against IDA before extending.
void handle_tag_47_server_accept(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x75, {0x00, 0x02});
	ctx.result.label = "in-game 0x47->0x75";
}

// Retail server tag 0x33 = `NapiNPServerMsg_0x033 @ 0x515230`. Server-info
// request — replied via tag 0x60 chunked KV-pair transfer.
void handle_tag_33_server_info(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x60, build_tag60_server_info(ctx.config));
	ctx.result.label = "in-game 0x33->0x60 server-info";
}

// Retail server tag 0x37 = `NapiNPServerMsg_0x037 @ 0x5152E0`. Mission file
// request — replied via tag 0x64 chunked mission-metadata transfer + a
// 0x75 ack. Our mission-bootstrap state-machine entry follows.
//
// 2026-04-26: previously called `reset_for_mission_request` unconditionally,
// which clobbered `spawned`/`game_start_bundle_sent` every time the client
// re-requested tag=0x37 (it does so when our chunked tag=0x64 reply is
// "incomplete" per dispatcher_findings.md). Once spawned, we now skip the
// state reset and just re-send the 0x64 chunk + 0x75 ack — that's what
// the client actually wants from a re-request, not a full re-init. Without
// this guard, adding the spawn-gate-clearing tag=0x25 caused the
// loading bar to complete-then-restart in a loop (live test 2026-04-26).
void handle_tag_37_mission_request(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x75, {0x00, 0x02});
	add_reply(ctx.result, 0x64, build_tag64_mission_metadata(ctx.config));
	if (ctx.state.game_start_bundle_sent) {
		ctx.result.label = "in-game 0x37->mission re-request (spawned, no reset)";
		return;
	}
	reset_for_mission_request(ctx.state);
	queue_mission_bootstrap(ctx.state, ctx.config);
	ctx.result.label = "in-game 0x37->mission bootstrap";
}

// Client's `NapiClient_WaitForDisconnect @ 0x42CB20` sends tag=0x09 as its
// first action then loops until `dword_A82358 != 0`. The ONLY writer of
// that variable is the 1-instruction stub
// `NapiNPClientMsg_0x011 @ 0x4226E0`:
//     void NapiNPClientMsg_0x011() { dword_A82358 = 1; }
// Without our tag=0x11 reply the client sits in WaitForDisconnect forever
// — its loading bar animates up to the ~17% cap (target 7% + 10% slack
// per `sub_586BE0`) and then stalls. Sending an empty tag=0x11 (handler
// reads no payload) advances the bar past `WaitForDisconnect`; the
// follow-on `NapiClient_WaitForGameStart @ 0x42CC10` then sends tag=0x0A
// which our `handle_tag_0a_initial_sync` already handles.
//
// `tshark -Y "novaworld.msg.type == 0x11"` against capture7.pcapng
// (frames 327, 328) shows retail emits tag=0x11 ONLY in the bundle
// `[0x1C(0), 0x0B(616), 0x66(1), 0x76(2), 0x11(0)]` — never standalone.
// The post-WaitForDisconnect path runs `Game_LoadTerrainDuringConnect @
// 0x520710`, whose `sub_610940(MultiByteStr=byte_A761D0+0x44, ...)` reads
// the BMS basename qmemcpy'd by tag=0x0B's handler. If tag=0x11 ships
// before tag=0x0B, MultiByteStr is stale when validation runs and the
// terrain load returns 0 → ghw.txt "Mission loading aborted".
//
// We therefore queue tag=0x11 in the mission bootstrap immediately after
// tag=0x76. The mission-bootstrap drain emits
// `[0x1C, 0x0B, 0x66, 0x76, 0x11]` together, so tag=0x09 only queues a
// fallback ack if that bootstrap ack is neither pending nor already sent.
void handle_tag_09_transition_marker(InboundTagContext &ctx) {
	if (!ctx.state.mission_disconnect_ack_sent &&
			!queued_replies_have_tag(ctx.state, 0x11)) {
		queue_reply(ctx.state, 0x11);
		ctx.result.label = "in-game 0x09->fallback tag=0x11 queued";
		return;
	}
	ctx.result.label = "in-game 0x09 transition marker consumed";
}

// Retail server tag 0x0A = `NapiNPServerMsg_0x00A @ 0x513260`. Triggers
// initial-sync flow on our side: roster + tick + player-spawn entity batch.
void handle_tag_0a_initial_sync(InboundTagContext &ctx) {
	begin_initial_sync(ctx.state);
	ctx.started_initial_sync = true;
	if (ctx.state.queued_replies.empty()) {
		add_initial_sync_player_state(ctx.result, ctx.player, ctx.config, ctx.now_tick);
	} else {
		queue_initial_sync_player_state(ctx.state, ctx.player, ctx.config, ctx.now_tick);
	}
	ctx.result.label = "in-game 0x0A->initial-sync player roster";
}

void handle_tag_0e_spawn_request(InboundTagContext &ctx,
                                 const std::vector<uint8_t> &payload) {
	uint16_t requested_spawn = 0xFFFEu;
	if (payload.size() >= 2) {
		requested_spawn = static_cast<uint16_t>(payload[0]) |
				(static_cast<uint16_t>(payload[1]) << 8);
	}
	if (requested_spawn == 0xFFFFu) {
		ctx.result.label = "in-game 0x0E no-op spawn request";
		return;
	}
	if (ctx.state.spawn_acceptance_sent) {
		// Per IDA `NapiNPServerMsg_0x00E_ProcessClientRequestRespawn @ 0x519AF0`:
		// every non-FFFF spawn click (specific slot OR FFFE auto-pick) flows
		// into `Server_ProcessPlayerDeath @ 0x517740`, which at line 0x517a22
		// emits tag=0x1E via `NapiNPServer_SendFiltered(&ctx, 0x1Eu, ...)`.
		// The client's spawn-select menu (cmap.mnu) uses this game-event as
		// the "spawn confirmed, close menu" trigger. Without it the click is
		// silent on the wire and the menu stays open. The 8-byte payload is
		// IDA-witnessed at `GameEvent_BuildPayload @ 0x5054E0` (Phase D.0.7);
		// the verbatim retail capture bytes live in
		// `build_tag_1e_game_event_post_spawn`.
		add_reply(ctx.result, 0x1E, build_tag_1e_game_event_post_spawn());
		ctx.result.label = "in-game 0x0E->tag=0x1E respawn ack";
		return;
	}
	if (is_ready_for_late_spawn_acceptance(ctx.state, ctx.config)) {
		emit_reference_game_start_bundle(ctx,
				"in-game 0x0E->reference game-start bundle");
	} else {
		ctx.state.phase = GameSessionPhase::SpawnRequested;
		ctx.result.label = "in-game 0x0E spawn request deferred";
	}
}

// Retail server tag 0x2F = `NapiNPServerMsg_0x02F @ 0x515790`. Client
// loadout-request → server replies tag 0x5A WEAPON-LOADOUT and bumps our
// loadout-synced flag (which the spawn-acceptance gate checks).
void handle_tag_2f_loadout_request(InboundTagContext &ctx) {
	add_reply(ctx.result, 0x5A, build_tag_5a_weapon_loadout());
	ctx.state.loadout_synced = true;
	++ctx.state.loadout_sync_count;
	ctx.result.label = "in-game 0x2F->0x5A loadout-sync";
}

// Retail server tag 0x0C = `NapiNPServerMsg_0x00C @ 0x501C30`. Player-input
// frame; client sends the 5-byte entity packet sub-header followed by a
// type-10 extended player uplink body. Use the shared decoder so this path
// stays aligned with the byte-witnessed C2S parser.
void handle_tag_0c_player_input(InboundTagContext &ctx,
                                const std::vector<uint8_t> &payload) {
	EntityPacketSubHeader hdr;
	size_t header_consumed = 0;
	if (decode_entity_packet_sub_header(payload.data(), payload.size(), hdr, header_consumed) &&
	    hdr.sub_op == 0x0A) {
		PlayerExtendedUplink uplink;
		size_t body_consumed = 0;
		if (decode_player_extended_uplink(payload.data() + header_consumed,
		                                  payload.size() - header_consumed,
		                                  uplink, body_consumed) &&
		    header_consumed + body_consumed == payload.size()) {
			ctx.state.client_entity_handle = hdr.handle;
			ctx.state.client_item_type_id = hdr.item_type_id;
			ctx.state.client_vehicle_handle = uplink.vehicle_handle;
			ctx.state.client_pos_x = static_cast<uint32_t>(uplink.pos_x);
			ctx.state.client_pos_y = static_cast<uint32_t>(uplink.pos_y);
			ctx.state.client_pos_z = static_cast<uint32_t>(uplink.pos_z);
			ctx.state.client_heading = uplink.heading;
			ctx.state.client_pitch = uplink.pitch;
			ctx.state.client_pos_valid = true;
		}
	}
	ctx.result.label = "in-game 0x0C player input consumed";
}

// Retail server tag 0x22 = `NapiNPServerMsg_0x022 @ 0x514C90`. ACK from
// client for player-sync (sent in tag=0x46 reply chain). Used to advance
// the state4 player-sync sub-phase.
void handle_tag_22_player_sync_ack(InboundTagContext &ctx) {
	if (!ctx.state.state4_player_sync_sent) {
		ctx.state.state4_player_sync_requested = true;
	}
	ctx.result.label = "in-game 0x22 player-sync ack consumed";
}

// Retail server tag 0x0F = `NapiNPServerMsg_0x00F @ 0x514180`. Per the
// IDA decompile retail's server WOULD reply with tag=0x18 entity data
// when validation passes (pool <= 1, slot < pool capacity), but per
// `notes/spawn_flow_capture7.md` the cap7 retail capture shows ZERO
// tag=0x18 emissions and ZERO C2S tag=0x0F len=2 queries — i.e. that
// IDA code path simply does not fire in normal multiplayer play. So we
// do nothing on receive (silent consume), matching retail's observed
// behavior. We previously tried emitting tag=0x18 here speculatively;
// it didn't help and `notes/dispatcher_findings.md` confirms it as
// dead code on our side.
void handle_tag_0f_spawn_query(InboundTagContext &ctx,
                               const std::vector<uint8_t> & /*payload*/) {
	++ctx.state.spawn_query_count;
	ctx.result.label = "in-game 0x0F spawn-point query consumed (retail also ignores)";
}

// Retail server tag 0x29 = `NapiNPServerMsg_0x029 @ 0x514F10`. The client
// sends tag=0x29 with a u16 spawn-slot index after receiving our tag=0x0F
// WORLD-STATE-LOAD (per `NapiNPClientMsg_0x00F @ 0x42E200` line 0x42e61e).
// Retail's server replies with tag=0x51 PLAYER-SPAWN, which:
//   - Writes entity[+354] (team byte) on the client's local-player entity
//   - Initializes weapon/camera state if entity has the PLAYER flag
//   - Sets dword_B5CBB4 = 2 (spawn-complete state flag)
//   - Closes the spawn-select menu so the player can enter the world
//
// Without this reply the spawn-screen stays open and tag=0x0E spawn clicks
// never transition the player into the world (the prior tag=0x1E reply only
// shows a HUD pickup tip via NetPacket_HandleGameEvent @ 0x426270).
//
// HandlePlayerSpawn @ 0x431BB0 echoes back another tag=0x29 with payload
// (team+1) after processing our tag=0x51 — we use ctx.state.player_spawn_confirmed
// to break that loop, since the retail server only emits tag=0x51 when the
// looked-up CBufferList entry is non-null (which it isn't on subsequent calls
// for a single player session).
void handle_tag_29_spawn_slot_request(InboundTagContext &ctx,
                                       const std::vector<uint8_t> & /*payload*/) {
	if (ctx.state.player_spawn_confirmed) {
		ctx.result.label = "in-game 0x29 spawn-slot request consumed (already confirmed)";
		return;
	}
	add_reply(ctx.result, 0x51, build_tag_51_player_spawn(ctx.player));
	ctx.state.player_spawn_confirmed = true;
	ctx.result.label = "in-game 0x29->tag=0x51 player spawn confirm";
}

// Retail server tag 0x0B = `NapiNPServerMsg_0x00B @ 0x51AB10`. Mission-file
// status report from client (signals that our tag=0x64 mission transfer was
// fully received and parsed). Marks the gate flag the spawn-acceptance
// readiness check looks at.
void handle_tag_0b_mission_status(InboundTagContext &ctx) {
	ctx.state.mission_status_received = true;
	ctx.result.label = "in-game 0x0B mission-file status consumed";
}

void handle_unknown_or_passive_tag(InboundTagContext &ctx, uint8_t tag) {
	if (is_consumed_per_frame_tag(tag)) {
		ctx.result.label = "in-game per-frame client update consumed";
	} else if (is_spawn_confirm_tag(tag)) {
		ctx.result.label = "in-game spawn-confirm ack consumed";
	} else {
		ctx.result.label = "in-game unhandled tag consumed";
	}
}

} // namespace

GameSession::GameSession(GameSessionConfig config)
		: config_(std::move(config)) {
	std::sort(config_.replicated_entities.begin(), config_.replicated_entities.end(),
			[](const GameEntitySnapshot &a, const GameEntitySnapshot &b) {
				if (a.pool != b.pool) return a.pool < b.pool;
				return a.slot < b.slot;
			});
	std::sort(config_.spawn_points.begin(), config_.spawn_points.end(),
			[](const SpawnPointEntity &a, const SpawnPointEntity &b) {
				if (a.pool != b.pool) return a.pool < b.pool;
				return a.slot < b.slot;
			});
}

const char *game_session_phase_name(GameSessionPhase phase) {
	switch (phase) {
		case GameSessionPhase::Handshake:
			return "handshake";
		case GameSessionPhase::MissionReady:
			return "mission_ready";
		case GameSessionPhase::InitialSync:
			return "initial_sync";
		case GameSessionPhase::WorldStreaming:
			return "world_streaming";
		case GameSessionPhase::SpawnRequested:
			return "spawn_requested";
		case GameSessionPhase::Spawned:
			return "spawned";
	}
	return "unknown";
}

PlayerReplicationState GameSession::player_replication_state() const {
	PlayerReplicationState ctx;
	ctx.player_name = config_.player_name;
	ctx.player_slot = 0;
	ctx.entity_slot = 0;
	ctx.spawn_x = config_.spawn_x;
	ctx.spawn_y = config_.spawn_y;
	ctx.spawn_z = config_.spawn_z;
	return ctx;
}

GameSessionDispatchResult GameSession::handle_messages(
		GameSessionState &state,
		const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick) const {
	GameSessionDispatchResult result;
	const PlayerReplicationState rep_ctx = player_replication_state();
	bool started_initial_sync = false;

	if (messages.empty()) {
		arm_world_streaming(state);
		if (state.world_streaming_armed) {
			result.label = "in-game world stream armed after client ack";
		}
	}

	for (const ProtocolMessage &msg : messages) {
		std::vector<uint8_t> payload;
		if (!reassemble_protocol_payload(state.reassembly, msg, payload)) {
			result.label = "in-game fragment buffered";
			continue;
		}

		InboundTagContext ctx{
				state,
				result,
				rep_ctx,
				config_,
				now_tick,
				started_initial_sync,
		};

		switch (msg.tag) {
			case 0x00:
				handle_tag_00_init(ctx);
				break;
			case 0x01:
				handle_tag_01_handshake_push(ctx);
				break;
			case 0x02:
				handle_tag_02_post_handshake(ctx);
				break;
			case 0x47:
				handle_tag_47_server_accept(ctx);
				break;
			case 0x33:
				handle_tag_33_server_info(ctx);
				break;
			case 0x37:
				handle_tag_37_mission_request(ctx);
				break;
			case 0x09:
				handle_tag_09_transition_marker(ctx);
				break;
			case 0x0A:
				handle_tag_0a_initial_sync(ctx);
				break;
			case 0x0E:
				handle_tag_0e_spawn_request(ctx, payload);
				break;
			case 0x2F:
				handle_tag_2f_loadout_request(ctx);
				break;
			case 0x2C:
				result.label = "in-game 0x2C rtt echo consumed";
				break;
			case 0x0C:
				handle_tag_0c_player_input(ctx, payload);
				break;
			case 0x22:
				handle_tag_22_player_sync_ack(ctx);
				break;
			case 0x0F:
				handle_tag_0f_spawn_query(ctx, payload);
				break;
			case 0x29:
				handle_tag_29_spawn_slot_request(ctx, payload);
				break;
			case 0x0B:
				handle_tag_0b_mission_status(ctx);
				break;
			default:
				handle_unknown_or_passive_tag(ctx, msg.tag);
				break;
		}
	}

	if (!messages.empty() && !started_initial_sync) {
		arm_world_streaming(state);
	}

	if (is_ready_for_late_spawn_acceptance(state, config_)) {
		InboundTagContext ctx{
				state,
				result,
				rep_ctx,
				config_,
				now_tick,
				started_initial_sync,
		};
		emit_reference_game_start_bundle(ctx,
				"in-game post-load reference game-start bundle");
	}

	return result;
}

GameSessionDispatchResult GameSession::tick(GameSessionState &state,
                                            int elapsed_ms,
                                            uint32_t now_tick) const {
	GameSessionDispatchResult result;
	result.label = "in-game tick";

	if (!state.queued_replies.empty()) {
		size_t max_replies = 4;
		if (state.ida_initial_state == 2) {
			max_replies = 1;
			if (!state.queued_replies.empty() &&
					state.queued_replies.front().tag == 0x1C) {
				max_replies = std::min<size_t>(5, state.queued_replies.size());
			}
		} else if (state.phase == GameSessionPhase::WorldStreaming &&
				!state.game_start_bundle_sent) {
			max_replies = 1;
			if (!state.queued_replies.empty() &&
					state.queued_replies.front().tag == 0x16) {
				max_replies = std::min<size_t>(2, state.queued_replies.size());
			}
		}
		const size_t flushed = flush_queued_replies(state, result, max_replies);
		if (state.ida_initial_state == 2) {
			const size_t mission_flushed = std::min(flushed, state.ida_mission_replies_pending);
			state.ida_mission_replies_pending -= mission_flushed;
			state.ida_initial_subphase = static_cast<uint16_t>(
					state.ida_initial_subphase + static_cast<uint16_t>(mission_flushed));
			if (state.ida_mission_replies_pending == 0) {
				enter_ida_state4_world_streaming(state);
			}
		}
		result.label = "in-game queued replication";
		if (state.phase == GameSessionPhase::WorldStreaming &&
				!state.game_start_bundle_sent) {
			return result;
		}
		if (!state.queued_replies.empty() || !can_stream_world(state.phase)) {
			return result;
		}
	}

	if (!can_stream_world(state.phase)) {
		result.label = "in-game tick skipped (world not streaming)";
		return result;
	}

	if (state.phase == GameSessionPhase::WorldStreaming &&
			!state.world_streaming_armed) {
		result.label = "in-game tick skipped (awaiting local-player ack)";
		return result;
	}

	state.ms_since_tag10 += elapsed_ms;
	state.ms_since_tag0a += elapsed_ms;
	state.ms_since_tag57 += elapsed_ms;

	if (state.ms_since_tag10 >= kTag10IntervalMs) {
		const bool pre_game_state4 =
				state.phase == GameSessionPhase::WorldStreaming &&
				!state.spawned &&
				!state.game_start_bundle_sent;
		if (pre_game_state4 && !state.state4_player_list_sent &&
				state.entity_batch_count >= 5) {
			add_reply(result, 0x16, build_tag_16_player_list(player_replication_state()));
			state.state4_player_list_sent = true;
		}
		if (pre_game_state4 && state.state4_player_sync_requested &&
				!state.state4_player_sync_sent) {
			add_reply(result, 0x46, build_tag_46_player_sync(player_replication_state()));
			state.state4_player_sync_requested = false;
			state.state4_player_sync_sent = true;
		}
		if (pre_game_state4 &&
				state.state4_player_sync_sent &&
				!state.state4_loading_gate_queued &&
				state.entity_batch_count >= 17) {
			queue_state4_loading_gate(state, player_replication_state(), now_tick);
		}
		if (!config_.replicated_entities.empty()) {
			EntityBatchBuildResult batch = build_tag_10_entity_batch(
					config_.replicated_entities, state.entity_batch_cursor, 620);
			state.entity_batch_cursor = batch.next_cursor;
			add_reply(result, 0x10, std::move(batch.payload));
		} else {
			add_reply(result, 0x10, build_tag_10_entity_batch_empty());
		}
		++state.entity_batch_count;
		state.ms_since_tag10 = 0;
	}
	if (state.spawned && state.ms_since_tag0a >= kTag0aIntervalMs) {
		add_reply(result, 0x0A,
		          build_tag_0a_world_reference(player_replication_state(),
		                                       config_.replicated_entities));
		state.ms_since_tag0a = 0;
	}
	if (state.spawned && state.ms_since_tag57 >= kTag57IntervalMs) {
		add_reply(result, 0x57, build_tag_57_rtt_request(now_tick));
		state.ms_since_tag57 = 0;
	}

	return result;
}

} // namespace opennova
