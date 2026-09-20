// P7 Part 2 — the lobby HTTP flow (engine/net/novaworld/http_flow): the EPASK login chain, the GSB fetch,
// and the NWJoin handshake driven bytes-in/bytes-out (no Godot). Exercises the URL builders, the
// LoginStep/JoinStep machines, and the cookie glue against synthetic server responses.

#include <net/novaworld/http_flow.h>

#include <net/novaworld/gsb.h>
#include <net/novacrypto/epask.h>

#include <cstdio>
#include <string>
#include <vector>

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
static std::vector<std::string> set_cookie(const std::string &nv) {
	return {"Set-Cookie: " + nv};
}
static bool contains(const std::string &h, const std::string &needle) {
	return h.find(needle) != std::string::npos;
}

// A valid EPASK bundle the server "issues" in the prepare Set-Cookie.
static std::string make_epask_cookie() {
	return nw::epask_to_string(nw::generate_epask());
}

// Concrete (OpenNova) context: a non-templated startup_url -> login skips NWSTART, base derived from it.
static nw::LobbyHttpContext concrete_ctx() {
	nw::LobbyHttpContext c;
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

	// PREPARE response: the server issues the EPASK bundle as a cookie. Concrete url -> straight to POST.
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
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
	bool form_ct = false;
	for (const std::string &h : r.request.headers)
		if (contains(h, "Content-Type: application/x-www-form-urlencoded")) form_ct = true;
	expect(form_ct, "POST sets the form Content-Type");

	// POST response: LOGINSESSIONTAG -> the poll GET (carries ?tag=).
	r = f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=tag123"), {});
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "POST -> poll")) return false;
	expect(contains(r.request.url, "/NWLogin.dll?tag=tag123"), "poll carries the session tag");

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
	c.startup_url = "http://[domainname]/[VER1]/[VER2]/[CC]/[GT]/prep";
	c.web_domain = "gs.novaworld.net";
	c.locale = "en_US";
	f.set_context(c);

	nw::LoginResult r = f.login("p", "s");
	expect(r.kind == nw::LoginResult::Kind::NeedRequest, "templated login() -> prepare");
	r = f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
	if (!expect(r.kind == nw::LoginResult::Kind::NeedRequest, "templated prepare -> NWSTART")) return false;
	expect(r.request.method == nw::HttpMethod::Get, "NWSTART is a GET");
	expect(contains(r.request.url, "/NWStart.dll?MSGBASE=jop_2_msg.htm"), "NWSTART hits the dll with the literal query");
	expect(contains(r.request.url, "junction=jop_2_junction.htm"), "NWSTART query is byte-preserved");
	// NWSTART response -> POST.
	r = f.on_login_response(true, 200, {}, {});
	expect(r.kind == nw::LoginResult::Kind::NeedRequest && r.request.method == nw::HttpMethod::Post,
	       "NWSTART -> POST /NWLogin.dll");
	// Captured live .204 success needs two poll GETs. The first response is
	// still the relay page, where @MESSAGE@ is progress rather than failure.
	const std::string relay_page =
			"<HTML><IB3_SUBST name=\"@MESSAGE@\">Contacting login databases..."
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
	expect(contains(r.request.url, "/NWLogin.dll?tag=live-tag"),
	       "second poll retains the login session tag");
	r = f.on_login_response(true, 200,
			{"Set-Cookie: NWHANDLE=ljim", "Set-Cookie: PCID=A-A02-085D18"}, {});
	if (!expect(r.kind == nw::LoginResult::Kind::Succeeded,
	            "second live-shaped poll completes login")) return false;
	expect(r.nwhandle == "ljim" && r.pcid == "A-A02-085D18",
	       "live-shaped poll captures account identity");
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
		c.startup_url = "http://[domainname]/prep";
		c.web_domain = "gs.novaworld.net";
		f.set_context(c);
		f.login("p", "s");
		f.on_login_response(true, 200, set_cookie("EPASK=" + make_epask_cookie()), {});
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
	// [orig: parse_colon_delimited_string @0x666710 -1; EPASK_ModexpEncrypt @0x66668a -1]
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
		f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), {});
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
		f.on_login_response(true, 200, set_cookie("LOGINSESSIONTAG=t"), {});
		nw::LoginResult r;
		bool failed = false;
		for (int i = 0; i < 12; ++i) {
			r = f.on_login_response(true, 200, {}, {});
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

	// FIRST response: NWJOINSESSIONTAG -> the second leg (?rid=&tag=).
	r = f.on_join_response(true, 200, set_cookie("NWJOINSESSIONTAG=jtag"), {});
	if (!expect(r.kind == nw::JoinResult::Kind::NeedRequest, "NWJoin FIRST -> SECOND")) return false;
	expect(contains(r.request.url, "/NWJoin.dll?rid=777&tag=jtag"), "NWJoin second leg carries rid + tag");

	// SECOND response: the .joi body resolves host:port (NI/NP fallback) and
	// carries the proxy triple + lobby number verbatim for the transport.
	r = f.on_join_response(true, 200, {},
			bytes("<TITLE>[NI=192.168.5.9&NP=17479&BK=986119&LN=5&GS=x]</TITLE>"));
	if (!expect(r.kind == nw::JoinResult::Kind::Resolved, "NWJoin SECOND -> resolved")) return false;
	expect(r.host_ip == "192.168.5.9" && r.host_port == 17479, "join resolved host:port from the .joi");
	expect(r.ni == "192.168.5.9" && r.np == "17479" && r.bk == "986119",
	       "join carries the NI/NP/BK proxy triple verbatim");
	expect(r.ln == 5, "join carries the .joi lobby number");
	expect(!f.join_active(), "join machine resets on resolve");

	// A .joi with no connection string fails.
	nw::LobbyHttpFlow f2;
	f2.set_context(concrete_ctx());
	f2.join(1);
	f2.on_join_response(true, 200, {}, {});
	nw::JoinResult bad = f2.on_join_response(true, 200, {}, bytes("no brackets here"));
	expect(bad.kind == nw::JoinResult::Kind::Failed, "join with no .joi connection -> Failed");
	expect(bad.reason == "join: no connection string in .joi", "malformed .joi keeps its fallback reason");

	// Expansion and account restrictions arrive as a retail-style message page.
	nw::LobbyHttpFlow f3;
	f3.set_context(concrete_ctx());
	f3.join(2);
	f3.on_join_response(true, 200, {}, {});
	const std::string rejection =
			"<IB3_SUBST name=\"@GENERIC@\">This game requires the Team Sabre expansion."
			"<BR><I>Choose another game.</I></IB3_SUBST>";
	nw::JoinResult rejected = f3.on_join_response(true, 200, {}, bytes(rejection));
	expect(rejected.kind == nw::JoinResult::Kind::Failed, "message response rejects the join");
	expect(rejected.reason == "This game requires the Team Sabre expansion.\nChoose another game.",
	       "join failure preserves the rendered NovaWorld message");
	return g_fail == 0;
}

int main() {
	test_url_builders();
	test_login_success_concrete();
	test_login_templated_nwstart();
	test_login_failures();
	test_gsb_request_and_parse();
	test_join_resolves();
	std::fprintf(stderr, g_fail == 0 ? "OK\n" : "FAILED (%d)\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
