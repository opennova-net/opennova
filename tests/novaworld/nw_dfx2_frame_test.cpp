// The Delta Force: Xtreme 2 in-match bodies the codec reads by title (docs/titles/dfx2-vs-jo-re.md
// §8), against the values a retail DFX2 LAN pair put on the wire (fixtures/dfx2_wire, minted by
// tests/fixtures/dfx2_wire_gen.cpp from the decoded capture):
//
//   DFX2-1   a co-op host's S2C 0x0A carries a fifth objective u32, the co-op checkpoint
//            (0x0000FFFF outside a done marker), that DFX2's layout reads with the tail and the
//            records after it, and JO's misreads as the tail. JO's own four-value body is
//            unchanged.
//   DFX2-13  a LAN joiner's C2S 0x00 JOIN leads with its CDKIID, which decodes under the DFX2
//            wire chain.
//
// [orig: NetPacket_WritePlayerState @0x4ffc2d..0x4ffc99; NapiNPClientMsg_0x00A @0x430361..0x4303d0]
// [orig: NetPacket_WritePlayerState @0x4fcd84..0x4fcd9b (dfx2.exe); NapiNPClientMsg_0x00A
//  @0x431dee..0x431e01 (dfx2.exe); NapiNP_WriteClientAuthPayload @0x42c25b..0x42c2c5 (dfx2.exe)]

#include <base/gameprofile/gameprofile.h>
#include <net/novacrypto/cdkey.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_body_codec.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/title/title_identity.h>

#include "common/file_io.h"
#include "common/test_paths.h"
#include "fixtures/dfx2_wire_values.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::vector<uint8_t> fixture(const char *name) {
	std::vector<uint8_t> bytes;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/dfx2_wire/" + name;
	expect(test_io::read_file(path, bytes) && !bytes.empty(), "the DFX2 fixture reads");
	return bytes;
}

EntityClass player_only(uint16_t type_id) {
	return type_id == dfx2_wire::kPlayerTypeId ? EntityClass::Player : EntityClass::Unknown;
}

// The title rows pick the layouts: DFX2's objective body has the checkpoint, JO's does not.
void title_rows() {
	const TitleIdentity *jo = title_identity(gameprofile::GAME_JO);
	const TitleIdentity *dfx2 = title_identity(gameprofile::GAME_DFX2);
	expect(!title_in_match(jo).objective_checkpoint && title_in_match(dfx2).objective_checkpoint,
	       "DFX2-1 is DFX2's branch alone");
	expect(!title_in_match(nullptr).objective_checkpoint, "no title reads JO's");
	expect(frame_objective_body(false, true) == FrameObjectiveBody::None &&
	               frame_objective_body(true, false) == FrameObjectiveBody::Values &&
	               frame_objective_body(true, true) == FrameObjectiveBody::ValuesAndCheckpoint,
	       "the game type gates the body, the title the checkpoint");
	const IngameBodyCodecContext coop_dfx2 = IngameBodyCodecContext::for_session(0x30020u, dfx2);
	const IngameBodyCodecContext coop_jo = IngameBodyCodecContext::for_session(0x30020u, jo);
	const IngameBodyCodecContext tdm_dfx2 = IngameBodyCodecContext::for_session(0x10000u, dfx2);
	expect(coop_dfx2.objective_body == FrameObjectiveBody::ValuesAndCheckpoint &&
	               coop_jo.objective_body == FrameObjectiveBody::Values &&
	               tdm_dfx2.objective_body == FrameObjectiveBody::None,
	       "the codec context takes the session's game type and title");
}

