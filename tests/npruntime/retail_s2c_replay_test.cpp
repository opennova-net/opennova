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
//   - the Tab board == the last S2C 0x16, less the rows the retail client re-requested
//     right after it (its parser drops a row whose slot is not active yet);
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
//
// The client leg runs the corpus's two other runs too (run_20260426_113242,
// run_20260426_120102), which keep the join stream and a sample of steady packets with no load
// stream between (RunSpec): there it asserts the admission, the decode, the handlers, the
// S2C 0x04 slot, the frames and the board (where a 0x16 is kept), and the replies whose
// oracle the kept streams hold, as a prefix of the client's kept C2S (run_client_leg).
//
// The host twin. The capture's C2S stream (client_startup, client_runtime: one bundle per
// retail packet) is framed packet by packet as the joiner's 0x43 and fed, at the capture's
// own timing, into a socketless host (bring_up_host) that the twin's joiner reached through
// a 0x41 / 0x42 of its own (the capture keeps no handshake). The host is configured as the
// capture's was: the S2C 0x08's game type, the JOIN's expansion and VERSIONCRCSTRING, the
// S2C 0x04's slot capacity. The host's producer runs between packets (tick_connections, the
// spawn pump once per periodic second); a packet with an in-match record waits until the
// host has added the player, as the retail client's did (it answered the add's records).
// Asserted, each failure naming the tag and the packet:
//   - every packet deframes in order; no description (disconnect) is queued; the joiner is
//     never torn down;
//   - the 0x00 / 0x01 / 0x02 reach the admission walk in its turn; every other record reaches
//     the in-match switch;
//   - the replies a handler sends in the twin's state, tag for tag per packet (kReplyRules),
//     each a tag the retail host's own stream carries.
// The capture keeps no C2S of the joiner's load (its 0x2F, its 0x0B), so the twin's slot waits
// at state 5 and the in-game handlers that need a joined slot stop at their join gates; their
// reply tags are printed, not asserted.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/inmatch/napi_np_protocol.h>   // handle_server_datagram / tick_connections
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_spawn.h>       // Server_InitNewRoundState / the spawn pump
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_state.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/world/entity.h> // world::kRetailPoolCapacity
#include <runtime/world/world.h>

#include <base/io/crt_ftol.h> // retail_atol (the VERSIONCRCSTRING the host's check reads)
#include <base/io/tick_rate.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h> // mission::make_default
#include <net/npwire/flat_tlv.h>      // the C2S 0x00 JOIN body's fields
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h> // decode_session_config
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // the twin joiner's 0x41 / 0x42
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>

#include "common/nwmsg_fixture.h"
#include "conn_fixture.h"
#include "host_test_setup.h"

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
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace rep = opennova::replication;

int g_failures = 0;
const char *g_leg = "";

