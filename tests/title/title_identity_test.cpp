// The title identity table (engine/net/title/title_identity.h): JO's row complete and its
// wire bytes as before (the hello, the auth and the PV3 build string), DFX2's witnessed values
// with the rest refused by name, the gt.ssc gate-tag override over a mount (each file minted by
// our own encoder), and the consumers' refusal of a missing identity: the joiner, the game host,
// the NovaWorld lobby driver and the web flow.

#include <net/title/title_identity.h>

#include <base/gameprofile/gameprofile.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_session.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::gameprofile;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                            \
	do {                                                                                       \
		if (!(cond)) {                                                                         \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
			++g_failures;                                                                      \
		}                                                                                      \
	} while (0)

bool contains(const std::vector<std::string> &list, const char *name) {
	return std::find(list.begin(), list.end(), name) != list.end();
}

std::vector<uint8_t> bytes_of(const std::string &s) {
	return std::vector<uint8_t>(s.begin(), s.end());
}

// JO's row is complete and holds the values the code carried before the table: the in-match
// block of CNapiNetwork_Init, the lobby's of CNapiGameSession_InitNPConnection, the version pair,
// the product code, the gate tag and gt.ssc's key chain, the pfid and the jop_2 pages.
void jo_row() {
	const TitleIdentity *jo = title_identity(GAME_JO);
	CHECK(jo != nullptr);
	if (jo == nullptr) return;
	CHECK(title_identity_complete(*jo, TitleIdentityPart::Session));
	CHECK(title_identity_complete(*jo, TitleIdentityPart::WebFlow));
	CHECK(title_identity_missing(*jo, TitleIdentityPart::Session).empty());
	CHECK(title_identity_refusal(*jo, TitleIdentityPart::WebFlow).empty());
	CHECK(!jo->host_category_var);
	CHECK(std::string(jo->nvs) ==
			"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic");
	CHECK(std::string(jo->co) == "NovaLogic Inc, Calabasas CA U.S.A.");
	CHECK(std::string(jo->ap) == "Jointops.exe");
	CHECK(std::string(jo->bdat) == "Jul 21 2009 18:54:42");
	CHECK(std::string(jo->pn) == "JOINTOPERATIONS");
	const std::array<uint8_t, 16> guid{0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47,
	                                   0x92, 0xDA, 0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68};
	CHECK(jo->guid.has_value() && *jo->guid == guid);
	CHECK(std::string(jo->pv1) == "0.0.0 1/12/2004 EM");
	CHECK(std::string(jo->pv2) == "16");
	CHECK(jo->build.has_value() && jo->build->tag_address_a == 8165168u &&
			jo->build->tag_address_b == 8165140u);
	CHECK(title_protocol_version3(*jo) ==
			"CONSUMER RELEASE 1.7.5.7 [8165168.8165140] Jul 21 2009 18:54:42");
	CHECK(std::string(jo->lobby_pv1) == "0.0.0 2/10/2004 EM");
	CHECK(std::string(jo->lobby_bdat) == "Jul 21 2009 18:54:41");
	CHECK(std::string(jo->ver1) == "3" && std::string(jo->ver2) == "2345");
	CHECK(std::string(jo->product_code) == "jop");
	CHECK(std::string(jo->gate_tag) == "jop:cus2");
	CHECK(std::string(jo->gate_tag_keys) == "jop:2:oyez");
	CHECK(jo->pfid.has_value() && *jo->pfid == 28);
	CHECK(jo->pages.has_value() && std::string(jo->pages->server_list) == "jop_2.gsb" &&
			std::string(jo->pages->main) == "jop_2_main.htm");
	std::string error;
	CHECK(resolve_title_identity("jo", error) == jo);
	CHECK(resolve_title_identity("JO", error) == jo);
	CHECK(title_identity_by_protocol_name("jointoperations") == jo);
}

