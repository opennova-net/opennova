#pragma once

// The retail pages' helpers the login chain (retail_login_routes.cpp) and the host and join
// flow (retail_host_join_routes.cpp) share: a page link's first-call query, a form body's
// fields, a page's cookies and headers, the game a request's client plays, the message page,
// the expansion bits a join needs, and a log line's cookies. Service policy, not a port.

#include "program/catalog_repository.h"

#include <crow.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

// Whether a NWJoin.dll / NWHost.dll request carries its page link's first-call query (any of
// the fields the join / host page link carries, net/novaworld/relay_request.h).
bool has_relay_first_call_query(const crow::request &req);

// `s` with its '+' as spaces and its %XX escapes decoded (a malformed escape kept as it is).
std::string url_decode(std::string_view s);

// An application/x-www-form-urlencoded body's fields, each name and value url-decoded.
std::map<std::string, std::string> parse_form_body(std::string_view body);

// A Set-Cookie line for `name` (http_cookies.h add_set_cookie, "Path=/"): a value that could
// split the response or forge a cookie goes out empty.
void add_cookie(crow::response &res, const std::string &name, const std::string &value);

// The retail pages' no-cache headers (Expires, Pragma, Cache-Control).
void add_standard_headers(crow::response &res);

// The message the LAN-list join's page carries when the GSID it names is on no listing (see
// handle_join).
inline constexpr const char *kLanGameGoneMessage = "That game is no longer hosted on NovaWorld.";

// The message POST /NWLogin.dll's failure page carries when the login brake refuses the attempt
// (see handle_login_post).
inline constexpr const char *kLoginThrottledMessage =
		"Too many failed logins. Please try again later.";

// The game a retail request's client plays (catalog::GameCatalog::for_client): its gate tag,
// else its product family, else the prefix of the page it names; an empty catalogue answers a
// row with every field empty.
const catalog::GameRow &client_game(const catalog::GameCatalog &games, const crow::request &req,
		const std::map<std::string, std::string> &cookies, std::string_view pfid,
		std::string_view page);

// The message page (`msg_template`) carrying `message`, with the failure page and the host and
// GSB URLs it names, its no-cache headers and the YOURIP cookie.
crow::response render_legacy_message(const std::string &templates_dir, const std::string &message,
		const std::string &failure_template, const std::string &msg_template,
		const std::string &remote_ip, const std::string &host_url, const std::string &gsb_url);

// An expansion bits value (decimal, hex or octal, strtoull's base 0); false for anything else.
bool parse_exp_bits(const std::string &value, uint64_t &out);

// Whether the bits an account owns cover the ones a listing requires (none required: yes; text
// that reads as no bits compares as text).
bool expansion_bits_compatible(const std::string &owned, const std::string &required);

// One line naming a request's cookies for a log line, each value cut short and a secret's
// shown only as its stand-in.
std::string cookie_summary(std::string_view header);

} // namespace opennova::novaworld_server
