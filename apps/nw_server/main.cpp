// nw-server — the headless in-match game HOST (engine/net/npruntime P6). A pure C++ dedicated server: it
// loads a mission, stands up the npruntime runtime as a NovaWorld HostOnly session, opens a real UDP
// socket, and asks inmatch::Session to drive the host loop at the original fixed cadence so
// retail-wire-compatible clients (opennova or, as a follow-up, stock retail) can join -> spawn ->
// play. All protocol/crypto/framing and cadence live in the libs; this binary owns the socket and
// wall-clock pacing only.
//
// It NEVER links godot-cpp (godot-cpp is a separate SCons build, not in this CMake graph). Separate from
// the matchmaking apps/novaworld_server (gate/lobby/HTTP) — this is the authoritative game server.

#include <net/npwire/net_ports.h>
#include <net/npwire/game_type.h>
#include <net/npruntime/host_session.h> // the host owner loop, promoted to engine/net/npruntime (P7/A3)
#include <net/npruntime/session_status.h>
#include <net/inmatch/session.h>

#include "net_datagram_socket.h" // net::Socket-backed netsim::IDatagramSocket adapter
#include "net_sockets.h"         // net::startup / udp_bind / ScopedSocket
#include "environment_startup.h" // mission-selected ENV/BMS -> World::network_env
#include "wac_startup.h"         // resource-root WAC layers + retail startup order

#include <runtime/mission/event_runtime.h> // BmsEventSystem
#include <formats/mission/mission.h> // MissionDocument
#include <runtime/mission/promote.h> // promote_mission
#include <formats/rtxt/rtxt.h>        // the gametext "Server" strings (STRSRV_MEDREQ)

#include <runtime/wac/wac_system.h>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <base/io/log.h>