// JO's hello and auth through the title are the bytes the hand-filled retail identity makes.
void jo_wire_bytes_unchanged() {
	const TitleIdentity &jo = *title_identity(GAME_JO);
	ClientHello expected;
	expected.nvs = "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";
	expected.co = "NovaLogic Inc, Calabasas CA U.S.A.";
	expected.ap = "Jointops.exe";
	expected.bdat = "Jul 21 2009 18:54:42";
	expected.pn = "JOINTOPERATIONS";
	expected.pg = *jo.guid;
	expected.pg_present = true;
	expected.pv1 = "0.0.0 1/12/2004 EM";
	expected.pv2 = "16";
	expected.pv3 = "CONSUMER RELEASE 1.7.5.7 [8165168.8165140] Jul 21 2009 18:54:42";
	expected.ci = 5;
	const ClientHello hello = make_title_client_hello(jo, 5);
	CHECK(client_hello_to_bytes(hello) == client_hello_to_bytes(expected));
	CHECK(matches_title_identity(jo, hello));
	CHECK(client_hello_admits(jo, hello));
	const ClientAuth auth = make_title_client_auth(jo, 5, 0x1234ABCDu, 77u, "Writer", "SCRK");
	CHECK(auth.pn == expected.pn && auth.pv3 == expected.pv3 && auth.na == "Writer" &&
			auth.pg == expected.pg && auth.pv2 == "16");
	CHECK(matches_title_identity(jo, auth));
	ClientHello other = hello;
	other.pn = "DELTAFORCEXTREME2";
	CHECK(!matches_title_identity(jo, other));
	CHECK(!client_hello_admits(jo, other));
	other.pm = 3; // a peer host's announce is answered whatever its identity
	CHECK(client_hello_admits(jo, other));
}

// DFX2's row (docs/titles/dfx2-vs-jo-re.md §6): complete, its hello the bytes its own values
// make and not JO's.
void dfx2_row() {
	const TitleIdentity *dfx2 = title_identity(GAME_DFX2);
	CHECK(dfx2 != nullptr);
	if (dfx2 == nullptr) return;
	const TitleIdentity &jo = *title_identity(GAME_JO);
	CHECK(std::string(dfx2->nvs) == std::string(jo.nvs) && std::string(dfx2->co) == jo.co);
	CHECK(std::string(dfx2->ap) == "dfx2.exe");
	CHECK(std::string(dfx2->bdat) == "Jul  7 2009 11:33:09");
	CHECK(std::string(dfx2->pn) == "DELTAFORCEXTREME2");
	const std::array<uint8_t, 16> guid{0xA6, 0x94, 0xE0, 0xC3, 0x49, 0xB7, 0x60, 0x4B,
	                                   0xBB, 0x66, 0x4A, 0x04, 0x38, 0xEF, 0x9C, 0xEB};
	CHECK(dfx2->guid.has_value() && *dfx2->guid == guid);
	CHECK(std::string(dfx2->pv1) == "1.0.0 3-23-2005 EM dfxowkjzf");
	CHECK(std::string(dfx2->pv2) == "16");
	CHECK(title_protocol_version3(*dfx2) ==
			"CONSUMER RELEASE 1.7.5.7 [8160576.8160548] Jul  7 2009 11:33:09");
	CHECK(title_protocol_version3(*dfx2).size() == 63);
	CHECK(std::string(dfx2->lobby_pv1) == "0.0.0 2/10/2004 EM");
	CHECK(std::string(dfx2->lobby_bdat) == "Jul  7 2009 11:33:09");
	CHECK(std::string(dfx2->ver1) == "1" && std::string(dfx2->ver2) == "9013");
	CHECK(std::string(dfx2->product_code) == "dfx2");
	CHECK(std::string(dfx2->gate_tag) == "dfx2:0:cus:buffy");
	CHECK(std::string(dfx2->gate_tag_keys) == "dfx2:0:buffy");
	CHECK(dfx2->host_category_var);
	CHECK(dfx2->pfid.has_value() && *dfx2->pfid == 38);
	CHECK(title_identity_by_protocol_name("DELTAFORCEXTREME2") == dfx2);
	CHECK(title_identity_complete(*dfx2, TitleIdentityPart::Session));
	std::string error;
	CHECK(resolve_title_identity("dfx2", error) == dfx2);
	CHECK(title_identity_complete(*dfx2, TitleIdentityPart::WebFlow));
	CHECK(dfx2->pages.has_value() && std::string(dfx2->pages->server_list) == "dfx2_0.gsb" &&
			std::string(dfx2->pages->activate) == "dfx2_0_actlogin.htm" &&
			std::string(dfx2->pages->glsvss_request) == "[dfx]kills" &&
			std::string(dfx2->pages->join_lan) == "0");
	CHECK(jo.pages->activate == nullptr && jo.pages->glsvss_request == nullptr &&
			jo.pages->join_lan == nullptr);
	// The demo's web flow is not witnessed: refused by name.
	CHECK(title_identity_refusal(*title_identity(GAME_JO_DEMO), TitleIdentityPart::WebFlow) ==
			"the jodemo title's web flow identity is not witnessed yet: pfid, web pages");

	// The hello: DFX2's block, which a JO identity does not match and a DFX2 one does.
	const ClientHello hello = make_title_client_hello(*dfx2, 9);
	CHECK(hello.pn == "DELTAFORCEXTREME2" && hello.pg == guid && hello.ap == "dfx2.exe" &&
			hello.pv1 == "1.0.0 3-23-2005 EM dfxowkjzf" && hello.bdat == "Jul  7 2009 11:33:09");
	CHECK(matches_title_identity(*dfx2, hello));
	CHECK(!matches_title_identity(jo, hello));
	CHECK(!matches_title_identity(*dfx2, make_title_client_hello(jo, 9)));
	ClientHello round;
	const std::vector<uint8_t> bytes = client_hello_to_bytes(hello);
	CHECK(parse_client_hello(bytes.data(), bytes.size(), round) && round.pv3 == hello.pv3 &&
			round.pg == guid && matches_title_identity(*dfx2, round));

	// The rows no binary has been read for: refused, the demo's gate tag its one value.
	CHECK(resolve_title_identity("bhd", error) == nullptr);
	CHECK(error.find("the bhd title's session identity is not witnessed yet: version text, NVS") == 0);
	CHECK(resolve_title_identity("dfx", error) == nullptr);
	const TitleIdentity *demo = title_identity(GAME_JO_DEMO);
	CHECK(demo != nullptr && std::string(demo->gate_tag) == "jopd:cus4");
	CHECK(resolve_title_identity("jodemo", error) == nullptr);
	CHECK(error.find("PG") != std::string::npos && error.find("gate tag") == std::string::npos);
	CHECK(resolve_title_identity("nosuchgame", error) == nullptr);
	CHECK(error == "unknown game 'nosuchgame'");
	CHECK(title_identity_by_protocol_name("NOSUCHGAME") == nullptr);
}

