#pragma once

// The Shell's link to the running game's MCP endpoint for its mission logic (ADR 0046 S24, Live; session/
// live_logic.h): while the session asks for it (ProjectSession::live_logic_polling), the link asks the game's
// game_debug tool for its mission_logic_snapshot about four times a second and hands each answer to the session,
// and makes the calls play_debug queued, saying what each came to. Everything but the bytes is here: the HTTP
// requests (POST /mcp, JSON-RPC 2.0, the endpoint's own docs/mcp.md), reading the answers, the one exchange at a
// time; the embedder gives the byte stream (GameMcpChannel: the Shell's TCP stream to 127.0.0.1,
// godot/src/authoring/play_logic_link, stepped each frame by the editor app).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include <base/io/json.h>
#include <editor/session/live_logic.h>

namespace opennova::editor {

class ProjectSession;

// A byte stream to the game's endpoint on this machine, the embedder's (non-blocking: each call returns at once).
class GameMcpChannel {
public:
	enum class State { Closed, Connecting, Connected, Failed };
	virtual ~GameMcpChannel() = default;
	// Connects to 127.0.0.1:`port`, closing what was open.
	virtual void connect(int port) = 0;
	virtual State state() = 0;
	// Sends `bytes` whole; false when the stream cannot take them (it is then closed).
	virtual bool send(const std::string &bytes) = 0;
	// What has arrived since the last call ("" nothing).
	virtual std::string receive() = 0;
	virtual void close() = 0;
};

// An HTTP response read from the bytes received so far.
struct GameMcpResponse {
	int status = 0;
	std::string session_id; // its Mcp-Session-Id header
	std::string body;
	size_t consumed = 0; // the bytes it took
};
enum class GameMcpRead { Incomplete, Whole, Malformed };
GameMcpRead read_game_mcp_response(const std::string &bytes, GameMcpResponse &out);
// A POST of `body` to the endpoint at `port` (with `session_id` where the endpoint gave one).
std::string game_mcp_post(int port, const std::string &session_id, const std::string &body);
// The JSON-RPC requests the link makes: initialize, and a tools/call of game_debug with `arguments`.
std::string game_mcp_initialize_body(uint64_t id);
std::string game_mcp_debug_body(uint64_t id, const io::JsonValue &arguments);
// A tools/call answer's tool result (its structuredContent, else its text read as JSON) into `out`; false with
// `error` the tool's own words (an isError result) or why the answer does not read.
bool game_mcp_tool_result(const std::string &body, io::JsonValue &out, std::string &error);

class LiveLogicLink {
public:
	// How often the snapshot is asked while nothing else waits.
	static constexpr int64_t kPollMs = 250;
	// An exchange the game has not answered by then is given up (the stream closed, opened again).
	static constexpr int64_t kAnswerMs = 3000;

	// One step at `now_ms` (the Shell's frame): connects while the session polls and closes when it stops; sends
	// the next exchange when none waits (a queued call first, else the snapshot when it is due); reads an answer
	// and hands it on.
	void step(ProjectSession &session, GameMcpChannel &channel, int64_t now_ms);
	// What the link has done (for the tests and the Shell's log): snapshots read, calls answered, exchanges given
	// up or refused.
	struct Stats {
		size_t snapshots = 0;
		size_t calls = 0;
		size_t failures = 0;
	};
	const Stats &stats() const { return stats_; }

private:
	enum class Waiting { None, Initialize, Snapshot, Call };
	void reset(GameMcpChannel &channel);
	void answered(ProjectSession &session, const GameMcpResponse &response);

	int port_ = 0;
	bool initialized_ = false;
	std::string session_id_;
	std::string received_;
	Waiting waiting_ = Waiting::None;
	PlayDebugCall call_;          // the call waiting, when one is
	int64_t sent_ms_ = 0;
	int64_t next_poll_ms_ = 0;
	uint64_t calls_after_ = 0;    // the last call taken from the session
	std::deque<PlayDebugCall> queued_;
	uint64_t next_id_ = 1;
	Stats stats_;
};

} // namespace opennova::editor
