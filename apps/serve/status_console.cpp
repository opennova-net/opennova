#include "status_console.h"

#include "server.h"

#include <formats/rtxt/rtxt.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/server_status_feed.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace opennova::serve {

namespace {

// The roster grid in the compile's walk: column by column, the slot each
// cell shows (i on a peer, i + 1 on a host with no player slot of its own),
// each cell's colour carried into the next as the compile carries it
// [orig: Server_DrawStatusScreen @0x50a3ec..0x50a7d5].
std::vector<std::vector<std::string>> roster_cells(const hud::ServerStatusPageState &page) {
	const hud::ServerStatusRosterGrid grid = hud::server_status_roster_grid(page.capacity);
	std::vector<std::vector<std::string>> rows(static_cast<size_t>(std::max(grid.rows, 0)),
			std::vector<std::string>(static_cast<size_t>(std::max(grid.columns, 0))));
	uint32_t color = 0xFFFFFFFFu;
	int base = 0;
	for (int column = 0; column < grid.columns; ++column) {
		for (int row = 0; row < grid.rows; ++row) {
			const int slot_index = page.mp_session_peer ? base + row : base + row + 1;
			const hud::ServerStatusRosterCell cell =
					hud::server_status_roster_cell(page, slot_index, color);
			color = cell.color;
			rows[static_cast<size_t>(row)][static_cast<size_t>(column)] = cell.text;
		}
		base += grid.rows;
	}
	return rows;
}

void trim_right(std::string &s) {
	while (!s.empty() && s.back() == ' ') s.pop_back();
}

} // namespace

std::string render_status_page(const hud::ServerStatusPageState &page) {
	std::string out;
	std::string head = hud::server_status_server_line(page).text;
	hud::ServerStatusTextLine clock;
	if (hud::server_status_round_clock(page, clock)) head += "  " + clock.text;
	out += head + "\n";

	const std::vector<std::vector<std::string>> rows = roster_cells(page);
	size_t width = 0;
	for (const auto &row : rows)
		for (const std::string &cell : row) width = std::max(width, cell.size());
	for (const auto &row : rows) {
		std::string line;
		for (size_t c = 0; c < row.size(); ++c) {
			line += row[c];
			if (c + 1 < row.size()) line.append(width + 2 - row[c].size(), ' ');
		}
		trim_right(line);
		out += line + "\n";
	}

	for (const hud::ServerStatusTextLine &l : hud::server_status_team_block(page))
		out += l.text + "\n";

	std::string bottom;
	for (const hud::ServerStatusTextLine &l : hud::server_status_bottom_row(page)) {
		std::string field = l.text;
		trim_right(field);
		if (!bottom.empty()) bottom += "  ";
		bottom += field;
	}
	out += bottom + "\n";

	for (const hud::ServerStatusConsoleRow &row : page.console_rows)
		if (!row.text.empty()) out += row.text + "\n";
	return out;
}

std::string status_page_rows(const hud::ServerStatusPageState &page) {
	std::string key = hud::server_status_server_line(page).text + "\n";
	for (const auto &row : roster_cells(page))
		for (const std::string &cell : row) key += cell + "\n";
	for (const hud::ServerStatusTextLine &l : hud::server_status_team_block(page))
		key += l.text + "\n";
	for (const hud::ServerStatusConsoleRow &row : page.console_rows) key += row.text + "\n";
	return key;
}

bool StatusConsole::present(const hud::ServerStatusPageState &page, uint32_t now_ms) {
	// The page's 200 ms throttle [orig: Server_DrawStatusScreen
	// @0x50a305..0x50a35d]; a page that would be due is filled and compared.
	if (!hud::server_status_page_due(&last_ms_, now_ms, /*window_active=*/true)) return false;
	std::string rows = status_page_rows(page);
	if (rows == last_rows_) return false;
	last_rows_ = std::move(rows);
	const std::string text = render_status_page(page);
	std::fprintf(out_, "\n%s", text.c_str());
	std::fflush(out_);
	return true;
}

const hud::GameTextLookup &StatusConsole::gametext(Server &server) {
	if (gametext_loaded_) return gametext_;
	gametext_loaded_ = true;
	// The page's strings come from the mounted gametext.bin, as the game's
	// GameText_GetString reads them [orig: Game_InitSubsystems @0x4A6CD0].
	std::vector<uint8_t> bytes;
	auto table = std::make_shared<rtxt::File>();
	std::string error;
	if (server.index().read_file("gametext.bin", bytes) &&
			rtxt::parse(bytes.data(), bytes.size(), *table, error)) {
		gametext_ = [table](const char *section, const char *key, const char *fallback) {
			std::string value = table->get_in_section(section, key);
			return value.empty() ? std::string(fallback) : value;
		};
	}
	return gametext_;
}

bool StatusConsole::update(Server &server, uint32_t now_ms) {
	if (!server.running()) return false;
	if (!hud::server_status_page_due_at(last_ms_, now_ms, /*window_active=*/true)) return false;
	inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	hud::ServerStatusPageState page;
	inmatch::fill_server_status_page(page, ctx, &server.kernel().world);
	page.text = hud::server_status_text(gametext(server), page.game_type);
	return present(page, now_ms);
}

void StatusConsole::submit(Server &server, const std::string &line) {
	if (!server.running()) return;
	// The input's clock is the per-main-frame counter, which the admin's CHAT SEND reads too:
	// both check the one flood table (server_console.h).
	inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	(void)inmatch::server_console_submit(ctx, chat_, line, server.main_frame(), gametext(server));
}

void ConsoleInput::start() {
	std::thread([lines = lines_] {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::lock_guard<std::mutex> lock(lines->mutex);
			lines->queue.push_back(line);
		}
	}).detach();
}

bool ConsoleInput::poll(std::string &line) {
	std::lock_guard<std::mutex> lock(lines_->mutex);
	if (lines_->queue.empty()) return false;
	line = std::move(lines_->queue.front());
	lines_->queue.pop_front();
	return true;
}

} // namespace opennova::serve
