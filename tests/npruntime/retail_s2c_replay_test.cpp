// R3-lite, the net parity proof plan's core replay rung: the retail NovaWorld capture
// fixtures/novaworld/run_20260426_120859 (a stock Jointops.exe joiner on a stock host,
// AAS, decrypted in-match streams) replayed through our in-match joiner.
//
// The client leg. The capture's S2C stream (server_join_packets, server_load_packets,
// server_runtime_packets: one bundle per retail protocol packet, fragments as they rode
// the wire) is reframed packet by packet as a host's 0x83 and fed, at the capture's own
// timing (one client frame per io::kTickMs of the bundle timestamps), into a joiner
// ClientRuntime seeded past admission from the capture's own words: the self handle and
// type of the retail client's first C2S 0x0C, the game type of the S2C 0x08 and the team
// of the S2C 0x04 tail. Housekeeping stays live (no replay mode), so the timer sends run.
// Asserted, each failure naming the tag, the packet (file#index) or frame and the counts:
//   - every packet is admitted in order: no gap request, no session loss;
//   - no decode error: the replica's malformed count stays 0 (a side reducer fed the same
//     stream names the tag and packet of the first one);
//   - every S2C tag the capture carries has a client handler: the replica reducer, or the
//     connection / runtime leg kRuntimeHandled names;
//   - frames applied == the S2C 0x0A count; the self handle stays bound with its row;
//   - each pool's row count == what the capture's spawn pages decode to, at the load's
//     end and at the run's end;
//   - the Tab board == the last S2C 0x16;
//   - the reply tags, body for body and in order, == the retail client's own C2S
//     (client_startup + client_runtime): 0x0F carrier repairs, 0x20, 0x21, 0x22, 0x23,
//     0x28, 0x29, 0x2D, 0x32 and the 0x2C pongs; no other C2S tag leaves;
//   - the timer sends of the in-game window (from the frame that takes the first runtime
//     packet, where the retail client's final load wait ends) by rate: the 0x4C report
//     once per kNetQualityReportInterval + 1 frames, and the 0x2C pings and 0x0C uplinks
//     once per open send boundary, near client_runtime.nwmsg's counts.
// The checksum replies' sources are the retail client's own tables, read back off its own
// replies (C2S 0x20 value ^ the S2C 0x30 checksum, C2S 0x21 crc ^ key): what those
// replies then prove is the echo and the layout, one reply per challenge.
//
// Class resolution: the authored 14-row table the core nwmsg_codec_parity leg uses (no
// items.def). client_input.nwmsg is client_runtime.nwmsg's first bundle, recorded twice.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_state.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/world/entity.h> // world::retail_pool_capacity

#include <base/gameprofile/gameprofile.h>
#include <base/io/tick_rate.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h> // decode_session_config
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/title/title_identity.h>

#include "common/nwmsg_fixture.h"
#include "conn_fixture.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace rep = opennova::replication;

int g_failures = 0;

void fail(const char *fmt, ...) {
	std::va_list args;
	va_start(args, fmt);
	std::printf("FAIL R3L: ");
	std::vprintf(fmt, args);
	std::printf("\n");
	va_end(args);
	std::fflush(stdout);
	++g_failures;
}

std::string hex_head(const std::vector<uint8_t> &b, std::size_t max = 16) {
	std::string out;
	char buf[4];
	for (std::size_t i = 0; i < b.size() && i < max; ++i) {
		std::snprintf(buf, sizeof buf, "%02x", b[i]);
		out += buf;
	}
	if (b.size() > max) out += "..";
	if (out.empty()) out = "-";
	return out;
}

// --- the record classes the corpus needs (nwmsg_codec_parity's authored table) ---

struct AuthoredItem {
	int32_t definition_id;
	const char *ai_function;
	const char *move_function;
};

const AuthoredItem kCorpusItems[] = {
	{100074, "cveh", "cbike"}, // Drivable Motorcycle
	{100164, "cveh", "ctank"}, // Drivable M1A1 Tank
	{100165, "cveh", "ctank"}, // Drivable T80 Tank
	{101213, "cveh", "catv"},  // Drivable BTR-80, Indonesian
	{101214, "chel", "chel"},  // Flyable Mil-26, Indonesian helo
	{101215, "chel", "chel"},  // Flyable Nb0-105, Indonesian helo
	{101301, "cveh", "catv"},  // Drivable ATV
	{101302, "cveh", "cveh"},  // Drivable SUV with 50cal
	{101304, "cveh", "cveh"},  // Drivable Attack Vehicle with MK19
	{101305, "cveh", "cveh"},  // Drivable SUV with Mk19
	{101308, "chel", "chel"},  // Flyable Super Puma, Indonesian helo
	{102099, "chel", "chel"},  // Flyable Nb0-105 with miniguns, Indonesian helo
	{102100, "chel", "chel"},  // Flyable Nb0-105 with rocketpods, Indonesian helo
	{105305, "plyr", "org2"},  // Player #1, Multiplayer
};