// The minted co-op frame decodes, under DFX2's layout, to every value the capture held, the
// records after the checkpoint included; the body codec re-encodes it byte for byte.
void coop_frame_reads_as_dfx2() {
	const std::vector<uint8_t> body = fixture("s2c_0a_coop_objective.bin");
	FrameUpdate fu;
	const bool ok = decode_frame_update(body.data(), body.size(), player_only, fu,
			FrameObjectiveBody::ValuesAndCheckpoint);
	expect(ok && fu.complete && fu.consumed == body.size(), "the DFX2 frame walks to its terminator");
	expect(fu.anchor_x == dfx2_wire::kAnchor[0] && fu.anchor_y == dfx2_wire::kAnchor[1] &&
	               fu.anchor_z == dfx2_wire::kAnchor[2] && fu.flags1 == dfx2_wire::kFlags1 &&
	               fu.flags2 == dfx2_wire::kFlags2 && fu.sub_block == 3,
	       "the anchor and the flags are the capture's");
	expect(fu.objective.present && fu.objective.state[0] == 0 && fu.objective.state[1] == 0 &&
	               fu.objective.state[2] == 2 && fu.objective.state[3] == 0,
	       "the four objective values are the capture's");
	expect(fu.objective.has_coop_checkpoint && fu.objective.coop_checkpoint == 0x0000FFFFu,
	       "the fifth value is the co-op checkpoint, 0x0000FFFF");
	expect(fu.local_tail_present && fu.state_flag_byte == 0 && fu.carried_handle == 0xFFFF &&
	               fu.health == 100 && fu.state_word == 0,
	       "the tail after the checkpoint reads the recipient's own state");
	expect(fu.records.size() == 1 && fu.records[0].handle == dfx2_wire::kPlayerHandle &&
	               fu.records[0].type_id == dfx2_wire::kPlayerTypeId &&
	               fu.records[0].cls == EntityClass::Player &&
	               fu.records[0].player.anim_state_id == 43 &&
	               fu.records[0].player.anim_channel_ratio == 83 &&
	               fu.records[0].player.health_class_byte == 0x28,
	       "the record after the tail is the capture's player");
	expect(encode_frame_update(fu) == body, "the frame re-encodes byte for byte");
	IngameBodyCodecContext ctx = IngameBodyCodecContext::for_session(
			0x30020u, title_identity(gameprofile::GAME_DFX2));
	ctx.class_of = player_only;
	const IngameBodyCodec *codec = lookup_ingame_body_codec('S', 0x0A);
	expect(codec != nullptr, "the 0x0A body codec");
	if (codec != nullptr) {
		const IngameBodyCodecResult r = codec->run(body.data(), body.size(), ctx);
		expect(r.decode == BodyDecode::Exact && r.reencoded && r.bytes == body,
		       "the body codec proves the DFX2 frame against itself");
	}
}

// JO's layout reads the checkpoint's bytes as the tail: the misread the DFX2 layout removes.
void coop_frame_misreads_as_jo() {
	const std::vector<uint8_t> body = fixture("s2c_0a_coop_objective.bin");
	FrameUpdate fu;
	(void)decode_frame_update(body.data(), body.size(), player_only, fu, FrameObjectiveBody::Values);
	expect(fu.objective.present && !fu.objective.has_coop_checkpoint, "JO's layout takes four");
	expect(fu.state_flag_byte == 0xFF && fu.carried_handle == 0x00FF && fu.health == 0,
	       "JO's layout reads the checkpoint as the tail (state 0xFF, carried 0x00FF, health 0)");
	expect(!(fu.complete && fu.consumed == body.size() && fu.records.size() == 1 &&
	                 fu.records[0].handle == dfx2_wire::kPlayerHandle),
	       "and loses the record after it");
}

// JO's own body keeps its sixteen bytes: the checkpoint is not written without its flag, and a
// JO frame decodes as before.
void jo_frame_unchanged() {
	FrameUpdate fu = dfx2_wire::coop_objective_update();
	fu.objective.has_coop_checkpoint = false;
	const std::vector<uint8_t> jo = encode_frame_update(fu);
	expect(jo.size() + 4 == dfx2_wire::coop_objective_frame().size(), "JO's body is four bytes shorter");
	FrameUpdate back;
	expect(decode_frame_update(jo.data(), jo.size(), player_only, back, FrameObjectiveBody::Values) &&
	               back.complete && !back.objective.has_coop_checkpoint && back.health == 100 &&
	               back.records.size() == 1,
	       "a JO frame decodes as before");
}

// The LAN joiner's JOIN: CDKIID first, then VERSIONCRCSTRING "0", the CDKIID the wire key of the
// serial the capture's joiner sent (58 characters), decoding under the DFX2 wire chain.
void lan_join_carries_the_cdkiid() {
	const std::vector<uint8_t> body = fixture("c2s_00_join_lan.bin");
	expect(body == dfx2_wire::lan_join_body(), "the minted JOIN is the generator's");
	std::vector<std::string> names;
	std::string cdkiid;
	std::string crc;
	std::size_t pos = 0;
	while (pos < body.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		pos = next;
		names.emplace_back(field.name);
		const std::string value = load_value_string(field.value, body.data() + body.size(), SIZE_MAX);
		if (field.name == "CDKIID") cdkiid = value;
		if (field.name == "VERSIONCRCSTRING") crc = value;
	}
	expect(names == std::vector<std::string>{"CDKIID", "VERSIONCRCSTRING"} && crc == "0",
	       "the JOIN's fields in the capture's order");
	std::string serial;
	expect(cdkiid.size() == dfx2_wire::kJoinCdkiidChars && cdkey::decode_wire_key(cdkiid, serial) &&
	               serial == dfx2_wire::kJoinSerial,
	       "the CDKIID decodes under the DFX2 wire chain to the capture's serial");
}

} // namespace

int main() {
	title_rows();
	coop_frame_reads_as_dfx2();
	coop_frame_misreads_as_jo();
	jo_frame_unchanged();
	lan_join_carries_the_cdkiid();
	if (g_failures == 0) {
		std::printf("nw_dfx2_frame: all checks passed\n");
		return 0;
	}
	std::printf("nw_dfx2_frame: %d check(s) failed\n", g_failures);
	return 1;
}