namespace {

std::atomic<bool> g_shutdown{false};
void on_signal(int) { g_shutdown.store(true); }

// Every g_GameType code word Game_StartMission can produce; anything else is
// a typo, not a mode. [orig: Game_StartMission @0x524360 type switch]
bool is_retail_game_type_word(uint32_t value) {
	switch (value) {
	case opennova::game_type::kDeathmatch:
	case opennova::game_type::kKingOfTheHill:
	case opennova::game_type::kFlagMe:
	case opennova::game_type::kTeamDeathmatch:
	case opennova::game_type::kTeamKingOfTheHill:
	case opennova::game_type::kAttackDefend:
	case opennova::game_type::kCaptureTheFlag:
	case opennova::game_type::kFlagBall:
	case opennova::game_type::kAdvanceAndSecure:
	case opennova::game_type::kCoop:
	case opennova::game_type::kObjectiveCoop:
	case opennova::game_type::kConquerAndControl:
	case opennova::game_type::kSearchAndDestroy:
		return true;
	default:
		return false;
	}
}

// --- Command line. Everything the harness varies is a flag; nothing is read
//     from the environment (docs/dev-env-vars.md). ---
const char kUsage[] =
		"usage: nw_server --mission <path.bms> [--env <path.env>] [--resource-root <dir>]\n"
		"                 [--port <1..65535>] [--game-type <code>] [--num-teams <1..255>]\n"
		"                 [--capture-duration-seconds <i32>] [--capture-speed-setting <i32>]\n"
		"                 [--spawn-wave-time-base <i32>] [--spawn-wave-time-zone <i32>]\n"
		"                 [--default-spawn-requires-no-team-zone] [--log-debug]\n"
		"  --mission          the loose .bms to host; its directory is the default resource root\n"
		"  --env              the .env to publish when it is not beside the mission\n"
		"  --resource-root    where game.wac / server.wac / <mission>.wac, score.ini and\n"
		"                     gametext.bin live when they were exported elsewhere\n"
		"  --port             UDP bind port (default: the retail LAN range head)\n"
		"  --game-type        an exact g_GameType code, decimal or 0x hex (default: the\n"
		"                     mission's authored mode)\n"
		"  --num-teams        active-team count for the multi-team modes (default 2)\n"
		"  --capture-duration-seconds, --capture-speed-setting\n"
		"                     A&S / C&C takeover overrides (retail: 15 s, speed setting 1)\n"
		"  --spawn-wave-time-base, --spawn-wave-time-zone\n"
		"                     spawn-wave timing overrides\n"
		"  --default-spawn-requires-no-team-zone\n"
		"                     retail cfg nodefaultspawnpoints\n"
		"  --log-debug        forward io/log.h kDebug tracing to the console sink\n";

struct Options {
	const char *mission = nullptr;
	const char *env = nullptr;
	const char *resource_root = nullptr;
	uint16_t port = opennova::kRetailLanPortMin; // the retail mpnovaworldport default
	std::optional<uint32_t> game_type;
	std::optional<uint32_t> num_teams;
	std::optional<int32_t> capture_duration_seconds;
	std::optional<int32_t> capture_speed_setting;
	std::optional<int32_t> spawn_wave_time_base;
	std::optional<int32_t> spawn_wave_time_zone;
	bool default_spawn_requires_no_team_zone = false;
	bool log_debug = false;
};

bool parse_u32(const char *flag, const char *text, uint32_t &value) {
	char *end = nullptr;
	errno = 0;
	const unsigned long parsed = std::strtoul(text, &end, 0);
	if (*text == '\0' || *text == '-' || errno == ERANGE || end == text || *end != '\0' ||
			parsed > std::numeric_limits<uint32_t>::max()) {
		std::fprintf(stderr, "nw-server: %s must be a uint32 (decimal or 0x hex)\n", flag);
		return false;
	}
	value = static_cast<uint32_t>(parsed);
	return true;
}

bool parse_i32(const char *flag, const char *text, int32_t &value) {
	char *end = nullptr;
	errno = 0;
	const long parsed = std::strtol(text, &end, 0);
	if (*text == '\0' || errno == ERANGE || end == text || *end != '\0' ||
			parsed < std::numeric_limits<int32_t>::min() ||
			parsed > std::numeric_limits<int32_t>::max()) {
		std::fprintf(stderr, "nw-server: %s must be an int32 (decimal or 0x hex)\n", flag);
		return false;
	}
	value = static_cast<int32_t>(parsed);
	return true;
}

bool parse_port(const char *flag, const char *text, uint16_t &value) {
	uint32_t parsed = 0;
	if (!parse_u32(flag, text, parsed)) return false;
	if (parsed == 0 || parsed > 65535) {
		std::fprintf(stderr, "nw-server: %s must be 1..65535\n", flag);
		return false;
	}
	value = static_cast<uint16_t>(parsed);
	return true;
}

// Returns 0 when the options parsed, 2 on a usage error (already reported),
// and -1 when the caller asked for --help (usage printed, exit 0).
int parse_options(int argc, char **argv, Options &o) {
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--help" || a == "-h") {
			std::fputs(kUsage, stdout);
			return -1;
		}
		if (a == "--log-debug") {
			o.log_debug = true;
			continue;
		}
		if (a == "--default-spawn-requires-no-team-zone") {
			o.default_spawn_requires_no_team_zone = true;
			continue;
		}
		const bool takes_value = a == "--mission" || a == "--env" || a == "--resource-root" ||
				a == "--port" || a == "--game-type" || a == "--num-teams" ||
				a == "--capture-duration-seconds" || a == "--capture-speed-setting" ||
				a == "--spawn-wave-time-base" || a == "--spawn-wave-time-zone";
		if (!takes_value) {
			std::fprintf(stderr, "nw-server: unknown option '%s'\n%s", a.c_str(), kUsage);
			return 2;
		}
		if (i + 1 >= argc) {
			std::fprintf(stderr, "nw-server: %s needs a value\n%s", a.c_str(), kUsage);
			return 2;
		}
		const char *v = argv[++i];
		bool ok = true;
		if (a == "--mission") {
			o.mission = v;
		} else if (a == "--env") {
			o.env = v;
		} else if (a == "--resource-root") {
			o.resource_root = v;
		} else if (a == "--port") {
			ok = parse_port(a.c_str(), v, o.port);
		} else if (a == "--game-type") {
			uint32_t x = 0;
			ok = parse_u32(a.c_str(), v, x);
			o.game_type = x;
		} else if (a == "--num-teams") {
			uint32_t x = 0;
			ok = parse_u32(a.c_str(), v, x);
			o.num_teams = x;
		} else {
			int32_t x = 0;
			ok = parse_i32(a.c_str(), v, x);
			if (a == "--capture-duration-seconds") o.capture_duration_seconds = x;
			else if (a == "--capture-speed-setting") o.capture_speed_setting = x;
			else if (a == "--spawn-wave-time-base") o.spawn_wave_time_base = x;
			else o.spawn_wave_time_zone = x;
		}
		if (!ok) return 2;
	}
	if (o.mission == nullptr || *o.mission == '\0') {
		std::fprintf(stderr, "nw-server: --mission <path.bms> is required\n%s", kUsage);
		return 2;
	}
	return 0;
}

