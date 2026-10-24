#include <net/novaworld/http_flow.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <net/novaworld/browser_cache.h>
#include <net/novaworld/registration_url.h>
#include <net/title/title_identity.h>

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

// The page's <TITLE> text ("" when it has none).
std::string page_title(const std::string &html) {
	const std::string lower = to_lower(html);
	const std::size_t open = lower.find("<title");
	if (open == std::string::npos) return std::string();
	const std::size_t begin = lower.find('>', open);
	if (begin == std::string::npos) return std::string();
	const std::size_t end = lower.find("</title", begin + 1);
	return html.substr(begin + 1, end == std::string::npos ? std::string::npos : end - begin - 1);
}

// The locale three the gate reply's processing and the Cookie var-list's overlay write only with
// mpextractextendedmetricinformation set (D-NET-NEW-L9-32).
bool is_locale_identity_var(const std::string &name) {
	return strutil::iequals(name, "CountryName") || strutil::iequals(name, "Language") ||
	       strutil::iequals(name, "TimeZoneBias");
}

// The menu browser's markup white space: space, tab, LF, CR.
// [orig: D3DX_IsWhitespace @0x651A60]
bool html_space(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// One tag's attributes as the browser's attribute parser reads them, in the tag's order, from
// `cur` (just past the tag name). Each is a name of alphanumerics and '-' (the CRT isalnum of the
// sign-extended byte, cp1252's), then '=' and a value: a quoted one to its closing quote, else
// one to the next white space or '>'; no '=' leaves the value empty. White space and every quote
// after a value are passed over. The parse stops at a '>' where a name would start or at an empty
// name; only a '>' reached after a value moves `cur` to it. Retail's quoted and unquoted scans run
// on past the document's end; here they stop there.
// [orig: XML_ParseAttributes @0x655410 - the white space @0x655431..0x655452, the '>' test
//  @0x655454, the name's isalnum / '-' @0x655464 / @0x655470, an empty name @0x65547E, '='
//  @0x6554A2 (none: the empty value @0x6554A9), the white space after it @0x6554CC..0x6554EF, the
//  quoted value @0x6554F4..0x65553B, the unquoted one @0x655510..0x655527, the skip after a value
//  @0x655561..0x655579, the node linked at the list's head @0x655586..0x655589, the cursor at the
//  '>' @0x65558B..0x655598]
std::vector<std::pair<std::string, std::string>> html_tag_attributes(const std::string &html, std::size_t &cur) {
	const auto at = [&html](std::size_t i) { return i < html.size() ? html[i] : '\0'; };
	const auto name_char = [](char c) {
		return c == '-' || cp1252_isalpha_sign_extended(c) ||
		       cp1252_isdigit(static_cast<uint8_t>(c));
	};
	std::vector<std::pair<std::string, std::string>> attributes;
	std::size_t p = cur;
	for (;;) {
		while (html_space(at(p))) ++p;
		if (at(p) == '>') break;
		const std::size_t name_at = p;
		while (name_char(at(p))) ++p;
		if (p == name_at) break;
		std::string name = html.substr(name_at, p - name_at);
		std::string value;
		if (at(p) == '=') {
			++p;
			while (html_space(at(p))) ++p;
			const char quote = at(p);
			if (quote == '"' || quote == '\'') {
				const std::size_t value_at = ++p;
				while (p < html.size() && html[p] != quote) ++p;
				value = html.substr(value_at, p - value_at);
			} else {
				const std::size_t value_at = p;
				while (p < html.size() && html[p] != '>' && !html_space(html[p])) ++p;
				value = html.substr(value_at, p - value_at);
			}
		}
		while (html_space(at(p)) || at(p) == '"' || at(p) == '\'') ++p;
		attributes.emplace_back(std::move(name), std::move(value));
		if (at(p) == '>') {
			cur = p;
			break;
		}
	}
	return attributes;
}

// An attribute's value by its name, case-insensitively: the list is built head first, so the
// lookup's first match is the tag's LAST attribute of that name. Null when the tag has none.
// [orig: sub_6555C0 @0x6555C0 - the stricmp walk @0x6555EE]
const std::string *html_attribute(
		const std::vector<std::pair<std::string, std::string>> &attributes, const char *name) {
	for (auto it = attributes.rbegin(); it != attributes.rend(); ++it)
		if (strutil::iequals(it->first, name)) return &it->second;
	return nullptr;
}

// The browser's element table, in its order (the first whose name the tag's prefixes wins).
// [orig: the table @0x84C148, 12-byte rows {element, handler, name}]
constexpr const char *kHtmlElements[] = {"meta", "TITLE", "FORM", "IB3_TEMPLATE", "IB3_SUBST",
		"IB3_FORM", "IB3_DEFINE", "IB3_COND", "IB3_GLOBAL", "INPUT", "BR", "P"};

} // namespace

namespace {

// The browser's tag walk over a page: each tag the element table takes, `on_tag` given the
// element's table name and, for an opening tag, its attributes (a closing tag's handler does
// nothing). The page is read as a C string: nothing past a NUL.
// [orig: CUIHTMLParser_ParseTag @0x655E20 - the white space past '<' @0x655E40..0x655E50,
//  '/' @0x655E5C, the name's end at '>', '!' or white space @0x655E83..0x655E9B, the table
//  compare (String_CompareNoCaseN called @0x655EC4: strnicmp over the name's length, so a
//  name that prefixes an element's, the empty one included, is that element),
//  XML_ParseAttributes called @0x655EFC, the handler called @0x655F8B, the skip to the next
//  '>' @0x655FFC..0x65600F and past it @0x65601B]
template <typename OnTag>
void walk_page_tags(const std::string &page, OnTag &&on_tag) {
	const std::string html = page.substr(0, page.find('\0'));
	std::size_t cur = 0;
	while (cur < html.size()) {
		if (html[cur] != '<') {
			++cur;
			continue;
		}
		++cur;
		while (html_space(html[cur])) ++cur;
		const bool closing = html[cur] == '/';
		if (closing) {
			++cur;
			while (html_space(html[cur])) ++cur;
		}
		std::size_t name_end = cur;
		while (name_end < html.size() && html[name_end] != '>' && html[name_end] != '!' &&
		       !html_space(html[name_end]))
			++name_end;
		const std::string_view name(html.data() + cur, name_end - cur);
		const char *element = nullptr;
		for (const char *candidate : kHtmlElements) {
			const std::string_view full(candidate);
			if (name.size() <= full.size() && strutil::iequals(name, full.substr(0, name.size()))) {
				element = candidate;
				break;
			}
		}
		if (element != nullptr) {
			cur = name_end;
			if (!closing) on_tag(element, html_tag_attributes(html, cur));
		}
		while (cur < html.size() && html[cur] != '>') ++cur;
		if (cur < html.size()) ++cur;
	}
}

// The element table's rows by name.
const char *const kMetaElement = kHtmlElements[0];
const char *const kFormElement = kHtmlElements[2];
const char *const kTemplateElement = kHtmlElements[3];
const char *const kSubstElement = kHtmlElements[4];

} // namespace

