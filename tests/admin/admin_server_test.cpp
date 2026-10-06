// The remote-admin server (net/admin/admin_server.h) and its console
// (runtime/inmatch/admin_console.h), driven by the CLIENT half (net/admin/admin_protocol.h, the
// codec opennova-nw-lister's admin feed speaks) over in-memory bytes: the challenge, the login
// and its prefix rules, the QUERY report, every verb's replies in order and its side effects on
// a host with two remote players, the rights gates and the usage mismatch, and the framing
// edges. The witness record is docs/net/novaworld-net-re.md §6.9.
// [orig: CAdminServer_AcceptConnection @0x405580; CAdminServer_HandleLogin @0x405870;
//  CAdminServer_ProcessClientData @0x406EC0; CAdminServer_DispatchCommand @0x406720 and its
//  handlers; CAdminServer_HandleStatus @0x402E30]

#include "npruntime/conn_fixture.h"

#include <base/io/crt_rand.h>
#include <base/io/le.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h> // hightag::DESCRIPTION_PACKET
#include <net/npwire/session_hello.h>    // parse_disconnect_event
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_console.h> // Server_SendConsoleChat
#include <runtime/hud/hud_chat_entry.h>     // kChatDispatchGlobal
#include <runtime/hud/feed_format.h>    // kHudColorWhite / kHudColorLightBlue
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

void expect_eq(const std::string &got, const std::string &want, const char *what) {
	if (got != want) {
		std::fprintf(stderr, "FAIL: %s\n  got:  [%s]\n  want: [%s]\n", what, got.c_str(), want.c_str());
		++g_failures;
	}
}

// The rotation the console reaches through RotationAdmin, as retail's list ops behave on their
// witnessed paths (the PR3 rotation implements the real one).
struct FakeRotation final : inmatch::RotationAdmin {
	List state;
	std::vector<CatalogRow> rows;
	bool latch = false;
	std::vector<bool> marks;

	List list() const override { return state; }
	std::vector<CatalogRow> catalog() const override { return rows; }
	void set_launch_option(size_t catalog_index, int32_t option) override {
		rows[catalog_index].launch_option = option;
	}
	void add(size_t catalog_index, std::optional<int32_t> at, bool one_shot) override {
		state.exists = true;
		const Entry e{catalog_index, one_shot};
		if (at.has_value() && *at >= 0 && static_cast<size_t>(*at) <= state.entries.size())
			state.entries.insert(state.entries.begin() + *at, e);
		else
			state.entries.push_back(e);
	}
	bool remove(int32_t index) override {
		if (index < 0 || static_cast<size_t>(index) >= state.entries.size()) return false;
		state.entries.erase(state.entries.begin() + index);
		return true;
	}
	void clear(bool in_game) override {
		if (in_game) {
			state = List{};
			return;
		}
		marks.assign(rows.size(), false);
	}
	bool set_next(int32_t index) override {
		latch = true;
		if (index < 0 || static_cast<size_t>(index) >= state.entries.size()) return false;
		state.alt_cursor = index;
		return true;
	}
};

struct Host {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;
	gamecfg::GameCfg block;
	FakeRotation rotation;
	int saves = 0;
	int quits = 0;
	std::vector<std::string> window;
	std::vector<std::string> echoes;
	std::unique_ptr<inmatch::AdminConsole> console;

