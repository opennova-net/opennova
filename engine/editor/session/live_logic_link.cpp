#include <editor/session/live_logic_link.h>

#include <optional>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

// The header `name`'s value in an HTTP head (case as HTTP compares it), "" for none.
std::string header_value(const std::string &head, const char *name) {
	size_t at = 0;
	while (at < head.size()) {
		const size_t end = head.find("\r\n", at);
		const std::string line = head.substr(at, end == std::string::npos ? std::string::npos : end - at);
		const size_t colon = line.find(':');
		if (colon != std::string::npos && strutil::iequals(strutil::trim_view(std::string_view(line).substr(0, colon)), name))
			return strutil::trim(std::string_view(line).substr(colon + 1));
		if (end == std::string::npos) break;
		at = end + 2;
	}
	return std::string();
}

} // namespace

GameMcpRead read_game_mcp_response(const std::string &bytes, GameMcpResponse &out) {
	const size_t head_end = bytes.find("\r\n\r\n");
	if (head_end == std::string::npos) return bytes.size() > 65536 ? GameMcpRead::Malformed : GameMcpRead::Incomplete;
	const std::string head = bytes.substr(0, head_end);
	// "HTTP/1.1 200 OK"
	if (head.compare(0, 5, "HTTP/") != 0) return GameMcpRead::Malformed;
	const size_t space = head.find(' ');
	const std::optional<int> status = space == std::string::npos ? std::nullopt : strutil::parse_int(head.substr(space + 1, 3));
	const std::optional<int> length = strutil::parse_int(header_value(head, "Content-Length"));
	if (!status || !length || *length < 0) return GameMcpRead::Malformed;
	const size_t body_at = head_end + 4;
	if (bytes.size() < body_at + size_t(*length)) return GameMcpRead::Incomplete;
	out.status = *status;
	out.session_id = header_value(head, "Mcp-Session-Id");
	out.body = bytes.substr(body_at, size_t(*length));
	out.consumed = body_at + size_t(*length);
	return GameMcpRead::Whole;
}

std::string game_mcp_post(int port, const std::string &session_id, const std::string &body) {
	std::string out = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
	                  "\r\nContent-Type: application/json\r\nAccept: application/json\r\n";
	if (!session_id.empty()) out += "Mcp-Session-Id: " + session_id + "\r\n";
	out += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n";
	return out + body;
}

std::string game_mcp_initialize_body(uint64_t id) {
	JsonValue client = JsonValue::make_object();
	client.set("name", io::json_string("opennova-editor"));
	client.set("version", io::json_string("1"));
	JsonValue params = JsonValue::make_object();
	params.set("protocolVersion", io::json_string("2025-06-18"));
	params.set("capabilities", JsonValue::make_object());
	params.set("clientInfo", std::move(client));
	JsonValue out = JsonValue::make_object();
	out.set("jsonrpc", io::json_string("2.0"));
	out.set("id", io::json_number(double(id)));
	out.set("method", io::json_string("initialize"));
	out.set("params", std::move(params));
	return io::json_write(out);
}

std::string game_mcp_debug_body(uint64_t id, const JsonValue &arguments) {
	JsonValue params = JsonValue::make_object();
	params.set("name", io::json_string("game_debug"));
	params.set("arguments", arguments);
	JsonValue out = JsonValue::make_object();
	out.set("jsonrpc", io::json_string("2.0"));
	out.set("id", io::json_number(double(id)));
	out.set("method", io::json_string("tools/call"));
	out.set("params", std::move(params));
	return io::json_write(out);
}

bool game_mcp_tool_result(const std::string &body, JsonValue &out, std::string &error) {
	JsonValue answer;
	if (!io::json_parse(body, answer, error)) {
		error = "the game's answer does not read: " + error;
		return false;
	}
	if (const JsonValue *rpc_error = answer.get("error")) {
		error = rpc_error->get_string("message", "the game refused the request");
		return false;
	}
	const JsonValue *result = answer.get("result");
	if (!result || !result->is_object()) {
		error = "the game's answer holds no result";
		return false;
	}
	// The tool's words: its text blocks, joined.
	std::string text;
	if (const JsonValue *content = result->get("content"); content && content->is_array())
		for (const JsonValue &block : content->array)
			if (block.get_string("type", "") == "text") text += (text.empty() ? "" : "\n") + block.get_string("text", "");
	if (result->get_bool("isError", false)) {
		error = text.empty() ? std::string("the game refused the call") : text;
		return false;
	}
	if (const JsonValue *structured = result->get("structuredContent"); structured && structured->is_object()) {
		out = *structured;
		return true;
	}
	if (!io::json_parse(text, out, error)) {
		error = "the game's result does not read: " + error;
		return false;
	}
	return true;
}