// See http_flow.h.
std::string meta_refresh_url(const std::string &page) {
	std::string url;
	walk_page_tags(page, [&url](const char *element,
			const std::vector<std::pair<std::string, std::string>> &attributes) {
		// META's handler: an HTTP-EQUIV of REFRESH, whole, and a CONTENT. The URL is sought from
		// the CONTENT's first ';' as "URL=", one leading quote is skipped, and the copy runs to a
		// quote or the end; each such tag overwrites the last. With no ';' or no "URL=", retail
		// copies its uninitialized buffer (a retail bug): none here.
		// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620 - the HTTP-EQUIV lookup called
		//  @0x65565F, String_CompareNoCase against "REFRESH" called @0x655676, the CONTENT
		//  lookup called @0x655693, the ';' scan @0x6556B0..0x6556BC, String_CompareNoCaseN
		//  against "URL=" called @0x6556CD, one quote skipped @0x6556EE..0x6556F6, the copy
		//  to a quote or the end @0x655701..0x655717, the uninitialized buffer's copy on the
		//  other paths @0x655733]
		if (element != kMetaElement) return;
		const std::string *equiv = html_attribute(attributes, "HTTP-EQUIV");
		const std::string *content = html_attribute(attributes, "CONTENT");
		if (equiv == nullptr || !strutil::iequals(*equiv, "REFRESH") || content == nullptr) return;
		url.clear();
		const std::size_t semicolon = content->find(';');
		std::size_t u = semicolon == std::string::npos ? content->size() : semicolon;
		while (u < content->size() &&
		       !strutil::starts_with_icase(std::string_view(*content).substr(u), "URL="))
			++u;
		if (u < content->size()) {
			std::size_t begin = u + 4;
			if (begin < content->size() && ((*content)[begin] == '"' || (*content)[begin] == '\''))
				++begin;
			std::size_t end = begin;
			while (end < content->size() && (*content)[end] != '"' && (*content)[end] != '\'') ++end;
			url = content->substr(begin, end - begin);
		}
	});
	return url;
}

// See http_flow.h.
std::optional<std::string> page_template_file(const std::string &page) {
	std::optional<std::string> file;
	walk_page_tags(page, [&file](const char *element,
			const std::vector<std::pair<std::string, std::string>> &attributes) {
		if (element != kTemplateElement) return;
		if (const std::string *value = html_attribute(attributes, "FILE")) file = *value;
	});
	return file;
}