	Host() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		w::MatchRules rules;
		rules.game_type = 0x10000u;
		world.match.configure(rules);
		ctx.config.game_type = 0x10000u; // TDM
		ctx.config.max_players = 8;
		ctx.config.server_name = "Admin Host";
		ctx.config.mission_file = "TDM01.BMS";
		ctx.np_protocol.host_run_duration_ms = ((1u * 24u + 2u) * 3600u + 3u * 60u + 4u) * 1000u;
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.rules.mpattrib = ctx.config.mp_attributes;
		world.registry.configure_pool(0, 32);
		world.weather.tod_fixed24 = 12u << 24;
		ctx.world = &world;
		const uint8_t teams[2] = {1, 2};
		transports.reserve(2);
		for (size_t i = 0; i < 2; ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i + 1), 0.0f, 0.0f};
			spawn.team = teams[i];
			players.push_back(w::spawn_remote_player(world, spawn));
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.reply.player_name = "P" + std::to_string(i + 1);
			c.reply.rtt_ms = static_cast<uint32_t>(100 + i);
			c.assigned_team = teams[i];
			c.assigned_team_valid = true;
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
		// Two weapon.def rows, one selectable.
		world.tables.weapons.entries.resize(3);
		world.tables.weapons.entries[1].valid = true;
		world.tables.weapons.entries[1].name = "WPN_M4";
		world.tables.weapons.entries[1].loadout_selectable = 1;
		world.tables.weapons.entries[2].valid = true;
		world.tables.weapons.entries[2].name = "WPN_HIDDEN";
		block.game_name = "Admin Host";
		block.armory_reuse_time = 30;
		rotation.rows = {{"TDM01.BMS", "Desert Run", 0}, {"CTF02.BMS", "Two Flags", 0}, {"KOTH03.BMS", "Hill", 0}};

		inmatch::AdminConsole::Seams seams;
		seams.config_block = &block;
		seams.save_config = [this] { ++saves; };
		seams.quit_to_menu = [this] { ++quits; };
		seams.rotation = &rotation;
		// A listen host's HUD window and flood echo: this Serve Only host never reads them (its
		// CHAT SEND and CHAT GET run over the context's CHAT ring).
		seams.chat_window = [this] { return window; };
		seams.chat_echo = [this](const std::string &text) { echoes.push_back(text); };
		seams.game_text = [](std::string_view section, std::string_view key) -> std::string {
			if (section == "Overlays" && key == "STROVER64") return "Team Deathmatch";
			return {};
		};
		console = std::make_unique<inmatch::AdminConsole>(ctx, std::move(seams));
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
	std::vector<ns::Datagram> drain(size_t i) {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) out.push_back(d);
		return out;
	}
	// The connection-description punt staged on slot i's transport (DPC 0 when none).
	DisconnectEvent punt(size_t i) {
		DisconnectEvent event;
		for (const ns::Datagram &d : drain(i))
			if (d.tag == hightag::DESCRIPTION_PACKET && parse_disconnect_event(d.body.data(), d.body.size(), event))
				return event;
		return DisconnectEvent{};
	}
};

uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
	return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}

// The client half over the server's bytes.
struct Client {
	AdminServer &server;
	uint32_t id = 0;
	std::vector<uint8_t> challenge;
	bool open = false;

	// The packets in `bytes`, decoded the way a client reads them.
	static std::vector<std::vector<uint8_t>> payloads(const std::vector<std::vector<uint8_t>> &packets) {
		std::vector<std::vector<uint8_t>> out;
		for (const std::vector<uint8_t> &packet : packets) {
			size_t size = 0;
			if (packet.size() < ADMIN_PACKET_HEADER_BYTES || !admin_decode_header(packet.data(), size) ||
					packet.size() != ADMIN_PACKET_HEADER_BYTES + size) {
				out.push_back({});
				continue;
			}
			out.emplace_back(packet.begin() + ADMIN_PACKET_HEADER_BYTES, packet.end());
		}
		return out;
	}

	bool connect(uint32_t source) {
		const AdminServer::Accepted a = server.accept(source);
		if (!a.admitted) return false;
		id = a.connection;
		const auto p = payloads({a.send});
		challenge = p.front();
		open = true;
		return true;
	}

	std::vector<std::string> send_raw(const std::vector<uint8_t> &bytes) {
		const AdminServer::Received r = server.receive(id, bytes.data(), bytes.size());
		open = r.keep_open;
		std::vector<std::string> texts;
		for (const std::vector<uint8_t> &payload : payloads(r.send)) {
			std::string text;
			admin_reply_text(payload, text);
			texts.push_back(text);
		}
		return texts;
	}

	std::vector<std::string> send_payload(const std::vector<uint8_t> &payload) {
		return send_raw(admin_encode_packet(payload.data(), payload.size()));
	}

	std::vector<std::string> login(const std::string &user, const std::string &pass) {
		const auto login = admin_encode_login(challenge, user, pass);
		return send_payload(std::vector<uint8_t>(login.begin(), login.end()));
	}

	std::vector<std::string> command(const std::string &text) {
		return send_payload(admin_encode_command(text));
	}

	std::string one(const std::string &text) {
		const std::vector<std::string> r = command(text);
		return r.size() == 1 ? r.front() : std::string("<" + std::to_string(r.size()) + " replies>");
	}
};

admincfg::AdminConfig users_config() {
	const std::string text =
			"// the console users\r\n"
			"adm wrong 0D\r\n"
			"admin s3cret FFFFFFFF\r\n"
			"root s3cret FFFFFFFF\r\n"
			"boss pw 7F\r\n"
			"gotoer pw 40\r\n"
			"lister pw 380\r\n"
			"rabbit pw 80000000\r\n"
			"ip_restrict = 127.0.0.*\r\n"
			"ip_restrict = 192.168\r\n";
	return admincfg::parse(text.data(), text.size());
}

