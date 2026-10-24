// P7 Part 2 — the lobby HTTP flow (engine/net/novaworld/http_flow): the EPASK login chain, the GSB fetch,
// and the NWJoin handshake driven bytes-in/bytes-out (no Godot). Exercises the URL builders, the
// LoginStep/JoinStep machines, and the cookie glue against synthetic server responses.

#include <net/novaworld/http_flow.h>
#include <net/novaworld/browser_cache.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>

#include <net/novaworld/gsb.h>
#include <net/novacrypto/epask.h>
#include <net/novacrypto/url_cipher.h>
#include <net/novaworld/lobby_vars.h> // make_lobby_identity_vars

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

namespace nw = opennova;

static int g_fail = 0;
static bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_fail;
	}
	return cond;
}

static std::vector<uint8_t> bytes(const std::string &s) {
	return std::vector<uint8_t>(s.begin(), s.end());
}
// The service's relay pages (apps/novaworld_web/templates/jop_2_relay.htm): a META refresh to the
// bare .dll, whose session tag rides the jar's cookie.
static std::vector<uint8_t> relay_to(const std::string &dll) {
	const std::string page = "<HTML><HEAD><TITLE></TITLE><META http-equiv=\"refresh\" CONTENT=0;URL=\"" +
			dll + "\"></HEAD></HTML>";
	return std::vector<uint8_t>(page.begin(), page.end());
}
static std::vector<std::string> set_cookie(const std::string &nv) {
	return {"Set-Cookie: " + nv};
}
static bool contains(const std::string &h, const std::string &needle) {
	return h.find(needle) != std::string::npos;
}
// The service's start pages (apps/novaworld_web/templates/jop_2_start.htm, dfx2_0_start.htm): a
// META refresh to NWStart whose query keys run in the LN S1 retail login's NWStart GET order
// (MSGBASE, IN, OUT, verfile, newupdateavailable, newupdateavailablewithbypass, junction).
static std::string start_page(const std::string &nwstart_query) {
	return "<HTML>\r\n<HEAD>\r\n<META NAME=\"Author\" CONTENT=\"Eric Milota\">\r\n<TITLE></TITLE>\r\n"
	       "<META HTTP-EQUIV=\"refresh\" CONTENT=\"0;URL=NWStart.dll?" +
	       nwstart_query + "\">\r\n</HEAD>\r\n<BODY></BODY>\r\n</HTML>\r\n";
}
static const std::string kJoNwStartQuery =
		"MSGBASE=jop_2_msg.htm&IN=jop_2_main.htm&OUT=jop_2_login.htm&verfile=jop_2.ver"
		"&newupdateavailable=jop_2_newupdateavailable.htm"
		"&newupdateavailablewithbypass=jop_2_newupdateavailable2.htm&junction=jop_2_junction.htm";
static const std::string kDfx2NwStartQuery =
		"MSGBASE=dfx2_0_msg.htm&IN=dfx2_0_main.htm&OUT=dfx2_0_login.htm&ACT=dfx2_0_actlogin.htm"
		"&verfile=dfx2_0.ver&newupdateavailable=dfx2_0_newupdateavailable.htm"
		"&newupdateavailablewithbypass=dfx2_0_newupdateavailable2.htm&junction=dfx2_0_junction.htm";

// A valid EPASK bundle the server "issues" in the prepare Set-Cookie.
static std::string make_epask_cookie() {
	nw::EpaskParams params;
	expect(nw::generate_epask(params), "generate_epask converges");
	return nw::epask_to_string(params);
}

// Concrete (OpenNova) context: a non-templated startup_url -> login skips NWSTART, base derived from it.
static nw::LobbyHttpContext concrete_ctx() {
	nw::LobbyHttpContext c;
	c.title = &kJo;
	c.gate_tag = c.title->gate_tag;
	c.startup_url = "http://gs.opennova.test:8080/NWStart.dll?prepare=1";
	c.post_ip = "gs.opennova.test";
	c.post_port = "8080";
	c.identity_vars = {{"NWUID", ""}, {"CountryName", "us"}};
	c.server_nwuid = "ABCDEF0123456789";
	return c;
}

static bool test_url_builders() {
	nw::LobbyHttpFlow f;
	nw::LobbyHttpContext c;
	c.title = &kJo;
	c.gate_tag = c.title->gate_tag;
	// Templated startup_url + web_domain.
	c.startup_url = "http://[domainname]/[VER1]/[VER2]/[CC]/[GT]/start";
	c.web_domain = "gs.novaworld.net:80";
	c.locale = "en_GB";
	f.set_context(c);
	const std::string su = f.resolve_startup_url();
	if (!expect(su == "http://gs.novaworld.net:80/3/2345/gb/jop:cus2/start",
	            "resolve_startup_url fills the template (CC from locale, GT jop:cus2)"))
		std::fprintf(stderr, "  got: %s\n", su.c_str());
	if (!expect(f.http_base() == "http://gs.novaworld.net:80",
	            "http_base: templated -> http://web_domain"))
		std::fprintf(stderr, "  got: %s\n", f.http_base().c_str());

	// Every token is matched case-insensitively and [PRODUCTCODE] fills "jop"
	// [orig: Mission_DeobfuscateDescription @0x4cdaa0 via NapiUtil_ReplaceAllCaseInsensitive].
	nw::LobbyHttpFlow fm;
	nw::LobbyHttpContext cm;
	cm.title = &kJo;
	cm.gate_tag = cm.title->gate_tag;
	cm.startup_url = "http://[DomainName]/[ver1]/[Ver2]/[cc]/[gt]/[ProductCode]/start";
	cm.web_domain = "gs.novaworld.net:80";
	cm.locale = "en_GB";
	fm.set_context(cm);
	const std::string sm = fm.resolve_startup_url();
	if (!expect(sm == "http://gs.novaworld.net:80/3/2345/gb/jop:cus2/jop/start",
	            "resolve_startup_url matches every token case-insensitively and fills [PRODUCTCODE]"))
		std::fprintf(stderr, "  got: %s\n", sm.c_str());
	if (!expect(fm.http_base() == "http://gs.novaworld.net:80",
	            "http_base: a mixed-case template is still templated"))
		std::fprintf(stderr, "  got: %s\n", fm.http_base().c_str());
	nw::LobbyHttpFlow fp;
	nw::LobbyHttpContext cp;
	cp.title = &kJo;
	cp.gate_tag = cp.title->gate_tag;
	cp.startup_url = "http://static.example/[VER1]/start"; // no [DOMAINNAME]: still a template
	fp.set_context(cp);
	expect(fp.resolve_startup_url() == "http://static.example/3/start",
	       "any of the six tokens triggers substitution, not only [domainname]");

	// Concrete startup_url -> scheme://host[:port] stripped at the first path slash.
	nw::LobbyHttpFlow f2;
	f2.set_context(concrete_ctx());
	if (!expect(f2.resolve_startup_url() == "http://gs.opennova.test:8080/NWStart.dll?prepare=1",
	            "resolve_startup_url passes a concrete url through unchanged"))
		return false;
	if (!expect(f2.http_base() == "http://gs.opennova.test:8080", "http_base strips the path"))
		std::fprintf(stderr, "  got: %s\n", f2.http_base().c_str());

	// Fallback to post_ip:post_port when there is no startup_url.
	nw::LobbyHttpFlow f3;
	nw::LobbyHttpContext c3;
	c3.title = &kJo;
	c3.gate_tag = c3.title->gate_tag;
	c3.post_ip = "10.0.0.7";
	c3.post_port = "28910";
	f3.set_context(c3);
	expect(f3.http_base() == "http://10.0.0.7:28910", "http_base falls back to post_ip:post_port");
	return g_fail == 0;
}