std::shared_ptr<const rep::ItemReplicationCatalog> authored_catalog() {
	std::vector<rep::ItemReplicationDefinition> defs;
	for (const AuthoredItem &item : kCorpusItems) {
		rep::ItemReplicationDefinition d;
		d.definition_id = item.definition_id;
		d.ai_function = item.ai_function;
		d.move_function = item.move_function;
		defs.push_back(d);
	}
	return std::make_shared<const rep::ItemReplicationCatalog>(
			rep::ItemReplicationCatalog::from_definitions(defs));
}

// --- the corpus ---

const char kRun[] = "run_20260426_120859";

struct Packet { // one capture bundle = one retail protocol packet
	std::string file;
	std::size_t index = 0;
	uint64_t ts_us = 0;
	std::vector<nwmsg::FixtureMessage> physical;
};

struct Logical { // one message after fragment reassembly
	uint16_t full_tag = 0;
	std::vector<uint8_t> body;
	std::size_t packet = 0; // the packet carrying its final piece
};

struct Stream {
	std::vector<Packet> packets;
	std::vector<Logical> messages;
	std::string where(std::size_t packet) const {
		if (packet >= packets.size()) return "?";
		return packets[packet].file + "#" + std::to_string(packets[packet].index);
	}
	std::size_t first_packet_of(const std::string &file) const {
		for (std::size_t i = 0; i < packets.size(); ++i)
			if (packets[i].file == file) return i;
		return packets.size();
	}
	std::size_t count(uint16_t full_tag) const {
		std::size_t n = 0;
		for (const Logical &m : messages)
			if (m.full_tag == full_tag) ++n;
		return n;
	}
	const Logical *first(uint16_t full_tag) const {
		for (const Logical &m : messages)
			if (m.full_tag == full_tag) return &m;
		return nullptr;
	}
	const Logical *last(uint16_t full_tag) const {
		const Logical *found = nullptr;
		for (const Logical &m : messages)
			if (m.full_tag == full_tag) found = &m;
		return found;
	}
};

// One direction's files in order, reassembled with ONE state, as the connection does.
bool load_stream(const std::string &dir, const std::vector<std::string> &files, Stream &out) {
	for (const std::string &name : files) {
		std::vector<nwmsg::FixtureBundle> bundles;
		std::string err;
		if (!nwmsg::load_nwmsg(dir + "/" + name, bundles, err)) {
			fail("load %s: %s", name.c_str(), err.c_str());
			return false;
		}
		for (std::size_t i = 0; i < bundles.size(); ++i) {
			Packet p;
			p.file = name;
			p.index = i;
			p.ts_us = bundles[i].ts_us;
			p.physical = std::move(bundles[i].messages);
			out.packets.push_back(std::move(p));
		}
	}
	ProtocolReassemblyState state;
	for (std::size_t p = 0; p < out.packets.size(); ++p) {
		for (const nwmsg::FixtureMessage &m : out.packets[p].physical) {
			std::vector<uint8_t> payload;
			bool fragmented = false;
			if (!reassemble_protocol_payload(state, nwmsg::message_from_fixture(m), payload,
					&fragmented))
				continue;
			Logical l;
			l.full_tag = nwmsg::runtime_full_type(m);
			l.body = std::move(payload);
			l.packet = p;
			out.messages.push_back(std::move(l));
		}
	}
	if (!state.buffer.empty()) {
		fail("a fragment chain is still open at the end of %s", files.back().c_str());
		return false;
	}
	return true;
}

uint32_t read_u32(const std::vector<uint8_t> &b, std::size_t at) {
	if (at + 4 > b.size()) return 0;
	return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) |
			(uint32_t(b[at + 3]) << 24);
}

// --- the reducer-unmodelled tags and the leg that handles each ---

// The S2C tags the replica reducer leaves to its default arm whose handler is the joiner
// connection's admission pass or the client runtime's own fold. The join legs (0x00,
// 0x02, 0x05, 0x1A, 0x60, 0x64) are stage-gated there: a joiner seeded past admission
// takes them as no-ops, so their effects are the golden join tests' (client_runtime_test).
struct RuntimeHandler {
	uint8_t tag;
	const char *site;
};