void test_challenge_and_whitelist() {
	Host host;
	io::CrtRand rand;
	rand.seed(1234);
	std::vector<std::string> log;
	AdminServer server(users_config(), *host.console, rand, [&](std::string_view line) { log.emplace_back(line); });
	Client blocked{server};
	expect(!blocked.connect(ip(10, 0, 0, 1)), "an address no pattern admits is refused before the challenge");
	expect(!log.empty() && log.back() == "New connection blocked - invalid IP (0100000a)\n",
	       "the blocked log prints the raw in_addr dword");
	Client c{server};
	expect(c.connect(ip(127, 0, 0, 1)), "127.0.0.* admits loopback");
	expect(log.back() == "New connection accepted (0100007f)\n", "the accepted log");
	expect(admin_challenge_valid(c.challenge), "the client half accepts the challenge");
	io::CrtRand replay;
	replay.seed(1234);
	bool draws = true;
	for (size_t i = 0; i < 32; ++i) {
		const uint8_t b = static_cast<uint8_t>(replay.next() % 255u + 1u);
		if (i > 0 && c.challenge[i] != b) draws = false;
	}
	expect(draws, "the challenge is 32 draws of rand() % 255 + 1, byte 0 forced to 1");
	Client lan{server};
	expect(lan.connect(ip(192, 168, 7, 9)), "192.168 (two tokens) admits all of 192.168/16");
	expect(server.connection_count() == 2, "two slots held");
}

void test_login_rules() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	{
		Client c{server};
		c.connect(ip(127, 0, 0, 1));
		const auto r = c.login("admin", "s3cret");
		expect(r.empty() && !c.open,
		       "the first user whose name prefixes the typed one is the only one tried ('adm' first)");
	}
	{
		Client c{server};
		c.connect(ip(127, 0, 0, 1));
		const auto r = c.login("BOSSMAN", "pw-and-more");
		expect(r.size() == 1 && admin_login_accepted(r.front()) && c.open,
		       "a case-insensitive name prefix and a case-sensitive password prefix log in");
		expect_eq(r.empty() ? "" : r.front(), "OK - User: BOSSMAN successfully logged in.",
		          "the reply names the typed user");
	}
	{
		Client c{server};
		c.connect(ip(127, 0, 0, 1));
		const auto r = c.login("boss", "PW");
		expect(r.empty() && !c.open, "the password is case-sensitive; a failure sends nothing and closes");
	}
	{
		Client c{server};
		c.connect(ip(127, 0, 0, 1));
		std::vector<uint8_t> big(ADMIN_LOGIN_BYTES + 1, 1);
		const auto r = c.send_payload(big);
		expect(r.empty() && !c.open, "a login payload over 65 bytes closes");
	}
	{
		Client c{server};
		c.connect(ip(127, 0, 0, 1));
		std::vector<uint8_t> raw = admin_encode_command("QUERY");
		raw[3] = 'r'; // "QUEry": stricmp
		const auto r = c.send_payload(raw);
		expect(r.size() == 1 && r.front().rfind("SERVER: Admin Host\r\n", 0) == 0 && !c.open,
		       "a plaintext QUERY (any case) answers the report and closes");
	}
	// A user line without a password admits any password; `ip_restrict=x` is a user.
	{
		const std::string text = "open\r\nip_restrict=127.*\r\n";
		const admincfg::AdminConfig cfg = admincfg::parse(text.data(), text.size());
		expect(cfg.users.size() == 2 && cfg.ip_restrictions.empty() && cfg.users[1].name == "ip_restrict=127.*",
		       "ip_restrict= with no separator is a user");
		AdminServer open_server(cfg, *host.console, rand);
		Client c{open_server};
		expect(c.connect(ip(8, 8, 8, 8)), "no ip_restrict line admits every address");
		const auto r = c.login("openSESAME", "anything");
		expect(r.size() == 1 && admin_login_accepted(r.front()), "an empty stored password matches any");
	}
}

Client logged_in(AdminServer &server, const char *user) {
	Client c{server};
	c.connect(ip(127, 0, 0, 1));
	c.login(user, "pw");
	return c;
}

