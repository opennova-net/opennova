#include "novaworld/http_flow.h"

#include <cctype>
#include <cstdlib> // std::atoi

#include <io/strutil.h>

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
std::string strip_edges(const std::string &s) { return opennova::strutil::trim(s); }
std::string to_string_body(const std::vector<uint8_t> &body) {
	return body.empty() ? std::string() : std::string(reinterpret_cast<const char *>(body.data()), body.size());
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

// [orig: nova_world_client.cpp http_base()] — three derivation tiers.
std::string LobbyHttpFlow::http_base() const {
	const bool templated = !ctx_.startup_url.empty() && ctx_.startup_url.find("[domainname]") != std::string::npos;
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

// [orig: resolve_startup_url()] — fill the [domainname]/[VER1]/[VER2]/[CC]/[GT] template.
std::string LobbyHttpFlow::resolve_startup_url() const {
	std::string su = ctx_.startup_url;
	if (su.empty() || su.find("[domainname]") == std::string::npos) return su; // concrete (OpenNova) pass-through
	su = replace_all(su, "[domainname]", ctx_.web_domain);
	su = replace_all(su, "[VER1]", "3");
	su = replace_all(su, "[VER2]", "2345");
	std::string cc = "us"; // [CC] = ISO country from the OS locale (injected)
	const std::size_t us = ctx_.locale.find("_");
	if (us != std::string::npos) {
		const std::string region = to_lower(ctx_.locale.substr(us + 1));
		if (!region.empty()) cc = region;
	}
	su = replace_all(su, "[CC]", cc);
	su = replace_all(su, "[GT]", "jop:cus2");
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

// [orig: send_login_post()] — the 13-field LoginFormField list, byte-preserved.
HttpRequestSpec LobbyHttpFlow::login_post_request() {
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
	login_step_ = LoginStep::Post;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Post;
	req.url = http_base() + "/NWLogin.dll";
	req.headers = request_headers(true);
	req.body = build_login_post_body(epask_, fields);
	return req;
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
	(void)body;
	const LoginStep step = login_step_;
	if (!transport_ok || code != 200) {
		login_step_ = LoginStep::Idle;
		return login_fail("login HTTP failed (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers); // store Set-Cookie BEFORE branching

	switch (step) {
		case LoginStep::Prepare: {
			const std::string *epask = jar_.find("EPASK");
			if (epask == nullptr || epask->empty()) {
				login_step_ = LoginStep::Idle;
				return login_fail("server issued no EPASK cookie");
			}
			try {
				epask_ = epask_from_string(*epask);
			} catch (const std::exception &e) {
				login_step_ = LoginStep::Idle;
				return login_fail(std::string("bad EPASK bundle: ") + e.what());
			}
			seed_identity_cookies();
			const bool templated =
					!ctx_.startup_url.empty() && ctx_.startup_url.find("[domainname]") != std::string::npos;
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
			return login_need(login_post_request());
		}
		case LoginStep::NwStart:
			return login_need(login_post_request());
		case LoginStep::Post: {
			const std::string *tag = jar_.find("LOGINSESSIONTAG");
			if (tag == nullptr || tag->empty()) {
				login_step_ = LoginStep::Idle;
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
			if (!conn.ok) return join_fail("join: no connection string in .joi");
			const int port = std::atoi(conn.host_port.c_str());
			if (port <= 0 || port > 65535) return join_fail("join: bad host port");
			JoinResult r;
			r.kind = JoinResult::Kind::Resolved;
			r.host_ip = conn.host_ip;
			r.host_port = static_cast<uint16_t>(port);
			return r;
		}
		case JoinStep::Idle:
		default:
			join_step_ = JoinStep::Idle;
			return join_fail("join response with no pending step");
	}
}

} // namespace opennova