const RuntimeHandler kRuntimeHandled[] = {
	{s2c::INIT, "JoinerConnection::process_session_admission, the join-ack leg"},
	{s2c::SYNC_STATE, "JoinerConnection::process_session_admission, verification_value_"},
	{s2c::JOIN_PADDING_PROBE, "JoinerConnection::process_session_admission, the padding echo"},
	{s2c::SYNC_TICK, "JoinerConnection::process_session_admission, fold_join_queue_record"},
	{s2c::SESSION_SLOT_CONFIG, "JoinerConnection::process_session_admission, the slot/team latch"},
	{s2c::GAME_START_SIGNAL, "JoinerConnection::process_session_admission, the game-start ack"},
	{s2c::BMS_HEADER, "JoinerConnection::process_session_admission, mission_header_bytes_"},
	{s2c::DISCONNECT_UNLOCK, "JoinerConnection::process_session_admission, the sync-tail flag"},
	{s2c::SPAWN_ACK_TIMESTAMP, "JoinerConnection::process_session_admission, spawn_ack_timestamp_"},
	{s2c::WAIT_FOR_GAME_START_ACK, "JoinerConnection::process_session_admission, the world-stream end"},
	{s2c::MISSION_MAP_NAMES, "JoinerConnection::process_session_admission, map_file_"},
	{s2c::ENTITY_CHECKSUM_REQ, "JoinerConnection::process_session_admission, the C2S 0x20 reply"},
	{s2c::LOADOUT_CRC_REQ, "JoinerConnection::process_session_admission, the C2S 0x21 reply"},
	{s2c::CHARATTR_DISABLED_PROPERTIES, "JoinerConnection::process_session_admission, charattr_unpack_disabled"},
	{s2c::TERRAIN_LOAD, "JoinerConnection::process_session_admission, retain_terrain_load_page"},
	{s2c::RTT_ECHO, "JoinerConnection::process_session_admission, the C2S 0x2C pong / RTT ring"},
	{s2c::WEAPON_LOADOUT, "JoinerConnection::process_session_admission + ClientRuntime loadout grants"},
	{s2c::FILE_TRANSFER_CHUNK, "JoinerConnection::process_session_admission, the server-info transfer"},
	{s2c::TICK_SEED, "JoinerConnection::process_session_admission + ClientRuntime current_tick_"},
	{s2c::MISSION_DATA_CHUNK, "JoinerConnection::process_session_admission, the mission-data transfer"},
	{s2c::ZONE_TIMER_VALUE, "ClientRuntime::apply_zone_timer_body"},
	{s2c::CLASS_ALLOW_MASK, "JoinerConnection::process_session_admission, class_allow_mask_"},
};

const RuntimeHandler *runtime_handler(uint8_t tag) {
	for (const RuntimeHandler &h : kRuntimeHandled)
		if (h.tag == tag) return &h;
	return nullptr;
}

// --- pools ---

using PoolCounts = std::array<std::size_t, world::kEntityPoolCount>;

PoolCounts count_pools(const rep::ClientState &state) {
	PoolCounts counts{};
	for (const rep::ClientEntityState &es : state.entities) {
		const std::size_t pool = es.handle >> 12;
		if (pool < counts.size()) ++counts[pool];
	}
	return counts;
}

bool in_pool_tables(uint16_t handle) {
	const int pool = handle >> 12;
	return handle != 0xFFFFu && pool < world::kEntityPoolCount &&
			static_cast<std::size_t>(handle & 0x0FFFu) < world::retail_pool_capacity(pool);
}