void test_rights_and_usage() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	Client gotoer = logged_in(server, "gotoer");
	expect_eq(gotoer.one("GET GAMESTATE"), "USAGE - [QUIT | GOTO]", "rights 0x40 lists only GOTO");
	expect_eq(gotoer.one("CHAT FROB"), "USAGE -  CHAT [GET | SEND]", "but 0x40 opens CHAT");
	expect_eq(gotoer.one("ADMINUSER LIST"), "ERROR - This feature not yet implemented.", "and ADMINUSER");
	expect_eq(gotoer.one("banlist"), "USAGE -  BAN [LIST | DELETE | CREATE]", "and BANLIST, whose usage says BAN");
	Client lister = logged_in(server, "lister");
	expect_eq(lister.one("CHAT GET"), "USAGE - [QUIT | CHAT | ADMIN | BANLIST]",
	          "0x380 lists CHAT, ADMIN and BANLIST and opens none of them");
	Client rabbit = logged_in(server, "rabbit");
	expect_eq(rabbit.one("PETERRABBIT"), "", "PETERRABBIT with no argument answers an empty reply");
	expect_eq(rabbit.one("peterrabbit hop"), "OK - Hippity Hoppity", "any sub-verb hops");
	expect_eq(rabbit.one("   "), "USAGE - [QUIT | PETERRABBIT]", "an empty line is the usage");
	Client admin{server};
	admin.connect(ip(127, 0, 0, 1));
	admin.login("root", "s3cret");
	expect_eq(admin.one("NOPE"), "USAGE - [QUIT | GET | SET | MISSION | PLAYER | WEAPON | CMD | GOTO | CHAT | ADMIN | "
	                             "BANLIST | PETERRABBIT]",
	          "every right lists every verb");
	const auto quit = admin.command("quit");
	expect(quit.empty() && !admin.open, "QUIT closes with no reply");
	expect(inmatch::admin_split_command("a b c d e f g h i j k l m n o p q r s t u v w x y z").back() == "y z",
	       "the 25th token runs to the end of the line");
}

void test_get_and_set() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	Client c = logged_in(server, "boss");
	host.console->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Menus", "the main menu");
	host.console->set_scene(inmatch::AdminScene::Other);
	expect_eq(c.one("get gamestate"), "OK - Current State = Unknown", "a load");
	host.console->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(c.one("GET"), "USAGE -  GET [GAMESTATE] [GAMESETTINGS]", "GET's usage");

	host.ctx.config.respawn_time = 25;
	host.world.match.set_remaining_ticks(21 * 3720 + 100);
	host.block.game_name = "A Very Long Server Name Of 31 C";
	const std::string settings = c.one("GET GAMESETTINGS");
	size_t rows = 0;
	for (char ch : settings) rows += ch == '\n';
	expect(rows == 29, "29 rows");
	expect(settings.rfind("AutoBalanceOnRecycle = 0\nPuntVote             = 0\nVotePercent          = 0.660000\n", 0) == 0,
	       "the first rows, the key padded to 21 and %f six decimals");
	expect(settings.find("GameTime             = 21/25\n") != std::string::npos, "GameTime remaining/limit");
	expect(admin_parse_time_left_minutes(settings) == 21, "the client half reads the time left");
	expect(settings.find("ServerName           = A Very Long Server Name Of \n") != std::string::npos,
	       "a ServerName past 27 characters is cut (D-NET-359)");
	expect(settings.find("ArmoryTimer          = 30\n") != std::string::npos, "ArmoryTimer from the cfg block");

	expect_eq(c.one("SET KillLimit 500"), "OK - Setting Changed.", "a set");
	expect(host.saves == 1 && host.block.max_kills == 500 && host.ctx.config.score_limit == 500 &&
	               host.world.match.rules().score_limit == 500,
	       "KillLimit writes g_ScoreLimit live (no 500 fold), the cfg's max_kills, and saves");
	const auto unknown = c.command("SET LevelRestrict 1");
	expect(unknown.size() == 2 && unknown[1] == "ERROR - Setting Not Found." && unknown[0].rfind("USAGE -  SET [", 0) == 0 &&
	               host.saves == 1,
	       "an unknown key: the usage, then the error, unsaved");
	expect_eq(c.one("SET FriendlyFire 0"), "OK - Setting Changed.", "FriendlyFire");
	expect((host.world.rules.mpattrib & 0x200u) != 0 && host.world.rules.no_friendly_fire &&
	               (static_cast<uint32_t>(host.block.mpattrib) & 0x200u) != 0,
	       "FriendlyFire 0 sets the inverted bit live and in its shadow");
	expect(c.one("GET GAMESETTINGS").find("FriendlyFire         = 0\n") != std::string::npos, "and GET reads it back");
	c.one("SET VotePercent 0.5");
	c.one("SET FatBullets 7");
	const std::string after = c.one("GET GAMESETTINGS");
	expect(after.find("VotePercent          = 0.500000\n") != std::string::npos &&
	               after.find("FatBullets           = 7\n") != std::string::npos && host.world.rules.fat_bullets,
	       "atof and the raw atol word");
	c.one("SET GameTime 2");
	expect(host.world.match.remaining_ticks() == 2 * 3720 && host.ctx.config.respawn_time == 2,
	       "GameTime restarts the round clock at 3720 x value");
	expect_eq(c.one("SET ServerName a b c"), "OK - Setting Changed.", "ServerName");
	expect(host.block.game_name == "a b c " && host.ctx.np_protocol.session_name == "a b c ",
	       "ServerName: each token followed by one space, into the block and the protocol");
	host.block.mp_host_game_password = "old";
	host.ctx.config.server_password = "old";
	expect_eq(c.one("SET ServerPassword"), "OK - Setting Changed.", "a lone password key clears it");
	expect(host.block.mp_host_game_password.empty() && host.ctx.config.server_password.empty() &&
	               (host.ctx.np_protocol.build_flags & 0x8u) == 0,
	       "and republishes the flag words");
	expect_eq(c.one("SET MaxPing"), std::string("USAGE -  SET [AutoBalanceOnRecycle | LevelRestrict | PuntVote | ") +
	                                         "VotePercent | VoteNumPlayersReq | ChangeTeam | ChangeTeamInterval | "
	                                         "ChangeTeamPenalty | ChangeTeamDelay | StartDelay | DoMinPingCheck | MinPing | "
	                                         "DoMaxPingCheck | MaxPing | MaxFriendlyKills | GameTime | FriendlyFire | "
	                                         "FriendlyTags | TeamTriggerClaymore | Tracers | KOTHLimit | KillLimit | "
	                                         "FatBullets | OneShotKill] #",
	          "any other lone key is the usage");
}

