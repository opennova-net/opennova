#pragma once

// A hosted server's settings and the files rendered from them. The owner never
// writes a file: the site sends these fields, the backend range-checks them
// (check_settings), and the supervisor renders the server's working directory
// from scratch at every start (the host file, game.cfg, admin.cfg and the
// NovaWorld credentials), in retail's own formats, read by opennova-serve
// (apps/serve/README.md). The product is a row (products/product_repository.h, ADR
// 0064): a base the service offers (one with a ready current tree) and, when
// the owner picked one, an expansion of it; the whole-number ranges are the
// base's title's (host_limits.h) and the rotation names missions the products'
// trees list; the byte caps are the fields' own (the host screen's
// GAME_NAME and SERVER_PASSWORD reads, 32 and 17 bytes with their terminators,
// docs/mnu/menu-re.md "Host dialog rules").

#include "host_limits.h"
#include "machine_driver.h"
#include "products/product_repository.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr size_t kGameNameMaxBytes = 31;     // cp1252 bytes
inline constexpr size_t kGamePasswordMaxBytes = 16;
// The longest rotation (service policy: retail's rotation regrows as it fills;
// the default rotation is every multiplayer mission of the base and its
// expansion, more than 32 for Joint Operations with Escalation).
inline constexpr size_t kMissionsMax = 128;
inline constexpr size_t kMissionNameMaxBytes = 36;  // the stem's 32 and ".bms"

// The owner's settings, as stored (hosted_servers' columns); text in UTF-8.
struct HostedSettings {
	// The product it runs (products.id, a game: its title opennova-serve
	// speaks with /game, and its lobby, the games row it lists in) and the expansion of
	// it, when the owner picked one (/exp with its code).
	int64_t product_id = 0;
	std::optional<int64_t> expansion_id;
	std::string game_name;                // GameName
	int max_players = 32;                 // MaxPlayers
	std::string game_password;            // MPHostGamePassword ("" for none)
	int time_limit = 30;                  // Time_Limit
	int kill_limit = 50;                  // KillLimit
	int max_score = 5;                    // MaxScore
	int respawn = 5;                      // Respawn
	int start_delay = 0;                  // Delay
	bool team_ff = false;                 // TeamFF
	// The rotation in play order: the first map is the one the server starts on.
	std::vector<std::string> missions;
	// Also list the server on NovaLogic's NovaWorld (opennova-serve's second listing,
	// --also-master-host), at each start while the service offers it (NovaLogicListing).
	bool list_on_novalogic = false;
};

// Why check_settings refused: the field (its JSON name) and the reason.
struct SettingsRefusal {
	std::string field;
	std::string reason;
};

// Every field against its rule; the first refusal, or nullopt for settings the
// supervisor may render. The product is one of `offered` (offered_products:
// the bases with a ready current tree), whose title's limits the whole numbers
// take and whose offered expansions the expansion is one of; the rotation
// names 1..kMissionsMax missions, each among the base's tree's missions plus
// the expansion's (by file name, without case). A name is 1..kGameNameMaxBytes
// bytes once in cp1252 (the host's code page), not blank, with no control byte
// (C1 included) and no double quote (the host file quotes it and has no
// escape); a password is printable ASCII with no space or double quote; a
// mission is a .bms file name of letters, digits, '_' and '-'.
std::optional<SettingsRefusal> check_settings(
		const HostedSettings &settings, const std::vector<OfferedProduct> &offered);

// A rotation entry's file name: a .bms name of letters, digits, '_' and '-',
// at most kMissionNameMaxBytes bytes (check_settings' rule, and the console's
// MISSION ADD).
bool is_mission_file_name(std::string_view name);

// The missions column: one name per line.
std::string join_missions(const std::vector<std::string> &missions);
std::vector<std::string> split_missions(const std::string &column);

// The working directory's file names.
inline constexpr const char *kHostFileName = "server.host";
inline constexpr const char *kCredentialsFileName = "credentials.txt";
// The second listing's account (--also-credentials), the project's NovaLogic one.
inline constexpr const char *kNovaLogicCredentialsFileName = "novalogic.txt";
inline constexpr const char *kAdminCfgFileName = "admin.cfg";
inline constexpr const char *kGameCfgFileName = "game.cfg";
// The remote-admin user admin.cfg grants the service, with every verb's right
// (GET..GOTO, 0x7F).
inline constexpr const char *kAdminUser = "svc";
inline constexpr uint32_t kAdminRights = 0x7F;
// The admin login's password: the 31 characters the console keeps of the field
// (engine/net/admin/admin_protocol.h ADMIN_LOGIN_FIELD_MAX_CHARS).
inline constexpr size_t kAdminPasswordChars = 31;
// The service account's NovaWorld password.
inline constexpr size_t kServicePasswordChars = 32;

// One file of the server's working directory: a bare name and its bytes.
// `secret` marks one holding a credential (owner-only on the machine, never
// logged).
struct LaunchFile {
	std::string name;
	std::string content;
	bool secret = false;
};

// Everything a slot needs to run hosted server `server_id`, rendered from
// scratch at every start: the working directory's files and opennova-serve's
// arguments, relative to that directory. The supervisor hands it to the
// machine's agent as the container's files and arguments (hosted/protocol/agent_protocol.h
// DesiredContainer, with the trees the server is pinned to beside them, which
// the machine binds as the install the arguments' /game and /exp speak over).
struct LaunchSpec {
	int64_t server_id = 0;
	std::vector<LaunchFile> files;
	std::vector<std::string> serve_args;
};