void LiveLogicLink::reset(GameMcpChannel &channel) {
	channel.close();
	port_ = 0;
	initialized_ = false;
	session_id_.clear();
	received_.clear();
	waiting_ = Waiting::None;
}

void LiveLogicLink::step(ProjectSession &session, GameMcpChannel &channel, int64_t now_ms) {
	const int port = session.view().activity.play_mcp_port;
	if (!session.live_logic_polling()) {
		if (port_ != 0) reset(channel);
		queued_.clear();
		calls_after_ = 0;
		return;
	}
	// The calls play_debug asked, in order.
	for (PlayDebugCall &call : session.play_debug_since(calls_after_)) {
		calls_after_ = call.serial;
		queued_.push_back(std::move(call));
	}
	if (port_ != port) {
		reset(channel);
		port_ = port;
		channel.connect(port);
		next_poll_ms_ = now_ms;
	}
	switch (channel.state()) {
	case GameMcpChannel::State::Connecting: return;
	case GameMcpChannel::State::Closed:
	case GameMcpChannel::State::Failed:
		// Not up yet (the game is still starting its endpoint), or the stream dropped: try again shortly.
		if (waiting_ != Waiting::None) ++stats_.failures;
		waiting_ = Waiting::None;
		initialized_ = false;
		session_id_.clear();
		received_.clear();
		if (now_ms >= next_poll_ms_) {
			channel.connect(port_);
			next_poll_ms_ = now_ms + kPollMs;
		}
		return;
	case GameMcpChannel::State::Connected: break;
	}
	if (waiting_ != Waiting::None) {
		received_ += channel.receive();
		GameMcpResponse response;
		const GameMcpRead read = read_game_mcp_response(received_, response);
		if (read == GameMcpRead::Whole) {
			received_.erase(0, response.consumed);
			answered(session, response);
		} else if (read == GameMcpRead::Malformed || now_ms - sent_ms_ > kAnswerMs) {
			++stats_.failures;
			if (waiting_ == Waiting::Call) session.answer_play_debug(call_.serial, false, "the game did not answer");
			reset(channel);
			port_ = port; // connected again on the next step
			channel.connect(port);
			next_poll_ms_ = now_ms + kPollMs;
			return;
		}
		if (waiting_ != Waiting::None) return;
	}
	// The next exchange: initialize once a stream is up, then a queued call before the snapshot.
	std::string body;
	if (!initialized_) {
		body = game_mcp_initialize_body(next_id_++);
		waiting_ = Waiting::Initialize;
	} else if (!queued_.empty()) {
		call_ = queued_.front();
		queued_.pop_front();
		body = game_mcp_debug_body(next_id_++, play_debug_arguments(call_));
		waiting_ = Waiting::Call;
	} else if (now_ms >= next_poll_ms_) {
		body = game_mcp_debug_body(next_id_++, live_snapshot_arguments());
		waiting_ = Waiting::Snapshot;
		next_poll_ms_ = now_ms + kPollMs;
	} else {
		return;
	}
	if (!channel.send(game_mcp_post(port_, session_id_, body))) {
		++stats_.failures;
		if (waiting_ == Waiting::Call) session.answer_play_debug(call_.serial, false, "the game's endpoint closed");
		waiting_ = Waiting::None;
		return;
	}
	sent_ms_ = now_ms;
}

void LiveLogicLink::answered(ProjectSession &session, const GameMcpResponse &response) {
	const Waiting was = waiting_;
	waiting_ = Waiting::None;
	if (was == Waiting::Initialize) {
		// The endpoint's session, which every later request names.
		initialized_ = true;
		session_id_ = response.session_id;
		return;
	}
	JsonValue result;
	std::string error;
	const bool ok = response.status == 200 && game_mcp_tool_result(response.body, result, error);
	if (response.status != 200 && error.empty()) error = "the game answered HTTP " + std::to_string(response.status);
	if (was == Waiting::Call) {
		++stats_.calls;
		session.answer_play_debug(call_.serial, ok, error);
		return;
	}
	if (!ok) {
		++stats_.failures;
		return;
	}
	std::string report_error;
	if (session.report_live_logic(io::json_write(result), report_error)) ++stats_.snapshots;
	else ++stats_.failures;
}

} // namespace opennova::editor
