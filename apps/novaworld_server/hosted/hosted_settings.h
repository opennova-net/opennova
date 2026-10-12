#pragma once

// A hosted server's settings and the files rendered from them. The owner never
// writes a file: the site sends these fields, the backend range-checks them
// (check_settings), and the supervisor renders the server's working directory
// from scratch at every start (the host file, game.cfg, admin.cfg and the
// NovaWorld credentials), in retail's own formats, read by opennova-serve
// (apps/serve/README.md). The ranges are service policy, not a witnessed
// limit (the host screen's spin ranges are unread); the byte caps are the
// fields' own (the host screen's GAME_NAME and SERVER_PASSWORD reads, 32 and
// 17 bytes with their terminators, docs/mnu/menu-re.md "Host dialog rules").

#include "machine_provider.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr size_t kGameNameMaxBytes = 31;     // cp1252 bytes
inline constexpr size_t kGamePasswordMaxBytes = 16;
inline constexpr int kMaxPlayersMin = 2;
inline constexpr int kMaxPlayersMax = 64;           // the host screen's MAX_PLAYERS cap
inline constexpr int kTimeLimitMin = 1;
inline constexpr int kTimeLimitMax = 300;
inline constexpr int kKillLimitMin = 1;
inline constexpr int kKillLimitMax = 500;           // 500: the host screen's "no limit"
inline constexpr int kMaxScoreMin = 1;
inline constexpr int kMaxScoreMax = 500;            // 500: likewise
inline constexpr int kRespawnMin = 0;
inline constexpr int kRespawnMax = 300;
inline constexpr int kStartDelayMin = 0;
inline constexpr int kStartDelayMax = 300;
inline constexpr size_t kMissionsMax = 32;
inline constexpr size_t kMissionNameMaxBytes = 36;  // the stem's 32 and ".bms"
// The one expansion a hosted server may mount besides base Joint Operations
// (Escalation; the machine's data holds expansion/jox01/). Joiners need the same.
inline constexpr const char *kExpansionEscalation = "jox01";

// The owner's settings, as stored (hosted_servers' columns); text in UTF-8.
struct HostedSettings {
	std::string game_name;                // GameName
	int max_players = 32;                 // MaxPlayers
	std::string game_password;            // MPHostGamePassword ("" for none)
	int time_limit = 30;                  // Time_Limit
	int kill_limit = 50;                  // KillLimit
	int max_score = 5;                    // MaxScore
	int respawn = 5;                      // Respawn
	int start_delay = 0;                  // Delay
	bool team_ff = false;                 // TeamFF
	std::string expansion;                // "" or kExpansionEscalation (/exp)
	// The rotation in play order: the first map is the one the server starts on.
	std::vector<std::string> missions;
};

// Why check_settings refused: the field (its JSON name) and the reason.
struct SettingsRefusal {
	std::string field;
	std::string reason;
};

// Every field against its rule; the first refusal, or nullopt for settings the
// supervisor may render. A name is 1..kGameNameMaxBytes bytes once in cp1252
// (the host's code page), not blank, with no control byte (C1 included) and no
// double quote (the host file quotes it and has no escape); a password is
// printable ASCII with no space or double quote; a mission is a .bms file name
// of letters, digits, '_' and '-'.
std::optional<SettingsRefusal> check_settings(const HostedSettings &settings);

// The missions column: one name per line.
std::string join_missions(const std::vector<std::string> &missions);
std::vector<std::string> split_missions(const std::string &column);

// The working directory's file names.
inline constexpr const char *kHostFileName = "server.host";
inline constexpr const char *kCredentialsFileName = "credentials.txt";
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

// What a start renders, beside the settings.
struct RenderInputs {
	int64_t server_id = 0;
	MachinePlan plan;
	std::string service_username; // NOVAWORLD_USER
	std::string service_password; // NOVAWORLD_PASS
	std::string admin_password;   // admin.cfg's kAdminUser
	std::string master_host;      // --master-host: where the server reaches NovaWorld
	uint16_t master_gate_port = 0;
};

// The host file, in the host's cp1252 bytes and CR LF lines (the tokenizer
// splits on the pair alone): the settings' keys, Replay 1 (so the rotation
// starts over and the session never ends by itself) and one Mission line per
// map, the first map's line last (the last line is the starting map).
std::string render_host_file(const HostedSettings &settings, int64_t server_id);
// game.cfg: the defaults (Config_SetDefaults), on the NovaWorld network type,
// the NovaWorld port range pinned to `plan.nw_port`, and the remote admin on
// `plan.admin_port`; written by the format's own writer.
std::string render_game_cfg(const MachinePlan &plan);
// admin.cfg: kAdminUser with `admin_password` and kAdminRights, and one
// ip_restrict line admitting `plan.admin_allowed_ip`.
std::string render_admin_cfg(const MachinePlan &plan, const std::string &admin_password);
// The --credentials file: NOVAWORLD_USER and NOVAWORLD_PASS.
std::string render_credentials(const std::string &username, const std::string &password);

// The whole start: the four files (every one but game.cfg secret: the host file
// carries the game password) and opennova-serve's arguments (/HOST, --credentials,
// --master-host, --master-gate-port, and /exp for an expansion).
LaunchSpec render_launch(const HostedSettings &settings, const RenderInputs &inputs);

// `length` letters and digits from the OS CSPRNG.
std::string random_secret(size_t length);

} // namespace opennova::novaworld_server::hosted
