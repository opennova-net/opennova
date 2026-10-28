#include "catalog_repository.h"

#include <net/novacrypto/pubcrypto.h>
#include <net/title/title_identity.h>

namespace opennova::novaworld_server::catalog {

namespace {

GameRow row_to_game(const opennova::db::Row &r) {
	GameRow g;
	g.id           = r.as_int(0).value_or(0);
	g.slug         = r.as_text(1).value_or("");
	g.display_name = r.as_text(2).value_or("");
	g.lobby_name   = r.as_text(3).value_or("");
	g.gate_tag     = r.as_text(4).value_or("");
	g.title        = r.as_text(5).value_or("");
	g.start_page   = r.as_text(6).value_or("");
	g.host_page    = r.as_text(7).value_or("");
	g.gsb_path     = r.as_text(8).value_or("");
	g.ver1         = r.as_text(9).value_or("");
	g.ver2         = r.as_text(10).value_or("");
	g.exp_bits     = r.as_text(11).value_or("");
	g.pfid         = r.as_text(12).value_or("");
	g.startup_query  = r.as_text(13).value_or("");
	g.glsvss_request = r.as_text(14).value_or("");
	g.glsvss_rims    = r.as_int(15).value_or(0);
	g.glsvss_agrms   = r.as_int(16).value_or(0);
	g.join_lan_url   = r.as_text(17).value_or("");
	return g;
}

} // namespace

std::string GameRow::gsb_route() const {
	return "/" + gsb_path.substr(0, gsb_path.find('?'));
}

std::string GameRow::page_prefix() const {
	const auto last = start_page.rfind('_');
	return last == std::string::npos ? std::string() : start_page.substr(0, last + 1);
}

std::vector<GameRow> list_games(opennova::db::Database &db) {
	auto rows = db.query(
			"SELECT id, slug, display_name, lobby_name, gate_tag, title, start_page, host_page, "
			"gsb_path, ver1, ver2, exp_bits, pfid, startup_query, glsvss_request, glsvss_rims, "
			"glsvss_agrms, join_lan_url "
			"FROM games ORDER BY slug;");
	std::vector<GameRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_game(r));
	return out;
}

std::vector<GameRow> available_games(db::Database &db, std::optional<int64_t> caller) {
	const auto visible = db.query("SELECT DISTINCT lobby FROM products WHERE kind = 'game' AND "
								  "(owner_id IS NULL OR owner_id = $1 OR "
								  "(visibility = 'public' AND published_release_id IS NOT NULL));",
			{ db::int_or_null(caller) });
	std::vector<GameRow> out;
	for (GameRow &game : list_games(db))
		for (const auto &row : visible)
			if (row.as_text(0) == game.slug) {
				out.push_back(std::move(game));
				break;
			}
	return out;
}

std::vector<GameRow> selectable_lobbies(db::Database &db, const LobbyChooser &chooser) {
	if (chooser.admin)
		return list_games(db);
	const auto retail =
			db.query("SELECT DISTINCT lobby FROM products WHERE retail AND lobby <> '';");
	std::vector<GameRow> out;
	for (GameRow &game : available_games(db, chooser.account)) {
		bool taken = false;
		for (const auto &row : retail)
			taken = taken || row.as_text(0) == game.slug;
		if (!taken)
			out.push_back(std::move(game));
	}
	return out;
}

bool lobby_available(db::Database &db, std::string_view slug, std::string_view title,
		const LobbyChooser &chooser) {
	for (const GameRow &game : selectable_lobbies(db, chooser))
		if (game.slug == slug && game.title == title)
			return true;
	return false;
}

