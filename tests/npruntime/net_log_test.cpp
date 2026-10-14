// The console's headless arms (inmatch/server_console.h): the protocol manager's net log
// (_NETLOG.TXT: its start and stop lines, the stamp, the 5000 ms flush, the first open's
// truncate and the appends after it, the shutdown), the NETLOG / STAT handler, the authority's
// C2S 0x04 (the chat throttle's slot word, the 1023-character cut, the S2C 0x06 echo, the host's
// own NETLOG and its S2C 0x14 reply) and SAVENFO's debug.nfo text.
// [orig: CNapiNetwork_SetNetLogFile @0x4C69A0; NapiLog_SetEnabled @0x62F2B0;
//  NapiLog_WriteEntry @0x62EE30; NapiLog_FlushIfExpired @0x62F2F0; NapiLog_Flush @0x62F1A0;
//  NapiLog_EnsureOpen @0x62EDC0; NapiLog_Shutdown @0x62F310; Server_HandleAdminNetlogCommand
//  @0x5175D0; NapiNPServerMsg_HandleConsoleLine @0x5199D0; Entity_DumpUsageStatistics @0x40E620]
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode

#include <formats/def/def.h>
#include <net/npwire/ingame_decode_session.h> // decode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace w = opennova::world;

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
	int opens = 0;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		++opens;
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

constexpr const char *kStamp = "2026-10-13 21:07:05: ";

void test_net_log() {
	MemoryFiles files;
	files.files["_NETLOG.TXT"] = "a previous run\r\n";
	inmatch::NetLog log(files);
	log.set_stamp_text([] { return std::string(kStamp); });
	CHECK(!log.enabled());
	log.set_enabled(false); // the same state writes nothing
	CHECK(log.pending().empty());
	log.set_enabled(true);
	CHECK(log.enabled() && log.pending() == std::string(kStamp) + "NapiLog Started!\r\n");
	log.flush_if_expired(4999); // the interval since the last flush (0) has not passed
	CHECK(files.opens == 0);
	log.flush_if_expired(5000);
	CHECK(files.files["_NETLOG.TXT"] == std::string(kStamp) + "NapiLog Started!\r\n" &&
	      log.pending().empty()); // the first open truncates the earlier run's file
	log.flush_if_expired(9999); // nothing pending: a flush stamps the time and writes nothing
	log.flush_if_expired(10000);
	CHECK(files.opens == 1);
	log.set_enabled(false);
	log.flush(10001);
	CHECK(files.files["_NETLOG.TXT"] ==
	      std::string(kStamp) + "NapiLog Started!\r\n" + kStamp + "NapiLog Stopped!\r\n"); // appended
	log.set_enabled(true);
	log.shutdown(10002); // the stop line, then the flush
	CHECK(!log.enabled() && files.files["_NETLOG.TXT"] ==
	      std::string(kStamp) + "NapiLog Started!\r\n" + kStamp + "NapiLog Stopped!\r\n" + kStamp +
	              "NapiLog Started!\r\n" + kStamp + "NapiLog Stopped!\r\n");
	CHECK(inmatch::napi_log_stamp(0).size() == std::strlen(kStamp));
}

struct Host {
	inmatch::NapiNPServerCtx ctx;
	MemoryFiles files;
	inmatch::NetLog log{files};
	Host() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		log.set_stamp_text([] { return std::string(kStamp); });
		ctx.logs.net = &log;
		ctx.logs.files = &files;
		ctx.np_protocol.connection_list.reserve(8); // the references add() returns stay valid
	}
	inmatch::NapiNPConnection &add(uint8_t slot, bool local) {
		inmatch::NapiNPConnection c;
		c.type = local ? inmatch::NapiNPConnection::kTypeClientSide : inmatch::NapiNPConnection::kTypeServerSide;
		c.phase = inmatch::ConnectionPhase::PlayerAdded;
		c.reply.player_slot = slot;
		ctx.np_protocol.connection_list.push_back(std::move(c));
		return ctx.np_protocol.connection_list.back();
	}
};

std::vector<uint8_t> cstr(const std::string &s) {
	std::vector<uint8_t> out(s.begin(), s.end());
	out.push_back(0);
	return out;
}

void test_netlog_handler() {
	Host host;
	std::vector<ProtocolMessage> replies;
	// The admin console's own line (no slot): NETLOG toggles and is not taken; STAT is taken.
	CHECK(!inmatch::Server_HandleAdminNetlogCommand(host.ctx, "netlog ", nullptr, replies));
	CHECK(host.log.enabled() && replies.empty());
	CHECK(inmatch::Server_HandleAdminNetlogCommand(host.ctx, "STATUS ", nullptr, replies));
	CHECK(!inmatch::Server_HandleAdminNetlogCommand(host.ctx, "   ", nullptr, replies));
	CHECK(!inmatch::Server_HandleAdminNetlogCommand(host.ctx, "NETLOGX ", nullptr, replies) && host.log.enabled());
	// No device: nothing toggles.
	host.ctx.logs.net = nullptr;
	CHECK(!inmatch::Server_HandleAdminNetlogCommand(host.ctx, "NETLOG", nullptr, replies));
	host.ctx.logs.net = &host.log;
	CHECK(host.log.enabled());
}

