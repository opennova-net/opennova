// The authority's two ban lists (runtime/inmatch/server_ban_lists.h): their round-init loads
// and saves in a directory (a temp one standing for the working directory), the console's BAN /
// UNBAN / PUNT / BANDWIDTH line, and the game-layer join's refusals, PCID code 29 on a
// NovaWorld session and name code 31 in any session.
// [orig: BanList_InitFromMission @0x5098F0; Server_InitNewRoundState @0x51CB25..0x51CB3B;
//  BanList_SaveToFile @0x4FDD70; Server_HandleBanPuntCommand @0x50B380;
//  Server_ValidatePlayerJoinRequest @0x512737 (29), @0x512A6C (31)]

#include <formats/banlist/address_ban_list.h>
#include <formats/banlist/pcid_ban_list.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/inmatch/server_ban_lists.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::string read_text(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct TempDir {
	fs::path path;
	TempDir() {
		path = fs::temp_directory_path() / ("opennova_ban_lists_" + std::to_string(std::rand()) + "_" +
		                                    std::to_string(reinterpret_cast<uintptr_t>(this)));
		fs::create_directories(path);
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

inmatch::NapiNPConnection slot(uint8_t number, const std::string &name, const std::string &pcid) {
	inmatch::NapiNPConnection c;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.phase = inmatch::ConnectionPhase::InMatch;
	c.link.mode = replication::TransportMode::Client;
	c.reply.player_slot = number;
	c.reply.player_name = name;
	c.account.pcid = pcid;
	return c;
}

void test_files_and_console() {
	TempDir dir;
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.config.max_players = 8;
	ctx.bans.directory = dir.path.string();
	ctx.np_protocol.connection_list.push_back(slot(1, "Alpha", "A-PC-ID0001"));
	ctx.np_protocol.connection_list.push_back(slot(2, "Bravo", ""));
	replication::UdpSessionTransport bravo_link(replication::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list[1].link.transport = &bravo_link;

	// LAN: no PCID list; BAN is handled and does nothing.
	ctx.transport_mode = inmatch::NetworkType::Lan;
	inmatch::Server_ReloadPcidBanList(ctx);
	expect(!ctx.bans.pcids.has_value() && !fs::exists(dir.path / "banlist.txt"), "a LAN authority keeps no PCID list");
	expect(inmatch::Server_HandleBanPuntCommand(ctx, "BAN 1 ") && !ctx.bans.pcids.has_value(),
	       "BAN without the list is still handled");

	// NovaWorld: a missing file yields a fresh list, saved over the file.
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	inmatch::Server_ReloadPcidBanList(ctx);
	expect(ctx.bans.pcids.has_value() && ctx.bans.pcids->entries.empty(), "a fresh empty list");
	const std::string fresh = read_text(dir.path / "banlist.txt");
	expect(fresh == banlist::write_pcid_list(banlist::PcidBanList{}), "saved over the missing file");
	expect(inmatch::Server_HandleBanPuntCommand(ctx, "BAN 1 "), "the console BAN");
	expect(ctx.bans.pcids->entries.size() == 1 && ctx.bans.pcids->entries[0].pcid == "A-PC-ID0001" &&
	               ctx.bans.pcids->entries[0].name == "Alpha",
	       "the slot's PCID and name appended");
	expect(read_text(dir.path / "banlist.txt").find("BAN \"A-PC-ID0001\" \"Alpha\"\r\n") != std::string::npos,
	       "and the file rewritten at once");
	expect(!ctx.np_protocol.connection_list[0].host_disconnect_sent, "the console BAN does not punt");
	inmatch::Server_HandleBanPuntCommand(ctx, "BAN 2");
	expect(ctx.bans.pcids->entries.size() == 1, "a slot without a PCID adds nothing");
	inmatch::Server_ReloadPcidBanList(ctx);
	expect(ctx.bans.pcids->entries.size() == 1 && inmatch::Server_PcidBanned(ctx, "a-pc-id0001"),
	       "the next round init reloads it (the find is case-insensitive)");
	expect(inmatch::Server_HandleBanPuntCommand(ctx, "UNBAN 1") && ctx.bans.pcids->entries.empty(),
	       "UNBAN takes a connected slot's PCID off the list");

	// PUNT and BANDWIDTH.
	expect(inmatch::Server_HandleBanPuntCommand(ctx, "PUNT 2"), "PUNT is handled");
	replication::Datagram staged;
	DisconnectEvent punt;
	expect(bravo_link.pop_outbound(staged) && parse_disconnect_event(staged.body.data(), staged.body.size(), punt) &&
	               punt.dpc == 33 && punt.ddstr == "S.C:SP#",
	       "the punt: 33, S.C:SP#");
	expect(!inmatch::Server_HandleBanPuntCommand(ctx, "PUNT 2 3"), "PUNT with three tokens is not this arm's");
	expect(!inmatch::Server_HandleBanPuntCommand(ctx, "BANDWIDTH 100") && ctx.config.entity_send_budget == 100,
	       "BANDWIDTH clamps n / 5 to 100 and leaves the line to the next arm");
	inmatch::Server_HandleBanPuntCommand(ctx, "bandwidth 99999");
	expect(ctx.config.entity_send_budget == 1600, "the 1600 ceiling");
	inmatch::Server_HandleBanPuntCommand(ctx, "BANDWIDTH");
	expect(ctx.config.entity_send_budget == 600, "no value: 600");

	// banned.txt: the reload, and the shutdown save gated on the dirty word.
	{
		std::ofstream out(dir.path / "banned.txt", std::ios::binary);
		out << "10.0.0.9 \"Cheater\"\r\n";
	}
	inmatch::Server_ReloadAddressBanList(ctx);
	expect(ctx.bans.addresses.entries.size() == 1 && inmatch::Server_NameBanned(ctx, "Cheater") &&
	               !inmatch::Server_NameBanned(ctx, "cheater"),
	       "banned.txt read at the round init; the name compare is exact");
	expect(inmatch::Server_AddressBanned(ctx, 10u | (9u << 24)), "the packed address");
	ctx.bans.addresses.entries.push_back({1u, "?"});
	inmatch::Server_SaveAddressBanList(ctx);
	expect(read_text(dir.path / "banned.txt") == "10.0.0.9 \"Cheater\"\r\n", "not dirty: the shutdown writes nothing");
	ctx.bans.addresses_dirty = true;
	inmatch::Server_SaveAddressBanList(ctx);
	expect(read_text(dir.path / "banned.txt") == banlist::write_address_list(ctx.bans.addresses),
	       "dirty: rewritten whole");
}

// The game-layer join (C2S 0x00) through the dispatcher, as the handshake tests drive it.
uint32_t join_punt_code(inmatch::NapiNPServerCtx &ctx, inmatch::NapiNPConnection conn) {
	replication::UdpSessionTransport transport(replication::UdpSessionTransport::Role::Host);
	conn.link.transport = &transport;
	conn.phase = inmatch::ConnectionPhase::Joined;
	conn.admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
	conn.join_environment = {0, 2, 1, 0, 20042002, 180};
	std::vector<uint8_t> payload;
	const char name[] = "VERSIONCRCSTRING";
	payload.insert(payload.end(), name, name + sizeof(name));
	payload.insert(payload.end(), {2, 0, '0', 0});
	std::vector<inmatch::NapiNPConnection> roster{conn};
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	ctx.config.expansion.clear();
	(void)inmatch::dispatch_session_replies(ctx.config, roster[0], {make_protocol_message(0x00, payload)}, 5, roster,
			nullptr, inputs);
	replication::Datagram staged;
	if (!transport.pop_outbound(staged)) return 0;
	DisconnectEvent event;
	if (!parse_disconnect_event(staged.body.data(), staged.body.size(), event)) return 0;
	return event.dpc;
}

void test_join_refusals() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	ctx.bans.pcids = banlist::PcidBanList{};
	banlist::add(*ctx.bans.pcids, "A-BA-NNED01", "");
	ctx.bans.addresses.entries.push_back({0u, "Cheater"});
	inmatch::NapiNPConnection good = slot(3, "Honest", "A-OK-PCID01");
	good.player_name = "Honest";
	expect(join_punt_code(ctx, good) == 0, "an unlisted joiner passes");
	inmatch::NapiNPConnection pcid = good;
	pcid.account.pcid = "A-BA-NNED01";
	expect(join_punt_code(ctx, pcid) == 29, "a listed PCID on a NovaWorld session is refused 29");
	inmatch::NapiNPConnection named = good;
	named.player_name = "Cheater";
	expect(join_punt_code(ctx, named) == 31, "a listed name is refused 31");
	ctx.transport_mode = inmatch::NetworkType::Lan;
	expect(join_punt_code(ctx, pcid) == 0, "off NovaWorld there is no local address and no PCID leg");
	expect(join_punt_code(ctx, named) == 31, "the name leg holds in any session");
}

} // namespace

int main() {
	test_files_and_console();
	test_join_refusals();
	if (g_failures == 0) {
		std::printf("npruntime_ban_lists: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
