#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::gamecfg {
enum class ConfigTable : uint8_t; // formats/gamecfg/game_cfg.h
} // namespace opennova::gamecfg

namespace opennova {

// The container format a NEW archive of any title is written in, as a PffFormat value (0 =
// PFF_FORMAT_PFF3): PFF3, ADR 0008's default for a newly created archive, which every JO-family
// title opens [orig: PFF_Open @ 0x7682e0]. One value for every title: a title that ships another
// flavour makes it a Title field.
inline constexpr int kNewArchivePffFormat = 0;

// A title (CONTEXT.md "Title"): the retail game a game runs as, by its short code (the `/game
// <code>` launch flag, a project's target game). Its rows live in games/<code>/<code>_title.cpp,
// which games::install() hands to install_titles; engine/ holds the type and the lookups alone.
// A field no binary of the title has been read for is null, never another title's value.
struct Title {
	// The short launch-flag token, lowercase ("jo", "dfx2", "acidgambit").
	const char *code = nullptr;
	// The name a player and an artist read ("Joint Operations").
	const char *display_name = nullptr;
	// The title text the executable copies into g_TitleText: the _connectlog.txt header's TITLE
	// line and the VERSION command's title word.
	const char *title_text = nullptr;
	// VfsScrPolicy (base/vfs/vfs_decode.h): how the title's SCR payloads key.
	int scr_policy = 0;
	// The program the title's install ships beside its archives, which a player starts (and the
	// editor's Play in the game install).
	const char *executable = nullptr;
	// The command file a game folder's stock UPDATE.WIZ names for the title (its CMD), which
	// the launch reads for the folder's product (ADR 0067 d2).
	const char *command_file = nullptr;
	// The game.cfg table the title's executable walks (gamecfg::config_var_rows).
	gamecfg::ConfigTable config_table{};
	// DFX2-8: the player caps the title's executable compiles in. `max_players` is the most a
	// session takes: the config default's clamp and the NovaWorld host leg's (64, dfx2.exe's 250),
	// one more with a dedicated host's own slot (the session create's, the published and the slot
	// table's ceiling: 65, 251); `max_players_host_screen` the host screen's MAX_PLAYERS cap (64,
	// dfx2.exe's 100). On the Title, not the title's rules (runtime/world/title_rules.h) nor its
	// Protocol: they are rules, not wire, and the host leg (net/novaworld/connect_or_host.h) and the
	// service's hosted limits read them beside the session's caps (inmatch title_player_caps). The
	// game.cfg rows and ceilings keyed by config_table stay formats/gamecfg's. 0 unwitnessed.
	int32_t max_players = 0;
	int32_t max_players_host_screen = 0;
	// The roles of the boot's required resources (base/gameprofile/required_resources.h) the
	// title's stock data never ships and its program still reads unchecked: each is optional for
	// the title. Null-terminated; null for none.
	const char *const *absent_resource_roles = nullptr;
};

// Fills the registry with `titles` (each outliving the program) and names its default: the title
// a launch, a project or a session that names none runs as. A second call replaces the first.
void install_titles(const Title *const *titles, size_t count, const Title *default_title);

// Every lookup below aborts on an EMPTY registry, naming the invariant: a program that reaches
// them calls games::install() at its start (games/games.h).

// The title whose code is `code`, compared case-insensitively; null for an unknown one.
const Title *title_by_code(std::string_view code);
// The title a launch that names none runs as.
const Title &default_title();
// The installed titles, in install order; title_at is null past the end.
size_t title_count();
const Title *title_at(size_t index);
// The title whose command_file is `command_file` (compared case-insensitively, as the updater
// opens the file); null for none.
const Title *title_by_command_file(std::string_view command_file);

// The SCR policy (VfsScrPolicy) for a title `code`: VFS_SCR_VERSION_DETECT for a null or unknown
// code, so a missing or bad `/game` value keys as JO's does. The engine's one game-to-policy seam.
int title_scr_policy_for_code(const char *code);

} // namespace opennova