static bool test_login_success_concrete() {
	nw::LobbyHttpFlow f;
	f.set_context(concrete_ctx());

	nw::LoginResult r = f.login("player", "secret");
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "login() -> prepare request")) return false;
	expect(r.request.method == nw::HttpMethod::Get && r.request.valid, "prepare is a GET");
	expect(r.request.url == "http://gs.opennova.test:8080/NWStart.dll?prepare=1", "prepare hits startup_url");
	// D-NET-NEW-L9-26: the identity set is in the jar before the web lobby's first request, which
	// carries it in its order (the live nwuid for the empty NWUID), and every cookie a page sets
	// follows it in the later Cookie lists, the UDP statements' var-list included.
	// [orig: CNapiGameSession_OnNovaWorldConnected @0x4D1570 - CUIBrowser_AddFormField @0x4D16A9..
	//  @0x4D17AA; CNapiSession_ReadLocaleInfo @0x4CE390 - the jar's walk @0x4CE3E5]
	// The cookie lines follow the menu browser's four GET headers (D-NET-NEW-L9-31).
	// [orig: CUIBrowser_SendHTTPRequest @0x658840 - @0x658969, @0x658979, @0x658989, @0x6589D5]
	expect(r.request.headers ==
	               std::vector<std::string>{"Accept: image/jpeg, image/x-pcx, image/pcx, image/png, text/html",
	                       "Accept-Language: en", "User-Agent: NovaLogic IBrowse 2.0", "Pragma: no-cache",
	                       "Cookie: NWUID=ABCDEF0123456789;", "Cookie: CountryName=us;"},
	       "the prepare GET carries the identity cookies, in their order");

	// PREPARE response: the server issues the EPASK bundle as a cookie. Concrete url -> straight to POST.
	const std::string epask_cookie = make_epask_cookie();
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + epask_cookie), {});
	{
		const std::vector<std::pair<std::string, std::string>> vars = f.session_cookie_vars();
		std::vector<std::string> names;
		for (const auto &kv : vars) names.push_back(kv.first);
		expect(names == std::vector<std::string>{"NWUID", "CountryName", "EPASK"},
		       "the Cookie var-list leads with the identity set, the page's cookies after it");
	}
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "prepare -> POST (concrete skips NWSTART)"))
		return false;
	expect(r.request.method == nw::HttpMethod::Post, "login leg is a POST");
	expect(r.request.url == "http://gs.opennova.test:8080/NWLogin.dll", "POST hits /NWLogin.dll");
	// The 13-field form body: EPASK present (plaintext bundle), NAME/PASSWORD encrypted, the jop_2_* templates.
	// The field NAMES are plaintext; the encrypt=true values (NAME/PASSWORD/pfid/the jop_2_* templates)
	// are ciphertext in the body (only the EPASK bundle is sent plaintext) — encryption is covered by
	// the epask / http_login tests, so here we just confirm the 13-field set is assembled.
	expect(contains(r.request.body, "EPASK="), "form carries the EPASK field");
	expect(contains(r.request.body, "NAME=") && contains(r.request.body, "PASSWORD="), "form carries NAME + PASSWORD");
	expect(contains(r.request.body, "success=") && contains(r.request.body, "failure="), "form carries the success/failure fields");
	expect(contains(r.request.body, "pfid="), "form carries the pfid field");
	// The form post's headers in the menu browser's order, past its request line and Host
	// (D-NET-NEW-L9-31). [orig: GopherWebWidget_SendHttpPost @0x658B30 - @0x658BBE, @0x658BCE,
	//  @0x658BDE, @0x658BEF, @0x658BFF, the Cookie lines from @0x658C67]
	expect(r.request.headers.size() >= 5 && r.request.headers[0] == "Accept: text/html" &&
	               r.request.headers[1] == "Accept-Language: en" &&
	               r.request.headers[2] == "User-Agent: NovaLogic IBrowse 2.0" &&
	               r.request.headers[3] == "Content-Length: " + std::to_string(r.request.body.size()) &&
	               r.request.headers[4] == "Content-type: application/x-www-form-urlencoded",
	       "POST carries the browser's headers in its order, the lower-case-t Content-type");
	for (std::size_t i = 5; i < r.request.headers.size(); ++i)
		expect(contains(r.request.headers[i], "Cookie: "), "the POST's cookie lines follow its form headers");

	// POST response: LOGINSESSIONTAG and the relay page -> the poll GET its META refresh names, the
	// bare NWLogin.dll (the tag rides the jar's cookie), with the browser's GET headers.
	// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620; CUIBrowser_SendHTTPRequest @0x658840 -
	//  @0x658969, @0x658979, @0x658989, @0x6589D5]
	r = f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=tag123"), relay_to("NWLogin.dll"));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "POST -> poll")) return false;
	expect(r.request.url == "http://gs.opennova.test:8080/NWLogin.dll",
	       "poll follows the relay page's refresh to the bare NWLogin.dll");
	expect(r.request.headers.size() >= 4 &&
	               r.request.headers[0] == "Accept: image/jpeg, image/x-pcx, image/pcx, image/png, text/html" &&
	               r.request.headers[1] == "Accept-Language: en" &&
	               r.request.headers[2] == "User-Agent: NovaLogic IBrowse 2.0" &&
	               r.request.headers[3] == "Pragma: no-cache",
	       "a GET carries the browser's headers in its order");
	{
		bool tag_cookie = false;
		for (const std::string &h : r.request.headers)
			if (h == "Cookie: LOGINSESSIONTAG=tag123;") tag_cookie = true;
		expect(tag_cookie, "the poll's session tag rides its cookie line");
	}
	// A POST answer that is no relay page leaves the login there.
	{
		nw::LobbyHttpFlow g;
		g.set_context(concrete_ctx());
		g.login("p", "s");
		g.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		const nw::LoginResult stuck = g.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), {});
		expect(stuck.kind == nw::LoginResult::Kind::Failed, "a POST answer with no refresh fails the login");
	}

	// POLL response: NWHANDLE + PCID -> success.
	r = f.on_login_response(true, 200, {"Set-Cookie: NWHANDLE=PlayerOne", "Set-Cookie: PCID=42"}, {});
	if (!expect(r.kind == nw::LoginResult::Kind::Succeeded, "poll with NWHANDLE -> success")) return false;
	expect(r.nwhandle == "PlayerOne", "captured nwhandle");
	expect(r.pcid == "42", "captured pcid");
	expect(!f.login_active(), "login machine returns to Idle on success");

	// The identity cookies were seeded with the live nwuid substituted for the empty NWUID.
	const std::string *nwuid = f.cookies().find("NWUID");
	expect(nwuid != nullptr && *nwuid == "ABCDEF0123456789", "empty NWUID identity var filled from server_nwuid");
	return g_fail == 0;
}