void test_mission() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	Client c = logged_in(server, "boss");
	host.console->set_scene(inmatch::AdminScene::Other);
	expect_eq(c.one("MISSION LIST"), "No missions in queue.", "LIST outside the Game Loop");
	expect_eq(c.one("MISSION ADD tdm01.bms"), "OK - Entry Added\n", "ADD outside the Game Loop");
	expect(host.rotation.rows[0].launch_option == 1 && host.rotation.state.entries.empty(),
	       "outside the Game Loop ADD sets the launch option only");
	host.console->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(c.one("MISSION ADD TDM01.BMS 0"), "OK - Entry Added\n", "ADD in the Game Loop");
	expect_eq(c.one("MISSION ADD ctf02.bms 1 9 ONESHOT"), "OK - Entry Added\n", "ADD with an INSERT_AT and ONESHOT");
	expect_eq(c.one("MISSION ADD KOTH03.BMS 0 0 1"), "OK - Entry Added\n", "ONESHOT (1|0) never matches");
	expect(host.rotation.state.entries.size() == 3 && !host.rotation.state.entries[0].one_shot &&
	               host.rotation.state.entries[2].one_shot && host.rotation.rows[1].launch_option == 1,
	       "the literal ONESHOT only");
	host.rotation.state.cursor = 1;
	host.rotation.state.alt_cursor = 2;
	expect_eq(c.one("MISSION LIST"),
	          "0: KOTH03.BMS - () () () <> <>\n1: TDM01.BMS - () () () <CURRENT MISSION> <>\n"
	          "2: CTF02.BMS - (2x) () (ONE_SHOT) <> <NEXT MISSION>\n",
	          "LIST's marks");
	expect(admin_parse_current_mission(c.one("MISSION LIST")) == "TDM01", "the client half reads the current mission");
	expect_eq(c.one("MISSION AVAILABLE"), "0. TDM01.BMS (Desert Run)\n1. CTF02.BMS (Two Flags)\n2. KOTH03.BMS (Hill)\n",
	          "AVAILABLE");
	expect_eq(c.one("MISSION ADD NOPE.BMS"), "ERROR - File Not Found.", "a miss");
	expect_eq(c.one("MISSION ADD"), "USAGE -  MISSION ADD 'FILENAME' [AUTO_SWITCH_SIDES (1|0)] [INSERT_AT] [ONESHOT (1|0)]",
	          "ADD's usage");
	expect_eq(c.one("MISSION REMOVE 99"), "OK - Mission Removed.", "REMOVE replies OK on a failure");
	expect_eq(c.one("MISSION SETNEXT 99"), "OK - Next Mission Set.", "SETNEXT replies OK on a refusal");
	expect(host.rotation.latch, "and the latch is set either way");
	expect_eq(c.one("MISSION REMOVE"), "USAGE -  MISSION [LIST | AVAILABLE | ADD | REMOVE | CLEAR | CYCLE | SETNEXT ] [#]",
	          "REMOVE without a number");
	const std::string query = host.console->status_report();
	char line0[96];
	char line1[96];
	std::snprintf(line0, sizeof(line0), "%d: %s - %s %s %s %s %s\n", 0, "KOTH03.BMS", "", "", "", "", "");
	std::snprintf(line1, sizeof(line1), "%d: %s - %s %s %s %s %s\n", 1, "TDM01.BMS", "", "", "", "<CURRENT MISSION>", "");
	expect(query.find(std::string("MAP QUEUE: \r\n") + line0 + line1) != std::string::npos,
	       "QUERY's queue prints empty strings where LIST prints () and <>");
	expect_eq(c.one("MISSION CYCLE"), "OK - Server is cycling...", "CYCLE in the Game Loop");
	expect(host.world.match.outcome().ended && host.ctx.round_end_linger_override_ticks == 620,
	       "the round ends with the 620 linger");
	expect_eq(c.one("MISSION CLEAR"), "OK - Mission list reset.", "CLEAR");
	expect(!host.rotation.state.exists, "CLEAR in the Game Loop frees the list");
	expect_eq(c.one("MISSION LIST"), "No missions in queue.", "no list");
	host.console->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("MISSION CYCLE"), "ERROR - Must be in Game State to cycle server.", "CYCLE outside");
}

