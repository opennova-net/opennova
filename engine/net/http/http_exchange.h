#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <net/socket/socket.h>
#include <net/socket/stream_socket.h>

namespace opennova::net {

// One completed HTTP exchange, in the shape LobbyHttpFlow's on_*_response hooks take.
struct HttpReply {
	bool transport_ok = false;        // the exchange itself completed (a reply came back)
	bool connected = false; // the connection was made (the request may have reached a server)
	int code = 0;                     // the HTTP status
	std::vector<std::string> headers; // raw "Name: value" lines
	std::vector<uint8_t> body;
	std::string error;                // why transport_ok is false
};

// Resolve an http:// URL's host into an endpoint (its port left alone); false refuses it.
using HttpResolver = std::function<bool(const std::string &host, Endpoint &out)>;

inline constexpr size_t kHttpMaxReplyBytes = 8u * 1024u * 1024u;

// How a reply's bytes are read into its status, headers and body.
enum class HttpFraming {
	// The head to its first CR LF CR LF, the status the three digits past the status line's first
	// space, the body to the Content-Length line's value (cut there) or to the server's close.
	Plain,
	// The NovaWorld menu browser's (the game's and serve's lobby HTTP, browser_transport.h): the
	// head split off at its first CR LF CR LF once the bytes begin "HTTP" (any case); the length
	// the number past the first "CONTENT-LENGTH" anywhere in the head (any case), none reading to
	// the server's close; the reply complete once the body holds that length (its excess kept);
	// at a close before any head the first "\n\r\n\r", "\r\r" or "\n\n" splits it, else the whole
	// text is the head and the body alike; and the status the number past the head's first
	// whitespace, 100 for a reply with no head.
	// [orig: HTTP_RecvResponse @0x666EF0 - the recv of 0x4000 @0x666F55, the head test only while
	//  the length is -1 @0x666FF2, String_CompareNoCaseN(.., "HTTP", 4) @0x66700E, strstr "\r\n\r\n"
	//  @0x66702B, the head copied @0x66706A and the body moved down @0x66708B, "CONTENT-LENGTH"
	//  sought @0x6670AB and atol at +15 @0x6670C7, none: -2 @0x6670DF, the completion test
	//  `body_len >= content_length` @0x667100, the close @0x6671B4 and its separator walk
	//  @0x6671F8..0x66724D, the whole-text head @0x667256..0x6672C7 (the body left whole);
	//  HTTP_ResponseHandler @0x659480 - no head: 100 @0x6596F7, the first whitespace @0x6594B4..
	//  0x6594D7, atol @0x6594DA]
	Browser,
};

struct HttpExchangeOptions {
	int connect_ms = 0; // the connect's wait (and each send's, the stream's)
	// >0: a wait for the reply's bytes past this ends a plain reply as the close would and fails
	// a browser one; 0: no bound (the menu browser's)
	int io_ms = 0;
	int total_ms = 0;   // >0: a reply still arriving this long after the send is refused
	size_t max_reply_bytes = kHttpMaxReplyBytes; // a reply past it is refused
	HttpFraming framing = HttpFraming::Plain;
	// Set from another thread: the exchange stops at its next wait (at most kHttpCancelSliceMs)
	// and reports "cancelled".
	const std::atomic<bool> *cancelled = nullptr;
};

// The longest a wait for the reply's bytes runs between two looks at `cancelled`.
inline constexpr int kHttpCancelSliceMs = 50;

// One HTTP/1.0 exchange on its own connection over `connector`: `method` ("GET", "POST", "PUT",
// "DELETE"; every method but GET sends `body` with a Content-Length, after the given lines unless
// they carry one), the request line and Host first (the URL's authority, unless the given lines
// carry a Host line of their own), then the "Name: value" header lines as given, each ended
// CR LF, then a CR LF and the body, sent at once; then the reply, read as `options.framing`
// says. http:// only; the connect goes to the address `resolve` gives the URL's host, at the
// URL's port (80 when it names none).
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - "GET %s%s%s HTTP/1.0\r\n" @0x6588DC, "Host: %s\r\n"
//  @0x658959, each line's "\r\n", the closing "\r\n" (word_7C1310) @0x658AEA, one
//  CNapiWinsock_SendData @0x658AFF; GopherWebWidget_SendHttpPost @0x658B30 - "POST %s HTTP/1.0\r\n"
//  @0x658BA9, the closing "\r\n" @0x658D09 and the body behind it @0x658D1D, one send @0x658D32;
//  CGopherWnd_PumpRequests @0x659740 - the URL's port atol'd, 80 when none @0x6597D5..0x6597EF]
HttpReply http_exchange(IStreamConnector &connector, const std::string &method,
		const std::string &url, const std::vector<std::string> &headers, const std::string &body,
		const HttpResolver &resolve, const HttpExchangeOptions &options);

// The same exchange over the platform's TCP, its reply read plainly: every connect, send and wait
// for bytes at most `timeout_ms`; a reply past `max_reply_bytes` is refused, and with `total_ms`
// > 0 so is one still arriving `total_ms` after the send (a peer that trickles bytes inside each
// wait).
HttpReply http_exchange(const std::string &method, const std::string &url,
		const std::vector<std::string> &headers, const std::string &body, int timeout_ms,
		const HttpResolver &resolve, size_t max_reply_bytes = kHttpMaxReplyBytes, int total_ms = 0);

} // namespace opennova::net
