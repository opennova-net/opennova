// nw-server — the headless in-match dedicated game HOST over the engine's ONE
// mission kernel (ADR 0042 d3). It mounts the resource root, boots the loose
// mission through mission::MissionKernel (terrain, item/weapon/ammo tables,
// collision, infantry .adm — the same boot every embedder drives), stands the
// npruntime runtime up as a HostOnly session through inmatch::listen_host,
// opens a real UDP socket, and asks inmatch::Session to drive the listen frame
// at the original fixed cadence so retail-wire-compatible clients (opennova
// or, as a follow-up, stock retail) can join -> spawn -> play. All protocol,
// crypto, framing, cadence, and mission state live in the libs; this binary
// owns the flag surface, the socket, and the wall-clock pacing only.
//
// It NEVER links godot-cpp (godot-cpp is add_subdirectory'd only from the GDExtension's own
// CMake root). Separate from the matchmaking apps/novaworld_server (gate/lobby/HTTP) — this is
// the authoritative game server.

#include <base/io/log.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>
#include <formats/mission/bms.h>
#include <formats/pcx/pcx_io.h>
#include <formats/rtxt/rtxt.h> // the gametext "Server" strings (STRSRV_MEDREQ)
#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>
#include <net/inmatch/listen_host.h>
#include <net/inmatch/session.h>
#include <net/npruntime/host_session.h>
#include <net/npruntime/napi_np_server_ctx.h>
#include <net/npruntime/session_status.h>
#include <net/npwire/game_type.h>
#include <net/npwire/net_ports.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/world/tick_accumulator.h>

#include "net_datagram_socket.h" // net::Socket-backed netsim::IDatagramSocket adapter
#include "net_sockets.h"         // net::startup / udp_bind / ScopedSocket

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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_shutdown{false};
void on_signal(int) { g_shutdown.store(true); }

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
		"  --resource-root    the directory the mission's resources mount from (terrain,\n"
		"                     items/weapon/ammo tables, WAC layers, score.ini, gametext.bin)\n"
		"                     when they were exported away from the mission\n"
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

// The engine/ diagnostic channel (io/log.h): libraries are silent until the host
// installs a sink. Reproduce the historical stream split — lifecycle to stdout,
// warnings and errors to stderr; per-tick kDebug tracing opts in via --log-debug.
bool g_log_debug_enabled = false;
void app_log_sink(opennova::io::LogLevel level, const char *msg) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug_enabled) return;
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n", msg);
}

// The mission's .cpt/.trn(+charmap) height field, built into the kernel's own
// terrain field store BEFORE boot — the embedder-side format-typed leg on the
// far side of the ADR 0020 seam, through the engine's one loader. The raw .til
// bytes feed the S2C 0x45 terrain-tile load a wire joiner streams (net-re
// §5.37); absent, the tile stream is skipped.
bool load_terrain(opennova::mission::MissionKernel &kernel,
		const opennova::ResourceIndex &index,
		std::vector<uint8_t> &til_bytes, std::string &error) {
	return opennova::terrain::terrain_field_store_load(kernel.terrain_store, index,
			kernel.mission.get_terrain(), error, &til_bytes);
}

// The dedicated host's one adapter to inmatch::Session: the portable session
// decides when a fixed tick is due; the tick itself is the ONE listen-host
// frame over the kernel. viewport_height 0 is the headless seam — npruntime
// then suppresses S2C 0x68 instead of inventing a screen size (D-NET-206).
class DedicatedTickTarget final : public opennova::inmatch::TickTarget {
public:
	DedicatedTickTarget(opennova::mission::MissionKernel &kernel,
			opennova::inmatch::ListenHostState &host,
			opennova::netsim::IDatagramSocket &socket)
			: kernel_(kernel), host_(host), socket_(socket) {}

	opennova::inmatch::TickOutcome advance_mission_tick(
			const opennova::inmatch::TickInput &) override {
		opennova::inmatch::listen_host::frame(kernel_, host_, socket_,
				/*viewport_height=*/0);
		return {opennova::inmatch::TickStatus::Ran,
				static_cast<int32_t>(kernel_.world.logic_tick), {}};
	}

	bool reset_mission_to_baseline(opennova::inmatch::SessionError &error) override {
		error = {opennova::inmatch::SessionErrorCode::NetworkRoleLocked,
				"dedicated hosts cannot reset a live mission"};
		return false;
	}

	void close_mission() override {}

private:
	opennova::mission::MissionKernel &kernel_;
	opennova::inmatch::ListenHostState &host_;
	opennova::netsim::IDatagramSocket &socket_;
};

} // namespace