static bool test_login_templated_nwstart() {
	nw::LobbyHttpFlow f;
	nw::LobbyHttpContext c;
	c.title = &kJo;
	c.gate_tag = c.title->gate_tag;
	c.startup_url = "http://[domainname]/[VER1]/[VER2]/[CC]/[GT]/prep";
	c.web_domain = "gs.novaworld.net";
	c.locale = "en_US";
	f.set_context(c);

	nw::LoginResult r = f.login("p", "s");
	expect(r.kind == nw::LoginResult::Kind::NeedRequest, "templated login() -> prepare");
	// The start page's refresh is the NWStart GET, as it stands, the browser's nocachefix pair added
	// on the way out. [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620; UI_ProcessWebResponseContent
	//  @0x63D450 - @0x63D501; CUIScene_EndFrame @0x63E600 - UIScene_OpenUrl called @0x63E635]
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()),
			bytes(start_page(kJoNwStartQuery)));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "templated prepare -> NWSTART")) return false;
	expect(r.request.method == nw::HttpMethod::Get && r.request.cache_bust, "NWSTART is a GET");
	if (!expect(r.request.url == "http://gs.novaworld.net/NWStart.dll?" + kJoNwStartQuery,
	            "NWSTART follows the start page's refresh, its query byte for byte"))
		std::fprintf(stderr, "  got: %s\n", r.request.url.c_str());
	// NWSTART response -> POST.
	r = f.on_login_response(true, 200, {}, {});
	expect(r.kind == nw::LoginResult::Kind::NeedRequest && r.request.method == nw::HttpMethod::Post,
	       "NWSTART -> POST /NWLogin.dll");
	// Captured live .204 success needs two poll GETs. The first response is
	// still the relay page, where @MESSAGE@ is progress rather than failure.
	const std::string relay_page =
			"<HTML><HEAD><META http-equiv=\"refresh\" CONTENT=0;URL=\"NWLogin.dll\"></HEAD>"
			"<IB3_SUBST name=\"@MESSAGE@\">Contacting login databases..."
			"<BR></IB3_SUBST></HTML>";
	r = f.on_login_response(true, 200,
			{"Set-Cookie: LOGINSESSIONTAG=live-tag", "Set-Cookie: NWHANDLE=",
			 "Set-Cookie: PCID="},
			bytes(relay_page));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest,
	            "templated POST with session tag -> first poll")) return false;
	r = f.on_login_response(true, 200, {}, bytes(relay_page));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest,
	            "pending relay message keeps polling")) return false;
	expect(contains(r.request.url, "/NWLogin.dll") && !contains(r.request.url, "tag="),
	       "second poll follows the relay page's refresh, its tag in the jar");
	r = f.on_login_response(true, 200,
			{"Set-Cookie: NWHANDLE=ljim", "Set-Cookie: PCID=A-A02-085D18"}, {});
	if (!expect(r.kind == nw::LoginResult::Kind::Succeeded,
	            "second live-shaped poll completes login")) return false;
	expect(r.nwhandle == "ljim" && r.pcid == "A-A02-085D18",
	       "live-shaped poll captures account identity");

	// DFX2's start page names its ACT page between OUT and verfile: the refresh carries it as the
	// page has it.
	{
		nw::LobbyHttpFlow d;
		nw::LobbyHttpContext dc;
		dc.title = nw::title_identity(nw::gameprofile::GAME_DFX2);
		dc.gate_tag = dc.title->gate_tag;
		dc.startup_url = "http://[domainname]/nwprepare.dll?gt=[GT]&url=dfx2_0_start.htm";
		dc.web_domain = "gs.novaworld.net";
		d.set_context(dc);
		d.login("p", "s");
		const nw::LoginResult dr = d.on_login_response(true, 200,
				set_cookie("EPASK=" + make_epask_cookie()), bytes(start_page(kDfx2NwStartQuery)));
		expect(dr.kind == nw::LoginResult::Kind::NeedRequest &&
		               dr.request.url == "http://gs.novaworld.net/NWStart.dll?" + kDfx2NwStartQuery,
		       "DFX2's NWSTART is its start page's refresh, ACT included");
	}
	return g_fail == 0;
}

static bool test_login_failures() {
	// No EPASK cookie on prepare.
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		const std::string page =
				"<IB3_SUBST name=\"@GENERIC@\">The login service is unavailable.</IB3_SUBST>";
		nw::LoginResult r = f.on_login_response(true, 200, {}, bytes(page));
		expect(r.kind == nw::LoginResult::Kind::Failed, "prepare with no EPASK -> Failed");
		expect(r.reason == "The login service is unavailable.",
		       "prepare failure preserves the rendered service message");
		expect(!f.login_active(), "machine resets on failure");
	}
	// NWStart can return a 200 message page instead of the expected login form.
	{
		nw::LobbyHttpFlow f;
		nw::LobbyHttpContext c;
		c.title = &kJo;
		c.gate_tag = c.title->gate_tag;
		c.startup_url = "http://[domainname]/prep";
		c.web_domain = "gs.novaworld.net";
		f.set_context(c);
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()),
				bytes(start_page(kJoNwStartQuery)));
		const std::string page =
				"<IB3_SUBST name=\"@GENERIC@\">Matchmaking is under maintenance.</IB3_SUBST>";
		nw::LoginResult r = f.on_login_response(true, 200, {}, bytes(page));
		expect(r.kind == nw::LoginResult::Kind::Failed, "NWStart message page -> Failed");
		expect(r.reason == "Matchmaking is under maintenance.",
		       "NWStart failure preserves the rendered service message");
	}
	// HTTP non-200.
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		nw::LoginResult r = f.on_login_response(true, 500, {}, {});
		expect(r.kind == nw::LoginResult::Kind::Failed, "non-200 -> Failed");
	}
	// A malformed EPASK cookie (missing separator) or one whose params the
	// modexp gate rejects fails the login before any POST is built.
	// [orig: EPASK_ParseColonDelimitedString @0x666710 -1; EPASK_ModexpEncrypt @0x66668a -1]
	for (const char *bad : {"7", "0:0:k", "3:258:k"}) {
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		nw::LoginResult r = f.on_login_response(true, 200, set_cookie(std::string("EPASK=") + bad), {});
		expect(r.kind == nw::LoginResult::Kind::Failed, "rejected EPASK bundle -> Failed");
		expect(!f.login_active(), "machine resets on a rejected EPASK bundle");
	}
	// POST with no LOGINSESSIONTAG (bad credentials).
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		const std::string page =
				"<HTML><IB3_SUBST name=\"@GENERIC@\">The account name or password is incorrect."
				"<BR>Please try again.</IB3_SUBST></HTML>";
		nw::LoginResult r = f.on_login_response(true, 200, {}, bytes(page));
		expect(r.kind == nw::LoginResult::Kind::Failed, "POST without session tag -> Failed");
		expect(r.reason == "The account name or password is incorrect.\nPlease try again.",
		       "login failure preserves the rendered NovaWorld message");
	}
	// Message extraction strips markup and decodes entities emitted by the template renderer.
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		const std::string page =
				"<IB3_SUBST NAME='@MESSAGE@'><B>This account is restricted</B> &amp; can't log in."
				"</IB3_SUBST>";
		nw::LoginResult r = f.on_login_response(true, 200, {}, bytes(page));
		expect(r.reason == "This account is restricted & can't log in.",
		       "message markup and HTML entities are normalized");
	}
	// Unlike @MESSAGE@ relay progress, @GENERIC@ during Poll is terminal.
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), relay_to("NWLogin.dll"));
		const std::string page =
				"<IB3_SUBST name=\"@GENERIC@\">This account cannot sign in.</IB3_SUBST>";
		nw::LoginResult r = f.on_login_response(true, 200, {}, bytes(page));
		expect(r.kind == nw::LoginResult::Kind::Failed, "poll generic message -> Failed");
		expect(r.reason == "This account cannot sign in.",
		       "poll failure preserves the terminal message");
	}
	// Poll exhaustion (10 polls, no NWHANDLE).
	{
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), relay_to("NWLogin.dll"));
		nw::LoginResult r;
		bool failed = false;
		for (int i = 0; i < 12; ++i) {
			r = f.on_login_response(true, 200, {}, relay_to("NWLogin.dll"));
			if (r.kind == nw::LoginResult::Kind::Failed) { failed = true; break; }
		}
		expect(failed, "poll exhausts after kMaxLoginPolls without NWHANDLE");
	}
	return g_fail == 0;
}

