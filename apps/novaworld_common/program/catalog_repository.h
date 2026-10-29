#pragma once

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

// The games catalogue: one `games` row per lobby (the system lobbies
// backend/migrations/0003_project_lobbies.sql writes, OP: Acid Gambit's given its own
// pages and pfid by 0004_acidgambit_lobby_pages.sql, and those players' games create,
// create_lobby). A client's gate tag selects its row, and the row decides the
// LobbyName and the start page the gate answers, the .gsb list its browser
// fetches, the per-game values the NW*.dll pages fill in, and the game its
// listings land under (active_hosts.game holds the listing's LobbyName).
//
// The gate and HTTP listeners each load a GameCatalog once, in start(), so the
// gate's receive thread answers a probe without touching the database; a change to
// the table takes a restart. /api/lobbies and /api/games read the table live.
// Handlers never write SQL for it directly: this is the one place to look.
namespace catalog {

struct GameRow {
	int64_t     id = 0;
	std::string slug;
	std::string display_name;
	std::string lobby_name;
	std::string gate_tag;
	// The engine's game profile code the row's clients speak ('jo', 'dfx2';
	// engine/base/gameprofile). The OpenNova row speaks JO's.
	std::string title;
	std::string start_page; // the gate's startupurl `url=` page
	std::string host_page;
	std::string gsb_path; // e.g. "jop_2.gsb?a=1"
	// The main page's @JOINLAN_URL@ global: the NWJoin link a LAN-list join opens
	// with the discovered server's GSID in place of @GSID@ (DFX2's); empty when NULL
	// (no page of the row's sets the global).
	std::string join_lan_url;
	std::string ver1;
	std::string ver2;
	// The expansion bits a listing that reports none requires, and that an
	// account with no access row of its own owns.
	std::string exp_bits;
	std::string pfid; // the product family id (NWPF); empty when NULL
	// The gate's startupurl after "nwprepare.dll?", as the title's own gate
	// answers it (DFX2's carries pfid=38; JO's names none).
	std::string startup_query;
	// The gate's GLSVSS leg; an empty request sends none.
	std::string glsvss_request;
	int64_t glsvss_rims = 0;
	int64_t glsvss_agrms = 0;

	// The .gsb route the row's browser list is served at: "/" and gsb_path
	// up to its query ("/jop_2.gsb").
	std::string gsb_route() const;
	// The prefix the row's NW*.dll pages share, its start page's name up to
	// and including the last '_' ("jop_2_" of "jop_2_start.htm"); the OpenNova
	// row's is "acidgambit_0_", which the lobbies copied from it share.
	std::string page_prefix() const;
	// One of the row's pages by its suffix: page("main.htm") is
	// "jop_2_main.htm" for JO's row.
	std::string page(std::string_view suffix) const { return page_prefix() + std::string(suffix); }
};

// Every games row, in slug order. Throws db::Error.
std::vector<GameRow> list_games(opennova::db::Database &db);

// Publicly discoverable lobbies, plus those selected by the caller's own games.
// Internal routing always uses list_games: an unpublished project's lobby is reachable.
std::vector<GameRow> available_games(
		opennova::db::Database &db, std::optional<int64_t> caller = std::nullopt);
// Who selects a game's lobby: the account acting (none for the Bearer ADMIN_API_TOKEN) and
// whether it acts as an admin (the token, or an admin-role session).
struct LobbyChooser {
	std::optional<int64_t> account;
	bool admin = false;
};
// The lobbies `chooser` may select for a game, in slug order: an admin's, every games row (a
// system lobby a migration added, one every game has left); a player's, the available_games rows
// of its account but those a retail product names (Joint Operations' and Delta Force Xtreme 2's,
// the lobbies the original Jointops.exe and dfx2.exe browse).
std::vector<GameRow> selectable_lobbies(opennova::db::Database &db, const LobbyChooser &chooser);
// Whether a game speaking `title` may sit in a lobby whose title is `lobby_title`: the two speak one
// protocol (net/protocol/protocol.h title_protocol: OP: Acid Gambit's is Joint Operations'), until
// lobbies are keyed by protocol.
bool speaks_lobby_protocol(std::string_view title, std::string_view lobby_title);
// Whether `chooser` may select the lobby `slug` for a game speaking `title` (speaks_lobby_protocol).
bool lobby_available(opennova::db::Database &db, std::string_view slug, std::string_view title,
		const LobbyChooser &chooser);
// A fresh lobby with an immutable identity allocated from games.id, its title, pages and version
// pair its protocol's system lobby's (Acid Gambit's for Joint Operations' protocol, DFX2's for
// DFX2's). Empty if the title speaks neither.
std::string create_lobby(opennova::db::Database &db, std::string_view name, std::string_view title);
// The server-generated gt.ssc bytes, shared by project metadata and release builds.
bool gate_tag_file(const GameRow &game, std::string &bytes, std::string &why);

// A gate tag with no row of its own that answers as another tag's game: the
// Joint Operations demo's `jopd:cus4` (jodemo, docs/net/novaworld-net-re.md
// section 1) plays in JO's lobby.
struct GateTagAlias {
	std::string_view tag;
	std::string_view answers_as;
};
inline constexpr GateTagAlias kGateTagAliases[] = {
	{ "jopd:cus4", "jop:cus2" },
};

// The game of a request that names none, and the pick among games a request's
// hints cannot tell apart: JO's lobby, the service's first title (every
// request was JO's before the table chose).
inline constexpr std::string_view kDefaultGameSlug = "jop_2_consumer";

// What a retail HTTP request says about its client's game.
struct ClientHints {
	std::string_view gate_tag; // the gt query /nwprepare.dll carries, else the GT cookie
	std::string_view pfid; // a pfid field, else the NWPF cookie
	std::string_view page; // a page the request names (url=, success=, IN=, ...)
};

// One load of the table: the rows and the lookups over them, never written
// after it is built, so threads read it without a lock.
class GameCatalog {
public:
	GameCatalog() = default;
	explicit GameCatalog(std::vector<GameRow> rows) : rows_(std::move(rows)) {}

	// The table as it stands. Throws db::Error.
	static GameCatalog load(opennova::db::Database &db) { return GameCatalog(list_games(db)); }

	const std::vector<GameRow> &games() const { return rows_; }
	bool empty() const { return rows_.empty(); }

	// The row a gate probe's tag selects: the row with that gate_tag, else the
	// row its alias (kGateTagAliases) answers as; null for any other tag, which
	// the gate refuses.
	const GameRow *by_gate_tag(std::string_view tag) const;
	const GameRow *by_slug(std::string_view slug) const;
	const GameRow *by_lobby_name(std::string_view lobby_name) const;
	// kDefaultGameSlug's row, else the first row; null only when empty.
	const GameRow *default_game() const;

	// The game a retail request's client plays. The gate tag decides
	// (by_gate_tag); without a tag a row answers to, the pfid narrows the rows
	// to those of that product family and then the page to those whose page
	// prefix it starts with (a hint no row matches narrows nothing). One row
	// left is the game; several (the OpenNova row and the lobbies copied from
	// it share pfid 100 and its pages, so only the tag tells them apart) give
	// the default game when it is among them, else the first. Null only when
	// the catalog is empty.
	const GameRow *for_client(const ClientHints &hints) const;

private:
	std::vector<GameRow> rows_;
};

} // namespace catalog

} // namespace opennova::novaworld_server