// gt.ssc's decode as the boot reads it: the text to its first NUL under the key chain, the room
// for 2047 decoded bytes, the 0x1FFF-byte file bound and the 63-character tag.
void gate_tag_file_decode() {
	const char *keys = title_identity(GAME_JO)->gate_tag_keys;
	std::string tag = "unchanged";
	const std::string on = encode_key_chain(bytes_of("on:cus1"), keys);
	CHECK(decode_gate_tag_file(bytes_of(on), keys, tag) && tag == "on:cus1");
	CHECK(decode_gate_tag_file(bytes_of(on + "\r\n"), keys, tag) && tag == "on:cus1");
	// The text ends at its first NUL.
	std::vector<uint8_t> with_nul = bytes_of(on);
	with_nul.push_back(0);
	for (char c : std::string("GARBAGE")) with_nul.push_back(static_cast<uint8_t>(c));
	CHECK(decode_gate_tag_file(with_nul, keys, tag) && tag == "on:cus1");
	// The plaintext's C string, at most 63 characters.
	std::vector<uint8_t> nul_inside = bytes_of("ab");
	nul_inside.push_back(0);
	nul_inside.push_back('c');
	CHECK(decode_gate_tag_file(bytes_of(encode_key_chain(nul_inside, keys)), keys, tag) &&
			tag == "ab");
	const std::string long_tag(100, 'x');
	CHECK(decode_gate_tag_file(bytes_of(encode_key_chain(bytes_of(long_tag), keys)), keys, tag) &&
			tag == std::string(kGateTagChars, 'x'));
	// An empty plaintext empties the tag, as retail's copy does.
	CHECK(decode_gate_tag_file(bytes_of(encode_key_chain({}, keys)), keys, tag) && tag.empty());
	// What does not decode leaves the tag alone.
	tag = "kept";
	CHECK(!decode_gate_tag_file(bytes_of(on + "Z"), keys, tag) && tag == "kept");
	CHECK(!decode_gate_tag_file(bytes_of(encode_key_chain(bytes_of("on:cus1"), "jop")), keys, tag));
	CHECK(!decode_gate_tag_file(bytes_of(on), nullptr, tag) && tag == "kept");
	// The decoder's room: an A-P text of more than 2047 bytes does not decode.
	const std::vector<uint8_t> fits(kGateTagDecodeRoom - 12, 'y');
	const std::string fits_text = encode_key_chain(fits, keys);
	CHECK(fits_text.size() / 2 == kGateTagDecodeRoom);
	CHECK(decode_gate_tag_file(bytes_of(fits_text), keys, tag));
	const std::string over_text = encode_key_chain(std::vector<uint8_t>(fits.size() + 1, 'y'), keys);
	CHECK(over_text.size() <= kGateTagFileMaxBytes);
	tag = "kept";
	CHECK(!decode_gate_tag_file(bytes_of(over_text), keys, tag) && tag == "kept");
	// The file bound: past 0x1FFF bytes nothing is read.
	std::vector<uint8_t> padded = bytes_of(on);
	padded.resize(kGateTagFileMaxBytes + 1, '\n');
	CHECK(!decode_gate_tag_file(padded, keys, tag) && tag == "kept");
	padded.resize(kGateTagFileMaxBytes);
	CHECK(decode_gate_tag_file(padded, keys, tag) && tag == "on:cus1");
	// The room parameter on the codec itself.
	std::vector<uint8_t> plain;
	CHECK(decode_key_chain(on, keys, plain, (on.size() / 2)) && plain == bytes_of("on:cus1"));
	CHECK(!decode_key_chain(on, keys, plain, (on.size() / 2) - 1));
}

