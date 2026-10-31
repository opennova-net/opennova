// The NovaWorld menu browser's HTTP transport (net/novaworld/browser_transport.h), the game's and
// serve's alike, against a loopback peer:
//  - the request bytes are the menu browser's, byte for byte (fixtures/novaworld/browser_get.http
//    and browser_post.http): "GET <path><query><pair> HTTP/1.0" or "POST <path> HTTP/1.0", the
//    port-less Host line, the flow's lines each CR LF, the closing CR LF, a post's body behind it,
//    one send; the pair over the tick and the one address the request resolved, which the connect
//    reaches; the fragment never sent (D-NET-1251, D-NET-1329);
//  - the name list: a name resolves once and its address serves every later request, the names
//    compared case-insensitively; a dotted quad and a refused name are never listed;
//  - the menu's creation frees the name list, its teardown the name list and the cache list;
//  - a 301 / 302 reaches the caller as it came;
//  - the channel: one request off the caller's thread, its reply taken once; a cancelled or a
//    superseded request never reports; the connect's wait is 60 s and the reply's unbounded.
// [orig: CUIBrowser_SendHTTPRequest @0x658840; GopherWebWidget_SendHttpPost @0x658B30;
//  CNapiWinsock_ResolveHostAsync @0x667460; CNapiWinsock_ResolveHostThread @0x666C70;
//  CUINameValueList_FindByName @0x64FF00; CNapiWinsock_ConnectNonBlocking @0x667600 - 0xEA60
//  @0x6677D6; CUICache_ScanCacheDirectory @0x64D6B0; CUICache_PruneFileCache @0x64D900]

#include <net/novaworld/browser_transport.h>

#include <net/http/http_exchange.h>
#include <net/socket/socket.h>