static bool test_gsb_request_and_parse() {
	nw::LobbyHttpFlow f;
	f.set_context(concrete_ctx());
	const nw::HttpRequestSpec req = f.gsb_request();
	if (!expect(req.valid && req.url == "http://gs.opennova.test:8080/jop_2.gsb?a=1", "gsb url"))
		return false;

	// Build a one-server GSB blob and round-trip it through the flow's parser.
	nw::GsbServerEntry s;
	s.rid = 777;
	s.ip = "198.51.100.23";
	s.server_name = "Test Host";
	s.mission_name = "Island";
	s.players = 3;
	s.max_players = 16;
	const std::vector<uint8_t> blob = nw::gsb_build_response({s});
	nw::GsbResponse out;
	if (!expect(f.on_gsb_response(true, 200, blob, out), "gsb parse ok")) return false;
	expect(out.servers.size() == 1 && out.servers[0].rid == 777 &&
	           out.servers[0].ip == "198.51.100.23",
	       "gsb decoded the server row");
	// A failed fetch parses nothing.
	nw::GsbResponse out2;
	expect(!f.on_gsb_response(false, 0, {}, out2), "failed gsb fetch -> false");
	return g_fail == 0;
}

static bool test_join_resolves() {
	nw::LobbyHttpFlow f;
	f.set_context(concrete_ctx());

	nw::JoinResult r = f.join(777);
	if (!expect(r.kind == nw::JoinResult::Kind::NeedRequest, "join() -> first NWJoin request")) return false;
	expect(contains(r.request.url, "/NWJoin.dll?needexpkey=jop_2_key2err.htm"), "NWJoin first leg literal");
	expect(contains(r.request.url, "mode=Login&rid=777"), "NWJoin first leg carries the rid");

	// FIRST response: NWJOINSESSIONTAG and the relay page -> the second leg its META refresh names,
	// the bare NWJoin.dll, the tag in the jar (the LN S1b retail joiner's second GET; D-NET-NEW-L9-31).
	r = f.on_join_response(true, 200, set_cookie("NWJOINSESSIONTAG=jtag"), relay_to("NWJoin.dll"));
	if (!expect(r.kind == nw::JoinResult::Kind::NeedRequest, "NWJoin FIRST -> SECOND")) return false;
	expect(r.request.url == "http://gs.opennova.test:8080/NWJoin.dll",
	       "NWJoin second leg follows the relay page's refresh");

	// SECOND response: the .joi body resolves host:port from the decoded NK (the
	// only dial authority) and carries the proxy triple + lobby number verbatim
	// for the transport.
	const std::string nk = nw::url_cipher_encode("10.1.2.3:3001", nw::URL_CIPHER_KEY_NK);
	r = f.on_join_response(true, 200, {},
			bytes("<TITLE>[NK=" + nk + "&NI=192.168.5.9&NP=17479&BK=986119&LN=5&GS=x]</TITLE>"));
	if (!expect(r.kind == nw::JoinResult::Kind::Resolved, "NWJoin SECOND -> resolved")) return false;
	expect(r.host_ip == "10.1.2.3" && r.host_port == 3001, "join resolved host:port from the .joi NK");
	expect(r.ni == "192.168.5.9" && r.np == "17479" && r.bk == "986119",
	       "join carries the NI/NP/BK proxy triple verbatim");
	expect(r.ln == 5, "join carries the .joi lobby number");
	expect(!f.join_active(), "join machine resets on resolve");
	// B-13: the engine builds the six proxy fields from the resolved .joi: node =
	// inet_addr(NI) / atol(NP), cookie = atol(BK), relay = the NK endpoint, and the
	// all-six gate reads them as a proxy join. [orig: CNapiGameSession_InitTransportConnection
	//  @0x4ca051..0x4ca0c7]
	{
		const opennova::ProxyRendezvousConfig proxy = nw::proxy_rendezvous_from_join(r);
		expect(proxy.node_addr == opennova::proxy_inet_addr("192.168.5.9") && proxy.node_port == 17479 &&
		               proxy.cookie == 986119 && proxy.relay_addr == opennova::proxy_inet_addr("10.1.2.3") &&
		               proxy.relay_port == 3001 && proxy.enabled(),
		       "the resolved .joi installs all six proxy fields");
		nw::JoinResult bare = r;
		bare.ni.clear();
		bare.np.clear();
		bare.bk.clear();
		expect(!nw::proxy_rendezvous_from_join(bare).enabled(),
		       "a .joi without NI/NP/BK installs no proxy");
		bare = r;
		bare.np = " 17479x"; // the CRT atol: leading space skipped, trailing junk ignored
		expect(nw::proxy_rendezvous_from_join(bare).node_port == 17479,
		       "NP goes through the retail atol");
	}

	// A .joi with no connection string fails.
	nw::LobbyHttpFlow f2;
	f2.set_context(concrete_ctx());
	f2.join(1);
	f2.on_join_response(true, 200, {}, relay_to("NWJoin.dll"));
	nw::JoinResult bad = f2.on_join_response(true, 200, {}, bytes("no brackets here"));
	expect(bad.kind == nw::JoinResult::Kind::Failed, "join with no .joi connection -> Failed");
	expect(bad.reason == "join: no connection string in .joi", "malformed .joi keeps its fallback reason");

	// Expansion and account restrictions arrive as a retail-style message page.
	nw::LobbyHttpFlow f3;
	f3.set_context(concrete_ctx());
	f3.join(2);
	f3.on_join_response(true, 200, {}, relay_to("NWJoin.dll"));
	const std::string rejection =
			"<IB3_SUBST name=\"@GENERIC@\">This game requires the Team Sabre expansion."
			"<BR><I>Choose another game.</I></IB3_SUBST>";
	nw::JoinResult rejected = f3.on_join_response(true, 200, {}, bytes(rejection));
	expect(rejected.kind == nw::JoinResult::Kind::Failed, "message response rejects the join");
	expect(rejected.reason == "This game requires the Team Sabre expansion.\nChoose another game.",
	       "join failure preserves the rendered NovaWorld message");
	return g_fail == 0;
}

