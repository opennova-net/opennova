// The /PROFILE server log (.sph): the record writers against the decoder, and
// the recorder over a synthetic session — open, the mission-start roster, a
// player add, two eighth-tick frames, a deploy marker, a disconnect marker
// and the close — written to memory and decoded back field for field; plus
// the file name rule and its numbered rotation.
// [orig: ServerLog_OpenForWrite @0x4e1ec0; the CServerLog_* writers
//  @0x4e1a10..0x4e1e00; Game_ProcessMainFrame @0x526879..0x5268e7;
//  docs/net/novaworld-net-re.md §5.22]
#include "npruntime/conn_fixture.h"

#include <formats/sph/sph.h>
#include <net/npwire/serverlog_decode.h>
#include <net/npwire/serverlog_encode.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	std::vector<std::string> removed;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override {
		removed.push_back(name);
		files.erase(name);
	}
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

std::vector<uint8_t> bytes_of(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

uint32_t u32_at(const std::vector<uint8_t> &b, size_t at) {
	return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
			uint32_t(b[at + 3]) << 24;
}

// The writers, chunk by chunk, against the decoder and the witnessed layouts.
void test_records_round_trip() {
	std::vector<uint8_t> out;
	append_server_log_begin(out, "a_very_long_mission_name.bms");
	CHECK(out.size() == 28);
	CHECK(std::string(out.begin(), out.begin() + 4) == "NGEB");
	CHECK(out[4] == 28 && out[5] == 0 && out[6] == 0 && out[7] == 0);
	CHECK(u32_at(out, 8) == 2);
	// strncpy(.., 15): fifteen name bytes, the sixteenth never stored (zero).
	CHECK(std::string(reinterpret_cast<const char *>(out.data() + 12), 15) == "a_very_long_mis");
	CHECK(out[27] == 0);

	append_server_log_player(out, 3, 1, "Alice");
	append_server_log_player(out, 77, -1, ""); // a negative team byte sign-extends
	append_server_log_frame(out, 41);
	ServerLogEntity e;
	e.net_id = 3;
	e.pos_x = 0x00460000;
	e.pos_y = 0x00190000;
	e.pos_z = 0x00384e68;
	e.yaw_bam = 0xC0000000u;
	e.angle2 = 0x12345678u;
	e.flags = 0x100;
	e.vehicle_flag = 1;
	e.stat_byte = 60;
	const size_t pdat_at = out.size();
	append_server_log_entity(out, e);
	CHECK(out.size() - pdat_at == 44);
	CHECK(std::string(out.begin() + pdat_at, out.begin() + pdat_at + 4) == "TADP");
	CHECK(u32_at(out, pdat_at + 12) == static_cast<uint32_t>(-0x00190000)); // -Y
	CHECK(u32_at(out, pdat_at + 16) == 0x00384e68u);                        // Z
	CHECK(u32_at(out, pdat_at + 20) == 0x00460000u);                        // X
	CHECK(u32_at(out, pdat_at + 32) == 0);                                  // never stored
	append_server_log_event(out, ServerLogEventKind::Death, 3);
	append_server_log_event(out, ServerLogEventKind::Disconnect, 77);
	append_server_log_end(out);

	ServerLogDocument doc;
	CHECK(decode_server_log(out.data(), out.size(), doc));
	CHECK(doc.version == 2 && doc.mission == "a_very_long_mis");
	CHECK(doc.roster.size() == 2);
	CHECK(doc.roster[0].net_id == 3 && doc.roster[0].team == 1 && doc.roster[0].name == "Alice");
	CHECK(doc.roster[1].net_id == 77 && doc.roster[1].team == 0xFF && doc.roster[1].name.empty());
	CHECK(doc.frames.size() == 1 && doc.frames[0].frame_index == 41);
	CHECK(doc.frames[0].entities.size() == 1);
	const ServerLogEntity &d = doc.frames[0].entities[0];
	CHECK(d.net_id == 3 && d.pos_x == e.pos_x && d.pos_y == e.pos_y && d.pos_z == e.pos_z &&
			d.yaw_bam == e.yaw_bam && d.angle2 == e.angle2 && d.flags == e.flags &&
			d.vehicle_flag == 1 && d.stat_byte == 60);
	CHECK(doc.events.size() == 2);
	CHECK(doc.events[0].kind == ServerLogEventKind::Death && doc.events[0].net_id == 3 &&
			doc.events[0].at_frame == 41);
	CHECK(doc.events[1].kind == ServerLogEventKind::Disconnect && doc.events[1].net_id == 77);
	CHECK(doc.ended_clean && doc.leftover_clean && doc.leftover_bytes == 0);

	// The container writer refuses a chunk past the u16 length.
	std::vector<uint8_t> big(0xFFF8, 0);
	std::vector<uint8_t> sink;
	CHECK(!sph::append_chunk(sink, "XXXX", big.data(), big.size()) && sink.empty());
	CHECK(sph::append_chunk(sink, "XXXX", big.data(), big.size() - 1) && sink.size() == 0xFFFF);
}

world::EntityHandle add_player(world::World &world, uint32_t dcb, uint8_t team, const char *name,
		float x, float y, float z, int16_t yaw) {
	world::Entity e;
	e.kind = world::EntityKind::Organic;
	e.team = team;
	e.flags = world::kEntityFlagPlayer;
	e.engine_flags = world::kEntityFlagPlayer;
	e.owner_connection_id = dcb;
	e.display_name = name;
	e.position = {x, y, z};
	e.yaw = yaw;
	e.health = 100;
	e.alive = true;
	return world.registry.spawn(0, e);
}

void test_recorder_session() {
	world::World world;
	world.registry.configure_pool(0, 8);
	const world::EntityHandle alice = add_player(world, 3, 1, "Alice", 70.0f, 25.0f, 56.25f, 0);
	const world::EntityHandle bob = add_player(world, 5, 2, "Bob", -10.0f, 4.0f, 12.0f, 90);
	// A pool-0 row without the Player bit: an in-session recorder skips it.
	world::Entity ai;
	ai.kind = world::EntityKind::Organic;
	ai.net_id = 77;
	ai.team = 2;
	const world::EntityHandle bot = world.registry.spawn(0, ai);

	replication::LoopbackChannel wire_a;
	replication::LoopbackChannel wire_b;
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	inmatch::NapiNPConnection a = conn_fixture::make_conn(3, 1, &wire_a,
			replication::TransportMode::Client, alice, true);
	a.phase = inmatch::ConnectionPhase::InMatch;
	a.link.uplink_frame_rate = 60;
	ctx.np_protocol.connection_list.push_back(a);
	inmatch::NapiNPConnection b = conn_fixture::make_conn(5, 1, &wire_b,
			replication::TransportMode::Client, bob, true);
	b.phase = inmatch::ConnectionPhase::InMatch;
	b.link.uplink_frame_rate = 31;
	ctx.np_protocol.connection_list.push_back(b);

	MemoryFiles files;
	inmatch::ServerLogRecorder rec(files, "host");
	// Closed: nothing is written.
	rec.write_frame(ctx, world, 7);
	CHECK(files.files.empty());

	CHECK(rec.open("mission.bms"));
	CHECK(rec.file_name() == "host.sph");
	CHECK(files.files.count("host.sph") == 1 && files.files["host.sph"].empty());
	rec.write_mission_roster(ctx, world);       // Alice and Bob; the bot row has no Player bit
	rec.write_player(*world.registry.get(alice)); // already in the table: nothing
	rec.write_frame(ctx, world, 6);             // not an eighth tick
	rec.write_frame(ctx, world, 7);             // frame 0
	rec.write_death(*world.registry.get(alice));
	rec.write_death(*world.registry.get(bot)); // not in the table: nothing
	rec.write_frame(ctx, world, 15);            // frame 1
	rec.write_disconnect(*world.registry.get(bob));
	// Still buffered: only the open's create reached the file.
	CHECK(files.files["host.sph"].empty());
	rec.close();
	CHECK(!rec.is_open());

	const std::vector<uint8_t> file = bytes_of(files.files["host.sph"]);
	ServerLogDocument doc;
	CHECK(decode_server_log(file.data(), file.size(), doc));
	CHECK(doc.version == 2 && doc.mission == "mission.bms");
	CHECK(doc.roster.size() == 2);
	CHECK(doc.roster[0].net_id == 3 && doc.roster[0].team == 1 && doc.roster[0].name == "Alice");
	CHECK(doc.roster[1].net_id == 5 && doc.roster[1].team == 2 && doc.roster[1].name == "Bob");
	CHECK(doc.frames.size() == 2);
	CHECK(doc.frames[0].frame_index == 0 && doc.frames[1].frame_index == 1);
	for (const ServerLogFrame &f : doc.frames) {
		CHECK(f.entities.size() == 2);
		if (f.entities.size() != 2) continue;
		const ServerLogEntity &ea = f.entities[0];
		const auto sa = replication::snapshot_of(*world.registry.get(alice));
		CHECK(ea.net_id == 3 && ea.pos_x == world::to_fixed(70.0f) &&
				ea.pos_y == world::to_fixed(25.0f) && ea.pos_z == world::to_fixed(56.25f) &&
				ea.yaw_bam == static_cast<uint32_t>(sa.euler_z) && ea.flags == 0x100 &&
				ea.vehicle_flag == 0 && ea.stat_byte == 60);
		const ServerLogEntity &eb = f.entities[1];
		CHECK(eb.net_id == 5 && eb.pos_x == world::to_fixed(-10.0f) && eb.stat_byte == 31);
	}
	CHECK(doc.events.size() == 2);
	CHECK(doc.events[0].kind == ServerLogEventKind::Death && doc.events[0].net_id == 3 &&
			doc.events[0].at_frame == 0);
	CHECK(doc.events[1].kind == ServerLogEventKind::Disconnect && doc.events[1].net_id == 5 &&
			doc.events[1].at_frame == 1);
	CHECK(doc.ended_clean && doc.leftover_clean);

	// The file is exactly the writers' sequence (the recorder adds nothing of
	// its own).
	std::vector<uint8_t> expected;
	append_server_log_begin(expected, "mission.bms");
	append_server_log_player(expected, 3, 1, "Alice");
	append_server_log_player(expected, 5, 2, "Bob");
	CHECK(file.size() > expected.size() &&
			std::equal(expected.begin(), expected.end(), file.begin()));

	// A session-less authority records every pool-0 row, a non-player as
	// "#<DcbId>".
	ctx.is_in_session = 0;
	CHECK(rec.open("sp.bms"));
	CHECK(rec.file_name() == "host1.sph"); // host.sph exists
	rec.write_mission_roster(ctx, world);
	rec.close();
	const std::vector<uint8_t> sp = bytes_of(files.files["host1.sph"]);
	ServerLogDocument sp_doc;
	CHECK(decode_server_log(sp.data(), sp.size(), sp_doc));
	CHECK(sp_doc.roster.size() == 3 && sp_doc.roster[2].net_id == 77 &&
			sp_doc.roster[2].name == "#77");

	// An open that only buffered BEGN still writes .END at the close.
	CHECK(rec.open("e.bms"));
	rec.close();
	const std::vector<uint8_t> empty = bytes_of(files.files["host2.sph"]);
	CHECK(empty.size() == 28 + 8);
}

void test_file_names() {
	MemoryFiles files;
	// The extension replaces everything after the FIRST '.'.
	CHECK(inmatch::server_log_file_name(files, "host") == "host.sph");
	CHECK(inmatch::server_log_file_name(files, "host.log") == "host.sph");
	CHECK(inmatch::server_log_file_name(files, "logs.d/host") == "logs.sph");
	// Taken names count up from the digits before the dot.
	files.files["host.sph"] = "";
	CHECK(inmatch::server_log_file_name(files, "host") == "host1.sph");
	files.files["run5.sph"] = "";
	CHECK(inmatch::server_log_file_name(files, "run5") == "run6.sph");
	// Past 10 the search wraps to 1 and deletes what it lands on.
	for (int i = 1; i <= 10; ++i) files.files["host" + std::to_string(i) + ".sph"] = "x";
	files.removed.clear();
	CHECK(inmatch::server_log_file_name(files, "host") == "host1.sph");
	CHECK(files.removed.size() == 1 && files.removed[0] == "host1.sph");
	CHECK(files.files.count("host1.sph") == 0);
}

} // namespace

int main() {
	test_records_round_trip();
	test_recorder_session();
	test_file_names();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("server log recorder: ok\n");
	return 0;
}