void test_player() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	Client c = logged_in(server, "boss");
	host.console->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("PLAYER LIST"), "ERROR - Not in Game State.", "LIST outside the Game Loop");
	host.console->set_scene(inmatch::AdminScene::GameLoop);
	const std::string table = c.one("PLAYER LIST");
	expect_eq(table,
	          "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
	          "P1              \t 1\t 1\t" + std::to_string(host.entity(0).player_class) + "\t0\t0\t100\n"
	          "P2              \t 2\t 2\t" + std::to_string(host.entity(1).player_class) + "\t0\t0\t101\n",
	          "the table");
	const auto parsed = admin_parse_player_list(table);
	expect(parsed.size() == 2 && parsed[1].name == "P2" && parsed[1].team == "2", "the client half parses it");
	expect_eq(c.one("PLAYER KILL 7"), "ERROR - Player Not Found.", "an unknown slot");
	expect_eq(c.one("PLAYER PUNT 0"), "ERROR - Player Not Found.", "no slot 0 on this host (D-NET-352)");
	expect_eq(c.one("PLAYER PUNT"), "USAGE -  PLAYER [LIST | PUNT | BAN | SWAPTEAM | KILL | ZEROSCORE] [# | ALL]",
	          "no target");
	expect(c.command("PLAYER FROB 1").empty(), "an unknown sub-verb with a target draws no reply");
	expect_eq(c.one("PLAYER CEASEFIRE ALL"), "OK - All Players Modified.", "CEASEFIRE changes nothing");
	expect_eq(c.one("PLAYER SWAPTEAM 1"), "OK - Player Swapped.", "SWAPTEAM");
	expect(host.entity(0).team == 2, "team 1 goes to 2");
	expect_eq(c.one("PLAYER KILL 2"), "OK - Player Killed.", "KILL");
	expect((host.entity(1).flags & 2u) != 0, "Flags |= 2 after the death transaction");
	expect_eq(c.one("PLAYER ZEROSCORE ALL"), "OK - All Players Zeroed.", "ZEROSCORE");
	expect_eq(c.one("PLAYER BAN 1"), "ERROR - Player #1 \"P1\" doesn't have a PCID.", "BAN with no PCID");
	host.conn(0).account.pcid = "A-MM-WXXIB2";
	host.ctx.bans.pcids = banlist::PcidBanList{};
	const auto ban = c.command("PLAYER BAN 1");
	expect(ban.size() == 2 && ban[0] == "OK - Banned Player #1 \"P1\"." && ban[1] == "OK - Player Banned.",
	       "a NovaWorld host's BAN draws two replies");
	const DisconnectEvent banned = host.punt(0);
	expect(host.ctx.bans.pcids->entries.size() == 1 && host.ctx.bans.pcids->entries[0].name == "P1" &&
	               banned.dpc == 33 && banned.ddstr == "AdminPunt",
	       "the PCID listed and the player punted (33, AdminPunt)");
	expect_eq(c.one("PLAYER PUNT ALL"), "OK - All Players punted.", "PUNT ALL");
	const DisconnectEvent all = host.punt(1);
	expect(all.dpc == 33 && all.ddstr == "AdminPuntAll", "PUNT ALL punts with AdminPuntAll");
}