// The Host leg: the login lands on the main page whose @HOST_URL@ global names the hosting URL; the
// relay page's refresh is followed with the tag cookie; the [HOSTKEY=...&] title resolves the key.
// [orig: UI_HandleHostSessionStart @0x556d00 @0x556f4b; UI_ProcessWebResponseContent
//  @0x63d5bb..0x63d604; URL_ParseConnectionQueryString @0x54dfb0 @0x54e0be]
static bool test_host_resolves() {
	nw::LobbyHttpFlow none;
	none.set_context(concrete_ctx());
	nw::HostKeyResult early = none.host();
	expect(early.kind == nw::HostKeyResult::Kind::Failed && !none.host_active(),
	       "host() before any main page set @HOST_URL@ fails (log in first)");

	nw::LobbyHttpFlow f;
	f.set_context(concrete_ctx());
	f.login("player", "secret");
	f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
	f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=tag123"), relay_to("NWLogin.dll"));
	const std::string main_page =
			"<HTML><BODY><A HREF=\"jop_2_host1.htm\">Host</A>"
			"<IB3_GLOBAL name=\"@HOST_URL@\">http://gs.opennova.test:8080/nwhost.dll</IB3_GLOBAL>"
			"</BODY></HTML>";
	nw::LoginResult login = f.on_login_response(
			true, 200, {"Set-Cookie: NWHANDLE=PlayerOne", "Set-Cookie: PCID=42"}, bytes(main_page));
	if (!expect(login.kind == nw::LoginResult::Kind::Succeeded, "host: the login lands on the main page"))
		return false;
	const std::string *global = f.browser_global("@host_url@");
	expect(global != nullptr && *global == "http://gs.opennova.test:8080/nwhost.dll",
	       "the main page's @HOST_URL@ global is kept (name case-insensitive)");

	nw::HostKeyResult r = f.host();
	if (!expect(r.kind == nw::HostKeyResult::Kind::NeedRequest, "host() -> the @HOST_URL@ GET")) return false;
	expect(r.request.method == nw::HttpMethod::Get &&
	               r.request.url == "http://gs.opennova.test:8080/nwhost.dll",
	       "the host leg opens @HOST_URL@");
	const std::string relay =
			"<HTML><HEAD><TITLE></TITLE>"
			"<META http-equiv=\"refresh\" CONTENT=0;URL=\"NWHost.dll\"></HEAD></HTML>";
	r = f.on_host_response(true, 200, set_cookie("NWJOINSESSIONTAG=htag"), bytes(relay));
	if (!expect(r.kind == nw::HostKeyResult::Kind::NeedRequest, "the relay page refreshes")) return false;
	expect(r.request.url == "http://gs.opennova.test:8080/NWHost.dll", "the relative refresh joins the base");
	bool tag_cookie = false;
	for (const std::string &h : r.request.headers)
		if (contains(h, "NWJOINSESSIONTAG=htag")) tag_cookie = true;
	expect(tag_cookie, "the refresh carries the relay's tag cookie");
	const std::string host2 =
			"<HTML><HEAD><TITLE>\n  [HOSTKEY=ABCDEFGHIJKLMNOP&]</TITLE>"
			"<META HTTP-EQUIV=\"refresh\" CONTENT=\"10;URL=jop_2_host3.htm\"></HEAD></HTML>";
	r = f.on_host_response(true, 200, {}, bytes(host2));
	if (!expect(r.kind == nw::HostKeyResult::Kind::Resolved, "the HOSTKEY title resolves")) return false;
	expect(r.host_key == "ABCDEFGHIJKLMNOP", "the key is trimmed at '&' then ']'");
	// The HOSTKEY page's refresh, which the owner ships once the host request's wait ends: the LN
	// S1 / S1b / S1c / S1d retail hosts GET jop_2_host3.htm after it whatever its "10;" delay.
	// [orig: CUIScene_EndFrame @0x63E600 - @0x63E626, UIScene_OpenUrl called @0x63E635;
	//  UI_DispatchScreenEvent @0x54E6A0 - ConnectOrHost called @0x54F33B]
	expect(r.request.valid && r.request.method == nw::HttpMethod::Get && r.request.cache_bust &&
	               r.request.url == "http://gs.opennova.test:8080/jop_2_host3.htm",
	       "the resolve carries the HOSTKEY page's refresh GET");
	expect(f.host_active(), "the leg waits for the refresh's answer");
	r = f.on_host_response(true, 200, set_cookie("YOURIP=10.0.0.9"), bytes("<HTML>host3</HTML>"));
	expect(r.kind == nw::HostKeyResult::Kind::Settled && !f.host_active(),
	       "the refresh's answer settles the leg");
	expect(f.cookies().find("YOURIP") != nullptr, "and its cookies merge");
	{
		// A HOSTKEY page with no refresh resolves with no request and an idle leg.
		nw::LobbyHttpFlow g;
		g.set_context(concrete_ctx());
		g.login("player", "secret");
		g.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		g.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), relay_to("NWLogin.dll"));
		g.on_login_response(true, 200, {"Set-Cookie: NWHANDLE=P"}, bytes(main_page));
		g.host();
		const nw::HostKeyResult bare = g.on_host_response(true, 200, {},
				bytes("<HTML><HEAD><TITLE>[HOSTKEY=QRSTUVWXYZ&]</TITLE></HEAD></HTML>"));
		expect(bare.kind == nw::HostKeyResult::Kind::Resolved && !bare.request.valid && !g.host_active(),
		       "a HOSTKEY page with no refresh leaves nothing to fetch");
	}

	// A message page fails with its rendered text; a refresh loop gives up.
	r = f.host();
	r = f.on_host_response(true, 200, {},
			bytes("<IB3_SUBST name=\"@GENERIC@\">You must agree first.</IB3_SUBST>"));
	expect(r.kind == nw::HostKeyResult::Kind::Failed && r.reason == "You must agree first.",
	       "a message page fails the host leg with its text");
	r = f.host();
	for (int hop = 0; hop < nw::LobbyHttpFlow::kMaxHostHops && r.kind == nw::HostKeyResult::Kind::NeedRequest;
	     ++hop) {
		r = f.on_host_response(true, 200, {}, bytes(relay));
	}
	expect(r.kind == nw::HostKeyResult::Kind::Failed, "a refresh loop gives up");
	return g_fail == 0;
}

// D-NET-NEW-L9-31: a menu-browser GET's nocachefix pair, "%d%d" of the tick and the resolved
// server's IPv4 address read as one little-endian int ('?' or '&' by the URL's query; none with no
// path), and which requests carry it: every GET, no form post. The LN S1 / S1b records' pairs have
// this form (a nine-digit tick, then the web server's address, negative above 127.x.x.x).
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - @0x65889F, @0x6588A3, @0x6588B5, @0x658901,
//  @0x658942; CNapiWinsock_InitFromUrl @0x658DA0 - CNapiWinsock_ResolveHostAsync called @0x658E4B]
static bool test_browser_cache_bust() {
	expect(nw::browser_cache_bust_url("http://nw.example.test/jop_2_login.mnx", 676778718u,
	               {198, 51, 100, 7}) ==
	               "http://nw.example.test/jop_2_login.mnx?nocachefix=676778718124007366",
	       "a GET with no query takes ?nocachefix= and the address's int");
	expect(nw::browser_cache_bust_url("http://nw.example.test/jop_2.gsb?a=1", 676811359u,
	               {203, 0, 113, 250}) ==
	               "http://nw.example.test/jop_2.gsb?a=1&nocachefix=676811359-93257525",
	       "behind a query it is &nocachefix=, a high octet printing negative");
	expect(nw::browser_cache_bust_url("http://nw.example.test", 5u, {198, 51, 100, 7}) ==
	               "http://nw.example.test",
	       "a URL with no path goes out as it is");
	expect(nw::browser_cache_bust_url("http://nw.example.test/a", 0x80000000u, {0, 0, 0, 0}) ==
	               "http://nw.example.test/a?nocachefix=-21474836480",
	       "the tick prints signed");

	nw::LobbyHttpFlow f;
	f.set_context(concrete_ctx());
	nw::LoginResult r = f.login("p", "s");
	expect(r.request.cache_bust, "the prepare GET is cache-busted");
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
	expect(r.request.method == nw::HttpMethod::Post && !r.request.cache_bust,
	       "the form post takes no pair");
	r = f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), relay_to("NWLogin.dll"));
	expect(r.request.cache_bust, "the poll GET is cache-busted");
	nw::JoinResult j = f.join(9);
	expect(j.request.cache_bust, "the NWJoin GET is cache-busted");
	return g_fail == 0;
}

// D-NET-NEW-L9-31: the .joi page's refresh. The menu browser follows every META refresh at its
// next frame whatever the delay (the delay's seconds stand where its test reads a GetTickCount
// deadline), so the JO .joi's "10;URL=jop_2_jend.htm" is fetched right behind the join (the LN S1b
// retail joiner: 0.25 s after the .joi). Its answer settles the flow; a failed fetch leaves the
// join standing.
// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620 - @0x655783; CUIScene_EndFrame @0x63E600 -
//  @0x63E626, UIScene_OpenUrl called @0x63E635]
static bool test_joi_page_refresh() {
	const std::string nk = nw::url_cipher_encode("10.1.2.3:3001", nw::URL_CIPHER_KEY_NK);
	const std::string joi = "<HTML><HEAD><META http-equiv=\"refresh\" CONTENT=\"10;URL=jop_2_jend.htm\">"
			"<TITLE>[NK=" + nk + "&GS=x]</TITLE></HEAD></HTML>";
	for (const bool fetched : {true, false}) {
		nw::LobbyHttpFlow f;
		f.set_context(concrete_ctx());
		f.join(5);
		f.on_join_response(true, 200, set_cookie("NWJOINSESSIONTAG=j"), relay_to("NWJoin.dll"));
		const nw::JoinResult r = f.on_join_response(true, 200, {}, bytes(joi));
		if (!expect(r.kind == nw::JoinResult::Kind::Resolved, ".joi with a refresh -> resolved")) return false;
		expect(r.request.valid && r.request.method == nw::HttpMethod::Get &&
		               r.request.url == "http://gs.opennova.test:8080/jop_2_jend.htm" &&
		               r.request.cache_bust,
		       "the resolve carries the .joi page's refresh GET");
		expect(f.join_active(), "the flow waits for the page's answer");
		const nw::JoinResult settled = fetched ? f.on_join_response(true, 200, {}, bytes("<HTML></HTML>"))
		                                       : f.on_join_response(false, 0, {}, {});
		expect(settled.kind == nw::JoinResult::Kind::Settled && !f.join_active(),
		       "the page's answer, or its failure, settles the flow");
	}
	return g_fail == 0;
}