// A socket that keeps what it sends and receives nothing.
class SentSocket final : public IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
	std::vector<std::vector<uint8_t>> sent;
};

fs::path fresh_dir(const std::string &tag) {
	const fs::path dir = fs::temp_directory_path() /
			("opennova_title_" + tag + "_" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

void write_file(const fs::path &path, const std::string &text) {
	std::ofstream out(path, std::ios::binary);
	out << text;
}

// The gate tag over a mount: gt.ssc, read loose first, replaces the title's tag when it decodes
// under the title's key chain; none, or one that does not decode, leaves the title's.
void gate_tag_over_mount() {
	const TitleIdentity &jo = *title_identity(GAME_JO);
	CHECK(title_gate_tag(jo, nullptr) == "jop:cus2");
	const fs::path dir = fresh_dir("gt");
	write_file(dir / "readme.txt", "a mount with no gt.ssc");
	{
		ResourceIndex index;
		CHECK(index.scan(dir.string(), std::string(), VfsMountMode::LooseOnly,
				VfsArchiveDiscovery::ScanAll));
		CHECK(title_gate_tag(jo, &index) == "jop:cus2");
	}
	write_file(dir / "GT.SSC", encode_key_chain(bytes_of("on:cus1"), jo.gate_tag_keys) + "\r\n");
	{
		ResourceIndex index;
		CHECK(index.scan(dir.string(), std::string(), VfsMountMode::LooseOnly,
				VfsArchiveDiscovery::ScanAll));
		CHECK(title_gate_tag(jo, &index) == "on:cus1");
	}
	write_file(dir / "GT.SSC", encode_key_chain(bytes_of("on:cus1"), "dfx2:0:buffy"));
	{
		ResourceIndex index;
		CHECK(index.scan(dir.string(), std::string(), VfsMountMode::LooseOnly,
				VfsArchiveDiscovery::ScanAll));
		CHECK(title_gate_tag(jo, &index) == "jop:cus2");
	}
	std::error_code ec;
	fs::remove_all(dir, ec);
}

// The consumers refuse a missing identity: the joiner does not start, the game host answers no
// hello (and a host of JO answers no other title's), the lobby driver does not probe, and the web
// flow requests nothing.
void consumers_refuse_without_identity() {
	const TitleIdentity &jo = *title_identity(GAME_JO);

	inmatch::JoinerConnection joiner("NoTitle");
	CHECK(joiner.start().empty());
	CHECK(joiner.last_error() == "no title identity to join with");
	inmatch::JoinerConnection titled("Titled");
	titled.set_title(&jo);
	CHECK(!titled.start().empty());

	const PeerAddr peer{0x0100007Fu, 30740};
	const std::vector<uint8_t> hello =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(make_title_client_hello(jo, 1)));
	for (const TitleIdentity *installed : {static_cast<const TitleIdentity *>(nullptr), &jo}) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(ctx, inmatch::SocketMode::Lan);
		inmatch::SessionStartup startup;
		startup.title = installed;
		startup.host_key = 0x5EED;
		inmatch::GameConfig config;
		config.max_players = 2;
		inmatch::create_session(ctx, config, startup, nullptr);
		CHECK(ctx.np_protocol.identity == installed);
		const inmatch::HandleResult reply =
				inmatch::handle_server_datagram(ctx, peer, hello.data(), hello.size(), 1);
		CHECK(reply.outbound.empty() == (installed == nullptr));
		if (installed == nullptr) continue;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ServerHello sh;
		CHECK(!reply.outbound.empty() &&
				nw_decode_inbound(reply.outbound[0].data(), reply.outbound[0].size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_HELLO && parse_server_hello(body.data(), body.size(), sh));
		CHECK(sh.pn == "JOINTOPERATIONS" && sh.pg == *jo.guid &&
				sh.pv3 == "CONSUMER RELEASE 1.7.5.7 [8165168.8165140] Jul 21 2009 18:54:42");
		ClientHello foreign = make_title_client_hello(jo, 2);
		foreign.pn = "DELTAFORCEXTREME2";
		const std::vector<uint8_t> foreign_dg =
				nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(foreign));
		CHECK(inmatch::handle_server_datagram(ctx, peer, foreign_dg.data(), foreign_dg.size(), 2)
						.outbound.empty());
	}

	std::vector<std::string> fatals;
	NwuLobbySession::Hooks hooks;
	hooks.on_fatal = [&fatals](const std::string &message) { fatals.push_back(message); };
	NwuLobbySession lobby(hooks, NwuLobbySession::Environment{});
	CHECK(lobby.title() == nullptr);
	SentSocket gate, session;
	lobby.open(gate, session);
	lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	CHECK(gate.sent.empty());
	CHECK(fatals.size() == 1 && fatals[0] == "no title identity to probe the gate with");
	lobby.close();
	// With the title, the probe carries the gate manager's tag (here gt.ssc's stand-in).
	lobby.set_title(&jo, "on:cus1");
	lobby.open(gate, session);
	lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	CHECK(gate.sent.size() == 1);
	const std::vector<uint8_t> inner = gate_probe_build("on:cus1");
	std::vector<uint8_t> packet(inner.size() + 4);
	std::size_t packet_size = 0;
	CHECK(napi_envelope_encode(inner.data(), inner.size(), packet.data(), packet.size(),
			&packet_size) == 0);
	packet.resize(packet_size);
	CHECK(gate.sent.size() == 1 && gate.sent[0] == packet);
	lobby.close();

	LobbyHttpFlow flow;
	LobbyHttpContext ctx;
	ctx.startup_url = "http://127.0.0.1:8080/NWStart.dll";
	flow.set_context(ctx);
	CHECK(flow.login("user", "pass").kind == LoginResult::Kind::Failed);
	CHECK(!flow.gsb_request().valid);
	ctx.title = &jo;
	ctx.gate_tag = jo.gate_tag;
	flow.set_context(ctx);
	CHECK(flow.login("user", "pass").kind == LoginResult::Kind::NeedRequest);
	CHECK(flow.gsb_request().valid &&
			flow.gsb_request().url == "http://127.0.0.1:8080/jop_2.gsb?a=1");
	// DFX2's flow requests its own pages: the dfx2_0 feed and the join link its main page carries
	// (lan=0 first, pfid 38).
	const TitleIdentity &dfx2 = *title_identity(GAME_DFX2);
	LobbyHttpFlow dfx2_flow;
	ctx.title = &dfx2;
	ctx.gate_tag = dfx2.gate_tag;
	dfx2_flow.set_context(ctx);
	CHECK(dfx2_flow.gsb_request().url == "http://127.0.0.1:8080/dfx2_0.gsb?a=1");
	const JoinResult join = dfx2_flow.join(7);
	CHECK(join.kind == JoinResult::Kind::NeedRequest &&
			join.request.url ==
					"http://127.0.0.1:8080/NWJoin.dll?lan=0&needexpkey=dfx2_0_key2err.htm"
					"&success=dfx2_0_join.joi&failure=dfx2_0_main.htm&relay=dfx2_0_relay.htm"
					"&msgbase=dfx2_0_msg.htm&nodb=dfx2_0_nodb.htm&pfid=38&mode=Login&rid=7");
	LobbyHttpFlow jo_flow;
	ctx.title = &jo;
	ctx.gate_tag = jo.gate_tag;
	jo_flow.set_context(ctx);
	CHECK(jo_flow.join(7).request.url ==
			"http://127.0.0.1:8080/NWJoin.dll?needexpkey=jop_2_key2err.htm&success=jop_2_join.joi"
			"&failure=jop_2_main.htm&relay=jop_2_relay.htm&msgbase=jop_2_msg.htm&nodb=jop_2_nodb.htm"
			"&pfid=28&mode=Login&rid=7");
	// The demo has no web flow to speak.
	LobbyHttpFlow demo_flow;
	ctx.title = title_identity(GAME_JO_DEMO);
	ctx.gate_tag = ctx.title->gate_tag;
	demo_flow.set_context(ctx);
	CHECK(demo_flow.login("user", "pass").reason ==
			"the jodemo title's web flow identity is not witnessed yet: pfid, web pages");
	CHECK(!demo_flow.gsb_request().valid && demo_flow.join(7).kind == JoinResult::Kind::Failed);
	ctx.title = &dfx2;
	ctx.gate_tag = dfx2.gate_tag;
	// Its start page's tokens are its own (the live gate's template, dfx2-vs-jo-re §6.5).
	ctx.startup_url = "http://[domainname]/nwprepare.dll?ver1=[VER1]&ver2=[VER2]&cc=[CC]&pfid=38"
	                  "&gt=[GT]&url=dfx2_0_start.htm&pc=[PRODUCTCODE]";
	ctx.web_domain = "nw.example";
	ctx.locale = "en_US";
	dfx2_flow.set_context(ctx);
	CHECK(dfx2_flow.resolve_startup_url() ==
			"http://nw.example/nwprepare.dll?ver1=1&ver2=9013&cc=us&pfid=38&gt=dfx2:0:cus:buffy"
			"&url=dfx2_0_start.htm&pc=dfx2");
}

