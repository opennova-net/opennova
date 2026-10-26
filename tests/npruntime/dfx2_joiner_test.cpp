// A joiner speaking Delta Force: Xtreme 2 takes DFX2's branches of the shared joiner code
// (docs/titles/dfx2-vs-jo-re.md §7, the §8 catalog), each checked against JO's:
//
//   DFX2-12  its C2S 0x2F class byte is 8 (rifleman), 7 (gunner) when the host's BMS header names
//            a mission of type 2, whatever the kit's class; JO's sends the kit's class
//   DFX2-13  its LAN C2S 0x00 JOIN leads with its CDKIID (the same code in both titles: retail
//            writes the TLVs in a LAN session when the registry key holds one); a NovaWorld join
//            (one carrying the .joi APPID) sends none
//   the 0x42 build CU values are the title's (BN 1, VN 2, MBN 20042002, SOPD 180 in both)
//   D-NET-723 a NovaWorld C2S 0x00 JOIN carries the gate's GLSVSSREQUEST last when the gate gave
//            one (DFX2's [dfx2]kills, any title's); a LAN JOIN never does
//
// [orig: NetPacket_SendLoadoutSubmit @0x42ceae; NapiNP_WriteClientAuthPayload @0x42a1cb..0x42a235;
//  CNapiServerInfo_SerializeToSession @0x4c3650] [orig: @0x42ec1f..0x42ec3e, @0x42c25b..0x42c2c5,
//  the twin @0x4c3730 (dfx2.exe)]

#include <base/gameprofile/gameprofile.h>
#include <formats/mission/bms.h>
#include <net/novacrypto/cdkey.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/joiner_connection.h>

#include "enumerated_join.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "joiner_receive.h"

using namespace opennova;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

const TitleIdentity *jo() { return title_identity(gameprofile::GAME_JO); }
const TitleIdentity *dfx2() { return title_identity(gameprofile::GAME_DFX2); }

constexpr uint32_t kClientKey = 0x11223344u;
constexpr uint32_t kServerKey = 0x55667788u;
const std::string kClientScrk = "CLIENT-DFX2-SCRK";
const std::string kServerScrk = "SERVER-DFX2-SCRK";

std::vector<uint8_t> frame_s2c(SessionSequencing &sequencing, std::vector<ProtocolMessage> messages,
		const std::string &server_scrk, uint32_t client_key) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(sequencing, SessionCrypto{server_scrk, {}, client_key}, messages, body))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

bool decode_c2s(const std::vector<uint8_t> &datagram, const std::string &client_scrk,
		std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ProtocolPacketHeader header;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
			decode_protocol_packet_plaintext(body.data(), body.size(), client_scrk, header, messages);
}

// The class byte of the 0x2F an in-match joiner speaking `title`, whose kit is a medic's (5),
// re-submits when an S2C 0x50 moves its own entity, after the host's BMS header named a mission
// of `mission_type` (0: no header). The sender fixes the class for every 0x2F (the armory's
// takes it in inmatch::armory_accept_in_session, pinned by damage_class_bytes).
// [orig: NapiNPClientMsg_TeamAssign @0x431910 - the reselect's submit @0x431AAD]
int resubmit_class(const TitleIdentity *title, uint8_t mission_type) {
	inmatch::JoinerConnection joiner("Dfx2Loadout");
	joiner.set_title(title);
	inmatch::JoinerConnection::LoadoutKit kit;
	kit.player_class = 5;
	joiner.set_loadout_kit(kit);
	joiner.seed_in_match(1, kClientKey, kClientScrk, kServerScrk, 1, 0, 0x0001, 0x14B9);
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	if (mission_type != 0) {
		std::vector<uint8_t> header(bms::kHeaderSize, 0);
		header[offsetof(bms::Header, mission_type)] = mission_type;
		const std::vector<uint8_t> dg =
				frame_s2c(seq, {make_protocol_message(s2c::BMS_HEADER, header)}, kServerScrk, kClientKey);
		(void)joiner_receive::receive(joiner, dg.data(), dg.size());
	}
	TeamAssign assign;
	assign.entity_handle = 0x0001;
	assign.team = 1;
	const std::vector<uint8_t> dg = frame_s2c(seq,
			{make_protocol_message(s2c::TEAM_ASSIGN, encode_team_assign(assign))}, kServerScrk, kClientKey);
	const joiner_receive::Received result = joiner_receive::receive(joiner, dg.data(), dg.size());
	for (const ProtocolMessage &m : result.queued_send_messages) {
		if (m.tag != 0x2F) continue;
		LoadoutSubmit submit;
		if (decode_loadout_submit(m.payload.data(), m.payload.size(), submit)) return submit.player_class;
	}
	return -1;
}

