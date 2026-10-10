#pragma once

// JSON request bodies and replies for the /api routes (http_listener.cpp,
// web_access.cpp). Crow's rvalue throws when a member is read as the wrong type
// (has() on a non-object, s() on a number), and a throw out of a handler is a
// 500; JsonBody reads a body that must be an object member by member, typed, so
// a malformed request is a 400 instead.

#include <crow.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

inline crow::response json_reply(int code, const crow::json::wvalue &body) {
	crow::response res(code);
	res.body = body.dump();
	res.set_header("Content-Type", "application/json");
	return res;
}

// {"error": <code>}, with "message" beside it when one is given.
inline crow::response json_error(int code, const char *error, const std::string &message = {}) {
	crow::json::wvalue body;
	body["error"] = error;
	if (!message.empty()) body["message"] = message;
	return json_reply(code, body);
}

class JsonBody {
public:
	// The body as a JSON object; nullopt when it is not one (answer 400
	// invalid_json).
	static std::optional<JsonBody> parse(const std::string &text) {
		JsonBody body;
		body.value_ = crow::json::load(text);
		if (!body.value_ || body.value_.t() != crow::json::type::Object) return std::nullopt;
		return std::optional<JsonBody>(std::move(body));
	}

	bool has(const char *key) const { return value_.has(key); }

	// A string member; nullopt when it is absent, and also when it is present
	// as another type, which wrong_type() then reports (answer 400
	// invalid_field).
	std::optional<std::string> text(const char *key) {
		if (!value_.has(key)) return std::nullopt;
		if (value_[key].t() != crow::json::type::String) {
			wrong_type_ = true;
			return std::nullopt;
		}
		return std::string(value_[key].s());
	}
	std::string text_or(const char *key, const std::string &fallback) {
		return text(key).value_or(fallback);
	}

	// An integer member (a number with no fraction), as text() reads strings.
	std::optional<int64_t> integer(const char *key) {
		if (!value_.has(key)) return std::nullopt;
		const auto &member = value_[key];
		if (member.t() != crow::json::type::Number ||
		    member.nt() == crow::json::num_type::Floating_point) {
			wrong_type_ = true;
			return std::nullopt;
		}
		return member.i();
	}

	// A list-of-strings member, as text() reads strings: a member that is no
	// list, or holds anything but strings, is the wrong type.
	std::optional<std::vector<std::string>> text_list(const char *key) {
		if (!value_.has(key)) return std::nullopt;
		const auto &member = value_[key];
		if (member.t() != crow::json::type::List) {
			wrong_type_ = true;
			return std::nullopt;
		}
		std::vector<std::string> out;
		for (const auto &item : member) {
			if (item.t() != crow::json::type::String) {
				wrong_type_ = true;
				return std::nullopt;
			}
			out.emplace_back(item.s());
		}
		return out;
	}

	// A true / false member, as text() reads strings.
	std::optional<bool> flag(const char *key) {
		if (!value_.has(key)) return std::nullopt;
		const auto type = value_[key].t();
		if (type != crow::json::type::True && type != crow::json::type::False) {
			wrong_type_ = true;
			return std::nullopt;
		}
		return type == crow::json::type::True;
	}

	bool wrong_type() const { return wrong_type_; }

private:
	crow::json::rvalue value_;
	bool wrong_type_ = false;
};

} // namespace opennova::novaworld_server
