#include <net/novaworld/server_browser.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>

namespace opennova {

namespace {

char ascii_lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string ascii_lowered(const std::string &s) {
	std::string out = s;
	for (char &c : out) c = ascii_lower(c);
	return out;
}

std::string trimmed(const std::string &s) {
	std::size_t a = 0;
	std::size_t b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
	return s.substr(a, b - a);
}

// The three-way case-insensitive compare the text columns and the name
// tiebreak use (-1 / 0 / 1).
int nocase_compare(const std::string &a, const std::string &b) {
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; ++i) {
		const char ca = ascii_lower(a[i]);
		const char cb = ascii_lower(b[i]);
		if (ca != cb) return ca < cb ? -1 : 1;
	}
	if (a.size() == b.size()) return 0;
	return a.size() < b.size() ? -1 : 1;
}

int sign_of(int v) { return (v > 0) - (v < 0); }

// The Ping column's sort key: a measured round-trip, else the sentinel that
// sorts last in either direction.
constexpr int kUnmeasured = INT_MAX;
int ping_sort_key(int ping) { return ping >= 0 ? ping : kUnmeasured; }

const std::string &text_key(const GsbServerEntry &row, BrowserColumn column) {
	switch (column) {
		case BrowserColumn::Mission: return row.mission_name;
		case BrowserColumn::Type: return row.game_type;
		default: return row.server_name;
	}
}

} // namespace

bool browser_row_is_locked(const GsbServerEntry &row) {
	return row.password == "Y" || row.locked == "Y";
}

std::string browser_ping_text(int ping) {
	if (ping == kPingPending) return "...";
	if (ping < 0) return "N/A";
	return std::to_string(ping);
}

std::array<std::string, kBrowserColumnCount> browser_row_cells(const GsbServerEntry &row,
                                                               int ping) {
	char players[32];
	std::snprintf(players, sizeof(players), "%d/%d", row.players, row.max_players);
	return {
		row.server_name,
		row.mission_name,
		row.game_type,
		players,
		browser_ping_text(ping),
		browser_row_is_locked(row) ? "Password" : "Open",
	};
}

std::vector<std::size_t> browser_filter_rows(const std::vector<GsbServerEntry> &rows,
                                             const BrowserFilter &filter) {
	const std::string text = ascii_lowered(trimmed(filter.text));
	const std::string game_type = trimmed(filter.game_type);
	std::vector<std::size_t> out;
	for (std::size_t i = 0; i < rows.size(); ++i) {
		const GsbServerEntry &row = rows[i];
		if (!text.empty()) {
			const std::string haystack =
					ascii_lowered(row.server_name + "\n" + row.mission_name + "\n" + row.mod);
			if (haystack.find(text) == std::string::npos) continue;
		}
		if (!game_type.empty() && nocase_compare(row.game_type, game_type) != 0) continue;
		if (filter.hide_full && row.players >= row.max_players) continue;
		if (filter.hide_empty && row.players <= 0) continue;
		if (filter.hide_locked && browser_row_is_locked(row)) continue;
		out.push_back(i);
	}
	return out;
}

void browser_sort_rows(std::vector<std::size_t> &order, const std::vector<GsbServerEntry> &rows,
                       const std::vector<int> &pings, BrowserColumn column, bool ascending) {
	const int direction = ascending ? 1 : -1;
	const auto ping_of = [&pings](std::size_t i) {
		return i < pings.size() ? ping_sort_key(pings[i]) : kUnmeasured;
	};
	std::stable_sort(order.begin(), order.end(), [&](std::size_t ia, std::size_t ib) {
		const GsbServerEntry &a = rows[ia];
		const GsbServerEntry &b = rows[ib];
		int cmp = 0;
		switch (column) {
			case BrowserColumn::Players:
				cmp = sign_of(a.players - b.players);
				break;
			case BrowserColumn::Ping: {
				const int va = ping_of(ia);
				const int vb = ping_of(ib);
				if (va == kUnmeasured && vb == kUnmeasured) {
					cmp = 0;
				} else if (va == kUnmeasured || vb == kUnmeasured) {
					return vb == kUnmeasured;  // unmeasured sorts last regardless of direction
				} else {
					cmp = sign_of(va - vb);
				}
				break;
			}
			case BrowserColumn::Access:
				cmp = sign_of(static_cast<int>(browser_row_is_locked(a)) -
				              static_cast<int>(browser_row_is_locked(b)));
				break;
			default:
				cmp = nocase_compare(text_key(a, column), text_key(b, column));
				break;
		}
		if (cmp == 0) cmp = nocase_compare(a.server_name, b.server_name);
		return cmp * direction < 0;
	});
}

std::vector<std::string> browser_details_lines(const GsbServerEntry &row) {
	std::vector<std::string> lines;
	const auto push = [&lines](const char *label, const std::string &value) {
		if (!trimmed(value).empty()) lines.push_back(std::string(label) + ": " + value);
	};
	push("Server", row.server_name);
	push("Message", row.msg);
	push("Map", row.mission_name);
	push("Type", row.game_type);
	char players[48];
	std::snprintf(players, sizeof(players), "Players: %d/%d", row.players, row.max_players);
	lines.emplace_back(players);
	push("Mod", row.mod);
	push("Version", row.ver1);
	push("Expansion", row.exp);
	push("Region", row.region);
	push("Country", row.country);
	push("Time of day", row.time_of_day);
	push("Time left", row.time_left);
	push("Level range", row.level_range);
	if (row.dedicated == "Y") lines.emplace_back("Dedicated server");
	if (row.pb_server == "Y") lines.emplace_back("PunkBuster on");
	if (browser_row_is_locked(row)) lines.emplace_back("Password protected");
	return lines;
}

std::string browser_row_tooltip(const GsbServerEntry &row) {
	std::string out;
	const auto push = [&out](const char *label, const std::string &value) {
		if (value.empty()) return;
		if (!out.empty()) out += "\n";
		out += label;
		out += value;
	};
	push("Mission: ", row.mission_name);
	push("Region: ", row.region);
	push("Country: ", row.country);
	push("Expansion: ", row.exp);
	if (row.ip != "0.0.0.0") push("Address: ", row.ip);
	return out;
}

} // namespace opennova