void loadout_class_byte() {
	expect(resubmit_class(jo(), 0) == 5 && resubmit_class(jo(), 2) == 5, "a JO joiner sends its kit's class");
	expect(resubmit_class(dfx2(), 0) == 8, "DFX2-12: a DFX2 joiner sends 8 whatever its kit");
	expect(resubmit_class(dfx2(), 1) == 8, "DFX2-12: 8 in a mission of type 1");
	expect(resubmit_class(dfx2(), 2) == 7, "DFX2-12: 7 in a mission of type 2");
}

// Drive a joiner speaking `title` from its hello to its JOIN and return the JOIN's TLV names, its
// CDKIID and GLSVSSREQUEST (each empty when it sent none), and the ClientAuth's CU (name, value)
// pairs.
struct JoinSent {
	std::vector<std::string> join_fields;
	std::string cdkiid;
	std::string glsvss_request;
	std::vector<std::pair<std::string, std::string>> cu;
	bool ok = false;
};

JoinSent join_of(const TitleIdentity *title, const cdkey::WireKeys &ids, const std::string &app_id,
		const std::string &glsvss_request = {}, const std::vector<uint8_t> &cd_cookie = {}) {
	JoinSent out;
	inmatch::JoinerConnection joiner("Dfx2Joiner");
	joiner.set_title(title);
	joiner.set_cd_key_ids(ids);
	if (!app_id.empty()) joiner.set_app_id(app_id);
	joiner.set_glsvss_request(glsvss_request);
	joiner.set_cd_cookie(cd_cookie);
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	const std::vector<uint8_t> hello_dg = joiner.start();
	if (!nw_decode_inbound(hello_dg.data(), hello_dg.size(), opcode, body) ||
			!parse_client_hello(body.data(), body.size(), hello))
		return out;
	ServerHello sh = build_server_hello(hello, 0x7F000001u, 32769);
	sh.hk = 0x55667788u;
	const std::vector<uint8_t> sh_dg = nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(sh));
	(void)joiner_receive::receive(joiner, sh_dg.data(), sh_dg.size());
	// The 0x42 leaves at the first pump past the connect state's start (D-NET-NEW-L2-25).
	const std::vector<uint8_t> auth_dg = inmatch::test::enumerated_join_auth(joiner);
	ClientAuth auth;
	if (!nw_decode_inbound(auth_dg.data(), auth_dg.size(), opcode, body) ||
			!parse_client_auth(body.data(), body.size(), auth))
		return out;
	for (const std::vector<uint8_t> &chunk : auth.cu) {
		uint8_t type = 0;
		std::string name, value;
		if (parse_client_cu_chunk(chunk.data(), chunk.size(), type, name, value)) out.cu.emplace_back(name, value);
	}
	ServerAuth sa = build_server_auth(auth, 0x7F000001u, 32769, kServerKey, kServerScrk, "", "", "", false);
	sa.mi = 3;
	const std::vector<uint8_t> sa_dg = nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(sa));
	(void)joiner_receive::receive(joiner, sa_dg.data(), sa_dg.size());
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings = frame_s2c(seq,
			{make_protocol_message(0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
			 make_protocol_message(0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0)},
			kServerScrk, auth.ck);
	const joiner_receive::Received result = joiner_receive::receive(joiner, settings.data(), settings.size());
	for (const std::vector<uint8_t> &dg : result.outbound) {
		std::vector<ProtocolMessage> messages;
		if (!decode_c2s(dg, auth.scrk, messages)) continue;
		for (const ProtocolMessage &m : messages) {
			if (m.tag != 0x00) continue;
			std::size_t pos = 0;
			while (pos < m.payload.size()) {
				FlatTlvField field;
				const size_t next = read_flat_tlv(m.payload.data(), m.payload.size(), pos, field);
				if (next == kFlatTlvEnd || field.name.empty()) break;
				pos = next;
				out.join_fields.emplace_back(field.name);
				if (field.name == "CDKIID")
					out.cdkiid = load_value_string(field.value, m.payload.data() + m.payload.size(), SIZE_MAX);
				if (field.name == "GLSVSSREQUEST") {
					const uint8_t *end = m.payload.data() + m.payload.size();
					out.glsvss_request = load_value_string(field.value, end, SIZE_MAX);
				}
			}
			out.ok = true;
		}
	}
	return out;
}