#include "common/loopback_http_peer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace net = opennova::net;
using opennova::BrowserChannel;
using opennova::BrowserNames;
using opennova::BrowserTransportOptions;
using opennova::HttpMethod;
using opennova::HttpRequestSpec;
using test_http::LoopbackHttpPeer;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::string fixture(const char *name) {
	std::ifstream in(std::string(FIXTURE_DIR) + "/" + name, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr uint32_t kTick = 676778718u;
uint32_t fixed_tick() { return kTick; }

std::atomic<int> g_lookups{0};

// The platform's lookup in the test: every name under opennova.test is the loopback.
bool test_lookup(const std::string &host, std::array<uint8_t, 4> &ip) {
	++g_lookups;
	if (host.size() < 13 || host.compare(host.size() - 13, 13, "opennova.test") != 0) {
		// A name the rule refuses (serve's destination rule) is never looked up past it.
		return false;
	}
	ip = {127, 0, 0, 1};
	return true;
}

BrowserTransportOptions options(int io_ms = 0) {
	BrowserTransportOptions o;
	o.lookup = test_lookup;
	o.tick_ms = fixed_tick;
	o.io_ms = io_ms;
	return o;
}

std::string port_of(const LoopbackHttpPeer &peer) { return std::to_string(peer.port()); }

HttpRequestSpec get_spec(const LoopbackHttpPeer &peer, const std::string &host,
		const std::string &path) {
	HttpRequestSpec spec;
	spec.valid = true;
	spec.method = HttpMethod::Get;
	spec.url = "http://" + host + ":" + port_of(peer) + path;
	spec.headers = {"Accept: image/jpeg, image/x-pcx, image/pcx, image/png, text/html",
			"Accept-Language: en", "User-Agent: NovaLogic IBrowse 2.0", "Pragma: no-cache",
			"Cookie: NWHANDLE=Walker;", "Cookie: PCID=4242;"};
	spec.cache_bust = true;
	return spec;
}

const std::vector<std::string> kOk = {"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok"};

void test_request_goldens() {
	opennova::process_browser_names().clear();
	{
		LoopbackHttpPeer peer(kOk);
		const net::HttpReply reply = opennova::browser_exchange(
				get_spec(peer, "nw.opennova.test", "/jop_2_main.mnx#top"), options());
		expect(reply.transport_ok && reply.code == 200, "the GET completes");
		const std::string golden = fixture("browser_get.http");
		expect(!golden.empty(), "the GET golden reads");
		expect(peer.request() == golden, "the GET's bytes are the menu browser's");
	}
	{
		LoopbackHttpPeer peer(kOk);
		HttpRequestSpec spec;
		spec.valid = true;
		spec.method = HttpMethod::Post;
		spec.url = "http://nw.opennova.test:" + port_of(peer) + "/NWLogin.dll";
		spec.body = "NWHANDLE=Walker&PASSWORD=x1";
		spec.headers = {"Accept: text/html", "Accept-Language: en",
				"User-Agent: NovaLogic IBrowse 2.0",
				"Content-Length: " + std::to_string(spec.body.size()),
				"Content-type: application/x-www-form-urlencoded", "Cookie: EPASK=1a2b;"};
		const net::HttpReply reply = opennova::browser_exchange(spec, options());
		expect(reply.transport_ok && reply.code == 200, "the form post completes");
		const std::string golden = fixture("browser_post.http");
		expect(!golden.empty(), "the post golden reads");
		expect(peer.request() == golden, "the post's bytes are the menu browser's");
	}
}

void test_name_list() {
	opennova::process_browser_names().clear();
	g_lookups = 0;
	{
		LoopbackHttpPeer first(kOk);
		(void)opennova::browser_exchange(get_spec(first, "gs.opennova.test", "/a"), options());
		LoopbackHttpPeer second(kOk);
		(void)opennova::browser_exchange(get_spec(second, "GS.OpenNova.Test", "/b"), options());
		expect(g_lookups.load() == 1, "a listed name is looked up once, its case ignored");
		expect(second.connections() == 1, "the listed address serves the later request");
	}
	{
		LoopbackHttpPeer peer(kOk);
		const int before = g_lookups.load();
		const net::HttpReply reply =
				opennova::browser_exchange(get_spec(peer, "127.0.0.1", "/c"), options());
		expect(reply.transport_ok && g_lookups.load() == before, "a dotted quad is never looked up");
	}
	{
		LoopbackHttpPeer peer(kOk);
		const net::HttpReply reply =
				opennova::browser_exchange(get_spec(peer, "gs.novaworld.net", "/d"), options());
		expect(!reply.transport_ok && reply.error == "cannot reach gs.novaworld.net",
		       "a refused name reaches nothing");
		expect(peer.connections() == 0, "and connects nowhere");
		expect(!opennova::process_browser_names().find("gs.novaworld.net"), "nor is it listed");
	}
	BrowserNames names;
	names.add("zero.opennova.test", {0, 0, 0, 0});
	expect(!names.find("zero.opennova.test"), "a listed 0.0.0.0 reads as none");
	names.add("Zero.OpenNova.Test", {10, 0, 0, 1});
	expect(names.find("zero.opennova.test") == std::array<uint8_t, 4>{10, 0, 0, 1},
	       "the newest entry of a name decides");
}

// The menu's creation frees the name list and leaves the cache serving nothing it did not; its
// teardown frees both, so the next fetch of a held template requests it again.
// [orig: CUICache_ScanCacheDirectory @0x64D6B0 - CUINameValueList_FreeAll called @0x64D6C5;
//  CUICache_PruneFileCache @0x64D900 - @0x64D918, CUICache_FreeEntryList called @0x64DA90]
void test_menu_lifetime() {
	BrowserNames names;
	opennova::BrowserCache cache;
	const std::string url = "http://gs.opennova.test/jop_2_main.mnx";
	names.add("gs.opennova.test", {10, 0, 0, 2});
	expect(cache.fetch(url), "a first fetch requests the template");
	cache.complete(url, std::vector<uint8_t>{'m', 'n', 'x'});
	opennova::browser_menu_created(names);
	expect(!names.find("gs.opennova.test"), "the menu's creation frees the name list");
	expect(cache.holds(url) && !cache.fetch(url), "and the held template still serves");
	names.add("gs.opennova.test", {10, 0, 0, 2});
	opennova::browser_menu_destroyed(names, cache);
	expect(!names.find("gs.opennova.test"), "the menu's teardown frees the name list");
	expect(!cache.holds(url) && cache.fetch(url), "and the cache list: the next fetch requests");
}

void test_redirect_handed_over() {
	LoopbackHttpPeer target(kOk);
	LoopbackHttpPeer peer({"HTTP/1.0 302 Found\r\nLocation: http://127.0.0.1:" + port_of(target) +
			"/x\r\nSet-Cookie: TAG=1\r\nContent-Length: 0\r\n\r\n"});
	const net::HttpReply reply =
			opennova::browser_exchange(get_spec(peer, "nw.opennova.test", "/r"), options());
	expect(reply.transport_ok && reply.code == 302, "the 302 reaches the caller");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	expect(target.connections() == 0, "and is followed nowhere");
}

// Wait at most `ms` for the channel's reply.
std::optional<net::HttpReply> await_reply(BrowserChannel &channel, int ms) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < until) {
		if (std::optional<net::HttpReply> reply = channel.take()) return reply;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return channel.take();
}

void test_channel() {
	expect(opennova::kBrowserConnectTimeoutMs == 60000, "the connect waits 60 s");
	expect(BrowserTransportOptions{}.connect_ms == 60000 && BrowserTransportOptions{}.io_ms == 0,
	       "the browser's waits: 60 s to connect, no bound on the reply");
	{
		LoopbackHttpPeer peer(kOk);
		BrowserChannel channel(options());
		expect(!channel.busy() && !channel.take(), "an idle channel has nothing out");
		channel.ship(get_spec(peer, "nw.opennova.test", "/one"));
		expect(channel.busy(), "a shipped request is out");
		const std::optional<net::HttpReply> reply = await_reply(channel, 5000);
		expect(reply && reply->transport_ok && reply->code == 200, "its reply is taken");
		expect(!channel.busy() && !channel.take(), "once");
	}
	{
		// A cancelled request never reports; the silent peer is still waited on, unbounded.
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		BrowserChannel channel(options());
		channel.ship(get_spec(silent, "nw.opennova.test", "/silent"));
		expect(silent.wait_request(3000), "the request reached the peer");
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		expect(!channel.take(), "a silent server's reply is waited on without a bound");
		channel.cancel();
		expect(!channel.busy(), "a cancel leaves nothing out");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		expect(!channel.take(), "and the cancelled request never reports");
	}
	{
		// A request shipped over a running one supersedes it: only the second reports.
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		LoopbackHttpPeer answering({"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n"});
		BrowserChannel channel(options());
		channel.ship(get_spec(silent, "nw.opennova.test", "/first"));
		expect(silent.wait_request(3000), "the first request went out");
		channel.ship(get_spec(answering, "nw.opennova.test", "/second"));
		const std::optional<net::HttpReply> reply = await_reply(channel, 5000);
		expect(reply && reply->code == 404, "the second request's reply");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		expect(!channel.take(), "the superseded one never reports");
	}
	{
		// A channel going away waits on nothing.
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		const auto at = std::chrono::steady_clock::now();
		{
			BrowserChannel channel(options());
			channel.ship(get_spec(silent, "nw.opennova.test", "/gone"));
			expect(silent.wait_request(3000), "the request went out");
		}
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - at).count();
		expect(took < 3500, "the channel's destructor does not wait on its request");
		// The cancelled worker ends at its next look at the cancel.
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	{
		// Serve's bound: a reply wait past it fails.
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		BrowserChannel channel(options(200));
		channel.ship(get_spec(silent, "nw.opennova.test", "/bound"));
		const std::optional<net::HttpReply> reply = await_reply(channel, 5000);
		expect(reply && !reply->transport_ok && reply->error == "reply timed out",
		       "a bound reply wait fails");
	}
}

} // namespace

int main() {
	net::startup();
	test_request_goldens();
	test_name_list();
	test_menu_lifetime();
	test_redirect_handed_over();
	test_channel();
	net::shutdown();
	std::fprintf(stderr, g_failures == 0 ? "OK\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