// D-NET-NEW-L9-34: the META refresh's URL as the menu browser's parser reads it. The tag's
// attributes are split as its attribute parser splits them; HTTP-EQUIV must be REFRESH whole; the
// URL is sought from the CONTENT's first ';' as "URL=", one quote skipped, and copied to a quote or
// the end, spaces included; no ';' or no "URL=" names none; the page's last refresh wins.
// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620 - @0x655676, @0x6556B0..0x6556BC, @0x6556CD,
//  @0x6556EE..0x6556F6, @0x655701..0x655717; XML_ParseAttributes @0x655410; sub_6555C0 @0x6555C0;
//  CUIHTMLParser_ParseTag @0x655E20 - @0x655EC4]
static bool test_meta_refresh_walk() {
	const auto url_of = [](const std::string &tag) {
		return nw::meta_refresh_url("<HTML><HEAD>" + tag + "</HEAD></HTML>");
	};
	expect(url_of("<META http-equiv=\"refresh\" CONTENT=0;URL=\"NWLogin.dll\">") == "NWLogin.dll",
	       "the service's relay form: an unquoted CONTENT, the URL's one quote skipped");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"5; URL=my page.htm\">") == "my page.htm",
	       "a spaced URL is copied whole, to the end");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"0;URL= a.htm\">") == " a.htm",
	       "no white space is skipped after URL=");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"0;URL='a.htm' b\">") == "a.htm",
	       "the copy ends at a quote");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"0; URL = x.htm\">").empty(),
	       "\"URL = x\" names none");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"URL=x.htm\">").empty(), "no ';' names none");
	expect(url_of("<META HTTP-EQUIV=\"refresh-now\" CONTENT=\"0;URL=x.htm\">").empty(),
	       "HTTP-EQUIV must be REFRESH whole");
	expect(url_of("<meta content=\"0;url=x.htm\" http-equiv=refresh>") == "x.htm",
	       "attribute names and URL= match case-insensitively, in any order");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"0;URL=a.htm\" CONTENT=\"0;URL=b.htm\">") ==
	               "b.htm",
	       "a tag's last CONTENT is the one read");
	expect(url_of("<META HTTP-EQUIV=\"Refresh\" CONTENT=\"0;URL=a.htm\"><META HTTP-EQUIV=\"Refresh\" "
	              "CONTENT=\"0;URL=b.htm\">") == "b.htm",
	       "the page's last refresh wins");
	expect(url_of("<METAX HTTP-EQUIV=\"Refresh\" CONTENT=\"0;URL=a.htm\">").empty(),
	       "a longer tag name is no META");
	return g_fail == 0;
}

// D-NET-NEW-L9-32: the gate reply's jar fields ahead of the connection-up's six. A set junction
// word leads with USEJUNCTION "1" and an empty JUNCTIONCODE; a clear mpextractextendedmetricinformation
// leaves CountryName / Language / TimeZoneBias out of the jar and the Cookie var-lists, and
// MyInstalledExpBits stays.
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - @0x4CF71B, @0x4CF74E, @0x4CF76E, @0x4CF773,
//  @0x4CF796, @0x4CF7AB, @0x4CF7DE, @0x4CF811; CNapiSession_ReadLocaleInfo @0x4CE390 - @0x4CE44E]
static bool test_gate_reply_jar_fields() {
	const auto identity = nw::make_lobby_identity_vars(nw::LobbyIdentityParams{});
	auto names_of = [](const std::vector<std::pair<std::string, std::string>> &vars) {
		std::vector<std::string> out;
		for (const auto &kv : vars) out.push_back(kv.first);
		return out;
	};
	{
		nw::LobbyHttpContext c = concrete_ctx();
		c.identity_vars = identity;
		c.use_junction = 1;
		nw::LobbyHttpFlow f;
		f.set_context(c);
		const auto vars = f.session_cookie_vars();
		const std::vector<std::string> want = {"USEJUNCTION", "JUNCTIONCODE", "CountryName", "Language",
				"TimeZoneBias", "MyInstalledExpBits", "NWUID", "NWCDKIID", "NWCDKIIDEXP1", "NWPSSK",
				"NWUSID", "NWHWI"};
		expect(names_of(vars) == want, "a set junction word leads the Cookie list with its pair");
		expect(vars.size() >= 2 && vars[0].second == "1" && vars[1].second.empty(),
		       "USEJUNCTION is \"1\" and JUNCTIONCODE empty");
		const nw::LoginResult r = f.login("p", "s");
		expect(r.request.headers.size() >= 6 && r.request.headers[4] == "Cookie: USEJUNCTION=1;" &&
		               r.request.headers[5] == "Cookie: JUNCTIONCODE=;",
		       "the first request's cookie lines lead with the junction pair");
	}
	{
		nw::LobbyHttpContext c = concrete_ctx();
		c.identity_vars = identity;
		c.extended_metrics = false;
		nw::LobbyHttpFlow f;
		f.set_context(c);
		const std::vector<std::string> want = {"MyInstalledExpBits", "NWUID", "NWCDKIID", "NWCDKIIDEXP1",
				"NWPSSK", "NWUSID", "NWHWI"};
		expect(names_of(f.session_cookie_vars()) == want,
		       "a clear extended-metrics switch drops the locale three from the Cookie list");
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
		expect(f.cookies().find("CountryName") == nullptr && f.cookies().find("MyInstalledExpBits") != nullptr,
		       "the jar seeds no locale three, MyInstalledExpBits stays");
	}
	return g_fail == 0;
}

