#pragma once

#include <net/novaworld/db/sqlite.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

// The games catalogue: one `games` row per lobby (migration 0013, the rows the
// seed writes). A client's gate tag selects its row, and the row decides the
// LobbyName and the start page the gate answers, the .gsb list its browser
// fetches, the per-game values the NW*.dll pages fill in, and the game its
// listings land under (active_hosts.game holds the listing's LobbyName).
//
// The gate and HTTP listeners each load a GameCatalog once, in start(), so the
// gate's receive thread answers a probe without touching SQLite; a change to
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
	// row shares JO's.
	std::string page_prefix() const;
	// One of the row's pages by its suffix: page("main.htm") is
	// "jop_2_main.htm" for JO's row.
	std::string page(std::string_view suffix) const { return page_prefix() + std::string(suffix); }
};

// Every games row, in slug order. Throws db::SqliteError.
std::vector<GameRow> list_games(opennova::db::Database &db);

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

	// The table as it stands. Throws db::SqliteError.
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
	// left is the game; several (JO and OpenNova share pfid 28 and JO's pages,
	// so only the tag tells them apart) give the default game when it is
	// among them, else the first. Null only when the catalog is empty.
	const GameRow *for_client(const ClientHints &hints) const;

private:
	std::vector<GameRow> rows_;
};

} // namespace catalog

} // namespace opennova::novaworld_server