// The dedicated host's one adapter to inmatch::Session. The portable session
// decides when a fixed tick is due; this adapter performs that real tick using
// the shared network owner loop.
class HeadlessTickTarget final : public opennova::inmatch::TickTarget {
public:
	HeadlessTickTarget(opennova::world::World &world,
			opennova::np::HostOwner &owner,
			opennova::netsim::IDatagramSocket &socket)
			: world_(world), owner_(owner), socket_(socket) {}

	opennova::inmatch::TickOutcome advance_mission_tick(
			const opennova::inmatch::TickInput &) override {
		world_.network_env.advance_tick();
		opennova::np::host_session_pump(owner_, socket_);
		return {opennova::inmatch::TickStatus::Ran,
				static_cast<int32_t>(world_.logic_tick), {}};
	}

	bool reset_mission_to_baseline(opennova::inmatch::SessionError &error) override {
		error = {opennova::inmatch::SessionErrorCode::NetworkRoleLocked,
				"dedicated hosts cannot reset a live mission"};
		return false;
	}

	void close_mission() override {}

private:
	opennova::world::World &world_;
	opennova::np::HostOwner &owner_;
	opennova::netsim::IDatagramSocket &socket_;
};

} // namespace


namespace {
// The engine/ diagnostic channel (io/log.h): libraries are silent until the host
// installs a sink. Reproduce the historical stream split — lifecycle to stdout,
// warnings and errors to stderr; per-tick kDebug tracing opts in via --log-debug.
bool g_log_debug_enabled = false;
void app_log_sink(opennova::io::LogLevel level, const char *msg) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug_enabled) return;
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n", msg);
}
} // namespace