void fail(const char *fmt, ...) {
	std::va_list args;
	va_start(args, fmt);
	std::printf("FAIL R3L: [%s] ", g_leg);
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

// One capture run's files. run_20260426_120859 keeps the whole session (the join, the load and
// the runtime S2C, one bundle per packet, and the client's runtime C2S); the other two keep the
// join stream and a sample of steady packets (the server's join bundle and one or two after it),
// the client's join-window C2S and its first in-game bundle, with no load stream between.
struct RunSpec {
	const char *run;
	std::vector<std::string> down;    // the S2C files, in session order
	std::string in_game_file;         // the S2C file whose first packet ends the final load wait
	std::string runtime_c2s_file;     // the client's in-game C2S
	bool full;                        // the run keeps its load and runtime streams
};

const RunSpec kRunSpecs[] = {
	{"run_20260426_120859",
			{"server_join_packets.nwmsg", "server_load_packets.nwmsg", "server_runtime_packets.nwmsg"},
			"server_runtime_packets.nwmsg", "client_runtime.nwmsg", true},
	{"run_20260426_113242", {"server_join.nwmsg", "server_steady.nwmsg"}, "server_steady.nwmsg",
			"client_input.nwmsg", false},
	{"run_20260426_120102", {"server_join.nwmsg", "server_steady.nwmsg"}, "server_steady.nwmsg",
			"client_input.nwmsg", false},
};

// The host twin's run: the one whose C2S stream is whole.
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
			static_cast<std::size_t>(handle & 0x0FFFu) < world::kRetailPoolCapacity[pool];
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
						static_cast<std::size_t>(rec.slot_id & 0x0FFFu) >= world::kRetailPoolCapacity[1])
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
				if (slot >= world::kRetailPoolCapacity[2]) continue;
				present[static_cast<uint16_t>(0x2000u | slot)] = !batch.records[i].is_empty_slot;
			}
		} else if (m.full_tag == s2c::POOL3_SYNC) {
			Pool3SyncBatch batch;
			if (!decode_pool3_sync_batch(m.body.data(), m.body.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			for (std::size_t i = 0; i < batch.records.size(); ++i) {
				const std::size_t slot = static_cast<std::size_t>(batch.start_index) + i;
				if (slot >= world::kRetailPoolCapacity[3]) continue;
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

// A run that keeps no load stream (RunSpec::full false) replays its join stream and its steady
// sample with the S2C gap between them closed to one second; what needs the missing load is
// printed, not asserted:
// the pool tally and the self row (the load's spawn pages), the timer sends (no runtime C2S),
// the C2S 0x28's first word (the load's S2C 0x19 stamp), the 0x0F carrier repairs (the load's
// carriers), and every retail reply record the client sent inside the gap (its trigger rode the
// missing stream). Its other replies compare as a prefix: the client's later C2S is not kept.
void run_client_leg(const std::string &fixture_root, const RunSpec &spec) {
	const std::string dir = fixture_root + "/" + spec.run;
	Stream down, startup, runtime_c2s, input;
	if (!load_stream(dir, spec.down, down) ||
			!load_stream(dir, {"client_startup.nwmsg"}, startup) ||
			!load_stream(dir, {spec.runtime_c2s_file}, runtime_c2s) ||
			!load_stream(dir, {"client_input.nwmsg"}, input))
		return;
	if (down.packets.empty() || runtime_c2s.packets.empty()) {
		fail("the corpus streams are empty");
		return;
	}
	if (spec.full) {
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
	const Protocol *protocol = title_protocol("jo");
	const std::shared_ptr<const rep::ItemReplicationCatalog> catalog = authored_catalog();
	uint64_t now_ms = 1000;
	auto runtime = std::make_unique<inmatch::ClientRuntime>(
			"R3L", [&now_ms]() { return now_ms; });
	runtime->set_protocol(protocol);
	runtime->view().set_item_catalog(catalog);
	runtime->set_integrity_sources(sources);
	runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0, self.handle,
			self.item_type_id, game_type, /*tick_seed=*/0, /*replay_mode=*/false, self_team);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	server_tx.outbound_message_limit = 0; // the replaying host retains nothing

	const inmatch::ClientRuntime::UplinkBuilder uplink = [](PlayerExtendedUplink &, uint16_t &) { return true; };

	const uint64_t tick_us = static_cast<uint64_t>(io::kTickMs) * 1000u;
	const uint64_t t0 = down.packets.front().ts_us;
	const std::size_t runtime_first = down.first_packet_of(spec.in_game_file);
	if (runtime_first == 0 || runtime_first >= down.packets.size()) {
		fail("no %s after the join stream", spec.in_game_file.c_str());
		return;
	}
	// A run with no load stream closes its S2C gap to one second: the gap held the retail
	// client's load, which a joiner seeded past it does not run, and a silence that long leaves
	// nothing to ACK the seeded client's own sends.
	const uint64_t gap_from = down.packets[runtime_first - 1].ts_us;
	const uint64_t gap_to = down.packets[runtime_first].ts_us;
	const uint64_t gap_shift =
			!spec.full && gap_to - gap_from > 1000000u ? gap_to - gap_from - 1000000u : 0;
	if (gap_shift != 0)
		std::printf("note: the S2C gap ahead of %s (%.1f s, the load) replays as 1 s\n",
				down.where(runtime_first).c_str(), static_cast<double>(gap_to - gap_from) / 1e6);
	auto tick_of = [&](uint64_t ts) {
		if (ts >= gap_to) ts -= gap_shift;
		return ts <= t0 ? uint64_t(0) : (ts - t0) / tick_us;
	};
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
		side->set_protocol(protocol);
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
	if (runtime->state().find(self.handle) == nullptr) {
		if (spec.full) fail("no replica row for the self handle 0x%04X", self.handle);
		else std::printf("note: no replica row for the self handle 0x%04X (its 0x0C rides the load)\n", self.handle);
	}

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
		if (!spec.full)
			std::printf("note: no load stream: the pool counts are not compared\n");
		else if (unmodelled.empty()) {
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
			// Retail's parser keeps only the rows whose connection slot is active when the 0x16
			// arrives; a row on an inactive slot is dropped and re-requested with one C2S 0x22
			// {slot, 0xF7, 0x1C} [orig: NapiNPClientMsg_PlayerList @0x42FAE0 -
			// the PlayerSlotTable_GetActiveSlot call @0x42FBE4, the drop and the re-request
			// @0x42FC05..0x42FC3A], and only the 0x16 parser writes the row count (@0x42FB46,
			// @0x42FC8E). Only an S2C 0x46 or 0x4C activates a slot [orig:
			// PlayerSlotTable_GetOrInitSlot @0x4346C0, called @0x431461 and @0x428643]. The
			// re-requests in the retail client's first packet after the 0x16 name the rows its
			// parser dropped, so those rows leave the expected board.
			std::set<uint8_t> retail_dropped;
			{
				const uint64_t at = down.packets[board->packet].ts_us;
				const Stream *next_stream = nullptr;
				std::size_t next_packet = 0;
				for (const Stream *s : {&startup, &runtime_c2s})
					for (std::size_t p = 0; p < s->packets.size(); ++p)
						if (s->packets[p].ts_us > at &&
								(next_stream == nullptr ||
										s->packets[p].ts_us < next_stream->packets[next_packet].ts_us)) {
							next_stream = s;
							next_packet = p;
						}
				if (next_stream != nullptr)
					for (const Logical &m : next_stream->messages)
						if (m.packet == next_packet && m.full_tag == c2s::PLAYER_SYNC_REQUEST &&
								m.body.size() == 3 && m.body[1] == 0xF7 && m.body[2] == 0x1C)
							retail_dropped.insert(m.body[0]);
			}
			std::vector<PlayerListRow> kept;
			for (const PlayerListRow &r : list.players)
				if (retail_dropped.count(r.slot_id) == 0) kept.push_back(r);
			if (kept.size() != list.players.size())
				std::printf("note: the retail client re-requested %zu of the 0x16's %zu rows at %s "
						"(C2S 0x22 {slot, F7 1C}): its board drops them\n",
						list.players.size() - kept.size(), list.players.size(),
						down.where(board->packet).c_str());
			const rep::ClientScoreboard &sb = runtime->state().scoreboard;
			if (!sb.known || sb.in_game_count != list.in_game_count ||
					sb.spectator_count != list.spectator_count || sb.team_count != list.team_count ||
					sb.teams.size() != list.teams.size() || sb.rows.size() != kept.size())
				fail("the board (known %d, in game %u, spectators %u, teams %u/%zu, rows %zu) != the "
						"last S2C 0x16 at %s (in game %u, spectators %u, teams %u/%zu, rows %zu of %zu)",
						sb.known ? 1 : 0, sb.in_game_count, sb.spectator_count, sb.team_count,
						sb.teams.size(), sb.rows.size(), down.where(board->packet).c_str(),
						list.in_game_count, list.spectator_count, list.team_count, list.teams.size(),
						kept.size(), list.players.size());
			for (std::size_t i = 0; i < sb.rows.size() && i < kept.size(); ++i) {
				const rep::ClientScoreboardRow &got = sb.rows[i];
				const PlayerListRow &want = kept[i];
				if (got.slot_id != want.slot_id || got.score1 != static_cast<int16_t>(want.score1) ||
						got.score2 != want.score2 || got.team != (want.flags >> 1) ||
						got.spectator != ((want.flags & 1u) != 0))
					fail("board row %zu: slot %u score %d/%u team %u spectator %d != the 0x16's slot %u "
							"score %u/%u flags 0x%02X", i, got.slot_id, got.score1, got.score2, got.team,
							got.spectator ? 1 : 0, want.slot_id, want.score1, want.score2, want.flags);
			}
		}
	} else if (spec.full) {
		fail("no S2C 0x16 in the capture");
	} else {
		std::printf("note: no S2C 0x16 in the kept streams: the board is not compared\n");
	}

	// --- the replies, body for body, and the timer sends ---
	// A run with no load stream drops the retail replies the client sent inside the S2C gap:
	// what they answered rode the stream the run does not keep.
	auto in_gap = [&](uint64_t ts) { return !spec.full && ts > gap_from && ts < gap_to; };
	C2STally retail;
	std::set<uint8_t> not_replayed;
	std::map<uint8_t, std::size_t> gap_dropped;
	for (const Logical &m : startup.messages) {
		const bool reply = is_reply_class(m.full_tag) || m.full_tag == c2s::RTT_CONSUMED;
		if (reply && in_gap(startup.packets[m.packet].ts_us)) ++gap_dropped[static_cast<uint8_t>(m.full_tag)];
		else if (reply) tally_message(retail, static_cast<uint8_t>(m.full_tag), m.body, false);
		else not_replayed.insert(static_cast<uint8_t>(m.full_tag));
	}
	for (const Logical &m : runtime_c2s.messages) {
		if (m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;
		tally_message(retail, static_cast<uint8_t>(m.full_tag), m.body, true);
	}
	for (const auto &kv : gap_dropped)
		std::printf("note: %zu retail C2S 0x%02X sent inside the S2C gap (not compared)\n", kv.second, kv.first);
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
		// With no load stream, the 0x28's first word is the load's S2C 0x19 stamp and a 0x0F
		// asks after a carrier the load spawned: neither has its oracle.
		const bool load_bound = cls == c2s::LOADOUT_REQUEST || cls == c2s::ENTITY_INFO_QUERY;
		std::printf("  %s0x%02X  %5zu  %6zu%s\n", cls == kPong ? "pong " : "     ", cls & 0xFF,
				got.size(), want.size(), !spec.full && load_bound ? "  (needs the load: printed only)" : "");
		if (!spec.full && load_bound) continue;
		std::size_t first_diff = std::min(got.size(), want.size());
		for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i)
			if (got[i] != want[i]) {
				first_diff = i;
				break;
			}
		// The whole run's replies compare exactly; a sample's, as the prefix its kept C2S covers.
		const bool counts_differ = spec.full ? got.size() != want.size() : got.size() < want.size();
		if (counts_differ || first_diff != std::min(got.size(), want.size())) {
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
	if (!spec.full) {
		std::printf("note: no runtime C2S: the timer sends are not compared\n");
		return;
	}
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

// --- the host twin ---

constexpr uint32_t kTwinHostKey = 0x4833524Cu;   // the host's HK, echoed in the twin's 0x42
constexpr uint32_t kTwinClientKey = 0x4A33524Cu; // the twin joiner's CK
const std::string kTwinClientScrk = "R3L-TWIN-CLIENT-SCRK";
const PeerAddr kTwinPeer{0x0100007Fu, 30002};

// The stock client's 0x42 environment block (the build values the host's JOIN check reads:
// the retail host admitted this client, so its environment was the title's).
void add_stock_environment(ClientAuth &auth) {
	const std::pair<const char *, const char *> fields[] = {
		{"BT", "0"}, {"VN", "2"}, {"BN", "1"}, {"DB", "0"}, {"MBN", "20042002"},
		{"SOPD", "180"}, {"VERSIONSTRING", "V1.7.5.7"}, {"COUNTRYCODE", "us"},
		{"TZB", "300"}, {"MPS", "1300"},
	};
	for (const auto &field : fields) auth.cu.push_back(make_client_cu_chunk(2, field.first, field.second));
}

// One string field of the C2S 0x00 JOIN body's flat-TLV run ("" when absent).
std::string join_field(const std::vector<uint8_t> &body, const std::string &name) {
	std::string value;
	std::size_t pos = 0;
	while (pos < body.size()) {
		FlatTlvField field;
		const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		if (field.name == name) value = load_value_string(field.value, body.data() + body.size(), 512);
		pos = next;
	}
	return value;
}

// The reply a record's handler sends in the twin's state, -1 for none asserted. Each pair is
// a requester-only answer with no state gate the twin's slot fails (the admission's three, the
// in-match legs a joiner's added slot answers before its join). Records whose answer needs a
// joined slot or host state the twin does not model (0x0E, 0x06, 0x25, 0x28, 0x29, 0x4E, ...)
// assert nothing here; their reply tags print.
int expected_reply(const Logical &record, bool admission, uint32_t capacity) {
	const std::vector<uint8_t> &b = record.body;
	if (admission) {
		switch (record.full_tag) {
			case c2s::JOIN: return s2c::INIT;                     // dispatch_admission's 0x00
			case c2s::JOIN_FORM_POST: return s2c::JOIN_PADDING_PROBE; // admission_padding_probe
			default: return -1; // the 0x02's four records: kAdmissionEchoReplies
		}
	}
	switch (record.full_tag) {
		case c2s::PING: return s2c::SPECTATOR_FLAGS;
		case c2s::FILE_CHUNK_REQUEST: return s2c::FILE_TRANSFER_CHUNK;
		case c2s::MISSION_CHUNK_REQUEST: return s2c::MISSION_DATA_CHUNK;
		case c2s::PLAYER_SYNC_REQUEST: return !b.empty() && b[0] < capacity ? s2c::PLAYER_SYNC : -1;
		case c2s::SPAWN_MENU_REQUEST: return s2c::SPAWN_ACK_TIMESTAMP;
		case c2s::VISIBLE_PLAYERS_REQUEST: return b.empty() ? s2c::VISIBLE_PLAYERS : -1;
		case c2s::BURST_MEMBER_2D: return s2c::SESSION_STATUS;
		case c2s::EMPTY_SLOT_SWEEP_REQUEST: return s2c::EMPTY_SLOT_SWEEP;
		case c2s::RTT_CONSUMED: return is_ping(b) ? s2c::RTT_ECHO : -1;
		default: return -1;
	}
}

// The admission's completing 0x02 answers its four records (emit_admission_metadata), behind
// the holdoff dictation's settings records.
const uint8_t kAdmissionEchoReplies[] = {s2c::SYNC_STATE, s2c::PLAYER_NAME, s2c::FULL_PLAYER_INFO,
		s2c::SYNC_TICK};

// Every reply tag the rules above can name: the per-packet comparison's columns.
const uint8_t kAssertedReplies[] = {s2c::INIT, s2c::SYNC_STATE, s2c::JOIN_PADDING_PROBE, s2c::SYNC_TICK,
		s2c::PLAYER_NAME, s2c::FULL_PLAYER_INFO, s2c::SPECTATOR_FLAGS, s2c::FILE_TRANSFER_CHUNK,
		s2c::MISSION_DATA_CHUNK, s2c::PLAYER_SYNC, s2c::SPAWN_ACK_TIMESTAMP, s2c::VISIBLE_PLAYERS,
		s2c::SESSION_STATUS, s2c::EMPTY_SLOT_SWEEP, s2c::RTT_ECHO};

std::string tags_text(const std::vector<uint8_t> &tags) {
	std::string out;
	char buf[8];
	for (uint8_t t : tags) {
		std::snprintf(buf, sizeof buf, " %02x", t);
		out += buf;
	}
	return out.empty() ? " -" : out;
}

struct TwinHost {
	std::unique_ptr<world::World> host_world = std::make_unique<world::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::unique_ptr<bms::File> mission_file = std::make_unique<bms::File>();
	replication::UdpSessionTransport transport{replication::UdpSessionTransport::Role::Host};

	inmatch::NapiNPConnection *joiner() {
		for (inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list)
			if (c.peer == kTwinPeer && c.type == inmatch::NapiNPConnection::kTypeServerSide) return &c;
		return nullptr;
	}

	// The low-table records the transport holds, in order; a queued description (the
	// disconnect / punt carrier) fails at once.
	std::vector<uint8_t> drain(const std::string &where) {
		std::vector<uint8_t> tags;
		replication::Datagram d;
		while (transport.pop_outbound(d)) {
			if ((d.protocol_flags_raw & PROTOCOL_MSG_FLAG_SETTINGS_UPDATE) != 0) {
				if (d.tag == hightag::DESCRIPTION_PACKET)
					fail("host twin: %s: the host queued a description (a disconnect) %s", where.c_str(),
							hex_head(d.body, 40).c_str());
				continue;
			}
			tags.push_back(d.tag);
		}
		return tags;
	}
};

void run_host_twin(const std::string &dir) {
	Stream c2s_stream, down;
	if (!load_stream(dir, {"client_startup.nwmsg", "client_runtime.nwmsg"}, c2s_stream) ||
			!load_stream(dir, {"server_join_packets.nwmsg", "server_load_packets.nwmsg",
					"server_runtime_packets.nwmsg"}, down))
		return;

	// --- the host, configured as the capture's was ---
	const Logical *join = c2s_stream.first(c2s::JOIN);
	const Logical *config_msg = down.first(s2c::SESSION_CONFIG);
	const Logical *slot_msg = down.first(s2c::SESSION_SLOT_CONFIG);
	SessionConfig session{};
	if (join == nullptr || config_msg == nullptr || slot_msg == nullptr || slot_msg->body.size() < 24 ||
			!decode_session_config(config_msg->body.data(), config_msg->body.size(), session)) {
		fail("host twin: the capture lacks its C2S 0x00, a decodable S2C 0x08 or a 24-byte S2C 0x04");
		return;
	}
	const std::string expansion = join_field(join->body, "EXP");
	const std::string version_crc = join_field(join->body, "VERSIONCRCSTRING");

	auto host = std::make_unique<TwinHost>();
	world::World &w = *host->host_world;
	inmatch::NapiNPServerCtx &ctx = *host->ctx;
	w.rules.mp_session = true;
	w.registry.configure_pool(0, 16);
	w.registry.configure_pool(1, 8);
	w.registry.configure_pool(2, 8);
	w.registry.configure_pool(3, 16);
	{
		world::Entity start; // a start marker for the joiner's add
		start.kind = world::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {10.0f, 20.0f, 1.0f};
		start.item_type_index = 1;
		start.has_item_def = true;
		w.registry.spawn(3, start);
	}
	mission::make_default(*host->mission_file);
	inmatch::GameConfig config;
	config.game_type = static_cast<uint32_t>(session.fields[3]);
	config.max_players = slot_msg->body[18]; // the capture's slot capacity
	config.expansion = expansion;
	config.expansion_version_checksum = io::retail_atol(version_crc.c_str());
	// A Serve Only host: its slot 0 is the host's row, so the joiner takes slot 1, as the
	// capture's (S2C 0x04 byte 17) did on its listen host.
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Socketless, kTwinHostKey, nullptr, config);
	ctx.world = &w;
	ctx.mission = host->mission_file.get();
	inmatch::Server_InitNewRoundState(ctx);
	const uint32_t capacity = ctx.config.total_player_slot_capacity();
	std::printf("host twin: game type 0x%X, EXP %s, VERSIONCRCSTRING %s, slot capacity %u (the "
			"capture's 0x04: %u), authority %u\n", config.game_type, expansion.c_str(), version_crc.c_str(),
			capacity, static_cast<unsigned>(slot_msg->body[18]), static_cast<unsigned>(ctx.is_authority));

	// --- the twin joiner's 0x41 / 0x42 ---
	uint32_t host_tick = 1;
	const Protocol &protocol = *title_protocol("jo");
	{
		const std::vector<uint8_t> hello = nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
				client_hello_to_bytes(make_protocol_client_hello(protocol, 1)));
		(void)inmatch::handle_server_datagram(ctx, kTwinPeer, hello.data(), hello.size(), host_tick);
	}
	uint32_t server_sk = 0;
	{
		ClientAuth auth = make_protocol_client_auth(protocol, 1, kTwinClientKey, kTwinHostKey, "R3L-JOINER",
				kTwinClientScrk);
		add_stock_environment(auth);
		const std::vector<uint8_t> datagram =
				nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		const inmatch::HandleResult accepted =
				inmatch::handle_server_datagram(ctx, kTwinPeer, datagram.data(), datagram.size(), host_tick);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ServerAuth server_auth;
		if (accepted.outbound.empty() ||
				!nw_decode_inbound(accepted.outbound[0].data(), accepted.outbound[0].size(), opcode, body) ||
				opcode != SESSION_OPCODE_SERVER_AUTH || !parse_server_auth(body.data(), body.size(), server_auth) ||
				host->joiner() == nullptr) {
			fail("host twin: the host did not accept the twin joiner's 0x42");
			return;
		}
		server_sk = server_auth.sk;
	}
	// The owner binds the accepted connection's transport (HostOwner's accept).
	host->joiner()->link.transport = &host->transport;
	host->joiner()->link.mode = replication::TransportMode::Client;

	// --- the corpus's C2S, packet by packet ---
	std::vector<std::vector<const Logical *>> records(c2s_stream.packets.size());
	for (const Logical &m : c2s_stream.messages) records[m.packet].push_back(&m);
	auto needs_slot = [&](std::size_t p) {
		for (const Logical *r : records[p])
			if (r->full_tag > c2s::JOIN_PADDING_ECHO) return true;
		return false;
	};

	SessionSequencing client_tx = inmatch::make_jo_game_session_sequencing();
	client_tx.outbound_message_limit = 0; // the replaying joiner retains nothing
	const uint64_t tick_us = static_cast<uint64_t>(io::kTickMs) * 1000u;
	const uint64_t t0 = c2s_stream.packets.front().ts_us;
	auto tick_of = [&](uint64_t ts) { return ts <= t0 ? uint64_t(0) : (ts - t0) / tick_us; };
	const uint64_t k_last = tick_of(c2s_stream.packets.back().ts_us);
	const uint64_t k_limit = k_last + 10u * static_cast<uint64_t>(io::kTicksPerSecondInt);

	std::map<uint16_t, std::size_t> seen, at_admission, at_switch;
	std::map<uint8_t, std::size_t> reply_histogram, producer_histogram;
	std::set<uint8_t> asserted_replies;
	std::size_t next = 0;
	uint32_t framed = 0;
	uint64_t held_since = 0;
	bool holding = false;
	for (uint64_t k = 0; next < c2s_stream.packets.size() && k <= k_limit; ++k, ++host_tick) {
		while (next < c2s_stream.packets.size() && tick_of(c2s_stream.packets[next].ts_us) <= k) {
			inmatch::NapiNPConnection *conn = host->joiner();
			if (conn == nullptr) {
				fail("host twin: the joiner's connection is gone before %s", c2s_stream.where(next).c_str());
				return;
			}
			if (needs_slot(next) && !player_slot_active(*conn)) {
				if (!holding) held_since = k;
				holding = true;
				break; // the retail client answered the add's records: hold for the add
			}
			if (holding && k != held_since)
				std::printf("host twin: %s held %llu frames for the player add\n", c2s_stream.where(next).c_str(),
						static_cast<unsigned long long>(k - held_since));
			holding = false;
			const std::string where = c2s_stream.where(next);
			for (uint8_t t : host->drain(where)) ++producer_histogram[t];

			// The state each record meets: the admission walk until the 0x02 completes it, the
			// in-match switch after (the slot gate open, no description sent).
			bool admission = conn->admission_stage != inmatch::GameAdmissionStage::Complete;
			const bool slot_active = player_slot_active(*conn);
			const uint32_t id_before = conn->connection_id;
			std::map<uint8_t, std::size_t> want_replies;
			std::vector<uint8_t> record_tags;
			for (const Logical *r : records[next]) {
				record_tags.push_back(static_cast<uint8_t>(r->full_tag));
				++seen[r->full_tag];
				if (admission) {
					if (r->full_tag > c2s::JOIN_PADDING_ECHO)
						fail("host twin: C2S 0x%02X at %s arrives ahead of the admission's end: no handler "
								"answers it", r->full_tag, where.c_str());
					else
						++at_admission[r->full_tag];
					if (r->full_tag == c2s::JOIN_PADDING_ECHO)
						for (uint8_t t : kAdmissionEchoReplies) ++want_replies[t];
				} else if (r->full_tag > c2s::JOIN_PADDING_ECHO && !slot_active) {
					fail("host twin: C2S 0x%02X at %s meets the slot gate (no player added): no handler "
							"answers it", r->full_tag, where.c_str());
				} else {
					++at_switch[r->full_tag];
				}
				const int reply = expected_reply(*r, admission, capacity);
				if (reply >= 0) ++want_replies[static_cast<uint8_t>(reply)];
				if (admission && r->full_tag == c2s::JOIN_PADDING_ECHO) admission = false;
			}

			std::vector<ProtocolMessage> messages;
			for (const nwmsg::FixtureMessage &m : c2s_stream.packets[next].physical)
				messages.push_back(nwmsg::message_from_fixture(m));
			std::vector<uint8_t> body;
			if (!frame_session_packet(client_tx, SessionCrypto{kTwinClientScrk, {}, server_sk}, messages, body)) {
				fail("host twin: framing %s failed", where.c_str());
				return;
			}
			const std::vector<uint8_t> datagram =
					nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
			const inmatch::HandleResult result = inmatch::handle_server_datagram(
					ctx, kTwinPeer, datagram.data(), datagram.size(), host_tick);
			++framed;
			++next;
			for (const inmatch::HostAcceptEvent &e : result.events)
				if (e.kind == inmatch::HostAcceptEvent::Kind::PeerGoodbye)
					fail("host twin: %s: the host dropped the joiner", where.c_str());
			conn = host->joiner();
			if (conn == nullptr) {
				fail("host twin: %s: the joiner's connection was torn down", where.c_str());
				return;
			}
			if (conn->seq.last_inbound_seq != framed || !conn->seq.queued_inbound.empty())
				fail("host twin: %s: the host's inbound frontier is %u (%zu queued), %u packets framed",
						where.c_str(), conn->seq.last_inbound_seq, conn->seq.queued_inbound.size(), framed);
			if (conn->host_disconnect_sent) fail("host twin: %s: the host sent the joiner a description", where.c_str());
			if (conn->connection_id != id_before)
				std::printf("note: host twin: %s moved the joiner's connection id %u -> %u (our C2S 0x48 "
						"leg; retail's handler reads nothing)\n", where.c_str(), id_before, conn->connection_id);

			// The replies, tag for tag.
			const std::vector<uint8_t> replies = host->drain(where);
			std::map<uint8_t, std::size_t> got;
			for (uint8_t t : replies) {
				++got[t];
				++reply_histogram[t];
			}
			for (uint8_t t : kAssertedReplies) {
				const std::size_t want = want_replies.count(t) ? want_replies[t] : 0;
				const std::size_t have = got.count(t) ? got[t] : 0;
				if (want != 0) asserted_replies.insert(t);
				if (want != have)
					fail("host twin: %s (C2S%s): S2C 0x%02X x%zu queued, x%zu expected (replies%s)", where.c_str(),
							tags_text(record_tags).c_str(), t, have, want, tags_text(replies).c_str());
			}
			if (c2s_stream.packets[next - 1].file == "client_startup.nwmsg")
				std::printf("host twin: %s C2S%s -> S2C%s\n", where.c_str(), tags_text(record_tags).c_str(),
						tags_text(replies).c_str());
		}
		// The host's frame: the spawn pump once per periodic second, then the producer.
		if (k % static_cast<uint64_t>(io::kTicksPerSecondInt) == 0)
			(void)inmatch::Server_ProcessPendingPlayerSpawns(ctx, w);
		(void)inmatch::tick_connections(ctx, static_cast<int>(io::kTickMs), host_tick);
		for (uint8_t t : host->drain("the producer")) ++producer_histogram[t];
	}

	// --- the run ---
	if (next != c2s_stream.packets.size())
		fail("host twin: only %zu of %zu packets delivered (%s waits for the player add)", next,
				c2s_stream.packets.size(), c2s_stream.where(next).c_str());
	if (const inmatch::NapiNPConnection *conn = host->joiner()) {
		if (conn->admission_stage != inmatch::GameAdmissionStage::Complete)
			fail("host twin: the admission never completed");
		if (!conn->c2s_reassembly.buffer.empty()) fail("host twin: a C2S fragment chain is still open");
		std::printf("host twin: %u packets, %zu records; the joiner's slot %u in state %u, phase %u\n", framed,
				c2s_stream.messages.size(), conn->reply.player_slot, static_cast<unsigned>(conn->slot_state),
				static_cast<unsigned>(conn->phase));
	}
	std::printf("host twin: C2S tag  records  admission  switch\n");
	for (const auto &kv : seen) {
		const uint16_t tag = kv.first;
		const std::size_t handled = at_admission[tag] + at_switch[tag];
		std::printf("  0x%02X  %7zu  %9zu  %6zu%s\n", tag, kv.second, at_admission[tag], at_switch[tag],
				tag == c2s::CLIENT_ACK
						? "  (retail: NapiNPServerMsg_0x048 @0x510F30, a lone retn)"
						: "");
		if (handled != kv.second)
			fail("host twin: C2S 0x%02X: %zu of %zu records reached a handler", tag, handled, kv.second);
	}
	std::string reply_line, producer_line;
	char buf[24];
	for (const auto &kv : reply_histogram) {
		std::snprintf(buf, sizeof buf, " %02x:%zu", kv.first, kv.second);
		reply_line += buf;
	}
	for (const auto &kv : producer_histogram) {
		std::snprintf(buf, sizeof buf, " %02x:%zu", kv.first, kv.second);
		producer_line += buf;
	}
	std::printf("host twin: S2C replies (tag:count)%s\nhost twin: S2C from the producer%s\n", reply_line.c_str(),
			producer_line.c_str());
	// Each asserted reply is one the retail host sent on this session.
	for (uint8_t t : asserted_replies)
		if (down.count(t) == 0)
			fail("host twin: the asserted reply S2C 0x%02X never appears in the retail host's stream", t);
}

} // namespace

int main() {
	const std::string root = FIXTURE_DIR;
	for (const RunSpec &spec : kRunSpecs) {
		g_leg = spec.run;
		std::printf("== the client leg: %s%s ==\n", spec.run, spec.full ? "" : " (join + steady sample)");
		run_client_leg(root, spec);
	}
	g_leg = kRun;
	std::printf("== the host twin: %s ==\n", kRun);
	run_host_twin(root + "/" + kRun);
	if (g_failures != 0) {
		std::printf("npruntime_retail_s2c_replay: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_retail_s2c_replay: ok\n");
	return 0;
}
