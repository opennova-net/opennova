#pragma once

// The seam the content store's client speaks HTTP through (ADR 0069): the protocol, the jobs and
// their tests are portable C++ here, and the host makes the exchanges, as the game's NovaWorld
// client hands its requests to Godot (engine/net/novaworld/http_flow.h). The editor's Shell makes
// them over Godot's HTTPClient on a worker thread (godot/src/authoring/content_http.h); a test
// over a fake, or over apps/common's HTTP client against the service's own routes. A request may
// send a file's bytes as its body (an upload) and write its reply's body to a file (a download), so
// no file a pull or a push moves is held in memory whole.

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
	// Where the reply's body goes in place of ContentResponse::body (a download), "" to keep it.
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