namespace {

// The page NWStart answered as OpenNova's panel shows it (D-NET-NEW-L9-18's page model): the lobby
// page sets @GSB_SERVER@ (an IB3_SUBST, the address its template's GLB_LOADANDPING action loads
// the list from), the sign-in page carries the form whose ACTION is NWLogin.dll; any other page
// (a message, the update or junction pages) is neither.
enum class StartPageKind { Lobby, SignIn, Other };
StartPageKind start_page_kind(const std::string &page) {
	bool lobby = false;
	bool sign_in = false;
	walk_page_tags(page, [&](const char *element,
			const std::vector<std::pair<std::string, std::string>> &attributes) {
		if (element == kSubstElement) {
			const std::string *name = html_attribute(attributes, "NAME");
			if (name != nullptr && strutil::iequals(*name, "@GSB_SERVER@")) lobby = true;
		} else if (element == kFormElement) {
			const std::string *action = html_attribute(attributes, "ACTION");
			if (action == nullptr) return;
			const std::string path = action->substr(0, action->find('?'));
			const std::size_t slash = path.rfind('/');
			if (strutil::iequals(slash == std::string::npos ? path : path.substr(slash + 1), "NWLogin.dll"))
				sign_in = true;
		}
	});
	return lobby ? StartPageKind::Lobby : sign_in ? StartPageKind::SignIn : StartPageKind::Other;
}

EnterResult enter_fail(std::string reason) {
	EnterResult r;
	r.kind = EnterResult::Kind::Failed;
	r.reason = std::move(reason);
	return r;
}

LoginResult login_need(HttpRequestSpec req) {
	req.cache_bust = req.method == HttpMethod::Get;
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
	req.cache_bust = true;
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

ProxyRendezvousConfig proxy_rendezvous_from_join(const JoinResult &resolved) {
	ProxyRendezvousConfig proxy;
	// [orig: CNapiGameSession_InitTransportConnection @0x4c9e10 - inet_addr(NI) and
	//  _atol of NP @0x4ca08a, _atol of BK @0x4ca05b; the relay is the NK endpoint, its
	//  port's atol @0x4ca0b9 already taken by the .joi parse (http_login.cpp)]
	proxy.node_addr = proxy_inet_addr(resolved.ni);
	proxy.node_port = static_cast<uint32_t>(io::retail_atol(resolved.np.c_str()));
	proxy.cookie = static_cast<uint32_t>(io::retail_atol(resolved.bk.c_str()));
	proxy.relay_addr = proxy_inet_addr(resolved.host_ip);
	proxy.relay_port = resolved.host_port;
	return proxy;
}

HostKeyResult host_need(HttpRequestSpec req) {
	req.cache_bust = true;
	HostKeyResult r;
	r.kind = HostKeyResult::Kind::NeedRequest;
	r.request = std::move(req);
	return r;
}
HostKeyResult host_fail(std::string reason) {
	HostKeyResult r;
	r.kind = HostKeyResult::Kind::Failed;
	r.reason = std::move(reason);
	return r;
}

void LobbyHttpFlow::reset() {
	jar_ = CookieJar{};
	host_step_ = HostStep::Idle;
	host_hops_ = 0;
	globals_.clear();
	epask_ = EpaskParams{};
	login_step_ = LoginStep::Idle;
	login_poll_count_ = 0;
	login_user_.clear();
	login_pass_.clear();
	join_step_ = JoinStep::Idle;
	enter_step_ = EnterStep::Idle;
	sign_in_page_shown_ = false;
	identity_seeded_ = false;
	login_page_url_.clear();
	join_page_url_.clear();
	host_page_url_.clear();
	enter_page_url_.clear();
	// A template GET the reset cancels ends with nothing stored: the next fetch requests it again.
	for (const std::string *url : {&login_template_.url, &join_template_.url, &host_template_.url,
			&enter_template_.url}) {
		if (!url->empty()) process_browser_cache().complete(*url, false);
	}
	login_template_ = {};
	join_template_ = {};
	host_template_ = {};
	enter_template_ = {};
}

// A processed page's template goes through the process's browser cache: a template the cache
// holds is served with no request, and the leg's next goes out at once; any other is requested
// first, with the page's own URL as its base, and the leg's next waits for its answer.
// [orig: UI_ProcessWebResponseContent @0x63D450 - the template test @0x63D63C,
//  CUIBrowser_AsyncFetchURLContent called @0x63D6D1 over the scene's +0x34 (the page's URL, copied
//  @0x63D599) and the node's +0xC (the FILE, read @0x63D6BE)]
template <typename Result>
Result LobbyHttpFlow::behind_template(TemplateHold<Result> &hold, const std::string &page_url,
		const std::vector<uint8_t> &page, Result next) {
	const std::optional<std::string> file = page_template_file(to_string_body(page));
	if (!file || page_url.empty()) return next;
	const std::string url = browser_combine_url(page_url, *file);
	if (!process_browser_cache().fetch(url)) return next;
	hold.url = url;
	hold.next = std::move(next);
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = url;
	req.headers = request_headers();
	req.cache_bust = true;
	Result r;
	r.kind = Result::Kind::NeedRequest;
	r.request = std::move(req);
	return r;
}

// The template's answer: its Set-Cookie lines go to the jar as every answer's do, and the cache
// files its end, a stored body (a 2xx with bytes, the window the browser writes its file in) held
// for the process.
// [orig: HTTP_ResponseHandler @0x659480 - the SET-COOKIE scan @0x65961B, called for every answer
//  by the pump @0x6599F3; HTTP_DownloadCompletionCallback @0x64DB20 - the 2xx window
//  @0x64DB8D..0x64DBB3, the body's tests @0x64DBC1 / @0x64DBCA]
template <typename Result>
std::optional<Result> LobbyHttpFlow::template_answered(TemplateHold<Result> &hold, bool transport_ok,
		int code, const std::vector<std::string> &response_headers, const std::vector<uint8_t> &body) {
	if (!hold.next) return std::nullopt;
	if (transport_ok) merge_response_cookies(response_headers);
	const bool window = (code >= 200 && code <= 206) || code == 100 || code == 101;
	process_browser_cache().complete(hold.url, transport_ok && window && !body.empty());
	std::optional<Result> next = std::move(hold.next);
	hold = {};
	return next;
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
// matched case-insensitively, from: the gate-supplied domain, the title's
// version pair (JO's "3"/"2345"), the OS country code, the session's gate tag
// (JO's "jop:cus2", or gt.ssc's) and the title's product code (JO's "jop").
// [orig: Mission_DeobfuscateDescription @0x4cdaa0 — NapiUtil_ReplaceAllCaseInsensitive
//  "[DOMAINNAME]" @0x4cdb88, "[VER1]" @0x4cdb9f, "[VER2]" @0x4cdbb6, "[CC]" @0x4cdbcd,
//  "[GT]" @0x4cdbe4, "[PRODUCTCODE]" @0x4cdbfe; called from
//  CNapiGameSession_OnNovaWorldConnected @0x4d1665]
std::string LobbyHttpFlow::resolve_startup_url() const {
	std::string su = ctx_.startup_url;
	if (su.empty() || !startup_url_is_templated(su)) return su; // concrete (OpenNova) pass-through
	if (ctx_.title == nullptr) return std::string();
	su = replace_all_icase(su, "[DOMAINNAME]", ctx_.web_domain);
	su = replace_all_icase(su, "[VER1]", ctx_.title->ver1);
	su = replace_all_icase(su, "[VER2]", ctx_.title->ver2);
	std::string cc = "us"; // [CC] = ISO country from the OS locale (injected)
	const std::size_t us = ctx_.locale.find("_");
	if (us != std::string::npos) {
		const std::string region = to_lower(ctx_.locale.substr(us + 1));
		if (!region.empty()) cc = region;
	}
	su = replace_all_icase(su, "[CC]", cc);
	su = replace_all_icase(su, "[GT]", ctx_.gate_tag);
	su = replace_all_icase(su, "[PRODUCTCODE]", ctx_.title->product_code);
	return su;
}

// The menu browser's GET headers past the transport's request line and Host, in its order: the
// image-first Accept, Accept-Language, the IBrowse User-Agent, Pragma, then ONE "Cookie:" header
// per jar entry (each cookie its own "Cookie: name=value;" line, not a merged header). No
// If-Modified-Since: the browser writes it only from a request's date word, which its cache sets
// only on the path that serves a stored download with no request, so no request carries one
// (D-NET-NEW-L9-31; browser_cache.h).
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - "Host: %s" @0x658959, the Accept @0x658969,
//  Accept-Language @0x658979, User-Agent @0x658989, the If-Modified-Since date test
//  @0x658994..0x65899C, Pragma @0x6589D5, the Cookie lines @0x658A44..0x658AC0;
//  CUIBrowser_AsyncFetchURLContent @0x64DE80 - a new entry's date word zeroed @0x64E0A6 and handed
//  to HTTP_RequestEnqueue (`mov eax, [esi+10h]` @0x64E105), the stored file's date read into it
//  @0x64E1B5 on the served path]
std::vector<std::string> LobbyHttpFlow::request_headers() const {
	std::vector<std::string> h{"Accept: image/jpeg, image/x-pcx, image/pcx, image/png, text/html",
			"Accept-Language: en", "User-Agent: NovaLogic IBrowse 2.0", "Pragma: no-cache"};
	for (const std::string &line : jar_.cookie_header_lines()) {
		h.push_back("Cookie: " + line);
	}
	return h;
}

// The form post's headers past its request line and Host, in its order: Accept, Accept-Language,
// the User-Agent, the body's Content-Length, the lower-case-t Content-type, then the Cookie lines
// (D-NET-NEW-L9-31). The transport writes no Content-Length of its own past this one.
// [orig: GopherWebWidget_SendHttpPost @0x658B30 - "Host: %s" @0x658BAE, "Accept: text/html"
//  @0x658BBE, Accept-Language @0x658BCE, User-Agent @0x658BDE, "Content-Length: %d" @0x658BEF,
//  "Content-type: application/x-www-form-urlencoded" @0x658BFF, the Cookie lines from its jar
//  read @0x658C67]
std::vector<std::string> LobbyHttpFlow::post_headers(std::size_t content_length) const {
	std::vector<std::string> h{"Accept: text/html", "Accept-Language: en",
			"User-Agent: NovaLogic IBrowse 2.0", "Content-Length: " + std::to_string(content_length),
			"Content-type: application/x-www-form-urlencoded"};
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

// The identity set as the jar takes it, ahead of every cookie a page sets. Two writers fill it
// before the web lobby's first request, so the first request carries it and every later Cookie
// list (the headers, the UDP statements' "Cookie" var-list) leads with it (D-NET-NEW-L9-26):
//  - the gate reply's processing: with the junction word set, USEJUNCTION "1" and an empty
//    JUNCTIONCODE first; with mpextractextendedmetricinformation set (cfg +0x33C), CountryName,
//    Language and TimeZoneBias; then MyInstalledExpBits, always (D-NET-NEW-L9-32);
//  - the connection-up's six, NWUID .. NWHWI (the live nwuid for the empty NWUID entry).
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the junction word's test @0x4CF71B,
//  CUIBrowser_AddFormField USEJUNCTION ("%ld" of 1) @0x4CF74E, JUNCTIONCODE "" @0x4CF76E, the
//  +0x33C test @0x4CF773, CountryName @0x4CF796, Language @0x4CF7AB, TimeZoneBias @0x4CF7DE,
//  MyInstalledExpBits @0x4CF811; CNapiGameSession_OnNovaWorldConnected @0x4D1570 - NWUID (the call
//  @0x4D16A9), NWCDKIID @0x4D16ED, NWCDKIIDEXP1 @0x4D1702, NWPSSK @0x4D1724, NWUSID @0x4D1746,
//  NWHWI @0x4D17AA; CNapiSession_ReadLocaleInfo @0x4CE390 - the jar's walk in its order (the call
//  @0x4CE3E5), CNapiVarList_SetOrCreate per entry (the call @0x4CE443)]
std::vector<std::pair<std::string, std::string>> LobbyHttpFlow::identity_seed() const {
	std::vector<std::pair<std::string, std::string>> seed;
	if (ctx_.use_junction != 0) {
		seed.emplace_back("USEJUNCTION", "1");
		seed.emplace_back("JUNCTIONCODE", "");
	}
	for (const auto &kv : ctx_.identity_vars) {
		if (!ctx_.extended_metrics && is_locale_identity_var(kv.first)) continue;
		seed.emplace_back(kv.first,
				kv.first == "NWUID" && kv.second.empty() ? ctx_.server_nwuid : kv.second);
	}
	return seed;
}

// The two writers run once per connect, ahead of the web lobby's first request: a later entry or
// login of the same session finds the jar as the pages left it (a page's USEJUNCTION stays).
void LobbyHttpFlow::seed_identity_cookies() {
	if (identity_seeded_) return;
	identity_seeded_ = true;
	for (const auto &kv : identity_seed()) jar_.set(kv.first, kv.second);
}

// The Cookie var-list reads the browser jar at use, then overlays the locale three when
// mpextractextendedmetricinformation is set (none at all when it is clear, D-NET-NEW-L9-32).
// The identity seed fills the pre-login case before seed_identity_cookies runs; once present,
// HTTP-issued cookie values (NWPF/NWPF2/account/ticket) win.
// [orig: CNapiSession_ReadLocaleInfo @0x4CE390 - the jar's walk (CUIBrowser_QueryMatchingCookies
//  called @0x4CE3E5), the +0x33C test @0x4CE44E / @0x4CE455, CountryName @0x4CE46B, Language
//  @0x4CE480, TimeZoneBias @0x4CE4AC]
std::vector<std::pair<std::string, std::string>> LobbyHttpFlow::session_cookie_vars() const {
	CookieJar cookies = jar_;
	for (const auto &kv : identity_seed()) {
		if (!cookies.find(kv.first) || is_locale_identity_var(kv.first)) cookies.set(kv.first, kv.second);
	}
	std::vector<std::pair<std::string, std::string>> vars;
	vars.reserve(cookies.names().size());
	for (const auto &name : cookies.names()) vars.emplace_back(name, *cookies.find(name));
	return vars;
}

// =============================== the menu's entry ===============================

EnterResult LobbyHttpFlow::enter_request(HttpRequestSpec req) {
	req.cache_bust = true;
	enter_page_url_ = req.url;
	EnterResult r;
	r.kind = EnterResult::Kind::NeedRequest;
	r.request = std::move(req);
	return r;
}

// The NovaWorld menu's entry opens the gate's startup URL, its tokens filled, whatever the session
// is: a re-entry after a match with the session still verified opens it as the first entry did.
// The jar stays as the session's connect and pages left it.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 9: CMissionInfo_ProcessDescription
//  called @0x559291 (the startup URL's tokens into g_NovaWorldWebURL), UIScene_OpenUrl called
//  @0x5592ED]
EnterResult LobbyHttpFlow::enter() {
	if (enter_active()) return enter_fail("the NovaWorld lobby is already opening");
	if (ctx_.title == nullptr) return enter_fail("no title identity to open the NovaWorld lobby with");
	const std::string url = resolve_startup_url();
	if (url.empty()) return enter_fail("no gate startup_url yet — connect first");
	seed_identity_cookies();
	sign_in_page_shown_ = false;
	enter_step_ = EnterStep::Prepare;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = url;
	req.headers = request_headers();
	return enter_request(std::move(req));
}

EnterResult LobbyHttpFlow::on_enter_response(bool transport_ok, int code,
                                             const std::vector<std::string> &response_headers,
                                             const std::vector<uint8_t> &body) {
	if (std::optional<EnterResult> held =
			template_answered(enter_template_, transport_ok, code, response_headers, body))
		return std::move(*held);
	const EnterStep step = enter_step_;
	if (step == EnterStep::Idle) return enter_fail("entry response with no pending step");
	if (!transport_ok || code != 200) {
		enter_step_ = EnterStep::Idle;
		const std::string message = extract_message(body);
		if (!message.empty()) return enter_fail(message);
		return enter_fail("the NovaWorld lobby did not open (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers);
	capture_globals(body);
	const std::string page_url = enter_page_url_;

	EnterResult next = [&]() -> EnterResult {
		if (step == EnterStep::Prepare) {
			// The start page refreshes to NWStart, which the browser follows (relay_refresh_url).
			const std::string refresh = relay_refresh_url(body);
			if (refresh.empty()) {
				enter_step_ = EnterStep::Idle;
				const std::string message = extract_message(body);
				return enter_fail(message.empty() ? std::string("the start page names no next page")
				                                  : message);
			}
			enter_step_ = EnterStep::Start;
			HttpRequestSpec req;
			req.valid = true;
			req.method = HttpMethod::Get;
			req.url = refresh;
			req.headers = request_headers();
			return enter_request(std::move(req));
		}
		enter_step_ = EnterStep::Idle;
		switch (start_page_kind(to_string_body(body))) {
			case StartPageKind::Lobby: {
				EnterResult r;
				r.kind = EnterResult::Kind::Lobby;
				const std::string *nh = jar_.find("NWHANDLE");
				r.nwhandle = nh != nullptr ? *nh : std::string();
				return r;
			}
			case StartPageKind::SignIn: {
				sign_in_page_shown_ = true;
				EnterResult r;
				r.kind = EnterResult::Kind::SignIn;
				return r;
			}
			case StartPageKind::Other:
			default: {
				const std::string message = extract_message(body);
				return enter_fail(message.empty()
						? std::string("NovaWorld answered neither its lobby nor its sign-in page")
						: message);
			}
		}
	}();
	// The page's template ahead of what follows it: the lobby page's ahead of the list load (the LN
	// S1 / S1b / S1d retail re-entries fetch no main.mnx: the login earlier in the process stored
	// it), the sign-in page's ahead of the panel's sign-in.
	return behind_template(enter_template_, page_url, body, std::move(next));
}

// =============================== EPASK login ===============================

LoginResult LobbyHttpFlow::login_request(HttpRequestSpec req) {
	login_page_url_ = req.url;
	return login_need(std::move(req));
}

LoginResult LobbyHttpFlow::login(const std::string &username, const std::string &password) {
	if (login_active()) return login_fail("login already in flight");
	if (ctx_.title == nullptr) return login_fail("no title identity to log in with");
	const std::string refusal = title_identity_refusal(*ctx_.title, TitleIdentityPart::WebFlow);
	if (!refusal.empty()) return login_fail(refusal);
	login_user_ = username;
	login_pass_ = password;
	login_poll_count_ = 0;
	// The sign-in page an entry landed on is the page shown: its submit posts its form under the
	// EPASK the start page set, with no second open of the startup URL.
	if (sign_in_page_shown_) {
		sign_in_page_shown_ = false;
		const std::string epask_refusal = take_epask({});
		if (!epask_refusal.empty()) return login_fail(epask_refusal);
		return login_post();
	}
	const std::string prepare_url = resolve_startup_url();
	if (prepare_url.empty()) return login_fail("no gate startup_url yet — connect first");
	seed_identity_cookies();
	login_step_ = LoginStep::Prepare;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = prepare_url;
	req.headers = request_headers();
	return login_request(std::move(req));
}

// The jar's EPASK bundle, which the sign-in page's submit encrypts its fields under, with the
// modexp gate retail applies at that submit (EPASK_ModexpEncrypt @0x66668a returns -1 for modulus
// <= 258 or a non-positive exponent): refused here, before any request, rather than at the
// post's body.
std::string LobbyHttpFlow::take_epask(const std::vector<uint8_t> &page) {
	const std::string *epask = jar_.find("EPASK");
	if (epask == nullptr || epask->empty()) {
		const std::string message = extract_message(page);
		return message.empty() ? std::string("server issued no EPASK cookie") : message;
	}
	std::string epask_error;
	if (!epask_from_string(*epask, epask_, &epask_error)) return "bad EPASK bundle: " + epask_error;
	if (epask_.modulus <= 258u || epask_.exponent == 0u)
		return "bad EPASK bundle: params rejected (modulus <= 258 or exponent 0)";
	return std::string();
}

// [orig: send_login_post()] — the 13-field LoginFormField list, byte-preserved. A body
// that does not build (params the modexp gate rejects, which the Prepare leg already
// refuses) fails the login instead of posting.
LoginResult LobbyHttpFlow::login_post() {
	const TitlePages &pages = *ctx_.title->pages;
	std::vector<LoginFormField> fields = {
		{"EPASK", epask_to_string(epask_), false}, // echoed bundle, PLAINTEXT
		{"NAME", login_user_, true},
		{"PASSWORD", login_pass_, true},
	};
	// The hidden fields as the client posts them, the reverse of the page's order (JO's captured
	// post): a page whose form ends with charglsvssrequest (DFX2's) posts it first.
	if (pages.glsvss_request != nullptr)
		fields.push_back({"charglsvssrequest", pages.glsvss_request, true});
	const std::vector<LoginFormField> hidden = {
		{"rememberlogindata", "", false},
		{"rememberlogin", "0", true},
		{"pfid", std::to_string(*ctx_.title->pfid), true},
		{"needtoagree", pages.need_to_agree, true},
		{"nodb", pages.no_db, true},
		{"relay", pages.relay, true},
		{"msgbase", pages.msgbase, true},
		{"enterkey", pages.enter_key, true},
		{"failure", pages.login, true},
		{"success", pages.main, true},
	};
	fields.insert(fields.end(), hidden.begin(), hidden.end());
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
	req.headers = post_headers(body.size());
	req.body = std::move(body);
	return login_request(std::move(req));
}

// A relay page's next request: the menu browser follows the page's META refresh to the URL it
// names (the service's relay names the bare .dll, whose session tag rides the jar's cookie), so
// the login's polls and the join's second leg go where the page sends them, as the host leg's do
// (D-NET-NEW-L9-31). Empty when the page names none (meta_refresh_url, D-NET-NEW-L9-34).
// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620 - String_CompareNoCase against "REFRESH"
//  called @0x655676, String_CompareNoCaseN against "URL=" called @0x6556CD, a leading quote
//  skipped @0x6556EE..0x6556F6, the copy to a closing quote @0x655703 / @0x655707]
std::string LobbyHttpFlow::relay_refresh_url(const std::vector<uint8_t> &body) const {
	const std::string next = meta_refresh_url(to_string_body(body));
	return next.empty() ? std::string() : absolute_url(next);
}

LoginResult LobbyHttpFlow::on_login_response(bool transport_ok, int code,
                                             const std::vector<std::string> &response_headers,
                                             const std::vector<uint8_t> &body) {
	if (std::optional<LoginResult> held =
			template_answered(login_template_, transport_ok, code, response_headers, body))
		return std::move(*held);
	const LoginStep step = login_step_;
	if (!transport_ok || code != 200) {
		login_step_ = LoginStep::Idle;
		const std::string message = extract_message(body);
		if (!message.empty()) return login_fail(message);
		return login_fail("login HTTP failed (code " + std::to_string(code) + ")");
	}
	if (step == LoginStep::Idle) return login_fail("login response with no pending step");
	merge_response_cookies(response_headers); // store Set-Cookie BEFORE branching
	capture_globals(body);
	const std::string page_url = login_page_url_;

	LoginResult next = [&]() -> LoginResult {
		switch (step) {
			case LoginStep::Prepare: {
				const std::string refusal = take_epask(body);
				if (!refusal.empty()) {
					login_step_ = LoginStep::Idle;
					return login_fail(refusal);
				}
				// The start page refreshes to NWStart (JO's jop_2_start.htm, "0;"), which the menu
				// browser follows at its next frame, as the entry's open does (enter): the page's
				// refresh is the scene's next URL, opened once the frame's deadline test passes. A
				// start page that names no refresh is the page shown, whose form posts at once.
				// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620; UI_ProcessWebResponseContent
				//  @0x63D450 - the refresh to the scene's +0x3C @0x63D501, its delay to +0x40
				//  @0x63D507; CUIScene_EndFrame @0x63E600 - `cmp eax, [esi+40h]` against
				//  GetTickCount @0x63E626, UIScene_OpenUrl called @0x63E635]
				const std::string refresh = relay_refresh_url(body);
				if (refresh.empty()) return login_post();
				login_step_ = LoginStep::NwStart;
				HttpRequestSpec req;
				req.valid = true;
				req.method = HttpMethod::Get;
				req.url = refresh;
				req.headers = request_headers();
				return login_request(std::move(req));
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
				const std::string refresh = relay_refresh_url(body);
				if (refresh.empty()) {
					login_step_ = LoginStep::Idle;
					return login_fail("login: the answer is no relay page");
				}
				login_step_ = LoginStep::Poll;
				login_poll_count_ = 0;
				HttpRequestSpec req;
				req.valid = true;
				req.method = HttpMethod::Get;
				req.url = refresh;
				req.headers = request_headers();
				return login_request(std::move(req));
			}
			case LoginStep::Poll:
			default: {
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
				const std::string refresh = relay_refresh_url(body);
				if (refresh.empty() || ++login_poll_count_ >= kMaxLoginPolls) {
					login_step_ = LoginStep::Idle;
					return login_fail("login did not complete (no account handle)");
				}
				HttpRequestSpec req;
				req.valid = true;
				req.method = HttpMethod::Get;
				req.url = refresh;
				req.headers = request_headers();
				return login_request(std::move(req));
			}
		}
	}();
	// The page's template is fetched ahead of the leg's next: the sign-in page's ahead of the post,
	// the relay page's ahead of its refresh, the lobby page's ahead of the list load the login's
	// success starts (the LN S1 / S1b / S1c / S1d retail logins: login.mnx after NWStart, relay.mnx
	// after the POST's answer and ahead of the first poll, main.mnx after the last poll's answer and
	// ahead of the GSB).
	return behind_template(login_template_, page_url, body, std::move(next));
}

// =============================== GSB browser ===============================

// [orig: gsb_url()] — <base>/<the title's server list>?a=1 (JO's jop_2.gsb).
std::string LobbyHttpFlow::gsb_url() const {
	const std::string base = http_base();
	if (base.empty() || ctx_.title == nullptr || !ctx_.title->pages.has_value())
		return std::string();
	return base + "/" + ctx_.title->pages->server_list + "?a=1";
}

HttpRequestSpec LobbyHttpFlow::gsb_request() const {
	HttpRequestSpec req;
	const std::string url = gsb_url();
	if (url.empty()) return req; // invalid (no base yet)
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = url;
	req.headers = request_headers();
	req.cache_bust = true;
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
	if (ctx_.title == nullptr) return join_fail("no title identity to join with");
	const std::string refusal = title_identity_refusal(*ctx_.title, TitleIdentityPart::WebFlow);
	if (!refusal.empty()) return join_fail(refusal);
	if (http_base().empty()) return join_fail("no server base URL — connect first");
	join_step_ = JoinStep::First;
	const TitlePages &pages = *ctx_.title->pages;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	const std::string lan =
			pages.join_lan != nullptr ? std::string("lan=") + pages.join_lan + "&" : std::string();
	req.url = http_base() + "/NWJoin.dll?" + lan + "needexpkey=" + pages.key_error +
			"&success=" + pages.join + "&failure=" + pages.main + "&relay=" + pages.relay +
			"&msgbase=" + pages.msgbase + "&nodb=" + pages.no_db +
			"&pfid=" + std::to_string(*ctx_.title->pfid) + "&mode=Login&rid=" + std::to_string(rid);
	req.headers = request_headers();
	join_page_url_ = req.url;
	return join_need(std::move(req));
}

// [orig: on_join_request_completed()] — FIRST reads NWJOINSESSIONTAG, SECOND parses the .joi.
JoinResult LobbyHttpFlow::on_join_response(bool transport_ok, int code,
                                           const std::vector<std::string> &response_headers,
                                           const std::vector<uint8_t> &body) {
	if (std::optional<JoinResult> held =
			template_answered(join_template_, transport_ok, code, response_headers, body))
		return std::move(*held);
	const JoinStep step = join_step_;
	if (!transport_ok || code != 200) {
		join_step_ = JoinStep::Idle;
		// The .joi page's refresh is the browser's page alone: the join it started goes on.
		if (step == JoinStep::Page) {
			JoinResult settled;
			settled.kind = JoinResult::Kind::Settled;
			return settled;
		}
		const std::string message = extract_message(body);
		if (!message.empty()) return join_fail(message);
		return join_fail("join HTTP failed (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers);

	switch (step) {
		case JoinStep::First: {
			// The relay page refreshes to the second leg (relay_refresh_url), behind the page's
			// template (behind_template; the relay template a login's relay page fetched earlier in
			// the process is served from the cache: the LN S1b / S1d retail joiners fetch none here).
			const std::string page_url = join_page_url_;
			const std::string next = relay_refresh_url(body);
			if (next.empty()) {
				join_step_ = JoinStep::Idle;
				const std::string message = extract_message(body);
				if (!message.empty())
					return behind_template(join_template_, page_url, body, join_fail(message));
				return behind_template(join_template_, page_url, body,
						join_fail("join: the answer is no relay page"));
			}
			join_step_ = JoinStep::Second;
			HttpRequestSpec req;
			req.valid = true;
			req.method = HttpMethod::Get;
			req.url = next;
			req.headers = request_headers();
			join_page_url_ = req.url;
			return behind_template(join_template_, page_url, body, join_need(std::move(req)));
		}
		// The .joi page and its refresh's page name no template on any witnessed service (the LN
		// records fetch none after them), so their answers settle here.
		case JoinStep::Second: {
			join_step_ = JoinStep::Idle;
			const JoiConnection conn = parse_joi_connection_string(to_string_body(body));
			if (!conn.ok) {
				const std::string message = extract_message(body);
				if (!message.empty()) return join_fail(message);
				return join_fail("join: no connection string in .joi");
			}
			// The NK port is the CRT atol of the decoded tail (io::retail_atol; D-NET-384).
			// [orig: UI_EnumerateAndJoinSession @0x569F50 — atol(g_NkExtraBuf) @0x569FDD]
			const int port = io::retail_atol(conn.host_port.c_str());
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
			// The .joi page refreshes (JO's to jop_2_jend.htm, "10;"), and the menu browser follows
			// it at its next frame whatever the delay: the page's delay, in seconds, is stored where
			// the frame's test reads a GetTickCount deadline, so the test passes at once (the LN S1b
			// retail joiner's jend GET 0.25 s after the .joi; D-NET-NEW-L9-31).
			// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620 - the delay's strtol stored to
			//  the parser's +4 @0x655783; CUIHTMLParser_BuildContentNodeFromHTML @0x656080 - the
			//  copy @0x6563EB; UI_ProcessWebResponseContent @0x63D450 - the scene's +0x40 @0x63D507;
			//  CUIScene_EndFrame @0x63E600 - `cmp eax, [esi+40h]` against GetTickCount @0x63E626,
			//  UIScene_OpenUrl called @0x63E635]
			const std::string next = relay_refresh_url(body);
			if (!next.empty()) {
				join_step_ = JoinStep::Page;
				r.request.valid = true;
				r.request.method = HttpMethod::Get;
				r.request.url = next;
				r.request.headers = request_headers();
				r.request.cache_bust = true;
			}
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
		case JoinStep::Page: {
			// The refresh's page is shown; its cookies are in the jar (merged above).
			join_step_ = JoinStep::Idle;
			JoinResult settled;
			settled.kind = JoinResult::Kind::Settled;
			return settled;
		}
		case JoinStep::Idle:
		default:
			join_step_ = JoinStep::Idle;
			return join_fail("join response with no pending step");
	}
}

// =============================== Host ===============================

// The menu browser keeps every page global a page sets, by name, for the substitutions and the
// menu's own reads. [orig: UI_ProcessWebResponseContent @0x63d50a..0x63d558 ->
//  CUIScene_SetProperty @0x63b480; the reader CUIScene_FindNamedValue @0x63B440]
void LobbyHttpFlow::capture_globals(const std::vector<uint8_t> &body) {
	const std::string html = to_string_body(body);
	const std::string lower = to_lower(html);
	std::size_t at = 0;
	while ((at = lower.find("<ib3_global", at)) != std::string::npos) {
		const std::size_t tag_end = lower.find('>', at);
		if (tag_end == std::string::npos) return;
		const std::size_t name_at = lower.find("name=", at);
		const std::size_t close = lower.find("</ib3_global", tag_end);
		if (name_at == std::string::npos || name_at > tag_end || close == std::string::npos) {
			at = tag_end + 1;
			continue;
		}
		std::size_t begin = name_at + 5;
		while (begin < tag_end && (html[begin] == '"' || html[begin] == '\'')) ++begin;
		std::size_t end = begin;
		while (end < tag_end && html[end] != '"' && html[end] != '\'' &&
		       !std::isspace(static_cast<unsigned char>(html[end])))
			++end;
		globals_[strutil::to_upper(html.substr(begin, end - begin))] =
				strip_edges(html.substr(tag_end + 1, close - tag_end - 1));
		at = close + 1;
	}
}

const std::string *LobbyHttpFlow::browser_global(const std::string &name) const {
	const auto it = globals_.find(strutil::to_upper(name));
	return it == globals_.end() ? nullptr : &it->second;
}

std::string LobbyHttpFlow::absolute_url(const std::string &url) const {
	if (begins_with(to_lower(url), "http://") || begins_with(to_lower(url), "https://")) return url;
	const std::string base = http_base();
	if (base.empty()) return std::string();
	return base + (url.empty() || url[0] != '/' ? "/" : "") + url;
}

HostKeyResult LobbyHttpFlow::host_get(const std::string &url) {
	const std::string absolute = absolute_url(url);
	if (absolute.empty()) {
		host_step_ = HostStep::Idle;
		return host_fail("no server base URL — connect first");
	}
	host_step_ = HostStep::Fetching;
	HttpRequestSpec req;
	req.valid = true;
	req.method = HttpMethod::Get;
	req.url = absolute;
	req.headers = request_headers();
	host_page_url_ = req.url;
	return host_need(std::move(req));
}

HostKeyResult LobbyHttpFlow::host() {
	if (host_active()) return host_fail("host already in flight");
	const std::string *host_url = browser_global("@HOST_URL@");
	if (host_url == nullptr || host_url->empty())
		return host_fail("the NovaWorld main page set no @HOST_URL@ — log in first");
	host_hops_ = 0;
	return host_get(*host_url);
}

HostKeyResult LobbyHttpFlow::on_host_response(bool transport_ok, int code,
                                              const std::vector<std::string> &response_headers,
                                              const std::vector<uint8_t> &body) {
	if (std::optional<HostKeyResult> held =
			template_answered(host_template_, transport_ok, code, response_headers, body))
		return std::move(*held);
	if (host_step_ == HostStep::Idle) return host_fail("host response with no pending step");
	// The HOSTKEY page's refresh is the browser's page alone: its answer, or its failure, settles
	// the leg.
	if (host_step_ == HostStep::Page) {
		host_step_ = HostStep::Idle;
		if (transport_ok) merge_response_cookies(response_headers);
		HostKeyResult settled;
		settled.kind = HostKeyResult::Kind::Settled;
		return settled;
	}
	if (!transport_ok || code != 200) {
		host_step_ = HostStep::Idle;
		const std::string message = extract_message(body);
		if (!message.empty()) return host_fail(message);
		return host_fail("host HTTP failed (code " + std::to_string(code) + ")");
	}
	merge_response_cookies(response_headers);
	capture_globals(body);
	const std::string html = to_string_body(body);
	// The title test skips leading characters that are neither a letter nor '[', then compares
	// case-insensitively. [orig: UI_ProcessWebResponseContent @0x63d5bb..0x63d604]
	const std::string title = page_title(html);
	std::size_t start = 0;
	while (start < title.size() && !std::isalpha(static_cast<unsigned char>(title[start])) &&
	       title[start] != '[')
		++start;
	if (strutil::starts_with_icase(std::string_view(title).substr(start), "[HOSTKEY=")) {
		host_step_ = HostStep::Idle;
		RegistrationUrl parsed;
		if (!registration_url_parse(title, parsed) || !parsed.has_host_key || parsed.host_key.empty())
			return host_fail("host: an empty HOSTKEY");
		HostKeyResult r;
		r.kind = HostKeyResult::Kind::Resolved;
		r.host_key = parsed.host_key;
		// The HOSTKEY page refreshes (JO's to jop_2_host3.htm, "10;"), and the menu browser follows
		// it at its next frame whatever the delay (the deadline slip D-NET-NEW-L9-31 ports for the
		// .joi page). The page's event 7 runs the host request's wait first, inside the page's
		// processing, so the refresh goes out once that wait has ended, its result what it may (the
		// LN S1 / S1b / S1c / S1d retail hosts GET jop_2_host3.htm 0.25 to 6.3 s after the HOSTKEY
		// page's answer).
		// [orig: UI_ProcessWebResponseContent @0x63D450 - the refresh to the scene's +0x3C
		//  @0x63D501, the title's event 7 (CUIScene_RaiseEvent called @0x63D634);
		//  UI_DispatchScreenEvent @0x54E6A0 - case 7's CNapiGameSession_ConnectOrHost called
		//  @0x54F33B, its wait inside; CUIScene_EndFrame @0x63E600 - `cmp eax, [esi+40h]`
		//  @0x63E626, UIScene_OpenUrl called @0x63E635]
		// The refresh combines with the page's own URL, as every page the browser opens does.
		const std::string next = meta_refresh_url(html);
		const std::string refresh =
				next.empty() ? std::string() : browser_combine_url(host_page_url_, next);
		if (!refresh.empty()) {
			host_step_ = HostStep::Page;
			r.request.valid = true;
			r.request.method = HttpMethod::Get;
			r.request.url = refresh;
			r.request.headers = request_headers();
			r.request.cache_bust = true;
			host_page_url_ = refresh;
		}
		return r;
	}
	// A relay page refreshes to the next one, which the menu browser follows behind the page's
	// template (behind_template: the LN S1 / S1b / S1c / S1d retail hosts fetch the relay template
	// after the NWHost relay page and ahead of its refresh). The HOSTKEY page resolves at once: its
	// title raises the host's event ahead of any template fetch (CUIScene_RaiseEvent called @0x63D634
	// with event 7, ahead of the fetch called @0x63D6D1).
	// The refresh is combined with the page's own URL (the scene's current URL, the base the
	// browser opens every URL against), so a leg the main page's @HOST_URL@ opened under its own
	// origin (the live service's names no port, its 0x82's web domain ":80") stays under it, and
	// the pages' templates key apart from the login's: the LN S1 / S2 retail host legs refetch
	// jop_2_relay.mnx (D-NET-NEW-L9-46).
	// [orig: CUIScene_EndFrame @0x63E600 - UIScene_OpenUrl called @0x63E635; UIScene_OpenUrl
	//  @0x63E2F0 - the scene's +0x34 pushed @0x63E3A7, CUIBrowser_AsyncFetchURLContent called
	//  @0x63E3AF; CUIBrowser_AsyncFetchURLContent @0x64DE80 - URL_CombineComponents called
	//  @0x64DE9B]
	const std::string page_url = host_page_url_;
	const std::string next = meta_refresh_url(html);
	if (!next.empty() && ++host_hops_ < kMaxHostHops)
		return behind_template(host_template_, page_url, body,
				host_get(browser_combine_url(page_url, next)));
	host_step_ = HostStep::Idle;
	const std::string message = extract_message(body);
	if (!message.empty()) return behind_template(host_template_, page_url, body, host_fail(message));
	return behind_template(host_template_, page_url, body, host_fail("host: no HOSTKEY page"));
}

// See http_flow.h.
std::string browser_cache_bust_url(const std::string &url, uint32_t tick_ms,
		const std::array<uint8_t, 4> &server_ip) {
	const std::size_t scheme = url.find("://");
	const std::size_t host_at = scheme == std::string::npos ? 0 : scheme + 3;
	const std::size_t path_at = url.find('/', host_at);
	if (path_at == std::string::npos) return url;
	const uint32_t address = static_cast<uint32_t>(server_ip[0]) |
			(static_cast<uint32_t>(server_ip[1]) << 8) | (static_cast<uint32_t>(server_ip[2]) << 16) |
			(static_cast<uint32_t>(server_ip[3]) << 24);
	char pair[64];
	std::snprintf(pair, sizeof pair, "nocachefix=%d%d", static_cast<int32_t>(tick_ms),
			static_cast<int32_t>(address));
	const bool has_query = url.find('?', path_at) != std::string::npos;
	return url + (has_query ? "&" : "?") + pair;
}

} // namespace opennova
