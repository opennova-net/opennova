#include "network/novaworld_server_browser.h"

#include "util/string_convert.h"

#include <vector>

namespace godot {

namespace {

std::vector<opennova::GsbServerEntry> entries_of(const TypedArray<NovaWorldServerRow> &p_rows) {
	std::vector<opennova::GsbServerEntry> out;
	out.reserve(static_cast<size_t>(p_rows.size()));
	for (int i = 0; i < p_rows.size(); ++i) {
		const Ref<NovaWorldServerRow> row = p_rows[i];
		out.push_back(row.is_valid() ? row->value() : opennova::GsbServerEntry());
	}
	return out;
}

TypedArray<NovaWorldServerRow> pick(const TypedArray<NovaWorldServerRow> &p_rows,
		const std::vector<std::size_t> &p_order) {
	TypedArray<NovaWorldServerRow> out;
	for (std::size_t i : p_order) out.push_back(p_rows[static_cast<int>(i)]);
	return out;
}

} // namespace

bool NovaWorldServerBrowser::row_is_locked(const Ref<NovaWorldServerRow> &p_row) {
	return p_row.is_valid() && opennova::browser_row_is_locked(p_row->value());
}

String NovaWorldServerBrowser::ping_text(int p_ping) {
	return String(opennova::browser_ping_text(p_ping).c_str());
}

PackedStringArray NovaWorldServerBrowser::row_cells(const Ref<NovaWorldServerRow> &p_row, int p_ping) {
	PackedStringArray out;
	if (!p_row.is_valid()) return out;
	for (const std::string &cell : opennova::browser_row_cells(p_row->value(), p_ping)) {
		out.push_back(opennova::cp1252_to_gd(cell));
	}
	return out;
}

TypedArray<NovaWorldServerRow> NovaWorldServerBrowser::filter_rows(
		const TypedArray<NovaWorldServerRow> &p_rows, const String &p_text,
		const String &p_game_type, bool p_hide_full, bool p_hide_empty, bool p_hide_locked) {
	opennova::BrowserFilter filter;
	filter.text = opennova::to_std(p_text);
	filter.game_type = opennova::to_std(p_game_type);
	filter.hide_full = p_hide_full;
	filter.hide_empty = p_hide_empty;
	filter.hide_locked = p_hide_locked;
	return pick(p_rows, opennova::browser_filter_rows(entries_of(p_rows), filter));
}

TypedArray<NovaWorldServerRow> NovaWorldServerBrowser::sort_rows(
		const TypedArray<NovaWorldServerRow> &p_rows, Column p_column, bool p_ascending,
		const Dictionary &p_pings) {
	const std::vector<opennova::GsbServerEntry> rows = entries_of(p_rows);
	std::vector<int> pings(rows.size(), opennova::kPingPending);
	std::vector<std::size_t> order(rows.size());
	for (std::size_t i = 0; i < rows.size(); ++i) {
		order[i] = i;
		const Variant ping = p_pings.get(static_cast<int64_t>(rows[i].rid), Variant());
		if (ping.get_type() != Variant::NIL) pings[i] = static_cast<int>(ping);
	}
	opennova::browser_sort_rows(order, rows, pings,
			static_cast<opennova::BrowserColumn>(p_column), p_ascending);
	return pick(p_rows, order);
}

PackedStringArray NovaWorldServerBrowser::details_lines(const Ref<NovaWorldServerRow> &p_row) {
	PackedStringArray out;
	if (!p_row.is_valid()) return out;
	for (const std::string &line : opennova::browser_details_lines(p_row->value())) {
		out.push_back(opennova::cp1252_to_gd(line));
	}
	return out;
}

String NovaWorldServerBrowser::row_tooltip(const Ref<NovaWorldServerRow> &p_row) {
	if (!p_row.is_valid()) return String();
	return opennova::cp1252_to_gd(opennova::browser_row_tooltip(p_row->value()));
}

void NovaWorldServerBrowser::_bind_methods() {
	BIND_ENUM_CONSTANT(COLUMN_NAME);
	BIND_ENUM_CONSTANT(COLUMN_MISSION);
	BIND_ENUM_CONSTANT(COLUMN_TYPE);
	BIND_ENUM_CONSTANT(COLUMN_PLAYERS);
	BIND_ENUM_CONSTANT(COLUMN_PING);
	BIND_ENUM_CONSTANT(COLUMN_ACCESS);
	BIND_ENUM_CONSTANT(PING_PENDING);
	BIND_ENUM_CONSTANT(PING_FAILED);
	BIND_ENUM_CONSTANT(PING_NEVER_ATTEMPTED);
	ClassDB::bind_static_method("NovaWorldServerBrowser", D_METHOD("row_is_locked", "row"),
			&NovaWorldServerBrowser::row_is_locked);
	ClassDB::bind_static_method("NovaWorldServerBrowser", D_METHOD("ping_text", "ping"),
			&NovaWorldServerBrowser::ping_text);
	ClassDB::bind_static_method("NovaWorldServerBrowser", D_METHOD("row_cells", "row", "ping"),
			&NovaWorldServerBrowser::row_cells);
	ClassDB::bind_static_method("NovaWorldServerBrowser",
			D_METHOD("filter_rows", "rows", "text", "game_type", "hide_full", "hide_empty", "hide_locked"),
			&NovaWorldServerBrowser::filter_rows);
	ClassDB::bind_static_method("NovaWorldServerBrowser",
			D_METHOD("sort_rows", "rows", "column", "ascending", "pings"),
			&NovaWorldServerBrowser::sort_rows);
	ClassDB::bind_static_method("NovaWorldServerBrowser", D_METHOD("details_lines", "row"),
			&NovaWorldServerBrowser::details_lines);
	ClassDB::bind_static_method("NovaWorldServerBrowser", D_METHOD("row_tooltip", "row"),
			&NovaWorldServerBrowser::row_tooltip);
}

} // namespace godot