int main(int argc, char **argv) {
	Options opt;
	const int parse_rc = parse_options(argc, argv, opt);
	if (parse_rc != 0) return parse_rc < 0 ? 0 : parse_rc;
	g_log_debug_enabled = opt.log_debug;
	opennova::io::set_log_sink(&app_log_sink);
	using namespace opennova;

	// Mission source: --mission (no committed fixture — ADR 0003 forbids inventing one).
	const char *mission_path = opt.mission;
	const uint16_t port = opt.port;

	// --- Load the mission + build the authoritative World (pools + nav + AI brains). ---
	mission::MissionDocument doc;
	if (!doc.load_bms_file(mission_path)) {
		std::fprintf(stderr, "nw-server: failed to load mission '%s'\n", mission_path);
		return 1;
	}
	world::World world;
	const mission::MissionInfo mission_info = doc.info();
	std::filesystem::path explicit_env_path;
	if (opt.env != nullptr && *opt.env != '\0') explicit_env_path = opt.env;
	if (mission_info.environment.empty() && explicit_env_path.empty()) {
		std::fprintf(stderr,
		             "nw-server: mission '%s' has no environment reference; "
		             "pass --env <path-to .env>\n",
		             mission_path);
		return 1;
	}
	const std::filesystem::path resolved_env =
			nw_server::resolve_environment_path(
					mission_path, mission_info.environment, explicit_env_path);
	std::string env_error;
	if (!nw_server::publish_initial_environment_file(
			resolved_env, doc.bms_file().header, world.network_env, env_error)) {
		std::fprintf(stderr,
		             "nw-server: %s; pass --env <path-to %s.env> when the "
		             "resource is not beside the mission\n",
		             env_error.c_str(), mission_info.environment.c_str());
		return 1;
	}
	world::AiSystem ai;
	world.ai = &ai; // Server_BuildPlayerInfoAndAdd requires an AiSystem to spawn a player
	const mission::PromoteResult pr = mission::promote_mission(doc.bms_file(), world, ai);
	std::fprintf(stderr, "nw-server: promoted '%s' (%d entities, %d brains, %d nav nodes)\n",
	             mission_path, pr.spawned, pr.brains, pr.nav_nodes);

	std::filesystem::path explicit_resource_root;
	if (opt.resource_root != nullptr && *opt.resource_root != '\0')
		explicit_resource_root = opt.resource_root;
	const std::filesystem::path resource_root =
			nw_server::resolve_resource_root(mission_path, explicit_resource_root);
	wac::WacSystem wac;
	mission::BmsEventSystem bms;
	bool wac_loaded = false;
	std::string startup_error;
	if (!nw_server::initialize_mission_startup(
			resource_root, std::filesystem::path(mission_path).stem().string(),
			doc.bms_file(), world, wac, bms, ai, wac_loaded, startup_error)) {
		std::fprintf(stderr, "nw-server: %s\n", startup_error.c_str());
		return 1;
	}
	std::fprintf(stderr, "nw-server: WAC %s from '%s'\n",
			wac_loaded ? "loaded" : "absent (BMS-only)",
			resource_root.string().c_str());

	// --- Open the UDP socket. ---
	if (net::startup() != 0) {
		std::fprintf(stderr, "nw-server: winsock init failed\n");
		return 1;
	}
	uint16_t bound = 0;
	net::ScopedSocket sock(net::udp_bind(port, &bound));
	if (!sock.is_valid()) {
		std::fprintf(stderr, "nw-server: bind on UDP %u failed\n", port);
		net::shutdown();
		return 1;
	}

	// --- Stand up the npruntime runtime as a dedicated HostOnly server. There is no synthetic
	//     loopback player; every roster row belongs to an admitted remote peer. ---
	np::HostOwner owner;
	owner.ctx.world = &world;
	owner.ctx.mission = &doc.bms_file();

	np::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova nw-server";
	host_cfg.config.max_players = 16;
	host_cfg.config.mission_name = mission_info.mission_name;
	host_cfg.config.mission_file =
			std::filesystem::path(mission_path).filename().string();
	host_cfg.config.game_type = game_type::for_mission_mode(
			bms::selected_game_mode(doc.bms_file().header.attrib_flags));
	// The harness has no host-options UI, so install the same fresh-host rule
	// defaults the retail config path would have applied before mission start.
	host_cfg.config.respawn_time = game_rules::kDefaultRespawnTime;
	host_cfg.config.time_limit_minutes = game_rules::kDefaultTimeLimitMinutes;
	host_cfg.config.replay_enabled = game_rules::kDefaultReplayEnabled;
	host_cfg.config.max_team_lives = game_rules::kDefaultMaxTeamLives;
	host_cfg.config.score_limit = game_rules::kDefaultScoreLimit;
	host_cfg.config.max_score = game_rules::kDefaultMaxScore;
	host_cfg.config.koth_delta = game_rules::kDefaultKothDelta;
	host_cfg.config.flag_return_ticks = game_rules::kDefaultFlagReturnTicks;
	host_cfg.config.capture_duration_seconds =
			game_rules::kDefaultCaptureDurationSeconds;
	host_cfg.config.capture_speed_setting =
			game_rules::kDefaultCaptureSpeedSetting;
	host_cfg.config.spawn_wave_time_base =
			game_rules::kDefaultSpawnWaveTimeBase;
	host_cfg.config.spawn_wave_time_zone =
			game_rules::kDefaultSpawnWaveTimeZone;
	host_cfg.config.default_spawn_requires_no_team_zone =
			game_rules::kDefaultSpawnRequiresNoTeamZone;
	host_cfg.config.num_teams = static_cast<uint8_t>(game_rules::kDefaultNumTeams);
	host_cfg.config.respawn_timeout = game_rules::kDefaultRespawnTimeout;
	host_cfg.config.start_delay = game_rules::kDefaultStartDelay;
	host_cfg.config.destroy_buildings = game_rules::kDefaultDestroyBuildings;
	host_cfg.config.death_messages = game_rules::kDefaultDeathMessages;
	// The BMS task vocabulary has no authored Flag Me bit even though retail's
	// Game_StartMission retains its type-12 -> g_GameType 8 branch. The harness
	// therefore accepts an explicit numeric code so every witnessed wire mode
	// remains capturable; ordinary hosts continue to derive the mission type.
	// [orig: AI_GetTaskTypeFromFlags @0x40DAE0;
	// Game_StartMission @0x524360]
	if (opt.game_type) host_cfg.config.game_type = *opt.game_type;
	if (!is_retail_game_type_word(host_cfg.config.game_type)) {
		std::fprintf(stderr,
				"nw-server: --game-type 0x%X is not a retail g_GameType code\n",
				host_cfg.config.game_type);
		return 2;
	}
	if (opt.num_teams) {
		if (*opt.num_teams > 0xFFu) {
			std::fprintf(stderr, "nw-server: --num-teams must fit a uint8\n");
			return 2;
		}
		host_cfg.config.num_teams = static_cast<uint8_t>(*opt.num_teams);
	}
	if (opt.capture_duration_seconds)
		host_cfg.config.capture_duration_seconds = *opt.capture_duration_seconds;
	if (opt.capture_speed_setting)
		host_cfg.config.capture_speed_setting = *opt.capture_speed_setting;
	if (opt.spawn_wave_time_base)
		host_cfg.config.spawn_wave_time_base = *opt.spawn_wave_time_base;
	if (opt.spawn_wave_time_zone)
		host_cfg.config.spawn_wave_time_zone = *opt.spawn_wave_time_zone;
	if (opt.default_spawn_requires_no_team_zone)
		host_cfg.config.default_spawn_requires_no_team_zone = 1u;
	// Retail starts from GameType_CreateDefaultSettings and overlays a loose
	// VERSION 40 score.ini when present. An absent file intentionally leaves the
	// optional row unset so Match and S2C 0x58 select that same default table.
	// [orig: GameType_CreateDefaultSettings @0x52DD00;
	// ScoreConfig_LoadFile @0x52D8A0]
	// The "Server" chat strings: retail reads GameText("Server", key) from the
	// gametext table loaded at init; this host reads a loose gametext.bin beside
	// the mission when one is present and otherwise leaves the strings empty,
	// which is retail's null lookup (the medic-call handler then no-ops).
	// [orig: Game_InitSubsystems @0x4A6CD0; Server_BroadcastMedicRequest
	// @0x5153C9]
	{
		const std::filesystem::path gametext_path = resource_root / "gametext.bin";
		std::error_code gametext_exists_error;
		if (std::filesystem::exists(gametext_path, gametext_exists_error)) {
			opennova::rtxt::File gametext;
			std::string gametext_error;
			if (opennova::rtxt::parse_file(gametext_path.string(), gametext,
					gametext_error)) {
				np::ServerTextTable server_text;
				server_text.medic_request_format =
						gametext.get_in_section("Server", "STRSRV_MEDREQ");
				np::set_server_text(owner.ctx, std::move(server_text));
			} else {
				std::fprintf(stderr, "nw-server: gametext '%s' unreadable: %s\n",
						gametext_path.string().c_str(), gametext_error.c_str());
			}
		}
	}
	const std::filesystem::path score_path = resource_root / "score.ini";
	std::error_code score_exists_error;
	if (std::filesystem::exists(score_path, score_exists_error)) {
		std::ifstream score_file(score_path, std::ios::binary);
		std::ostringstream score_bytes;
		if (!score_file || !(score_bytes << score_file.rdbuf()) ||
				!np::load_session_score_config(
						host_cfg.config, score_bytes.str())) {
			std::fprintf(stderr,
					"nw-server: invalid score config '%s'\n",
					score_path.string().c_str());
			net::shutdown();
			return 1;
		}
	} else if (score_exists_error) {
		std::fprintf(stderr,
				"nw-server: score config '%s' could not be inspected: %s\n",
				score_path.string().c_str(),
				score_exists_error.message().c_str());
		net::shutdown();
		return 1;
	}
	host_cfg.socket_mode = np::SocketMode::Lan; // a real LAN socket (Socketless=1 would be in-process SP)
	host_cfg.serve_and_play = false;            // headless dedicated host: no local-player registration
	if (!world.network_env.valid) {
		std::fprintf(stderr,
		             "nw-server: refusing to launch without an authoritative "
		             "environment sample\n");
		net::shutdown();
		return 1;
	}
	np::start_host_session(owner, host_cfg);

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::fprintf(stderr, "nw-server: hosting on UDP %u at 62.5 Hz (Ctrl+C to stop)\n", bound);

	// --- inmatch::Session owns fixed-tick cadence. The app only paces outer frames against an
	//     absolute deadline so wall-clock scheduling does not drift. ---
	using clock = std::chrono::steady_clock;
	const auto baseline = clock::now();
	constexpr int64_t kPeriodNs =
			static_cast<int64_t>(1000000000.0 * opennova::world::TickAccumulator::kTickDt);
	net::NetDatagramSocket dgram(sock.get()); // recv_timeout_ms = 0 (non-blocking; the loop self-paces)
	HeadlessTickTarget target(world, owner, dgram);
	inmatch::Session session(target, inmatch::Role::DedicatedHost);
	if (!session.begin_load().applied() || !session.complete_load().applied()) {
		std::fprintf(stderr, "nw-server: failed to start mission session\n");
		net::shutdown();
		return 1;
	}
	for (uint64_t frame = 0; !g_shutdown.load(); ++frame) {
		inmatch::FrameInput input;
		input.delta_seconds = world::TickAccumulator::kTickDt;
		const inmatch::FrameOutcome outcome = session.advance(input);
		if (outcome.terminal()) {
			std::fprintf(stderr, "nw-server: mission session failed: %s\n",
					outcome.error.message.c_str());
			break;
		}
		std::this_thread::sleep_until(
				baseline + std::chrono::nanoseconds(static_cast<int64_t>(frame + 1) * kPeriodNs));
	}

	std::fprintf(stderr, "nw-server: shutting down\n");
	(void)session.close();
	net::shutdown();
	return 0;
}