std::vector<std::string> names_of(const std::vector<ClientVar> &vars) {
	std::vector<std::string> out;
	for (const ClientVar &v : vars) out.push_back(v.name);
	return out;
}

std::string value_of(const std::vector<ClientVar> &vars, const char *name) {
	for (const ClientVar &v : vars)
		if (v.name == name) return v.value;
	return "<absent>";
}

// The host lists carry the title's version pair, and DFX2's DFXCATEGORY1 right after LAN on
// both lists (DFX2-7); no LobbyName until a gate names one.
void host_lists() {
	HostRegistration reg;
	CHECK(reg.lobby_name.empty());
	reg.ver1 = title_identity(GAME_DFX2)->ver1;
	reg.ver2 = title_identity(GAME_DFX2)->ver2;
	const std::vector<ClientVar> host = make_host_var_list(reg, HostLobbyText{}, /*full=*/true);
	CHECK(value_of(host, "Ver1") == "1" && value_of(host, "Ver2") == "9013");
	CHECK(value_of(host, "DFXCATEGORY1") == "<absent>");
	CHECK(value_of(make_host_setup_var_list(reg), "DFXCATEGORY1") == "<absent>");

	reg.category_var = true;
	reg.category = 4;
	const std::vector<std::string> setup = names_of(make_host_setup_var_list(reg));
	const std::vector<std::string> initial = names_of(make_host_var_list(reg, HostLobbyText{}, false));
	for (const std::vector<std::string> *list : {&setup, &initial}) {
		const auto lan = std::find(list->begin(), list->end(), "LAN");
		CHECK(lan != list->end() && lan + 1 != list->end() && *(lan + 1) == "DFXCATEGORY1");
	}
	CHECK(value_of(make_host_setup_var_list(reg), "DFXCATEGORY1") == "4");
	CHECK(value_of(make_host_var_list(reg, HostLobbyText{}, true), "DFXCATEGORY1") == "4");
}

} // namespace

int main() {
	jo_row();
	jo_wire_bytes_unchanged();
	dfx2_row();
	gate_tag_file_decode();
	gate_tag_over_mount();
	consumers_refuse_without_identity();
	host_lists();
	if (g_failures == 0) {
		std::printf("title_identity: all checks passed\n");
		return 0;
	}
	std::printf("title_identity: %d check(s) failed\n", g_failures);
	return 1;
}