// The menu browser's content cache (browser_cache.h): a page's relative reference combined with the
// page's URL as URL_CombineComponents combines them, the cache key AsyncFetchURLContent files a
// download under, and the process's list: a key requested once, joined while in flight, served
// once stored, requested again after an end with nothing stored.
// [orig: URL_CombineComponents @0x650BC0; URL_ParseComponents @0x650140;
//  CUIBrowser_AsyncFetchURLContent @0x64DE80; HTTP_DownloadCompletionCallback @0x64DB20]
static bool test_browser_cache_rule() {
	// A relative path replaces the base's whole path and query; the base's host and port stay.
	expect(nw::browser_combine_url("http://gs.novaworld.net/NWLogin.dll", "jop_2_relay.mnx") ==
	               "http://gs.novaworld.net/jop_2_relay.mnx",
	       "a template joins the page's host");
	expect(nw::browser_combine_url("http://gs.opennova.test:8080/NWStart.dll?MSGBASE=jop_2_msg.htm",
	               "jop_2_login.mnx") == "http://gs.opennova.test:8080/jop_2_login.mnx",
	       "the base's port stays, its query goes");
	expect(nw::browser_combine_url("http://h/dir/page.htm", "t.mnx") == "http://h/t.mnx",
	       "no directory of the base's is kept");
	expect(nw::browser_combine_url("http://h/page.htm", "t.mnx?v=2") == "http://h/t.mnx?v=2",
	       "the reference's query rides along");
	expect(nw::browser_combine_url("http://h/page.htm", "HTTP://other:81/t.mnx") ==
	               "HTTP://other:81/t.mnx",
	       "a reference with a scheme is taken whole");
	// An absolute path is no relative one: the base's path and query come back (the combine's
	// last branch), as retail combines it.
	expect(nw::browser_combine_url("http://h/a.htm?q=1", "/t.mnx") == "http://h/a.htm?q=1",
	       "an absolute path keeps the base's path and query");

	// The key: "cache\" and the URL to its '?', every byte but an alphanumeric or '.' as '_'.
	expect(nw::browser_cache_key("http://gs.opennova.test:8080/jop_2_relay.mnx?nocachefix=1") ==
	               "cache\\http___gs.opennova.test_8080_jop_2_relay.mnx",
	       "the cache key");
	expect(nw::browser_cache_key("http://h:80/x.mnx") != nw::browser_cache_key("http://h/x.mnx"),
	       "a port spelled out is another key");

	nw::BrowserCache cache;
	const std::string url = "http://gs.novaworld.net/jop_2_main.mnx";
	expect(cache.fetch(url), "a key's first fetch is requested");
	expect(!cache.fetch(url), "a fetch of a key in flight joins it");
	cache.complete(url, true);
	expect(cache.holds(url), "a stored download is held");
	expect(!cache.fetch("HTTP://GS.NOVAWORLD.NET/JOP_2_MAIN.MNX?a=1"),
	       "a held key serves any URL of that key, its case and query apart");
	const std::string relay = "http://gs.novaworld.net/jop_2_relay.mnx";
	expect(cache.fetch(relay), "another key is requested");
	cache.complete(relay, false);
	expect(!cache.holds(relay) && cache.fetch(relay), "an end with nothing stored is requested again");
	cache.clear();
	expect(cache.fetch(url), "the list's free forgets every key");
	return g_fail == 0;
}

// A page's IB3_TEMPLATE as the menu browser's parser reads it.
// [orig: the IB3_TEMPLATE handler @0x6558C0; CUIHTMLParser_ParseTag @0x655E20]
static bool test_page_template_file() {
	expect(!nw::page_template_file("<HTML><HEAD></HEAD></HTML>").has_value(), "no template");
	expect(nw::page_template_file("<IB3_TEMPLATE file=\"jop_2_login.mnx\">").value_or("") ==
	               "jop_2_login.mnx",
	       "the FILE attribute, case-insensitively");
	expect(nw::page_template_file("<IB3_TEMPLATE FILE=a.mnx><IB3_TEMPLATE><ib3_template file='b.mnx'>")
	                       .value_or("") == "b.mnx",
	       "the last template with a FILE wins; one without leaves it");
	expect(nw::page_template_file("<IB3_TEMPLATE file=\"a.mnx\"></IB3_TEMPLATE>").value_or("") ==
	               "a.mnx",
	       "a closing tag changes nothing");
	expect(nw::page_template_file("<IB3_TEMPLATE file>").value_or("x").empty(),
	       "a FILE with no value names the empty reference");
	return g_fail == 0;
}

static nw::LobbyHttpContext templated_ctx() {
	nw::LobbyHttpContext c;
	c.title = &kJo;
	c.gate_tag = c.title->gate_tag;
	c.startup_url = "http://[domainname]/nwprepare.dll?ver1=[VER1]&gt=[GT]&url=jop_2_start.htm";
	c.web_domain = "gs.novaworld.net";
	c.locale = "en_US";
	return c;
}

// The service's pages (apps/novaworld_web/templates): the start page's refresh to NWStart, the
// login page's form and template, the relay page's, the main page's @GSB_SERVER@ and template.
static const std::string kStartPage =
		"<HTML><HEAD><META HTTP-EQUIV=\"refresh\" CONTENT=\"0;URL=NWStart.dll?MSGBASE=jop_2_msg.htm"
		"&IN=jop_2_main.htm&OUT=jop_2_login.htm\"></HEAD></HTML>";
static const std::string kLoginPage =
		"<HTML><BODY><FORM ACTION=\"NWLogin.dll\" METHOD=\"POST\"></FORM>"
		"<IB3_TEMPLATE file=\"jop_2_login.mnx\"></BODY></HTML>";
static const std::string kRelayPage =
		"<HTML><HEAD><META http-equiv=\"refresh\" CONTENT=0;URL=\"NWLogin.dll\"></HEAD>"
		"<IB3_TEMPLATE file=\"jop_2_relay.mnx\"></HTML>";
static const std::string kMainPage =
		"<HTML><IB3_GLOBAL name=\"@HOST_URL@\">http://gs.novaworld.net/nwhost.dll</IB3_GLOBAL>"
		"<IB3_SUBST name=\"@GSB_SERVER@\">http://gs.novaworld.net/jop_2.gsb</IB3_SUBST>"
		"<IB3_TEMPLATE file=\"jop_2_main.mnx\"></HTML>";

// The login's template legs, per process: each page's template is requested ahead of the leg's
// next request, and only the first time the process meets it (the LN S1 / S1b / S1c / S1d retail
// logins: login.mnx after NWStart's answer, relay.mnx after the POST's and ahead of the first poll,
// none after the second relay page, main.mnx after the last poll's and ahead of the GSB).
static bool test_login_template_legs() {
	nw::process_browser_cache().clear();
	nw::LobbyHttpFlow f;
	f.set_context(templated_ctx());
	nw::LoginResult r = f.login("p", "s");
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), bytes(kStartPage));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest && contains(r.request.url, "/NWStart.dll?"),
	            "prepare -> NWStart"))
		return false;
	r = f.on_login_response(true, 200, set_cookie("USEJUNCTION=0"), bytes(kLoginPage));
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest &&
	                    r.request.method == nw::HttpMethod::Get &&
	                    r.request.url == "http://gs.novaworld.net/jop_2_login.mnx" && r.request.cache_bust,
	            "the login page's template goes ahead of the post, with its nocachefix pair"))
		return false;
	r = f.on_login_response(true, 200, set_cookie("YOURIP=10.0.0.9"), bytes("<IB3 mnx>"));
	expect(f.cookies().find("YOURIP") != nullptr, "the template's answer sets its cookies");
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest && r.request.method == nw::HttpMethod::Post,
	            "the template's answer releases the post"))
		return false;
	r = f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t1"), bytes(kRelayPage));
	expect(r.kind == nw::LoginResult::Kind::NeedRequest &&
	               r.request.url == "http://gs.novaworld.net/jop_2_relay.mnx",
	       "the relay page's template goes ahead of its refresh");
	r = f.on_login_response(true, 200, {}, bytes("<IB3 relay mnx>"));
	expect(r.kind == nw::LoginResult::Kind::NeedRequest &&
	               r.request.url == "http://gs.novaworld.net/NWLogin.dll",
	       "then the refresh");
	r = f.on_login_response(true, 200, {}, bytes(kRelayPage));
	expect(r.kind == nw::LoginResult::Kind::NeedRequest &&
	               r.request.url == "http://gs.novaworld.net/NWLogin.dll",
	       "the second relay page's template is the cache's: the refresh goes at once");
	r = f.on_login_response(true, 200, {"Set-Cookie: NWHANDLE=ljim", "Set-Cookie: PCID=42"},
			bytes(kMainPage));
	expect(r.kind == nw::LoginResult::Kind::NeedRequest &&
	               r.request.url == "http://gs.novaworld.net/jop_2_main.mnx" && f.login_active(),
	       "the main page's template goes ahead of the login's success");
	r = f.on_login_response(true, 200, {}, bytes("<IB3 main mnx>"));
	if (!expect(r.kind == nw::LoginResult::Kind::Succeeded && r.nwhandle == "ljim" && !f.login_active(),
	            "the template's answer releases the success"))
		return false;

	// A later login in the process: every template is the cache's.
	nw::LobbyHttpFlow g;
	g.set_context(templated_ctx());
	g.login("p", "s");
	g.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), bytes(kStartPage));
	r = g.on_login_response(true, 200, {}, bytes(kLoginPage));
	expect(r.kind == nw::LoginResult::Kind::NeedRequest && r.request.method == nw::HttpMethod::Post,
	       "a stored login template: NWStart's answer -> the post at once");

	// A template whose answer stored nothing is requested again by the next page naming it.
	nw::process_browser_cache().clear();
	nw::LobbyHttpFlow k;
	k.set_context(templated_ctx());
	k.login("p", "s");
	k.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), bytes(kStartPage));
	k.on_login_response(true, 200, {}, bytes(kLoginPage));
	r = k.on_login_response(true, 404, {}, {});
	expect(r.kind == nw::LoginResult::Kind::NeedRequest && r.request.method == nw::HttpMethod::Post,
	       "a failed template answer still releases the post");
	expect(!nw::process_browser_cache().holds("http://gs.novaworld.net/jop_2_login.mnx"),
	       "and leaves nothing held");
	nw::process_browser_cache().clear();
	return g_fail == 0;
}