void test_console_line_message() {
	Host host;
	inmatch::NapiNPConnection &remote = host.add(2, false);
	// A remote player's NETLOG: not the host's, so nothing toggles, and the line comes back.
	std::vector<ProtocolMessage> r = inmatch::Server_HandleConsoleCommandMessage(host.ctx, remote, cstr("NETLOG"), 100);
	CHECK(!host.log.enabled() && r.size() == 1 && r[0].tag == s2c::CLIENT_CONSOLE_COMMAND && !r[0].reliable &&
	      r[0].payload == cstr("NETLOG"));
	// The slot's throttle word, shared with the chat throttle: one a second.
	CHECK(inmatch::Server_HandleConsoleCommandMessage(host.ctx, remote, cstr("x"), 1099).empty());
	CHECK(inmatch::Server_HandleConsoleCommandMessage(host.ctx, remote, cstr("x"), 1100).size() == 1);
	CHECK(remote.reply.chat_last_ms == 1100);
	// STAT is taken whole: no echo.
	CHECK(inmatch::Server_HandleConsoleCommandMessage(host.ctx, remote, cstr("stat me"), 2100).empty());
	// The copy stops at 1023 characters.
	const std::string longline(1500, 'a');
	r = inmatch::Server_HandleConsoleCommandMessage(host.ctx, remote, cstr(longline), 3100);
	CHECK(r.size() == 1 && r[0].payload.size() == 1024);
	// A sender without a player slot draws nothing.
	inmatch::NapiNPConnection &pending = host.add(3, false);
	pending.phase = inmatch::ConnectionPhase::New;
	CHECK(inmatch::Server_HandleConsoleCommandMessage(host.ctx, pending, cstr("hi"), 5000).empty());
	// The host's own client (a listen host's local slot): NETLOG toggles, answers it S2C 0x14
	// [14][slot]["NETLOG: SERVER SET TO 1"] (reliable, 310 flushes), and echoes the line too.
	inmatch::NapiNPConnection &local = host.add(0, true);
	r = inmatch::Server_HandleConsoleCommandMessage(host.ctx, local, cstr("netlog"), 6000);
	CHECK(host.log.enabled() && r.size() == 2);
	if (r.size() == 2) {
		ChatBroadcast chat;
		CHECK(r[0].tag == s2c::CHAT_BROADCAST && r[0].reliable && r[0].retention_flushes == 310 &&
		      decode_chat_broadcast(r[0].payload.data(), r[0].payload.size(), chat) && chat.channel == 14 &&
		      chat.sender_slot == 0 && chat.text == "NETLOG: SERVER SET TO 1");
		CHECK(r[1].tag == s2c::CLIENT_CONSOLE_COMMAND && r[1].payload == cstr("netlog"));
	}
	// A joiner on a host that is not the authority: nothing.
	host.ctx.is_authority = 0;
	CHECK(inmatch::Server_HandleConsoleCommandMessage(host.ctx, local, cstr("netlog"), 9000).empty());
}

void test_usage_text() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->registry.configure_pool(1, 8);
	world->registry.configure_pool(3, 8);
	w::Entity body;
	body.item_type_index = 1;
	(void)world->registry.spawn(0, body);
	(void)world->registry.spawn(0, body);
	w::Entity freed = body;
	freed.item_type_index = 2;
	const w::EntityHandle gone = world->registry.spawn(1, freed);
	world->registry.despawn(gone); // a freed row below the used count counts as type 0
	w::Entity marker;
	marker.item_type_index = 3;
	(void)world->registry.spawn(3, marker);
	std::vector<def::DefItemDef> rows(4);
	for (size_t i = 0; i < rows.size(); ++i) rows[i].id = 6000 + static_cast<int>(i);
	std::strcpy(rows[1].display_name, "Player");
	std::strcpy(rows[3].display_name, "Marker");
	def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	CHECK(inmatch::entity_usage_text(*world, &items, "debug.nfo") ==
	      "Object usage for debug.nfo"
	      "\nNumber of OITEMS (people and players)        :\t2\n"
	      "\nNumber of MITEMS (vehicles and moving things):\t1\n"
	      "\nNumber of DITEMS (static things)             :\t0\n"
	      "\nNumber of MARKERS (You Know Markers)         :\t1\n\n"
	      "type id-0006001  count-0002  Player \n"
	      "type id-0006003  count-0001  Marker \n"
	      "\n\n");
	CHECK(inmatch::entity_usage_file_name("debug.nfo") == "debug.nfo");
	CHECK(inmatch::entity_usage_file_name("mission.bms") == "mission.nfo");
	CHECK(inmatch::entity_usage_file_name("noext").empty());
}

} // namespace

int main() {
	test_net_log();
	test_netlog_handler();
	test_console_line_message();
	test_usage_text();
	if (failures == 0) {
		std::printf("npruntime_net_log: OK\n");
		return 0;
	}
	std::printf("%d failure(s)\n", failures);
	return 1;
}
