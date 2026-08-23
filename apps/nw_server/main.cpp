// nw-server — the headless in-match game HOST (engine/net/npruntime P6). A pure C++ dedicated server: it
// loads a mission, stands up the npruntime runtime as a NovaWorld HostOnly session, opens a real UDP
// socket, and asks inmatch::Session to drive the host loop at the original fixed cadence so
// retail-wire-compatible clients (opennova or, as a follow-up, stock retail) can join -> spawn ->
// play. All protocol/crypto/framing and cadence live in the libs; this binary owns the socket and
// wall-clock pacing only.
//
// It NEVER links godot-cpp (godot-cpp is a separate SCons build, not in this CMake graph). Separate from
// the matchmaking apps/novaworld_server (gate/lobby/HTTP) — this is the authoritative game server.

#include <npwire/net_ports.h>
#include <npwire/game_type.h>
#include <npruntime/host_session.h> // the host owner loop, promoted to engine/net/npruntime (P7/A3)
#include <npruntime/session_status.h>
#include <inmatch/session.h>

#include "net_datagram_socket.h" // net::Socket-backed netsim::IDatagramSocket adapter
#include "net_sockets.h"         // net::startup / udp_bind / ScopedSocket
#include "environment_startup.h" // mission-selected ENV/BMS -> World::network_env
#include "wac_startup.h"         // resource-root WAC layers + retail startup order

#include <mission/event_runtime.h> // BmsEventSystem
#include <mission/mission.h> // MissionDocument
#include <mission/promote.h> // promote_mission

#include <wac/wac_system.h>

#include <world/ai.h>
#include <world/world.h>

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
#include <sstream>
#include <string>
#include <thread>
#include <io/log.h>

namespace {

std::atomic<bool> g_shutdown{false};
void on_signal(int) { g_shutdown.store(true); }

uint16_t env_port(const char *name, uint16_t fallback) {
	if (const char *v = std::getenv(name)) {
		const int p = std::atoi(v);
		if (p > 0 && p < 65536) return static_cast<uint16_t>(p);
	}
	return fallback;
}

bool apply_env_u32(const char *name, uint32_t &value) {
	const char *text = std::getenv(name);
	if (text == nullptr || *text == '\0') return true;
	char *end = nullptr;
	errno = 0;
	const unsigned long parsed = std::strtoul(text, &end, 0);
	if (*text == '-' || errno == ERANGE || end == text || *end != '\0' ||
			parsed > std::numeric_limits<uint32_t>::max()) {
		std::fprintf(stderr,
				"nw-server: %s must be a uint32 (decimal or 0x hex)\n", name);
		return false;
	}
	value = static_cast<uint32_t>(parsed);
	return true;
}

bool apply_env_i32(const char *name, int32_t &value) {
	const char *text = std::getenv(name);
	if (text == nullptr || *text == '\0') return true;
	char *end = nullptr;
	errno = 0;
	const long parsed = std::strtol(text, &end, 0);
	if (errno == ERANGE || end == text || *end != '\0' ||
			parsed < std::numeric_limits<int32_t>::min() ||
			parsed > std::numeric_limits<int32_t>::max()) {
		std::fprintf(stderr,
				"nw-server: %s must be an int32 (decimal or 0x hex)\n", name);
		return false;
	}
	value = static_cast<int32_t>(parsed);
	return true;
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
// warnings and errors to stderr; per-tick kDebug tracing opts in via NW_LOG_DEBUG.
bool g_log_debug_enabled = false;
void app_log_sink(opennova::io::LogLevel level, const char *msg) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug_enabled) return;
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n", msg);
}
} // namespace

int main() {
	g_log_debug_enabled = std::getenv("NW_LOG_DEBUG") != nullptr;
	opennova::io::set_log_sink(&app_log_sink);
	using namespace opennova;

	// Mission source: NW_MISSION env var (no committed fixture — ADR 0003 forbids inventing one).
	const char *mission_path = std::getenv("NW_MISSION");
	if (mission_path == nullptr || *mission_path == '\0') {
		std::fprintf(stderr, "nw-server: set NW_MISSION=<path-to .bms> (the mission to host)\n");
		return 2;
	}
	const uint16_t port = env_port("NW_LAN_PORT", opennova::kRetailLanPortMin); // the retail mpnovaworldport default

	// --- Load the mission + build the authoritative World (pools + nav + AI brains). ---
	mission::MissionDocument doc;
	if (!doc.load_bms_file(mission_path)) {
		std::fprintf(stderr, "nw-server: failed to load mission '%s'\n", mission_path);
		return 1;
	}
	world::World world;
	const mission::MissionInfo mission_info = doc.info();
	std::filesystem::path explicit_env_path;
	if (const char *env_path = std::getenv("NW_ENV");
	    env_path != nullptr && *env_path != '\0') {
		explicit_env_path = env_path;
	}
	if (mission_info.environment.empty() && explicit_env_path.empty()) {
		std::fprintf(stderr,
		             "nw-server: mission '%s' has no environment reference; "
		             "set NW_ENV=<path-to .env>\n",
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
		             "nw-server: %s; set NW_ENV=<path-to %s.env> when the "
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
	if (const char *root = std::getenv("NW_RESOURCE_ROOT");
	    root != nullptr && *root != '\0') {
		explicit_resource_root = root;
	}
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
	if (!apply_env_u32("NW_GAME_TYPE", host_cfg.config.game_type))
		return 2;
	uint32_t configured_teams = host_cfg.config.num_teams;
	if (!apply_env_u32("NW_NUM_TEAMS", configured_teams))
		return 2;
	if (!apply_env_i32(
			"NW_CAPTURE_DURATION_SECONDS",
			host_cfg.config.capture_duration_seconds) ||
			!apply_env_i32(
					"NW_CAPTURE_SPEED_SETTING",
					host_cfg.config.capture_speed_setting) ||
			!apply_env_i32(
					"NW_SPAWN_WAVE_TIME_BASE",
					host_cfg.config.spawn_wave_time_base) ||
			!apply_env_i32(
					"NW_SPAWN_WAVE_TIME_ZONE",
					host_cfg.config.spawn_wave_time_zone) ||
			!apply_env_u32(
					"NW_DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE",
					host_cfg.config.default_spawn_requires_no_team_zone))
		return 2;
	if (configured_teams > 0xFFu) {
		std::fprintf(stderr, "nw-server: NW_NUM_TEAMS must fit a uint8\n");
		return 2;
	}
	host_cfg.config.num_teams = static_cast<uint8_t>(configured_teams);
	// Retail starts from GameType_CreateDefaultSettings and overlays a loose
	// VERSION 40 score.ini when present. An absent file intentionally leaves the
	// optional row unset so Match and S2C 0x58 select that same default table.
	// [orig: GameType_CreateDefaultSettings @0x52DD00;
	// ScoreConfig_LoadFile @0x52D8A0]
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
