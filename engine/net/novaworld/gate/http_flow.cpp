#include <net/novaworld/http_flow.h>

#include <cctype>
#include <cstdlib> // std::atoi

#include <base/io/log.h>
#include <base/io/strutil.h>

namespace opennova {
namespace {

using opennova::strutil::to_lower;
bool begins_with(const std::string &s, const std::string &prefix) {
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
std::string replace_all(std::string s, const std::string &from, const std::string &to) {
	if (from.empty()) return s;
	std::size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}
// ASCII case-insensitive find / replace-all, the shape of retail's
// NapiUtil_ReplaceAllCaseInsensitive @0x617970 (every markup token is matched
// through it).
std::size_t find_icase(const std::string &s, const std::string &needle, std::size_t from = 0) {
	if (needle.empty() || needle.size() > s.size()) return std::string::npos;
	for (std::size_t i = from; i + needle.size() <= s.size(); ++i) {
		if (opennova::strutil::iequals(std::string_view(s).substr(i, needle.size()), needle)) return i;
	}
	return std::string::npos;
}
std::string replace_all_icase(std::string s, const std::string &from, const std::string &to) {
	std::size_t pos = 0;
	while ((pos = find_icase(s, from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}
// The six tokens the retail substituter knows; a startup URL is "templated"
// when it carries any of them.
constexpr const char *kStartupUrlTokens[] = {
		"[DOMAINNAME]", "[VER1]", "[VER2]", "[CC]", "[GT]", "[PRODUCTCODE]"};
bool startup_url_is_templated(const std::string &su) {
	for (const char *token : kStartupUrlTokens) {
		if (find_icase(su, token) != std::string::npos) return true;
	}
	return false;
}
std::string strip_edges(const std::string &s) { return opennova::strutil::trim(s); }
std::string to_string_body(const std::vector<uint8_t> &body) {
	return body.empty() ? std::string() : std::string(reinterpret_cast<const char *>(body.data()), body.size());
}

// Extract the message rendered in NovaWorld's message template. @MESSAGE@ is
// optionally excluded because the login relay uses it for non-terminal progress.
std::string extract_message(const std::vector<uint8_t> &body,
                            bool include_progress_message = true) {
	const std::string html = to_string_body(body);
	if (html.empty()) return std::string();
	const std::string lower = to_lower(html);
	std::size_t begin = std::string::npos;
	std::size_t end = std::string::npos;
	std::size_t at = 0;
	while ((at = lower.find("<ib3_subst", at)) != std::string::npos) {
		const std::size_t tag_end = lower.find('>', at);
		if (tag_end == std::string::npos) break;
		const std::string tag = lower.substr(at, tag_end - at + 1);
		if (tag.find("@generic@") != std::string::npos ||
		    (include_progress_message && tag.find("@message@") != std::string::npos)) {
			begin = tag_end + 1;
			end = lower.find("</ib3_subst", begin);
			break;
		}
		at = tag_end + 1;
	}
	if (begin == std::string::npos || end == std::string::npos) return std::string();

	const std::string fragment = html.substr(begin, end - begin);
	const std::string fragment_lower = to_lower(fragment);
	std::string plain;
	for (std::size_t i = 0; i < fragment.size();) {
		if (fragment[i] != '<') {
			plain.push_back(fragment[i++]);
			continue;
		}
		const std::size_t close = fragment.find('>', i + 1);
		if (close == std::string::npos) break;
		const std::string tag = fragment_lower.substr(i, close - i + 1);
		if (begins_with(tag, "<br") || begins_with(tag, "</p") || begins_with(tag, "</div")) plain.push_back('\n');
		i = close + 1;
	}
	plain = replace_all(plain, "&amp;", "&");
	plain = replace_all(plain, "&lt;", "<");
	plain = replace_all(plain, "&gt;", ">");
	plain = replace_all(plain, "&quot;", "\"");
	plain = replace_all(plain, "&#39;", "'");
	plain = replace_all(plain, "&#x27;", "'");
	plain = replace_all(plain, "&nbsp;", " ");

	std::string normalized;
	bool space = false;
	bool newline = false;
	for (const unsigned char ch : plain) {
		if (ch == '\r' || ch == '\n') {
			newline = !normalized.empty();
			space = false;
		} else if (std::isspace(ch)) {
			space = !normalized.empty();
		} else {
			if (newline && normalized.back() != '\n') normalized.push_back('\n');
			else if (space && normalized.back() != '\n' && normalized.back() != ' ') normalized.push_back(' ');
			newline = false;
			space = false;
			normalized.push_back(static_cast<char>(ch));
		}
	}
	return strip_edges(normalized);
}

LoginResult login_need(HttpRequestSpec req) {
	LoginResult r;
	r.kind = LoginResult::Kind::NeedRequest;
	r.request = std::move(req);
	return r;
}
LoginResult login_fail(std::string reason) {
	LoginResult r;
	r.kind = LoginResult::Kind::Failed;
	r.reason = std::move(reason);
	return r;
}
JoinResult join_need(HttpRequestSpec req) {
	JoinResult r;
	r.kind = JoinResult::Kind::NeedRequest;
	r.request = std::move(req);
	return r;
}
JoinResult join_fail(std::string reason) {
	JoinResult r;
	r.kind = JoinResult::Kind::Failed;
	r.reason = std::move(reason);
	return r;
}

} // namespace

void LobbyHttpFlow::reset() {
	jar_ = CookieJar{};
	epask_ = EpaskParams{};
	login_step_ = LoginStep::Idle;
	login_poll_count_ = 0;
	login_user_.clear();
	login_pass_.clear();
	join_step_ = JoinStep::Idle;
	join_rid_ = 0;
}

// (see godot/src/network/novaworld_client.cpp http_base()) — three derivation tiers.
std::string LobbyHttpFlow::http_base() const {
	const bool templated = !ctx_.startup_url.empty() && startup_url_is_templated(ctx_.startup_url);
	if (templated && !ctx_.web_domain.empty()) {
		std::string d = ctx_.web_domain;
		if (!begins_with(d, "http://") && !begins_with(d, "https://")) d = "http://" + d;
		return d;
	}
	if (!ctx_.startup_url.empty()) {
		const std::string &su = ctx_.startup_url;
		const std::size_t scheme = su.find("://");
		if (scheme != std::string::npos) {
			const std::size_t path = su.find("/", scheme + 3);
			return path != std::string::npos ? su.substr(0, path) : su;
		}
	}
	if (!ctx_.post_ip.empty() && !ctx_.post_port.empty()) {
		return "http://" + ctx_.post_ip + ":" + ctx_.post_port;
	}
	return std::string();
}

// Fill the gate markup template. Retail substitutes exactly six tokens, each
// matched case-insensitively, from: the gate-supplied domain, the literal
// version pair "3"/"2345", the OS country code, the session's gate tag
// ("jop:cus2") and the product code "jop".
// [orig: Mission_DeobfuscateDescription @0x4cdaa0 — NapiUtil_ReplaceAllCaseInsensitive
//  "[DOMAINNAME]" @0x4cdb88, "[VER1]" @0x4cdb9f, "[VER2]" @0x4cdbb6, "[CC]" @0x4cdbcd,
//  "[GT]" @0x4cdbe4, "[PRODUCTCODE]" @0x4cdbfe; called from
//  CNapiGameSession_OnNovaWorldConnected @0x4d1665]
std::string LobbyHttpFlow::resolve_startup_url() const {
	std::string su = ctx_.startup_url;
	if (su.empty() || !startup_url_is_templated(su)) return su; // concrete (OpenNova) pass-through
	su = replace_all_icase(su, "[DOMAINNAME]", ctx_.web_domain);
	su = replace_all_icase(su, "[VER1]", "3");
	su = replace_all_icase(su, "[VER2]", "2345");
	std::string cc = "us"; // [CC] = ISO country from the OS locale (injected)
	const std::size_t us = ctx_.locale.find("_");
	if (us != std::string::npos) {
		const std::string region = to_lower(ctx_.locale.substr(us + 1));
		if (!region.empty()) cc = region;
	}
	su = replace_all_icase(su, "[CC]", cc);
	su = replace_all_icase(su, "[GT]", "jop:cus2");
	su = replace_all_icase(su, "[PRODUCTCODE]", "jop");
	return su;
}

// [orig: request_headers()] — optional form Content-Type, then ONE "Cookie:"
// header per cookie (retail's IB3 client emits each cookie as its own
// "Cookie: name=value;" line, not a merged header)
// [orig: CUIBrowser_SendHTTPRequest @ 0x658840].
std::vector<std::string> LobbyHttpFlow::request_headers(bool form_content_type) const {
	std::vector<std::string> h;
	if (form_content_type) h.push_back("Content-Type: application/x-www-form-urlencoded");
	for (const std::string &line : jar_.cookie_header_lines()) {
		h.push_back("Cookie: " + line);
	}
	return h;
}

// [orig: merge_response_cookies()] — case-insensitive "set-cookie:" strip (11 chars) -> jar.
void LobbyHttpFlow::merge_response_cookies(const std::vector<std::string> &response_headers) {
	std::vector<std::string> values;
	for (const std::string &line : response_headers) {
		if (begins_with(to_lower(line), "set-cookie:")) {
			values.push_back(strip_edges(line.substr(11)));
		}
	}
	if (!values.empty()) jar_.merge_set_cookie_values(values);
}

// [orig: seed_identity_cookies()] — identity_vars -> jar, with the live nwuid for the empty NWUID entry.
void LobbyHttpFlow::seed_identity_cookies() {
	for (const auto &kv : ctx_.identity_vars) {
		std::string value = kv.second;
		if (kv.first == "NWUID" && value.empty()) value = ctx_.server_nwuid;
		jar_.set(kv.first, value);
	}
}

// The Cookie var-list reads the browser jar at use, then overlays locale.
// Initial identity fills the pre-login case before seed_identity_cookies runs;
// once present, HTTP-issued cookie values (NWPF/NWPF2/account/ticket) win.
// [orig: CNapiSession_ReadLocaleInfo @ 0x4ce390 -> cookie enumeration @ 0x64eb70]
std::vector<std::pair<std::string, std::string>> LobbyHttpFlow::session_cookie_vars() const {
	CookieJar cookies = jar_;
	for (const auto &kv : ctx_.identity_vars) {
		const bool locale = strutil::iequals(kv.first, "CountryName") ||
		                    strutil::iequals(kv.first, "Language") ||
		                    strutil::iequals(kv.first, "TimeZoneBias");
		if (!cookies.find(kv.first) || locale) {
			cookies.set(kv.first, kv.first == "NWUID" && kv.second.empty()
			                             ? ctx_.server_nwuid : kv.second);
		}
	}
	std::vector<std::pair<std::string, std::string>> vars;
	vars.reserve(cookies.names().size());
	for (const auto &name : cookies.names()) vars.emplace_back(name, *cookies.find(name));
	return vars;
}

// =============================== EPASK login ===============================

LoginResult LobbyHttpFlow::login(const std::string &username, const std::string &password) {
	if (login_step_ != LoginStep::Idle) return login_fail("login already in flight");
	const std::string prepare_url = resolve_startup_url();
	if (prepare_url.empty()) return login_fail("no gate startup_url yet — connect first");
	login_user_ = username;
	login_pass_ = password;
	login_step_ = LoginStep::Prepare;
	login_poll_count_ = 0;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = prepare_url;
	req.headers = request_headers(false);
	return login_need(std::move(req));
}

// [orig: send_login_post()] — the 13-field LoginFormField list, byte-preserved. A body
// that does not build (params the modexp gate rejects, which the Prepare leg already
// refuses) fails the login instead of posting.
LoginResult LobbyHttpFlow::login_post() {
	const std::vector<LoginFormField> fields = {
		{"EPASK", epask_to_string(epask_), false}, // echoed bundle, PLAINTEXT
		{"NAME", login_user_, true},
		{"PASSWORD", login_pass_, true},
		{"rememberlogindata", "", false},
		{"rememberlogin", "0", true},
		{"pfid", "28", true},
		{"needtoagree", "jop_2_needtoagree.htm", true},
		{"nodb", "jop_2_nodb.htm", true},
		{"relay", "jop_2_relay.htm", true},
		{"msgbase", "jop_2_msg.htm", true},
		{"enterkey", "jop_2_key.htm", true},
		{"failure", "jop_2_login.htm", true},
		{"success", "jop_2_main.htm", true},
	};
	std::string body;
	if (!build_login_post_body(epask_, fields, body)) {
		login_step_ = LoginStep::Idle;
		return login_fail("bad EPASK bundle: params rejected (modulus <= 258 or exponent 0)");
	}
	login_step_ = LoginStep::Post;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Post;
	req.url = http_base() + "/NWLogin.dll";
	req.headers = request_headers(true);
	req.body = std::move(body);
	return login_need(std::move(req));
}

std::string LobbyHttpFlow::nwlogin_poll_url() const {
	const std::string base = http_base();
	const std::string *tag = jar_.find("LOGINSESSIONTAG");
	if (tag && !tag->empty()) return base + "/NWLogin.dll?tag=" + *tag;
	return base + "/NWLogin.dll";
}

LoginResult LobbyHttpFlow::on_login_response(bool transport_ok, int code,
                                             const std::vector<std::string> &response_headers,
                                             const std::vector<uint8_t> &body) {
	const LoginStep step = login_step_;
	if (!transport_ok || code != 200) {
		login_step_ = LoginStep::Idle;
		const std::string message = extract_message(body);
		if (!message.empty()) return login_fail(message);
		return login_fail("login HTTP failed (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers); // store Set-Cookie BEFORE branching

	switch (step) {
		case LoginStep::Prepare: {
			const std::string *epask = jar_.find("EPASK");
			if (epask == nullptr || epask->empty()) {
				login_step_ = LoginStep::Idle;
				const std::string message = extract_message(body);
				if (!message.empty()) return login_fail(message);
				return login_fail("server issued no EPASK cookie");
			}
			std::string epask_error;
			if (!epask_from_string(*epask, epask_, &epask_error)) {
				login_step_ = LoginStep::Idle;
				return login_fail("bad EPASK bundle: " + epask_error);
			}
			// The modexp gate retail applies at the form submit (EPASK_ModexpEncrypt
			// @0x66668a returns -1 for modulus <= 258 or a non-positive exponent):
			// fail here, before NWStart, rather than at the post's body.
			if (epask_.modulus <= 258u || epask_.exponent == 0u) {
				login_step_ = LoginStep::Idle;
				return login_fail("bad EPASK bundle: params rejected (modulus <= 258 or exponent 0)");
			}
			seed_identity_cookies();
			const bool templated =
					!ctx_.startup_url.empty() && startup_url_is_templated(ctx_.startup_url);
			if (templated) {
				login_step_ = LoginStep::NwStart;
				HttpRequestSpec req;
				req.valid = true;
				req.method = HttpMethod::Get;
				req.url = http_base() +
						"/NWStart.dll?MSGBASE=jop_2_msg.htm&IN=jop_2_main.htm&OUT=jop_2_login.htm"
						"&verfile=jop_2.ver&newupdateavailable=jop_2_newupdateavailable.htm"
						"&newupdateavailablewithbypass=jop_2_newupdateavailable2.htm&junction=jop_2_junction.htm";
				req.headers = request_headers(false);
				return login_need(std::move(req));
			}
			return login_post();
		}
		case LoginStep::NwStart: {
			const std::string message = extract_message(body, false);
			if (!message.empty()) {
				login_step_ = LoginStep::Idle;
				return login_fail(message);
			}
			return login_post();
		}
		case LoginStep::Post: {
			const std::string *tag = jar_.find("LOGINSESSIONTAG");
			if (tag == nullptr || tag->empty()) {
				login_step_ = LoginStep::Idle;
				const std::string message = extract_message(body);
				if (!message.empty()) return login_fail(message);
				return login_fail("login rejected (no session tag)");
			}
			login_step_ = LoginStep::Poll;
			login_poll_count_ = 0;
			HttpRequestSpec req;
			req.valid = true;
			req.method = HttpMethod::Get;
			req.url = nwlogin_poll_url();
			req.headers = request_headers(false);
			return login_need(std::move(req));
		}
		case LoginStep::Poll: {
			const std::string *nh = jar_.find("NWHANDLE");
			const std::string *pc = jar_.find("PCID");
			if (nh && !nh->empty()) {
				login_step_ = LoginStep::Idle;
				LoginResult r;
				r.kind = LoginResult::Kind::Succeeded;
				r.nwhandle = *nh;
				r.pcid = (pc && !pc->empty()) ? *pc : std::string();
				return r;
			}
			// The relay page's @MESSAGE@ text is progress, not rejection.
			// Live .204 returns it while login is pending and needs another poll.
			const std::string message = extract_message(body, false);
			if (!message.empty()) {
				login_step_ = LoginStep::Idle;
				return login_fail(message);
			}
			constexpr int kMaxLoginPolls = 10;
			if (++login_poll_count_ >= kMaxLoginPolls) {
				login_step_ = LoginStep::Idle;
				return login_fail("login did not complete (no account handle)");
			}
			HttpRequestSpec req;
			req.valid = true;
			req.method = HttpMethod::Get;
			req.url = nwlogin_poll_url();
			req.headers = request_headers(false);
			return login_need(std::move(req));
		}
		case LoginStep::Idle:
		default:
			login_step_ = LoginStep::Idle;
			return login_fail("login response with no pending step");
	}
}

// =============================== GSB browser ===============================

// [orig: gsb_url()] — <base>/jop_2.gsb?a=1.
std::string LobbyHttpFlow::gsb_url() const {
	const std::string base = http_base();
	if (base.empty()) return std::string();
	return base + "/jop_2.gsb?a=1";
}

HttpRequestSpec LobbyHttpFlow::gsb_request() const {
	HttpRequestSpec req;
	const std::string url = gsb_url();
	if (url.empty()) return req; // invalid (no base yet)
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = url;
	req.headers = request_headers(false);
	return req;
}

// [orig: on_gsb_request_completed()] — parse only; the GSB leg does NOT merge Set-Cookie.
bool LobbyHttpFlow::on_gsb_response(bool transport_ok, int code, const std::vector<uint8_t> &body,
                                    GsbResponse &out) {
	if (!transport_ok || code != 200) return false;
	return gsb_parse_response(body.data(), body.size(), out);
}

// =============================== NWJoin ===============================

// [orig: join()] — the first NWJoin GET.
JoinResult LobbyHttpFlow::join(uint32_t rid) {
	if (join_step_ != JoinStep::Idle) return join_fail("join already in flight");
	if (http_base().empty()) return join_fail("no server base URL — connect first");
	join_rid_ = rid;
	join_step_ = JoinStep::First;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = http_base() +
			"/NWJoin.dll?needexpkey=jop_2_key2err.htm&success=jop_2_join.joi&failure=jop_2_main.htm"
			"&relay=jop_2_relay.htm&msgbase=jop_2_msg.htm&nodb=jop_2_nodb.htm&pfid=28&mode=Login&rid=" +
			std::to_string(rid);
	req.headers = request_headers(false);
	return join_need(std::move(req));
}

// [orig: on_join_request_completed()] — FIRST reads NWJOINSESSIONTAG, SECOND parses the .joi.
JoinResult LobbyHttpFlow::on_join_response(bool transport_ok, int code,
                                           const std::vector<std::string> &response_headers,
                                           const std::vector<uint8_t> &body) {
	const JoinStep step = join_step_;
	if (!transport_ok || code != 200) {
		join_step_ = JoinStep::Idle;
		const std::string message = extract_message(body);
		if (!message.empty()) return join_fail(message);
		return join_fail("join HTTP failed (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers);

	switch (step) {
		case JoinStep::First: {
			const std::string *tag = jar_.find("NWJOINSESSIONTAG");
			join_step_ = JoinStep::Second;
			HttpRequestSpec req;
			req.valid = true;
			req.method = HttpMethod::Get;
			req.url = http_base() + "/NWJoin.dll?rid=" + std::to_string(join_rid_) +
					((tag && !tag->empty()) ? ("&tag=" + *tag) : std::string());
			req.headers = request_headers(false);
			return join_need(std::move(req));
		}
		case JoinStep::Second: {
			join_step_ = JoinStep::Idle;
			const JoiConnection conn = parse_joi_connection_string(to_string_body(body));
			if (!conn.ok) {
				const std::string message = extract_message(body);
				if (!message.empty()) return join_fail(message);
				return join_fail("join: no connection string in .joi");
			}
			const int port = std::atoi(conn.host_port.c_str());
			if (port <= 0 || port > 65535) return join_fail("join: bad host port");
			JoinResult r;
			r.kind = JoinResult::Kind::Resolved;
			r.host_ip = conn.host_ip;
			r.host_port = static_cast<uint16_t>(port);
			r.ln = conn.ln;   // nonzero: dial the LAN-discovered endpoint, report "Lan"
			r.ni = conn.ni;   // the proxy-join triple, verbatim
			r.np = conn.np;
			r.bk = conn.bk;
			r.app_id = conn.app_id;  // the game-session APPID join token (decoded CK)
			// The PUB* identity cookies the authenticated login/NWJoin set, packed
			// as the CD blob the 0x00 JOIN relays (host code 23).
			r.cd_cookie = jar_.build_prefixed_blob("PUB");
			opennova::io::logf(opennova::io::LogLevel::kInfo,
					"joi: CD identity cookie = %zu bytes from %zu PUB* cookie(s)",
					r.cd_cookie.size(),
					[&] {
						size_t n = 0;
						for (const std::string &nm : jar_.names())
							if (nm.rfind("PUB", 0) == 0) ++n;
						return n;
					}());
			return r;
		}
		case JoinStep::Idle:
		default:
			join_step_ = JoinStep::Idle;
			return join_fail("join response with no pending step");
	}
}

} // namespace opennova