std::string create_lobby(db::Database &db, std::string_view name, std::string_view title) {
	// The template supplies protocol values only. The row id supplies the identity, so a
	// project rename or another project's choice can never rename or take over a lobby.
	const std::string from = title == "jo" ? "acidgambit_0_consumer"
			: title == "dfx2"			   ? "dfx2_consumer"
										   : "";
	if (from.empty() || name.empty())
		return {};
	const auto rows = db.query(
			"WITH identity AS (SELECT nextval(pg_get_serial_sequence('games', 'id')) AS id) "
			"INSERT INTO games (id, slug, display_name, lobby_name, gate_tag, title, start_page, "
			"host_page, gsb_path, join_lan_url, ver1, ver2, exp_bits, pfid, startup_query, "
			"glsvss_request, glsvss_rims, glsvss_agrms) OVERRIDING SYSTEM VALUE "
			"SELECT i.id, 'on_' || i.id, $1, 'on_' || i.id, 'on:' || i.id, t.title, "
			"t.start_page, t.host_page, 'on_' || i.id || '.gsb?a=1', t.join_lan_url, t.ver1, "
			"t.ver2, t.exp_bits, t.pfid, t.startup_query, t.glsvss_request, t.glsvss_rims, "
			"t.glsvss_agrms FROM games t CROSS JOIN identity i WHERE t.slug = $2 RETURNING slug;",
			{ db::BindValue(std::string(name)), db::BindValue(from) });
	return rows.empty() ? std::string() : rows.front().as_text(0).value_or("");
}

bool gate_tag_file(const GameRow &game, std::string &bytes, std::string &why) {
	bytes.clear();
	const TitleIdentity *title = resolve_title_identity(game.title, why);
	if (!title || !title->gate_tag_keys || game.gate_tag.empty() ||
			game.gate_tag.size() > kGateTagChars || game.gate_tag.find('\0') != std::string::npos) {
		why = "the lobby has no valid gate tag and title key chain";
		return false;
	}
	bytes = encode_key_chain(
			std::vector<uint8_t>(game.gate_tag.begin(), game.gate_tag.end()), title->gate_tag_keys);
	return true;
}

const GameRow *GameCatalog::by_gate_tag(std::string_view tag) const {
	for (const GameRow &g : rows_) {
		if (g.gate_tag == tag)
			return &g;
	}
	for (const GateTagAlias &alias : kGateTagAliases) {
		if (alias.tag != tag)
			continue;
		for (const GameRow &g : rows_) {
			if (g.gate_tag == alias.answers_as)
				return &g;
		}
	}
	return nullptr;
}

const GameRow *GameCatalog::by_slug(std::string_view slug) const {
	for (const GameRow &g : rows_) {
		if (g.slug == slug)
			return &g;
	}
	return nullptr;
}

const GameRow *GameCatalog::by_lobby_name(std::string_view lobby_name) const {
	for (const GameRow &g : rows_) {
		if (g.lobby_name == lobby_name)
			return &g;
	}
	return nullptr;
}

const GameRow *GameCatalog::default_game() const {
	if (const GameRow *g = by_slug(kDefaultGameSlug))
		return g;
	return rows_.empty() ? nullptr : &rows_.front();
}

const GameRow *GameCatalog::for_client(const ClientHints &hints) const {
	if (!hints.gate_tag.empty()) {
		if (const GameRow *g = by_gate_tag(hints.gate_tag))
			return g;
	}
	std::vector<const GameRow *> left;
	left.reserve(rows_.size());
	for (const GameRow &g : rows_)
		left.push_back(&g);
	// Keeps the rows `match` takes, unless it takes none.
	const auto narrow = [&left](auto match) {
		std::vector<const GameRow *> kept;
		for (const GameRow *g : left) {
			if (match(*g))
				kept.push_back(g);
		}
		if (!kept.empty())
			left = std::move(kept);
	};
	if (!hints.pfid.empty()) {
		narrow([&hints](const GameRow &g) { return g.pfid == hints.pfid; });
	}
	if (!hints.page.empty()) {
		narrow([&hints](const GameRow &g) {
			const std::string prefix = g.page_prefix();
			return !prefix.empty() && hints.page.substr(0, prefix.size()) == prefix;
		});
	}
	if (left.size() == 1)
		return left.front();
	const GameRow *fallback = default_game();
	for (const GameRow *g : left) {
		if (g == fallback)
			return g;
	}
	return left.empty() ? nullptr : left.front();
}

} // namespace opennova::novaworld_server::catalog