int main(int argc, char **argv) {
	Options opt;
	const int parse_rc = parse_options(argc, argv, opt);
	if (parse_rc != 0) return parse_rc < 0 ? 0 : parse_rc;
	g_log_debug_enabled = opt.log_debug;
	opennova::io::set_log_sink(&app_log_sink);
	using namespace opennova;

	// --- The loose mission (no committed fixture — ADR 0003 forbids inventing
	//     one). It may live outside the resource root, so it is read from its
	//     own path, not through the mount. ---
	const std::filesystem::path mission_path = opt.mission;
	bms::File mission_doc;
	{
		std::ifstream mission_file(mission_path, std::ios::binary);
		std::ostringstream mission_bytes;
		std::string parse_error;
		if (!mission_file || !(mission_bytes << mission_file.rdbuf())) {
			std::fprintf(stderr, "nw-server: failed to load mission '%s'\n", opt.mission);
			return 1;
		}
		const std::string bytes = mission_bytes.str();
		if (!bms::parse(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
					mission_doc, parse_error)) {
			std::fprintf(stderr, "nw-server: mission '%s' did not parse: %s\n",
					opt.mission, parse_error.c_str());
			return 1;
		}
	}

	// --- Mount the resource root (the mission's own directory unless
	//     --resource-root names the exported layers' home) the way the
	//     kernel's own open() does: the .pff set with loose overrides when
	//     archives exist, the loose tree otherwise (loose files win). ---
	const std::filesystem::path resource_root =
			opt.resource_root != nullptr && *opt.resource_root != '\0'
					? std::filesystem::path(opt.resource_root)
					: (mission_path.has_parent_path() ? mission_path.parent_path()
													  : std::filesystem::path("."));
	ResourceIndex index;
	if (!index.scan(resource_root.string()) &&
			!index.scan(resource_root.string(), std::string(), VfsMountMode::LooseOnly)) {
		std::fprintf(stderr, "nw-server: could not mount '%s': %s\n",
				resource_root.string().c_str(), index.last_error().c_str());
		return 1;
	}

	// --- Adopt the parsed mission over the mounted file source: the kernel is
	//     the one mission boot + state + tick (ADR 0042 d3). ---
	mission::MissionKernel kernel;
	kernel.set_asset_index(&index);
	mission::BootFileSource files;
	files.has_file = [&index](const std::string &name) { return index.has_file(name); };
	files.read_file = [&index](const std::string &name, std::vector<uint8_t> &out) {
		return index.read_file(name, out);
	};
	kernel.open_document(std::move(mission_doc), mission_path.stem().string(), files);

	// --- The authoritative T0 environment sample, published before the boot
	//     (retail order: ENV parse + BMS overrides before Game_StartMission
	//     snapshots the network-visible targets). ---
	const std::string environment_name = kernel.mission.get_environment();
	if (environment_name.empty() && (opt.env == nullptr || *opt.env == '\0')) {
		std::fprintf(stderr,
		             "nw-server: mission '%s' has no environment reference; "
		             "pass --env <path-to .env>\n",
		             opt.mission);
		return 1;
	}
	const std::filesystem::path resolved_env =
			opt.env != nullptr && *opt.env != '\0'
					? std::filesystem::path(opt.env)
					: mission_path.parent_path() / (environment_name + ".env");
	{
		std::ifstream env_input(resolved_env, std::ios::binary);
		std::string env_error;
		if (!env_input) {
			env_error = "environment resource '" + resolved_env.string() +
					"' could not be opened";
		} else if (!env::seed_weather_from_env(
						   env_input, kernel.mission.header,
						   kernel.world.weather, env_error)) {
			env_error = "failed to parse environment '" + resolved_env.string() +
					"': " + env_error;
		}
		if (!kernel.world.weather.valid) {
			std::fprintf(stderr,
			             "nw-server: %s; pass --env <path-to %s.env> when the "
			             "resource is not beside the mission\n",
			             env_error.c_str(), environment_name.c_str());
			return 1;
		}
	}

	// --- The mission's terrain field, built into the kernel's store before
	//     the boot (the ground solve, collision heightfield, surface picks). ---
	std::vector<uint8_t> terrain_til_bytes;
	{
		std::string terrain_error;
		if (!load_terrain(kernel, index, terrain_til_bytes, terrain_error))
			std::fprintf(stderr,
					"nw-server: terrain not loaded (%s) - the ground solve will not run\n",
					terrain_error.c_str());
	}

	// --- The consolidated HostConfig: identity, the mission-derived (or
	//     overridden) g_GameType, the fresh-host rule defaults, the flag
	//     overrides, and the optional loose score.ini overlay. ---
	np::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova nw-server";
	host_cfg.config.max_players = 16;
	host_cfg.config.mission_name = kernel.mission.get_mission_name();
	host_cfg.config.mission_file = mission_path.filename().string();
	host_cfg.config.game_type =
			game_type::for_mission_attribs(kernel.mission.header.attrib_flags);
	// The harness has no host-options UI, so install the same fresh-host rule
	// defaults the retail config path would have applied before mission start.
	np::apply_fresh_host_rule_defaults(host_cfg.config);
	// The BMS task vocabulary has no authored Flag Me bit even though retail's
	// Game_StartMission retains its type-12 -> g_GameType 8 branch. The harness
	// therefore accepts an explicit numeric code so every witnessed wire mode
	// remains capturable; ordinary hosts continue to derive the mission type
	// (game_type::for_mission_attribs).
	if (opt.game_type) host_cfg.config.game_type = *opt.game_type;
	if (!game_type::is_retail_code_word(host_cfg.config.game_type)) {
		std::fprintf(stderr,
				"nw-server: --game-type 0x%X is not a retail g_GameType code\n",
				host_cfg.config.game_type);
		return 2;
	}
	if (opt.num_teams) {
		if (*opt.num_teams < 1u || *opt.num_teams > 0xFFu) {
			std::fprintf(stderr, "nw-server: --num-teams must be 1..255\n");
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
	if (index.has_file("score.ini")) {
		std::vector<uint8_t> score_bytes;
		if (!index.read_file("score.ini", score_bytes) ||
				!np::load_session_score_config(host_cfg.config,
						std::string_view(reinterpret_cast<const char *>(score_bytes.data()),
								score_bytes.size()))) {
			std::fprintf(stderr, "nw-server: invalid score config 'score.ini' under '%s'\n",
					resource_root.string().c_str());
			return 1;
		}
	}
	host_cfg.socket_mode = np::SocketMode::Lan; // a real LAN socket (Socketless=1 would be in-process SP)
	host_cfg.serve_and_play = false;            // headless dedicated host: no local-player registration

	// The "Server" chat strings: retail reads GameText("Server", key) from the
	// gametext table loaded at init; this host reads a mounted gametext.bin
	// when one is present and otherwise leaves the strings empty, which is
	// retail's null lookup (the medic-call handler then no-ops).
	// [orig: Game_InitSubsystems @0x4A6CD0; Server_BroadcastMedicRequest
	// @0x5153C9]
	std::optional<std::string> medic_request_format;
	{
		std::vector<uint8_t> gametext_bytes;
		if (index.has_file("gametext.bin") &&
				index.read_file("gametext.bin", gametext_bytes)) {
			rtxt::File gametext;
			std::string gametext_error;
			if (rtxt::parse(gametext_bytes.data(), gametext_bytes.size(), gametext,
						gametext_error)) {
				medic_request_format =
						gametext.get_in_section("Server", "STRSRV_MEDREQ");
			} else {
				std::fprintf(stderr, "nw-server: mounted gametext.bin unreadable: %s\n",
						gametext_error.c_str());
			}
		}
	}

	// --- The kernel boot as DedicatedHost: terrain grounding, tables,
	//     collision, infantry .adm, the strict WAC walk (EVERY diagnostic is
	//     fatal here — running a partial script is a known wire-parity
	//     failure), and the HostOnly session bring-up at the witnessed spot
	//     inside the load. A refused boot aborts before the UDP socket opens. ---
	inmatch::ListenHostState host;
	mission::KernelBootOptions boot_options;
	boot_options.playable = false; // no synthetic loopback player; every roster row is a remote peer
	boot_options.wac_strict_diagnostics = true;
	boot_options.game_type = host_cfg.config.game_type;
	boot_options.bringup_net_session = [&] {
		inmatch::listen_host::bringup_dedicated(kernel, host, host_cfg);
	};
	std::string boot_error;
	if (!kernel.boot(boot_options, boot_error)) {
		std::fprintf(stderr, "nw-server: %s\n", boot_error.c_str());
		return 1;
	}
	std::fprintf(stderr, "nw-server: promoted '%s' (%d entities, %d brains, %d nav nodes)\n",
			opt.mission, kernel.promo.spawned, kernel.promo.brains, kernel.promo.nav_nodes);
	std::fprintf(stderr,
			"nw-server: kernel boot: terrain %s, weapon table %s, ammo table %s, "
			"%d collision instances, WAC %s from '%s'\n",
			kernel.has_terrain() ? "loaded" : "MISSING",
			kernel.weapon_defs_ok ? "loaded" : "MISSING",
			kernel.ammo_ok ? "loaded" : "MISSING",
			kernel.collision_attached,
			kernel.wac_loaded ? "loaded" : "absent (BMS-only)",
			resource_root.string().c_str());

	// Retail order: the eager WAC execution (the boot's tail) precedes
	// environment mission-start initialization and the 255 complete weather
	// ticks that settle before any client can observe phase 2.
	kernel.settle_weather_mission_start();
	// The per-join ctx feeds the bring-up left to the embedder: the S2C 0x45
	// terrain-tile source and the "Server" gametext table.
	host.host_owner.ctx.terrain_til_data = std::move(terrain_til_bytes);
	if (medic_request_format) {
		np::ServerTextTable server_text;
		server_text.medic_request_format = std::move(*medic_request_format);
		np::set_server_text(host.host_owner.ctx, std::move(server_text));
	}

	// --- Open the UDP socket. ---
	if (net::startup() != 0) {
		std::fprintf(stderr, "nw-server: winsock init failed\n");
		return 1;
	}
	uint16_t bound = 0;
	net::ScopedSocket sock(net::udp_bind(opt.port, &bound));
	if (!sock.is_valid()) {
		std::fprintf(stderr, "nw-server: bind on UDP %u failed\n", opt.port);
		net::shutdown();
		return 1;
	}

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
	DedicatedTickTarget target(kernel, host, dgram);
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
