#include "retail_pages.h"

#include "accounts/template_engine.h"
#include "core/loggable.h"
#include "http_cookies.h"

#include <cstdlib>

namespace opennova::novaworld_server {

// The page link's first-call query on NWJoin.dll / NWHost.dll: any of the
// fields the join / host page link carries (net/novaworld/relay_request.h).
bool has_relay_first_call_query(const crow::request &req) {
	for (const char *name : {"rid", "success", "failure", "relay", "msgbase", "needexpkey",
	                         "pfid", "mode", "gsid", "lan"}) {
		if (req.url_params.get(name) != nullptr) return true;
	}
	return false;
}

// ---- cookie + form helpers (Phase E.1; cookie reading: http_cookies.h) ----

std::string url_decode(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out.push_back(' ');
		} else if (s[i] == '%' && i + 2 < s.size()) {
			auto hex_to_int = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int hi = hex_to_int(s[i + 1]);
			int lo = hex_to_int(s[i + 2]);
			if (hi < 0 || lo < 0) {
				out.push_back(s[i]);
			} else {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
			}
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

std::map<std::string, std::string> parse_form_body(std::string_view body) {
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < body.size()) {
		const auto amp = body.find('&', pos);
		const auto chunk_end = (amp == std::string_view::npos) ? body.size() : amp;
		const auto eq = body.find('=', pos);
		if (eq != std::string_view::npos && eq < chunk_end) {
			out.emplace(url_decode(body.substr(pos, eq - pos)),
			            url_decode(body.substr(eq + 1, chunk_end - eq - 1)));
		}
		if (amp == std::string_view::npos) break;
		pos = amp + 1;
	}
	return out;
}

// Crow doesn't have a first-class set_cookie helper across all versions;
// append a Set-Cookie line (crow::response stores headers in a multimap).
// Through add_set_cookie (http_cookies.h): a value from a query, a form, a
// cookie or a stored account that holds a byte that could split the response
// or forge a cookie goes out empty.
void add_cookie(crow::response &res, const std::string &name, const std::string &value) {
	add_set_cookie(res, name, value, "Path=/");
}

void add_standard_headers(crow::response &res) {
	res.set_header("Expires", "Sat, 01 Jan 2000 00:00:00 GMT");
	res.set_header("Pragma", "no-cache");
	res.set_header("Cache-Control", "no-cache, must-revalidate");
}

// The game a retail request's client plays (catalog::GameCatalog::for_client):
// the gate tag it sends (the `gt` query /nwprepare.dll carries, else the GT
// cookie that page set, which a stock client sends back on every request,
// docs/net/novaworld-net-re.md section 7, Wave 6), else its product family
// (`pfid`, the request's own field, else the NWPF cookie /NWJoin.dll and
// /NWHost.dll set), else the prefix of the page it names (`page`). JO and
// OpenNova share pfid 28 and JO's pages, so only the tag tells them apart; a
// request with no tag is JO's. An empty catalogue (no games rows) answers a
// row with every field empty.
const catalog::GameRow &client_game(const catalog::GameCatalog &games,
                                    const crow::request &req,
                                    const std::map<std::string, std::string> &cookies,
                                    std::string_view pfid, std::string_view page) {
	static const catalog::GameRow kNoGame;
	auto cookie = [&cookies](const char *name) -> std::string_view {
		const auto it = cookies.find(name);
		return it == cookies.end() ? std::string_view() : std::string_view(it->second);
	};
	catalog::ClientHints hints;
	const char *gt = req.url_params.get("gt");
	hints.gate_tag = gt != nullptr && *gt != '\0' ? std::string_view(gt) : cookie("GT");
	hints.pfid = pfid.empty() ? cookie("NWPF") : pfid;
	hints.page = page;
	const catalog::GameRow *game = games.for_client(hints);
	return game != nullptr ? *game : kNoGame;
}

crow::response render_legacy_message(const std::string &templates_dir,
                                     const std::string &message,
                                     const std::string &failure_template,
                                     const std::string &msg_template,
                                     const std::string &remote_ip,
                                     const std::string &host_url,
                                     const std::string &gsb_url) {
	TemplateVars vars{
		{"MESSAGE",     message},
		{"IN",          failure_template},
		{"OUT",         failure_template},
		{"MSGBASE",     msg_template},
		{"HOST_URL",    host_url},
		{"GSB_SERVER",  gsb_url},
		{"JOINLAN_URL", ""},
	};
	crow::response res(200);
	res.body = render_template_file(templates_dir, msg_template, vars);
	res.set_header("Content-Type", "text/html");
	add_standard_headers(res);
	add_cookie(res, "YOURIP", remote_ip);
	return res;
}

bool parse_exp_bits(const std::string &value, uint64_t &out) {
	if (value.empty()) return false;
	char *end = nullptr;
	const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
	if (end == value.c_str() || *end != '\0') return false;
	out = static_cast<uint64_t>(parsed);
	return true;
}

bool expansion_bits_compatible(const std::string &owned,
                               const std::string &required) {
	if (required.empty()) return true;
	if (owned.empty()) return false;
	uint64_t owned_bits = 0;
	uint64_t required_bits = 0;
	if (parse_exp_bits(owned, owned_bits) && parse_exp_bits(required, required_bits)) {
		return (owned_bits & required_bits) == required_bits;
	}
	return owned == required;
}

// One-line summary of inbound cookies for logging. Each entry is
// `name=value` with the value truncated to ~20 chars, both loggable(); a
// bearer secret (a session tag or token, a persistent login: is_secret_cookie)
// shows only as its loggable_secret() stand-in. Tags showing up here that we
// didn't issue this run = retail bridging state from a prior process via its
// persistent cookie jar (PERSISTENTEXPRESSLOGINDATA is the usual suspect — see
// G.4).
std::string cookie_summary(std::string_view header) {
	std::string out;
	const auto cookies = parse_cookie_header(header);
	bool first = true;
	for (const auto &[k, v] : cookies) {
		if (!first) out += ',';
		out += loggable(k);
		out += '=';
		out += is_secret_cookie(k) ? loggable_secret(v) : loggable(v, 20);
		first = false;
	}
	return out;
}

} // namespace opennova::novaworld_server
