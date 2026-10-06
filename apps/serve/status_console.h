#pragma once

// opennova-serve's status console (ADR 0051 PR7). A retail dedicated host
// draws the server-status page in place of the scene on every frame its
// throttle allows [orig: Server_DrawStatusScreen @0x50a2d0 from
// GameLoop_RenderFrame @0x521cd6..0x521cef; the view set for a dedicated host
// by Server_InitNewRoundState @0x51cb43..0x51cb5d]. A headless server has no
// screen, so this is that page's device: it fills the engine's feed
// (inmatch/server_status_feed.h), lays the engine's lines
// (hud/hud_server_status.h) out as text, and prints the page on stdout when
// its rows change. The page's chat input line is the console's stdin: a retail
// operator opens it with the Global talk key on the server's window, types and
// presses Enter, and each stdin line is typed into it the same way
// (inmatch/server_console.h server_console_submit) [orig: Game_WindowProc
// @0x76276A -> Input_ProcessKeyboardEvents @0x49D42F (the binding scan) /
// @0x49D498 (Chat_HandleInputChar while capturing); the page draws the capture
// @0x50B245..0x50B25A]. Nothing on the page is the console's own; the colours
// and the ticker, which text cannot carry, are left out.

#include <runtime/hud/hud_chat_entry.h>
#include <runtime/hud/hud_server_status.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace opennova::serve {

class Server;

// The page as text: the server line (the round clock after it), the roster
// grid row by row with its columns side by side, the team block, the bottom
// row, then the console rows that hold a line.
std::string render_status_page(const hud::ServerStatusPageState &page);

// What decides a reprint: the server line, the roster cells, the team block
// and the console rows. The clock and the bottom row's statistics ride every
// print but change too often to cause one.
std::string status_page_rows(const hud::ServerStatusPageState &page);

class StatusConsole {
public:
	explicit StatusConsole(std::FILE *out) : out_(out) {}

	// One frame: at the page's 200 ms throttle (the server has no window to
	// lose focus), fill the page from the running server and print it when its
	// rows changed. True when it printed.
	bool update(Server &server, uint32_t now_ms);
	// The same over a filled page (the render test's seam).
	bool present(const hud::ServerStatusPageState &page, uint32_t now_ms);
	// One stdin line typed into the page's chat input.
	void submit(Server &server, const std::string &line);

private:
	const hud::GameTextLookup &gametext(Server &server);

	std::FILE *out_;
	uint32_t last_ms_ = 0;
	std::string last_rows_;
	hud::ChatEntry chat_;
	bool gametext_loaded_ = false;
	hud::GameTextLookup gametext_;
};

// The console's stdin, read on a thread of its own so the server's loop never
// blocks on it. Lines come out in order; a trailing CR is dropped.
class ConsoleInput {
public:
	void start();
	bool poll(std::string &line);

private:
	// Shared with the reader thread, which outlives nothing it needs.
	struct Lines {
		std::mutex mutex;
		std::deque<std::string> queue;
	};
	std::shared_ptr<Lines> lines_ = std::make_shared<Lines>();
};

} // namespace opennova::serve