// The one lobby of NovaLogic's NovaWorld a hosted server may also list in: Joint Operations'
// (its gate tag jop:cus2 names it, backend/seed/0001_games.sql). NovaLogic's DFX2 NovaWorld is
// down (its login answers NWNODB01, ADR 0055), and our own game, OP: Acid Gambit, speaks a gate
// tag only OpenNova's service has.
inline constexpr const char *kNovaLogicListedGame = "jop_2_consumer";
// Whether a server on `product` may also list on NovaLogic's NovaWorld (check_settings refuses
// list_on_novalogic for one that may not).
inline bool novalogic_lists(const OfferedProduct &product) {
	return product.lobby == kNovaLogicListedGame;
}

// The value the vault's NovaLogic account fields hold until the real account goes in
// (deploy/env/runtime.env.tpl: an optional field must exist with a value to sync). An account
// holding it is unset, so a deployment whose switch is on never logs in to NovaLogic's live
// service with a placeholder.
inline constexpr const char *kNovaLogicAccountPlaceholder = "REPLACE_ME";

// NovaLogic's NovaWorld as the second listing of the servers whose owners asked
// (HostedSettings::list_on_novalogic): its gate and the project's account there, which
// NovaLogic's service asks for before it hands out a HOSTKEY. Unoffered (an empty gate, or an
// account unset or the placeholder: the service's ONNET_NOVALOGIC_LISTING off, or its account
// not filled in) a server lists on this service alone, whatever its owner asked. Service
// policy, not a port.
struct NovaLogicListing {
	std::string gate_host;
	uint16_t gate_port = 0;
	std::string username;
	std::string password;
	bool account_set() const {
		const auto set = [](const std::string &v) {
			return !v.empty() && v != kNovaLogicAccountPlaceholder;
		};
		return set(username) && set(password);
	}
	bool offered() const { return !gate_host.empty() && gate_port != 0 && account_set(); }
};

// What a start renders, beside the settings.
struct RenderInputs {
	int64_t server_id = 0;
	SlotPlan plan; // the slot's ports and the address admin.cfg admits
	std::string service_username; // NOVAWORLD_USER
	std::string service_password; // NOVAWORLD_PASS
	std::string admin_password;   // admin.cfg's kAdminUser
	std::string master_host;      // --master-host: where the server reaches NovaWorld
	uint16_t master_gate_port = 0;
	NovaLogicListing novalogic;   // the second listing, for a server whose owner asked
};

// The host file, in the host's cp1252 bytes and CR LF lines (the tokenizer
// splits on the pair alone): the settings' keys, Replay 1 (so the rotation
// starts over and the session never ends by itself) and one Mission line per
// map, the first map's line last (the last line is the starting map).
std::string render_host_file(const HostedSettings &settings, int64_t server_id);
// game.cfg: the defaults (Config_SetDefaults), on the NovaWorld network type,
// the NovaWorld port range pinned to `plan.game_port` (the slot's: NovaWorld
// lists a server at its datagrams' source port, so a stock client joins each
// slot of a machine on its own port), and the remote admin on
// `plan.admin_port`; written by the format's own writer.
std::string render_game_cfg(const SlotPlan &plan);
// admin.cfg: kAdminUser with `admin_password` and kAdminRights, and one
// ip_restrict line admitting `plan.admin_allowed_ip`.
std::string render_admin_cfg(const SlotPlan &plan, const std::string &admin_password);
// The --credentials file: NOVAWORLD_USER and NOVAWORLD_PASS.
std::string render_credentials(const std::string &username, const std::string &password);

// opennova-serve's arguments for a server whose product speaks `title` (its
// base row's gameprofile code) with the expansion `expansion_code` ("" for
// none): /HOST, --credentials, --master-host, --master-gate-port, the status
// page off with the tick-health and exit files (hosted/protocol/slots.h), /game with the
// title, and /exp with the code; for a server whose owner asked to be listed on
// NovaLogic's NovaWorld, the second listing's state file (--second-listing-file,
// off while the service offers none), and while `novalogic` is offered the second
// listing too (--also-master-host, --also-master-gate-port, --also-credentials with
// kNovaLogicCredentialsFileName, and --allow-public, which its gate under
// novaworld.net needs). They name the working directory's files and carry no secret.
std::vector<std::string> render_serve_args(const HostedSettings &settings, std::string_view title,
		std::string_view expansion_code, const std::string &master_host, uint16_t master_gate_port,
		const NovaLogicListing &novalogic);
// Whether a start of `settings` lists on NovaLogic's NovaWorld: its owner asked and the
// service offers it.
bool lists_on_novalogic(const HostedSettings &settings, const NovaLogicListing &novalogic);
// The whole start of a server speaking `title` with `expansion_code`: the four
// files (every one but game.cfg secret: the host file carries the game
// password), the NovaLogic account's for a server that lists there
// (lists_on_novalogic), and render_serve_args.
LaunchSpec render_launch(const HostedSettings &settings, std::string_view title,
		std::string_view expansion_code, const RenderInputs &inputs);

} // namespace opennova::novaworld_server::hosted
