#pragma once

// The seam the content store's client speaks HTTP through (ADR 0069): the protocol, the jobs and
// their tests are portable C++ here, and the host makes the exchanges, as the game's NovaWorld
// client hands its requests to Godot (engine/net/novaworld/http_flow.h). The editor's Shell makes
// them over Godot's HTTPClient on a worker thread (godot/src/authoring/content_http.h); a test
// over a fake, or over apps/common's HTTP client against the service's own routes. A request may
// send a file's bytes as its body (an upload) and write its reply's body to a file (a download), so
// no file a pull moves is held in memory whole (the Shell's transport reads an upload's file whole,
// HTTPClient sending a body in one piece).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::editor {

struct ContentRequest {
	std::string method; // "GET", "POST", "PUT"
	std::string url; // absolute: the service's address and a route, or a URL a reply handed out
	std::vector<std::pair<std::string, std::string>> headers; // name, value
	std::string body;
	// A file whose bytes are the body in place of `body` (an upload), "" for none.
	std::string body_file;
	// Where a successful (2xx) reply's body goes in place of ContentResponse::body (a download), ""
	// to keep it; any other reply's body is kept, so its refusal reads.
	std::string response_file;
};

struct ContentResponse {
	bool transport_ok = false; // false: no reply (`error` says why)
	std::string error;
	int status = 0;
	std::vector<std::pair<std::string, std::string>> headers; // as sent, every Set-Cookie its own
	std::string body; // the reply's body, unless the request named a response_file
	// For a reply written to the request's response_file: how many bytes, and their SHA-256 (lower-
	// case hex), the transport's to count as it writes them, so no download is read twice.
	uint64_t file_bytes = 0;
	std::string file_sha256;

	// The value of the first header named `name` (its case aside), "" when none is.
	std::string header(const std::string &name) const;
};

// A URL the Shell's transport reaches: http or https, its host and its port (443 or 80 where it
// names none), and the path and query it asks, from its '/' (the fragment dropped); content_address
// is false for any other (no scheme of the two, a user part or a bracketed address, a port outside
// 1..65535, no host).
struct ContentAddress {
	bool tls = false;
	std::string host;
	int port = 0;
	std::string target;
	// "https://host:443": the connection a later request to it may use again.
	std::string origin() const;
};
bool content_address(const std::string &url, ContentAddress &out);

// How many more tries a request that may go again gets (content_goes_again).
inline constexpr int kContentRetries = 3;

// Whether a request goes again on a fresh connection after its try `attempt` (0 the first) came
// to this: a method a client may send again when a connection fails under it (RFC 9110 9.2.2:
// GET, PUT, DELETE), at most kContentRetries more tries, once its connection broke before or
// during the reply (`lost`) or a whole reply (`replied`) was a 5xx (a store's 500, S3's 503
// SlowDown). Every other end is the request's answer: a reply below 500, a request no retry may
// repeat (a POST: a push, a sign-in), a transfer that stalled or was given up.
bool content_goes_again(
		const std::string &method, int attempt, bool replied, int status, bool lost);

class ContentTransport {
public:
	virtual ~ContentTransport() = default;

	// `request` started: its id, which poll answers by.
	virtual uint64_t start(ContentRequest request) = 0;
	// Whether the request `id` is done: true once, with its response in `out`.
	virtual bool poll(uint64_t id, ContentResponse &out) = 0;
	// The request `id` given up on: it is never answered (a pull or push cancelled).
	virtual void cancel(uint64_t id) = 0;
	// How many requests it runs at once: a pull's downloads and a push's uploads go this many at a
	// time.
	virtual size_t parallel() const = 0;
};

} // namespace opennova::editor