void join_carries_the_cd_key() {
	const std::string key = cdkey::make_wire_key("OPENNOVA-0001");
	const JoinSent lan = join_of(dfx2(), cdkey::WireKeys{key, ""}, "");
	expect(lan.ok && lan.join_fields == std::vector<std::string>{"CDKIID", "VERSIONCRCSTRING"} &&
	               lan.cdkiid == key,
	       "DFX2-13: a LAN JOIN leads with the CDKIID (no CDKIIDEXP1 when it holds none)");
	const JoinSent both = join_of(dfx2(), cdkey::WireKeys{key, "EXPKEY"}, "");
	expect(both.ok && both.join_fields ==
	               std::vector<std::string>{"CDKIID", "CDKIIDEXP1", "VERSIONCRCSTRING"},
	       "a CDKIIDEXP1 the joiner holds rides after the CDKIID");
	const JoinSent bare = join_of(dfx2(), cdkey::WireKeys{}, "");
	expect(bare.ok && bare.join_fields == std::vector<std::string>{"VERSIONCRCSTRING"},
	       "a joiner holding no key sends neither (the stock LAN JOIN)");
	const JoinSent nw = join_of(dfx2(), cdkey::WireKeys{key, ""}, "3225");
	expect(nw.ok && nw.cdkiid.empty(), "a NovaWorld JOIN carries no CDKIID");
	const JoinSent jo_lan = join_of(jo(), cdkey::WireKeys{key, ""}, "");
	expect(jo_lan.ok && jo_lan.cdkiid == key, "JO's joiner writes the same TLVs (the same code)");
	for (const JoinSent *sent : {&lan, &jo_lan}) {
		const auto value = [sent](const char *name) {
			for (const auto &kv : sent->cu)
				if (kv.first == name) return kv.second;
			return std::string();
		};
		expect(value("BN") == "1" && value("VN") == "2" && value("MBN") == "20042002" &&
		               value("SOPD") == "180",
		       "the ClientAuth's build CU values are the title's (the same four)");
	}
}

// D-NET-723: retail's JOIN writer appends the gate's GLSVSSREQUEST, a NUL-terminated string,
// after the CD cookie when the transport is NovaWorld and the gate gave one; a LAN join and a
// gate without one send none. Both titles run the same code.
// [orig: NapiNP_WriteClientAuthPayload @0x42a180 - the NovaWorld transport test @0x42a2dd, the
//  buffer's test @0x42a2e6, the TLV @0x42a315]
// [orig: @0x42c36d, @0x42c376, the TLV @0x42c3a5 (dfx2.exe)]
void join_carries_the_glsvss_request() {
	const JoinSent nw = join_of(dfx2(), cdkey::WireKeys{}, "3225", "[dfx2]kills");
	const std::vector<std::string> fields{"VERSIONCRCSTRING", "GLSVSSREQUEST"};
	expect(nw.ok && nw.join_fields == fields && nw.glsvss_request == "[dfx2]kills",
	       "D-NET-723: a NovaWorld JOIN carries the gate's GLSVSSREQUEST last");
	const JoinSent jo_nw = join_of(jo(), cdkey::WireKeys{}, "3225", "[dfx]kills");
	expect(jo_nw.ok && jo_nw.glsvss_request == "[dfx]kills", "JO's joiner writes it alike");
	const JoinSent none = join_of(dfx2(), cdkey::WireKeys{}, "3225");
	expect(none.ok && none.join_fields == std::vector<std::string>{"VERSIONCRCSTRING"},
	       "a NovaWorld JOIN without a gate GLSVSSREQUEST sends none");
	const JoinSent lan = join_of(dfx2(), cdkey::WireKeys{}, "", "[dfx2]kills");
	expect(lan.ok && lan.join_fields == std::vector<std::string>{"VERSIONCRCSTRING"},
	       "a LAN JOIN never carries it");
	// After the CD cookie (the `g_AnnouncePayloadSize > 0` leg @0x42a2d5 ahead of the test
	// @0x42a2dd).
	const JoinSent cookie = join_of(dfx2(), cdkey::WireKeys{}, "3225", "[dfx2]kills", {0x50, 0x55, 0x42});
	expect(cookie.ok && cookie.join_fields ==
	               std::vector<std::string>{"VERSIONCRCSTRING", "CD", "GLSVSSREQUEST"},
	       "GLSVSSREQUEST rides after the CD cookie");
}

} // namespace

int main() {
	loadout_class_byte();
	join_carries_the_cd_key();
	join_carries_the_glsvss_request();
	if (g_failures == 0) {
		std::printf("npruntime_dfx2_joiner: all checks passed\n");
		return 0;
	}
	std::printf("npruntime_dfx2_joiner: %d check(s) failed\n", g_failures);
	return 1;
}