void test_weapon_cmd_goto_chat() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	Client a{server};
	a.connect(ip(127, 0, 0, 1));
	a.login("root", "s3cret");
	expect_eq(a.one("WEAPON LIST"), "  1.  ALWAYS\tWPN_M4\n", "LIST: the selectable rows");
	expect_eq(a.one("WEAPON SET 1 NEVER"), "OK - Weapon availbility changed.", "SET one (retail's spelling)");
	expect_eq(a.one("WEAPON LIST"), "  1.  NEVER \tWPN_M4\n", "NEVER carries a trailing space");
	expect_eq(a.one("WEAPON SET ALL ARMORY"), "OK - All weapons availbility changed.", "SET ALL");
	expect(host.ctx.weapon_restrictions.size() == 1 && host.ctx.weapon_restrictions[0].first == 1 &&
	               host.ctx.weapon_restrictions[0].second == 2,
	       "ALL writes the listed rows only");
	expect_eq(a.one("WEAPON GET 1 NEVER"), "USAGE -  WEAPON [LIST | SET] [# | ALL] [ALWAYS | NEVER | ARMORY]",
	          "another sub-verb");

	expect_eq(a.one("CMD"), "USAGE -  CMD 'IN GAME COMMAND STRING'", "CMD's usage");
	expect_eq(a.one("CMD BANDWIDTH 1000"), "OK - Command executed.", "CMD");
	expect(host.ctx.config.entity_send_budget == 200, "BANDWIDTH n / 5");
	a.one("CMD PUNT 2");
	const DisconnectEvent console_punt = host.punt(1);
	expect(console_punt.dpc == 33 && console_punt.ddstr == "S.C:SP#", "the console PUNT (33, S.C:SP#)");

	expect_eq(a.one("GOTO"), "USAGE -  GOTO [GAMESTATE | MENUSTATE]", "GOTO alone: the usage only");
	const auto gamestate = a.command("GOTO GAMESTATE");
	expect(gamestate.size() == 2 && gamestate[0] == "ERROR - In 'Game' State." &&
	               gamestate[1] == "OK - Server is cycling...",
	       "GOTO GAMESTATE in a match: two replies and a cycle");
	const auto menu = a.command("GOTO MENUSTATE");
	expect(menu.size() == 1 && menu[0] == "OK - Server is cycling..." && host.quits == 1,
	       "GOTO MENUSTATE in a match on a Serve Only host: input action 3 (record 3, the exit row, "
	       "no head-gate bit) quits, and the cycle tail sends its one reply");
	host.ctx.is_mp_session_peer = 1;
	const auto peer_menu = a.command("GOTO MENUSTATE");
	expect(peer_menu.size() == 1 && peer_menu[0] == "OK - Server is cycling..." && host.quits == 2,
	       "a listen host's GOTO MENUSTATE quits the same way");
	host.ctx.is_mp_session_peer = 0;
	host.console->set_scene(inmatch::AdminScene::MainMenu);
	const auto at_menu = a.command("GOTO MENUSTATE");
	expect(at_menu.size() == 2 && at_menu[0] == "ERROR - Already in 'Menu' State" &&
	               at_menu[1] == "ERROR - Must be in Game State to cycle server.",
	       "GOTO MENUSTATE at the menu");
	host.console->set_scene(inmatch::AdminScene::GameLoop);

	host.drain(0);
	host.drain(1);
	expect_eq(a.one("CHAT SEND hello <b>world"), "OK - Chat sent.", "SEND");
	const auto sent = host.drain(0);
	const std::vector<uint8_t> want = {10, 0xFF, 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', ' ', 0};
	expect(sent.size() == 1 && sent[0].tag == s2c::CHAT_BROADCAST && sent[0].body == want,
	       "S2C 0x14 [10][255] with the text stripped and the trailing space");
	expect(!host.ctx.console_chat.empty() && host.ctx.console_chat.back().text == "hello <b>world " &&
	               host.ctx.console_chat.back().color == hud::kHudColorWhite,
	       "the host's own CHAT ring takes the unstripped line in white");
	expect_eq(a.one("CHAT SEND hello <b>world"), "OK - Chat sent.", "a repeat");
	expect(host.drain(0).empty() && host.ctx.console_chat.size() == 2 &&
	               host.ctx.console_chat.back().color == hud::kHudColorLightBlue && host.echoes.empty(),
	       "the flood table refuses it and the sender echoes it into the ring in Global's flood colour");
	expect_eq(a.one("CHAT GET"), "hello <b>world \r\nhello <b>world \r\n",
	          "GET lists the ring oldest first, CR LF each");
	// The flood table is the context's one, which the console's typed line checks too: the
	// same words over CHAT SEND within 0x500 frames are refused.
	std::string typed = "from the console ";
	expect(inmatch::Server_SendConsoleChat(host.ctx, hud::kChatDispatchGlobal, typed, 10) ==
	               hud::ChatSendResult::Broadcast && host.drain(0).size() == 1,
	       "the console's typed line");
	host.console->set_main_frame(20);
	expect_eq(a.one("CHAT SEND from the console"), "OK - Chat sent.", "the same words over CHAT SEND");
	expect(host.drain(0).empty(), "the shared table refused the repeat");
}