// The NovaWorld menu's re-entry (LobbyHttpFlow::enter): the startup URL, the start page's refresh to
// NWStart, whose page is the lobby (a remembered login; its template the process holds, so no
// request) or the sign-in page, whose Login then posts its form at once. The LN S1 / S1b / S1d
// retail re-entries: nwprepare, NWStart answering the main page, no main.mnx, the GSB.
// [orig: UI_EnterNovaWorldMenu @0x5588F0; UI_ProcessLANSessionStateMachine @0x558DE0 - state 9,
//  UIScene_OpenUrl called @0x5592ED]
static bool test_enter_reentry() {
	nw::process_browser_cache().clear();
	nw::process_browser_cache().fetch("http://gs.novaworld.net/jop_2_main.mnx");
	nw::process_browser_cache().complete("http://gs.novaworld.net/jop_2_main.mnx", true);
	nw::LobbyHttpFlow f;
	f.set_context(templated_ctx());
	nw::EnterResult r = f.enter();
	if (!expect(r.kind == nw::EnterResult::Kind::NeedRequest && r.request.cache_bust &&
	                    r.request.url ==
	                            "http://gs.novaworld.net/nwprepare.dll?ver1=3&gt=jop:cus2&url=jop_2_start.htm",
	            "enter() opens the resolved startup URL"))
		return false;
	expect(f.enter().kind == nw::EnterResult::Kind::Failed, "one entry at a time");
	r = f.on_enter_response(true, 200,
			{"Set-Cookie: EPASK=" + make_epask_cookie(), "Set-Cookie: NWHANDLE=ljim"}, bytes(kStartPage));
	if (!expect(r.kind == nw::EnterResult::Kind::NeedRequest &&
	                    r.request.url == "http://gs.novaworld.net/NWStart.dll?MSGBASE=jop_2_msg.htm"
	                                     "&IN=jop_2_main.htm&OUT=jop_2_login.htm",
	            "the start page's refresh to NWStart"))
		return false;
	r = f.on_enter_response(true, 200, set_cookie("USEJUNCTION=0"), bytes(kMainPage));
	expect(r.kind == nw::EnterResult::Kind::Lobby && r.nwhandle == "ljim" && !f.enter_active(),
	       "NWStart's lobby page, its template the process's: the lobby at once");
	expect(f.browser_global("@HOST_URL@") != nullptr, "the lobby page's globals are kept");

	// The service remembers no login: the sign-in page, its template requested first.
	r = f.enter();
	r = f.on_enter_response(true, 200, {}, bytes(kStartPage));
	r = f.on_enter_response(true, 200, {}, bytes(kLoginPage));
	expect(r.kind == nw::EnterResult::Kind::NeedRequest &&
	               r.request.url == "http://gs.novaworld.net/jop_2_login.mnx",
	       "the sign-in page's template goes first");
	r = f.on_enter_response(true, 200, {}, bytes("<IB3 login mnx>"));
	if (!expect(r.kind == nw::EnterResult::Kind::SignIn, "then the sign-in page")) return false;
	const nw::LoginResult post = f.login("p", "s");
	expect(post.kind == nw::LoginResult::Kind::NeedRequest && post.request.method == nw::HttpMethod::Post &&
	               post.request.url == "http://gs.novaworld.net/NWLogin.dll",
	       "the sign-in page's Login posts its form, with no second open");

	// A page that is neither fails with its message.
	r = f.enter();
	r = f.on_enter_response(true, 200, {}, bytes(kStartPage));
	r = f.on_enter_response(true, 200, {},
			bytes("<IB3_SUBST name=\"@GENERIC@\">NovaWorld is down for maintenance.</IB3_SUBST>"));
	expect(r.kind == nw::EnterResult::Kind::Failed && r.reason == "NovaWorld is down for maintenance.",
	       "a message page fails the entry with its text");
	nw::process_browser_cache().clear();
	return g_fail == 0;
}

// The connect's identity set rides the session's first request, whichever leg sends it (the
// entry's at the validated connect, D-NET-NEW-L9-38), and is seeded once: a later entry or login
// finds the jar as the pages left it (D-NET-NEW-L9-26).
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - USEJUNCTION @0x4CF74E;
//  CNapiGameSession_OnNovaWorldConnected @0x4D1570 - NWUID (the call @0x4D16A9)]
static bool test_enter_seeds_identity_once() {
	nw::process_browser_cache().clear();
	nw::LobbyHttpContext c = templated_ctx();
	c.use_junction = 1;
	c.identity_vars = {{"NWUID", ""}, {"MyInstalledExpBits", "3"}};
	c.server_nwuid = "ABCDEF0123456789";
	nw::LobbyHttpFlow f;
	f.set_context(c);
	nw::EnterResult r = f.enter();
	const std::vector<std::string> want = {"Cookie: USEJUNCTION=1;", "Cookie: JUNCTIONCODE=;",
			"Cookie: NWUID=ABCDEF0123456789;", "Cookie: MyInstalledExpBits=3;"};
	expect(r.request.headers.size() == 8 &&
	               std::vector<std::string>(r.request.headers.begin() + 4, r.request.headers.end()) == want,
	       "the first entry's request carries the identity set, in its order");
	r = f.on_enter_response(true, 200, set_cookie("USEJUNCTION=0"), bytes(kStartPage));
	r = f.on_enter_response(true, 200, {}, bytes(kLoginPage));
	r = f.on_enter_response(true, 200, {}, bytes("<IB3 login mnx>"));
	expect(r.kind == nw::EnterResult::Kind::SignIn, "the sign-in page");
	r = f.enter();
	const std::string *junction = f.cookies().find("USEJUNCTION");
	expect(junction != nullptr && *junction == "0", "a later entry keeps the page's USEJUNCTION");
	nw::process_browser_cache().clear();
	return g_fail == 0;
}

int main() {
	test_gate_reply_jar_fields();
	test_meta_refresh_walk();
	test_joi_page_refresh();
	test_browser_cache_bust();
	test_host_resolves();
	test_url_builders();
	test_login_success_concrete();
	test_login_templated_nwstart();
	test_login_failures();
	test_gsb_request_and_parse();
	test_join_resolves();
	test_browser_cache_rule();
	test_page_template_file();
	test_login_template_legs();
	test_enter_reentry();
	test_enter_seeds_identity_once();
	std::fprintf(stderr, g_fail == 0 ? "OK\n" : "FAILED (%d)\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