// What the capture's spawn pages decode to through packet `limit` (exclusive), in wire
// order: each page's addressing (0x0C/0x0D by record handle, 0x10/0x20 by start index +
// ordinal) and its empty forms (a body-less 0x0C record, an itemType 0 0x10/0x20
// record), and the 0x5D sweep's raw pool-0 indices.
PoolCounts expected_pools(const Stream &down, std::size_t limit, std::set<uint16_t> &unmodelled) {
	std::map<uint16_t, bool> present;
	for (const Logical &m : down.messages) {
		if (m.packet >= limit) break;
		if (m.full_tag == s2c::ENTITY_SPAWN_BATCH) {
			OrganicSpawnBatch batch;
			if (!decode_organic_spawn_batch(m.body.data(), m.body.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			for (const OrganicSpawnRecord &rec : batch.records) {
				if (!in_pool_tables(rec.slot_id)) break;
				present[rec.slot_id] = rec.has_body();
			}
		} else if (m.full_tag == s2c::POOL_SPAWN) {
			PoolSpawnBatch batch;
			if (!decode_pool_spawn_batch(m.body.data(), m.body.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			for (const PoolSpawnRecord &rec : batch.records) {
				if ((rec.slot_id >> 12) != 1 ||
						static_cast<std::size_t>(rec.slot_id & 0x0FFFu) >= world::retail_pool_capacity(1))
					continue;
				present[rec.slot_id] = true;
			}
		} else if (m.full_tag == s2c::STATIC_ENTITY_BATCH) {
			StaticEntityBatch batch;
			if (!decode_static_entity_batch(m.body.data(), m.body.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			for (std::size_t i = 0; i < batch.records.size(); ++i) {
				const std::size_t slot = static_cast<std::size_t>(batch.start_index) + i;
				if (slot >= world::retail_pool_capacity(2)) continue;
				present[static_cast<uint16_t>(0x2000u | slot)] = !batch.records[i].is_empty_slot;
			}
		} else if (m.full_tag == s2c::POOL3_SYNC) {
			Pool3SyncBatch batch;
			if (!decode_pool3_sync_batch(m.body.data(), m.body.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			for (std::size_t i = 0; i < batch.records.size(); ++i) {
				const std::size_t slot = static_cast<std::size_t>(batch.start_index) + i;
				if (slot >= world::retail_pool_capacity(3)) continue;
				present[static_cast<uint16_t>(0x3000u | slot)] = !batch.records[i].is_empty_slot;
			}
		} else if (m.full_tag == s2c::EMPTY_SLOT_SWEEP) {
			DestroyEntityList list;
			if (decode_destroy_entity_list(m.body.data(), m.body.size(), list))
				for (uint16_t index : list.pool0_indices)
					if ((index >> 12) == 0) present[index] = false;
		} else if (m.full_tag == s2c::FULL_ENTITY_SPAWN || m.full_tag == s2c::DEPLOYED_ITEM ||
				m.full_tag == s2c::ENTITY_REMOVE || m.full_tag == s2c::TEAM_ASSIGN) {
			unmodelled.insert(m.full_tag); // row writers this tally does not model
		}
	}
	PoolCounts counts{};
	for (const auto &kv : present)
		if (kv.second) ++counts[kv.first >> 12];
	return counts;
}

std::string pools_text(const PoolCounts &c) {
	std::string out;
	for (std::size_t i = 0; i < c.size(); ++i) {
		if (i != 0) out += " ";
		out += "p" + std::to_string(i) + "=" + std::to_string(c[i]);
	}
	return out;
}

// --- the C2S classes ---

// The reply tags compared body for body. kPong is the C2S 0x2C answering an S2C 0x57
// whose echo flag is set (its last byte 0); a 0x2C with last byte 1 is the per-boundary
// ping, a timer send.
constexpr int kPong = 0x100 | c2s::RTT_CONSUMED;
const int kReplyClasses[] = {
	c2s::ENTITY_INFO_QUERY, c2s::ENTITY_CHECKSUM_REPLY, c2s::CHECKSUM_REPLY,
	c2s::PLAYER_SYNC_REQUEST, c2s::VISIBLE_PLAYERS_REQUEST, c2s::LOADOUT_REQUEST,
	c2s::TEAM_SPAWN_ACK, c2s::BURST_MEMBER_2D, c2s::EMPTY_SLOT_SWEEP_REQUEST, kPong,
};

bool is_reply_class(int cls) {
	for (int r : kReplyClasses)
		if (r == cls) return true;
	return false;
}

bool is_ping(const std::vector<uint8_t> &body) { return body.size() == 5 && body[4] != 0; }

struct C2STally {
	std::map<int, std::vector<std::vector<uint8_t>>> replies; // class -> bodies in order
	std::size_t pings = 0;
	std::size_t uplinks = 0;
	std::size_t reports = 0;
	std::vector<std::vector<uint8_t>> report_bodies;
	std::map<int, std::size_t> other; // any other tag
};

void tally_message(C2STally &t, uint8_t tag, const std::vector<uint8_t> &body, bool timers) {
	if (tag == c2s::RTT_CONSUMED) {
		if (!is_ping(body)) t.replies[kPong].push_back(body);
		else if (timers) ++t.pings;
		return;
	}
	if (is_reply_class(tag)) {
		t.replies[tag].push_back(body);
		return;
	}
	if (tag == c2s::ENTITY_UPLINK) {
		if (timers) ++t.uplinks;
		return;
	}
	if (tag == c2s::CLIENT_QUALITY) {
		if (timers) {
			++t.reports;
			t.report_bodies.push_back(body);
		}
		return;
	}
	++t.other[tag];
}

std::size_t timer_tolerance(std::size_t retail) {
	return std::max<std::size_t>(4, retail * 3 / 100);
}

std::size_t abs_diff(std::size_t a, std::size_t b) { return a > b ? a - b : b - a; }

// --- the client leg ---

constexpr uint32_t kSessionId = 0x52334C31u; // the seeded 0x43 header key
constexpr uint32_t kClientKey = 0x00000001u; // the joiner's local key (ClientAuth.ck)
const std::string kClientScrk = "R3L-CLIENT-SCRK";
const std::string kServerScrk = "R3L-SERVER-SCRK";

void run_client_leg(const std::string &dir) {
	Stream down, startup, runtime_c2s, input;
	if (!load_stream(dir, {"server_join_packets.nwmsg", "server_load_packets.nwmsg",
			"server_runtime_packets.nwmsg"}, down) ||
			!load_stream(dir, {"client_startup.nwmsg"}, startup) ||
			!load_stream(dir, {"client_runtime.nwmsg"}, runtime_c2s) ||
			!load_stream(dir, {"client_input.nwmsg"}, input))
		return;
	if (down.packets.empty() || runtime_c2s.packets.empty()) {
		fail("the corpus streams are empty");
		return;
	}
	{
		std::vector<std::vector<uint8_t>> first_bundle;
		for (const Logical &m : runtime_c2s.messages)
			if (m.packet == 0) first_bundle.push_back(m.body);
		std::vector<std::vector<uint8_t>> input_bodies;
		for (const Logical &m : input.messages) input_bodies.push_back(m.body);
		if (input.packets.size() != 1 || input.packets[0].ts_us != runtime_c2s.packets[0].ts_us ||
				input_bodies != first_bundle)
			std::printf("note: client_input.nwmsg is not client_runtime.nwmsg's first bundle\n");
	}

	// --- the seed, from the capture's own words ---
	const Logical *config = down.first(s2c::SESSION_CONFIG);
	SessionConfig session{};
	if (config == nullptr || !decode_session_config(config->body.data(), config->body.size(), session)) {
		fail("no decodable S2C 0x08 in the capture");
		return;
	}
	const uint32_t game_type = static_cast<uint32_t>(session.fields[3]);
	const Logical *slot = down.first(s2c::SESSION_SLOT_CONFIG);
	if (slot == nullptr || slot->body.size() < 24) {
		fail("no 24-byte S2C 0x04 in the capture");
		return;
	}
	const uint8_t self_team = slot->body[23];
	const uint8_t self_slot = slot->body[17];
	const Logical *first_uplink = runtime_c2s.first(c2s::ENTITY_UPLINK);
	EntityPacketSubHeader self{};
	std::size_t consumed = 0;
	if (first_uplink == nullptr || !decode_entity_packet_sub_header(first_uplink->body.data(),
			first_uplink->body.size(), self, consumed)) {
		fail("no decodable C2S 0x0C in client_runtime.nwmsg");
		return;
	}
	std::printf("seed: game type 0x%X, self 0x%04X type 0x%04X, team %u, slot %u\n",
			game_type, self.handle, self.item_type_id, self_team, self_slot);

	// The retail client's own checksum sources, read back off its replies.
	inmatch::IntegritySources sources;
	{
		std::vector<const Logical *> asks, answers;
		for (const Logical &m : down.messages)
			if (m.full_tag == s2c::ENTITY_CHECKSUM_REQ) asks.push_back(&m);
		for (const Logical &m : runtime_c2s.messages)
			if (m.full_tag == c2s::ENTITY_CHECKSUM_REPLY) answers.push_back(&m);
		if (asks.size() != answers.size())
			std::printf("note: %zu S2C 0x30 against %zu retail C2S 0x20\n", asks.size(), answers.size());
		for (std::size_t i = 0; i < asks.size() && i < answers.size(); ++i) {
			EntityChecksumRequest ask{};
			std::size_t used = 0;
			if (!decode_entity_checksum_request(asks[i]->body.data(), asks[i]->body.size(), ask, used) ||
					answers[i]->body.size() != 5 || ask.entity_id != 0xFFu || ask.checksum != 0)
				continue;
			const uint32_t source = read_u32(answers[i]->body, 1) ^ ask.checksum;
			if (sources.weapon_table_known && sources.weapon_table_crc != source)
				fail("the retail C2S 0x20 replies name two weapon table CRCs (0x%08X, 0x%08X)",
						sources.weapon_table_crc, source);
			sources.weapon_table_known = true;
			sources.weapon_table_crc = source;
		}
		asks.clear();
		answers.clear();
		for (const Logical &m : down.messages)
			if (m.full_tag == s2c::LOADOUT_CRC_REQ) asks.push_back(&m);
		for (const Logical &m : runtime_c2s.messages)
			if (m.full_tag == c2s::CHECKSUM_REPLY) answers.push_back(&m);
		if (asks.size() != answers.size())
			std::printf("note: %zu S2C 0x31 against %zu retail C2S 0x21\n", asks.size(), answers.size());
		for (std::size_t i = 0; i < asks.size() && i < answers.size(); ++i) {
			LoadoutCrcRequest ask{};
			std::size_t used = 0;
			if (!decode_loadout_crc_request(asks[i]->body.data(), asks[i]->body.size(), ask, used) ||
					answers[i]->body.size() != 9 || ask.ammo_index >= 0x80u)
				continue;
			const uint32_t row = read_u32(answers[i]->body, 1) ^ read_u32(answers[i]->body, 5);
			if (sources.ammo_row_crcs.size() <= ask.ammo_index)
				sources.ammo_row_crcs.resize(std::size_t(ask.ammo_index) + 1, 0u);
			sources.ammo_row_crcs[ask.ammo_index] = row;
			sources.ammo_known = true;
		}
	}

	// --- the runtime ---
	const TitleIdentity *title = title_identity(gameprofile::GAME_JO);
	const std::shared_ptr<const rep::ItemReplicationCatalog> catalog = authored_catalog();
	uint64_t now_ms = 1000;
	auto runtime = std::make_unique<inmatch::ClientRuntime>(
			"R3L", [&now_ms]() { return now_ms; });
	runtime->set_title(title);
	runtime->view().set_item_catalog(catalog);
	runtime->set_integrity_sources(sources);
	runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0, self.handle,
			self.item_type_id, game_type, /*tick_seed=*/0, /*replay_mode=*/false, self_team);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	server_tx.outbound_message_limit = 0; // the replaying host retains nothing

	const inmatch::ClientRuntime::UplinkBuilder uplink = [](PlayerExtendedUplink &) { return true; };

	const uint64_t tick_us = static_cast<uint64_t>(io::kTickMs) * 1000u;
	const uint64_t t0 = down.packets.front().ts_us;
	auto tick_of = [&](uint64_t ts) { return ts <= t0 ? uint64_t(0) : (ts - t0) / tick_us; };
	const std::size_t runtime_first = down.first_packet_of("server_runtime_packets.nwmsg");
	const uint64_t k_end =
			std::max(tick_of(down.packets.back().ts_us), tick_of(runtime_c2s.packets.back().ts_us)) + 2;

	C2STally ours, ours_before_window;
	std::size_t next = 0;
	std::size_t framed = 0;
	bool in_window = false;
	PoolCounts pools_at_load_end{};
	std::size_t malformed_seen = 0;
	std::size_t window_frames = 0;
	for (uint64_t k = 0; k <= k_end; ++k) {
		now_ms = 1000 + k * static_cast<uint64_t>(io::kTickMs);
		while (next < down.packets.size() && tick_of(down.packets[next].ts_us) <= k) {
			if (next == runtime_first) {
				pools_at_load_end = count_pools(runtime->state());
				in_window = true;
			}
			std::vector<ProtocolMessage> messages;
			for (const nwmsg::FixtureMessage &m : down.packets[next].physical)
				messages.push_back(nwmsg::message_from_fixture(m));
			const std::vector<uint8_t> datagram =
					conn_fixture::frame_server_session(server_tx, kServerScrk, kClientKey, messages);
			if (datagram.empty())
				fail("framing %s failed", down.where(next).c_str());
			else {
				runtime->receive(datagram.data(), datagram.size());
				++framed;
			}
			++next;
		}
		const std::vector<std::vector<uint8_t>> outbound =
				runtime->Client_ProcessNetworkFrame(uplink, static_cast<uint32_t>(k));
		if (in_window) ++window_frames;
		for (const std::vector<uint8_t> &datagram : outbound) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!conn_fixture::decode_client_session(datagram, kClientScrk, header, messages)) {
				uint8_t opcode = 0;
				std::vector<uint8_t> body;
				const bool unpacked = nw_decode_inbound(datagram.data(), datagram.size(), opcode, body);
				fail("frame %llu (after %s): a datagram with opcode 0x%02X%s left the client",
						static_cast<unsigned long long>(k), down.where(next == 0 ? 0 : next - 1).c_str(),
						opcode, unpacked ? "" : " (undecodable)");
				continue;
			}
			// The replaying host takes every client packet in order: its ACK retires them.
			if (header.seq_num > server_tx.last_inbound_seq) server_tx.last_inbound_seq = header.seq_num;
			for (const ProtocolMessage &m : messages) {
				if (m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) {
					++(in_window ? ours : ours_before_window).other[m.full_tag];
					continue;
				}
				if (m.flags.frag_cont || m.flags.frag_end) {
					fail("frame %llu: a fragmented C2S 0x%02X", static_cast<unsigned long long>(k), m.tag);
					continue;
				}
				// The replies compare over the whole run (the join's 0x16 draws one); the
				// timers and anything else count by window.
				const bool reply = is_reply_class(m.tag) ||
						(m.tag == c2s::RTT_CONSUMED && !is_ping(m.payload));
				tally_message(in_window || reply ? ours : ours_before_window, m.tag, m.payload, true);
			}
		}
		if (runtime->view().malformed_bodies() != malformed_seen) {
			fail("frame %llu (after %s): %zu new malformed bodies",
					static_cast<unsigned long long>(k), down.where(next == 0 ? 0 : next - 1).c_str(),
					runtime->view().malformed_bodies() - malformed_seen);
			malformed_seen = runtime->view().malformed_bodies();
		}
		if (runtime->session_lost()) {
			fail("frame %llu (after %s): the session was lost", static_cast<unsigned long long>(k),
					down.where(next == 0 ? 0 : next - 1).c_str());
			break;
		}
	}

	// --- admission ---
	std::printf("replayed %zu packets (%zu messages) over %llu frames, %zu in the in-game window\n",
			framed, down.messages.size(), static_cast<unsigned long long>(k_end + 1), window_frames);
	if (next != down.packets.size()) fail("only %zu of %zu packets delivered", next, down.packets.size());
	if (runtime->inbound_frontier_seq() != framed)
		fail("the inbound frontier is %u, %zu packets framed", runtime->inbound_frontier_seq(), framed);
	if (runtime->inbound_gap_depth() != 0)
		fail("%zu packets wait behind a gap", runtime->inbound_gap_depth());
	if (runtime->has_disconnect_event()) fail("the connection latched a disconnect event");

	// --- decode, per tag through a side reducer fed the same stream ---
	{
		auto side = std::make_unique<rep::ClientReplicaPipeline>();
		side->set_title(title);
		side->set_item_catalog(catalog);
		side->set_game_type(game_type);
		side->set_mp_session(true);
		side->set_viewer_handle(self.handle);
		std::map<uint16_t, std::size_t> seen, unknown, malformed;
		for (const Logical &m : down.messages) {
			++seen[m.full_tag];
			if (m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;
			const std::size_t u = side->unknown_tags();
			const std::size_t b = side->malformed_bodies();
			side->apply(static_cast<uint8_t>(m.full_tag), m.body);
			if (side->unknown_tags() != u) ++unknown[m.full_tag];
			if (side->malformed_bodies() != b && malformed[m.full_tag]++ == 0)
				std::printf("first malformed S2C 0x%02X at %s body %s\n", m.full_tag,
						down.where(m.packet).c_str(), hex_head(m.body).c_str());
		}
		std::printf("S2C tag  count  reducer  handler\n");
		for (const auto &kv : seen) {
			const uint16_t tag = kv.first;
			std::string handler;
			if (tag >= PROTOCOL_FULL_TAG_HIGH_BASE) {
				handler = "connection control (CS config)";
				if (tag != (PROTOCOL_FULL_TAG_HIGH_BASE | hightag::CS_CONFIG_UPDATE))
					fail("S2C high-table 0x%03X has no client handler in this replay", tag);
			} else if (unknown.count(tag) == 0) {
				handler = "ClientReplicaPipeline::apply";
			} else if (const RuntimeHandler *h = runtime_handler(static_cast<uint8_t>(tag))) {
				handler = h->site;
			} else {
				handler = "NONE";
				fail("unknown S2C tag 0x%02X (%zu messages, first at %s): no reducer arm and no "
						"connection / runtime handler", tag, kv.second,
						down.where(down.first(tag)->packet).c_str());
			}
			std::printf("  0x%03X  %5zu  %7s  %s%s\n", tag, kv.second,
					unknown.count(tag) ? "default" : "case", handler.c_str(),
					malformed.count(tag) ? "  MALFORMED" : "");
		}
		for (const auto &kv : malformed)
			fail("S2C 0x%02X: %zu malformed bodies in the side reducer", kv.first, kv.second);
	}
	if (runtime->view().malformed_bodies() != 0)
		fail("the replica counted %zu malformed bodies", runtime->view().malformed_bodies());

	// --- the connection legs' own counts ---
	const auto challenges = runtime->challenge_diagnostics();
	if (challenges.entity_checksum_seen != down.count(s2c::ENTITY_CHECKSUM_REQ) ||
			challenges.entity_checksum_answered != down.count(s2c::ENTITY_CHECKSUM_REQ))
		fail("S2C 0x30: %zu in the capture, %u seen, %u answered", down.count(s2c::ENTITY_CHECKSUM_REQ),
				challenges.entity_checksum_seen, challenges.entity_checksum_answered);
	if (challenges.loadout_crc_seen != down.count(s2c::LOADOUT_CRC_REQ) ||
			challenges.loadout_crc_answered != down.count(s2c::LOADOUT_CRC_REQ))
		fail("S2C 0x31: %zu in the capture, %u seen, %u answered", down.count(s2c::LOADOUT_CRC_REQ),
				challenges.loadout_crc_seen, challenges.loadout_crc_answered);
	if (runtime->local_player_slot() != self_slot)
		fail("S2C 0x04: the local slot is %u, the capture's byte 17 is %u",
				runtime->local_player_slot(), self_slot);

	// --- frames, self ---
	if (runtime->view().frames_applied() != down.count(s2c::PER_FRAME_UPDATE))
		fail("frames applied %u != the capture's %zu S2C 0x0A", runtime->view().frames_applied(),
				down.count(s2c::PER_FRAME_UPDATE));
	if (!runtime->has_self_handle() || runtime->self_handle() != self.handle)
		fail("the self handle is %s0x%04X, the capture's 0x%04X", runtime->has_self_handle() ? "" : "unbound ",
				runtime->self_handle(), self.handle);
	if (runtime->state().find(self.handle) == nullptr)
		fail("no replica row for the self handle 0x%04X", self.handle);

	// --- pools ---
	{
		std::set<uint16_t> unmodelled;
		const PoolCounts want_load = expected_pools(down, runtime_first, unmodelled);
		const PoolCounts want_end = expected_pools(down, down.packets.size(), unmodelled);
		const PoolCounts got_end = count_pools(runtime->state());
		std::printf("pools at the load's end: ours %s, capture %s\n", pools_text(pools_at_load_end).c_str(),
				pools_text(want_load).c_str());
		std::printf("pools at the run's end:  ours %s, capture %s\n", pools_text(got_end).c_str(),
				pools_text(want_end).c_str());
		for (uint16_t tag : unmodelled)
			std::printf("note: S2C 0x%02X writes rows the pool tally does not model\n", tag);
		if (unmodelled.empty()) {
			if (pools_at_load_end != want_load) fail("pool counts at the load's end differ");
			if (got_end != want_end) fail("pool counts at the run's end differ");
		}
	}

	// --- the Tab board against the last S2C 0x16 ---
	if (const Logical *board = down.last(s2c::PLAYER_LIST)) {
		PlayerList list;
		if (!decode_player_list(board->body.data(), board->body.size(), list)) {
			fail("the last S2C 0x16 (%s) does not decode", down.where(board->packet).c_str());
		} else {
			const rep::ClientScoreboard &sb = runtime->state().scoreboard;
			if (!sb.known || sb.in_game_count != list.in_game_count ||
					sb.spectator_count != list.spectator_count || sb.team_count != list.team_count ||
					sb.teams.size() != list.teams.size() || sb.rows.size() != list.players.size())
				fail("the board (known %d, in game %u, spectators %u, teams %u/%zu, rows %zu) != the "
						"last S2C 0x16 at %s (in game %u, spectators %u, teams %u/%zu, rows %zu)",
						sb.known ? 1 : 0, sb.in_game_count, sb.spectator_count, sb.team_count,
						sb.teams.size(), sb.rows.size(), down.where(board->packet).c_str(),
						list.in_game_count, list.spectator_count, list.team_count, list.teams.size(),
						list.players.size());
			for (std::size_t i = 0; i < sb.rows.size() && i < list.players.size(); ++i) {
				const rep::ClientScoreboardRow &got = sb.rows[i];
				const PlayerListRow &want = list.players[i];
				if (got.slot_id != want.slot_id || got.score1 != static_cast<int16_t>(want.score1) ||
						got.score2 != want.score2 || got.team != (want.flags >> 1) ||
						got.spectator != ((want.flags & 1u) != 0))
					fail("board row %zu: slot %u score %d/%u team %u spectator %d != the 0x16's slot %u "
							"score %u/%u flags 0x%02X", i, got.slot_id, got.score1, got.score2, got.team,
							got.spectator ? 1 : 0, want.slot_id, want.score1, want.score2, want.flags);
			}
		}
	} else {
		fail("no S2C 0x16 in the capture");
	}

	// --- the replies, body for body, and the timer sends ---
	C2STally retail;
	std::set<uint8_t> not_replayed;
	for (const Logical &m : startup.messages) {
		const bool reply = is_reply_class(m.full_tag) || m.full_tag == c2s::RTT_CONSUMED;
		if (reply) tally_message(retail, static_cast<uint8_t>(m.full_tag), m.body, false);
		else not_replayed.insert(static_cast<uint8_t>(m.full_tag));
	}
	for (const Logical &m : runtime_c2s.messages) {
		if (m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;
		tally_message(retail, static_cast<uint8_t>(m.full_tag), m.body, true);
	}
	for (const auto &kv : retail.other) not_replayed.insert(static_cast<uint8_t>(kv.first));
	std::string skipped;
	for (uint8_t tag : not_replayed) {
		char buf[8];
		std::snprintf(buf, sizeof buf, " 0x%02X", tag);
		skipped += buf;
	}
	std::printf("retail C2S not replayed (the join legs and the player's own input):%s\n", skipped.c_str());

	std::printf("C2S reply  ours  retail\n");
	for (int cls : kReplyClasses) {
		const std::vector<std::vector<uint8_t>> &got = ours.replies[cls];
		const std::vector<std::vector<uint8_t>> &want = retail.replies[cls];
		std::printf("  %s0x%02X  %5zu  %6zu\n", cls == kPong ? "pong " : "     ", cls & 0xFF,
				got.size(), want.size());
		std::size_t first_diff = std::min(got.size(), want.size());
		for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i)
			if (got[i] != want[i]) {
				first_diff = i;
				break;
			}
		if (got.size() != want.size() || first_diff != std::min(got.size(), want.size())) {
			const std::vector<uint8_t> none;
			fail("C2S %s0x%02X: ours %zu, retail %zu; first difference at #%zu: ours %s retail %s",
					cls == kPong ? "pong " : "", cls & 0xFF, got.size(), want.size(), first_diff,
					hex_head(first_diff < got.size() ? got[first_diff] : none).c_str(),
					hex_head(first_diff < want.size() ? want[first_diff] : none).c_str());
		}
	}
	for (const auto &kv : ours.other)
		fail("C2S 0x%03X left the client %zu times in the in-game window; the retail client sent none "
				"of it there", kv.first, kv.second);
	for (const auto &kv : ours_before_window.other)
		fail("C2S 0x%03X left the client %zu times before the in-game window", kv.first, kv.second);

	const std::size_t interval = inmatch::ClientProcessState::kNetQualityReportInterval + 1;
	std::printf("timers in the window (%zu frames): 0x4C ours %zu retail %zu (one per %zu frames), "
			"0x2C pings ours %zu retail %zu, 0x0C ours %zu retail %zu; before it: 0x4C %zu, pings %zu, "
			"0x0C %zu\n", window_frames, ours.reports, retail.reports, interval, ours.pings, retail.pings,
			ours.uplinks, retail.uplinks, ours_before_window.reports, ours_before_window.pings,
			ours_before_window.uplinks);
	if (abs_diff(ours.reports, retail.reports) > 1)
		fail("C2S 0x4C: ours %zu, retail %zu over the window", ours.reports, retail.reports);
	if (abs_diff(ours.pings, retail.pings) > timer_tolerance(retail.pings))
		fail("C2S 0x2C pings: ours %zu, retail %zu over the window (tolerance %zu)", ours.pings,
				retail.pings, timer_tolerance(retail.pings));
	if (abs_diff(ours.uplinks, retail.uplinks) > timer_tolerance(retail.uplinks))
		fail("C2S 0x0C: ours %zu, retail %zu over the window (tolerance %zu)", ours.uplinks,
				retail.uplinks, timer_tolerance(retail.uplinks));
	if (!ours.report_bodies.empty() && !retail.report_bodies.empty() &&
			ours.report_bodies.front() != retail.report_bodies.front())
		std::printf("note: C2S 0x4C level ours %s, retail %s\n", hex_head(ours.report_bodies.front()).c_str(),
				hex_head(retail.report_bodies.front()).c_str());
}

} // namespace

int main() {
	const std::string dir = std::string(FIXTURE_DIR) + "/" + kRun;
	run_client_leg(dir);
	if (g_failures != 0) {
		std::printf("npruntime_retail_s2c_replay: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_retail_s2c_replay: ok\n");
	return 0;
}