void test_framing() {
	Host host;
	io::CrtRand rand;
	AdminServer server(users_config(), *host.console, rand);
	{
		Client c = logged_in(server, "boss");
		std::vector<uint8_t> bad = admin_encode_packet(admin_encode_command("GET GAMESTATE").data(), 14);
		bad[0] = 1;
		expect(c.send_raw(bad).empty() && !c.open, "a wrong marker closes");
	}
	{
		Client c = logged_in(server, "boss");
		const std::string text = "GET GAMESTATE";
		const auto r = c.send_payload(std::vector<uint8_t>(text.begin(), text.end()));
		expect(r.empty() && !c.open, "an authenticated packet whose last byte is not NUL closes");
	}
	{
		Client c = logged_in(server, "boss");
		const std::vector<uint8_t> one = admin_encode_command("GET GAMESTATE");
		std::vector<uint8_t> two = admin_encode_packet(one.data(), one.size());
		const std::vector<uint8_t> second = admin_encode_packet(one.data(), one.size());
		two.insert(two.end(), second.begin(), second.end());
		const auto r = c.send_raw(two);
		expect(r.size() == 1 && c.open, "a pipelined second packet in the same read is lost");
	}
	{
		Client c = logged_in(server, "boss");
		std::vector<uint8_t> head = admin_encode_command("GET GAMESTATE");
		std::vector<uint8_t> packet = admin_encode_packet(head.data(), head.size());
		std::vector<uint8_t> first(packet.begin(), packet.begin() + 5);
		std::vector<uint8_t> rest(packet.begin() + 5, packet.end());
		expect(c.send_raw(first).empty() && c.open, "a partial packet waits");
		expect(c.send_raw(rest).size() == 1, "and completes on the next read");
		std::vector<uint8_t> wedge(8, 0);
		io::write_u32_le(wedge.data(), 0x0A0D0000u);
		io::write_u32_le(wedge.data() + 4, 2000);
		c.send_raw(wedge);
		expect(c.open && server.receive_room(c.id) == ADMIN_SERVER_BUFFER_BYTES - 8,
		       "a declared length past 1024 waits for bytes the buffer cannot hold");
	}
}

void test_query_report() {
	Host host;
	const std::string report = host.console->status_report();
	expect_eq(report.substr(0, report.find("CURRENT PLAYERS:")),
	          "SERVER: Admin Host\r\nUP-TIME: 1 02:03:04\r\nCURRENT TOD: 3072\r\nCURRENT # PLAYERS: 2\r\n"
	          "CURRENT MAP: TDM01.BMS\r\nCURRENT GAME TYPE: Team Deathmatch\r\n",
	          "the header lines");
	expect(report.find("CURRENT PLAYERS: \r\nNAME            \t #\tTEAM") != std::string::npos &&
	               report.find("PING\n") != std::string::npos &&
	               report.substr(report.find("MAP QUEUE: ")) == "MAP QUEUE: \r\nNo missions in queue.",
	       "the table, then MAP QUEUE with no rotation");
}

} // namespace

int main() {
	test_challenge_and_whitelist();
	test_login_rules();
	test_rights_and_usage();
	test_get_and_set();
	test_mission();
	test_player();
	test_weapon_cmd_goto_chat();
	test_framing();
	test_query_report();
	if (g_failures == 0) {
		std::printf("admin_server: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
