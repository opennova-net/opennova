// §5.15 guided weapon record — per-(mode, field-group) round-trip + size matrix.
//
// encode_guided_field_group ports the WRITE switches (modes 1/3) of
// Entity_SerializeGuidedMissileState; decode_guided_field_group ports the READ
// switches (modes 2/4). They cite different halves of the same function, so a
// clean round-trip pins the per-group wire layout the way nw_ingame_encode_test
// pins the §5.10/§5.13/§5.14 compacts.
//
// This is a STRUCTURAL self-consistency check only: wire integration into the
// 0x0C entity-packet path and field validation against real guided traffic are
// deferred (no capture in hand carries rocket/missile state — §5.15).

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

namespace {

#define EXPECT(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL %s:%d  EXPECT(%s)\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

GuidedRecord sample() {
	GuidedRecord r;
	r.target_slot = 0x1234;
	r.weapon_type = 0x0042;
	r.pos_x = 0x11111111;
	r.pos_y = 0x22222222;
	r.pos_z = int32_t(0xCCCCCCCC); // negative — exercises sign round-trip
	r.attach_x = 0x0A0A0A0A;
	r.attach_y = int32_t(0xB0B0B0B0);
	r.attach_z = 0x0C0C0C0C;
	return r;
}

// WriteFull (mode 1) -> ReadFull (mode 2), groups 3-6.
int test_full_roundtrip() {
	GuidedRecord r = sample();

	// group 3 TargetPos: u16 target + 3x i32 pos = 14 B.
	std::vector<uint8_t> w3 =
		encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::TargetPos, r);
	EXPECT(w3.size() == 14);
	GuidedRecord d3; size_t c3 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadFull, GuidedFieldGroup::TargetPos,
	                                 w3.data(), w3.size(), d3, c3));
	EXPECT(c3 == 14);
	EXPECT(d3.target_slot == r.target_slot);
	EXPECT(d3.pos_x == r.pos_x && d3.pos_y == r.pos_y && d3.pos_z == r.pos_z);
	EXPECT(d3.target_bound);

	// group 4 TargetTypePos: u16 target + i32 type + 3x i32 pos = 18 B.
	std::vector<uint8_t> w4 =
		encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::TargetTypePos, r);
	EXPECT(w4.size() == 18);
	GuidedRecord d4; size_t c4 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadFull, GuidedFieldGroup::TargetTypePos,
	                                 w4.data(), w4.size(), d4, c4));
	EXPECT(c4 == 18);
	EXPECT(d4.target_slot == r.target_slot);
	EXPECT(d4.weapon_type == r.weapon_type);
	EXPECT(d4.pos_x == r.pos_x && d4.pos_z == r.pos_z);

	// group 5 Pos: 3x i32 = 12 B.
	std::vector<uint8_t> w5 =
		encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::Pos, r);
	EXPECT(w5.size() == 12);
	GuidedRecord d5; size_t c5 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadFull, GuidedFieldGroup::Pos,
	                                 w5.data(), w5.size(), d5, c5));
	EXPECT(c5 == 12);
	EXPECT(d5.pos_x == r.pos_x && d5.pos_y == r.pos_y && d5.pos_z == r.pos_z);
	EXPECT(d5.target_cleared);

	// group 6 AttachOffsets: 3x i32 = 12 B.
	std::vector<uint8_t> w6 =
		encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::AttachOffsets, r);
	EXPECT(w6.size() == 12);
	GuidedRecord d6; size_t c6 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadFull, GuidedFieldGroup::AttachOffsets,
	                                 w6.data(), w6.size(), d6, c6));
	EXPECT(c6 == 12);
	EXPECT(d6.attach_x == r.attach_x && d6.attach_y == r.attach_y && d6.attach_z == r.attach_z);

	std::printf("PASS guided_full_roundtrip\n");
	return 0;
}

// WriteDelta (mode 3) -> ReadApply (mode 4): groups 3/4 shrink (target dropped).
int test_delta_roundtrip() {
	GuidedRecord r = sample();

	// group 3 delta: u16 target only = 2 B (pos NOT carried).
	std::vector<uint8_t> w3 =
		encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::TargetPos, r);
	EXPECT(w3.size() == 2);
	GuidedRecord d3; size_t c3 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadApply, GuidedFieldGroup::TargetPos,
	                                 w3.data(), w3.size(), d3, c3));
	EXPECT(c3 == 2);
	EXPECT(d3.target_slot == r.target_slot);
	EXPECT(d3.pos_x == 0); // pos not in delta group 3

	// group 4 delta: i32 type + 3x i32 pos, NO target = 16 B.
	std::vector<uint8_t> w4 =
		encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::TargetTypePos, r);
	EXPECT(w4.size() == 16);
	GuidedRecord d4; size_t c4 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadApply, GuidedFieldGroup::TargetTypePos,
	                                 w4.data(), w4.size(), d4, c4));
	EXPECT(c4 == 16);
	EXPECT(d4.weapon_type == r.weapon_type);
	EXPECT(d4.pos_x == r.pos_x && d4.pos_z == r.pos_z);
	EXPECT(d4.target_slot == 0xFFFF); // target not in delta group 4

	// group 5 is identical full/delta (12 B) — spot-check the delta pairing.
	std::vector<uint8_t> w5 =
		encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::Pos, r);
	EXPECT(w5.size() == 12);
	GuidedRecord d5; size_t c5 = 0;
	EXPECT(decode_guided_field_group(GuidedMode::ReadApply, GuidedFieldGroup::Pos,
	                                 w5.data(), w5.size(), d5, c5));
	EXPECT(c5 == 12);
	EXPECT(d5.pos_y == r.pos_y);

	std::printf("PASS guided_delta_roundtrip\n");
	return 0;
}

// Status / ClearTarget: write emits a 1-byte 0x00 marker; read consumes 0 B and
// sets the status flags (the marker byte is the dispatcher's framing, not the
// serializer's read payload — see §5.15).
int test_status_groups() {
	GuidedRecord r = sample();
	const uint8_t buf[4] = {0};

	std::vector<uint8_t> wfs = encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::Status, r);
	std::vector<uint8_t> wfc = encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::ClearTarget, r);
	std::vector<uint8_t> wds = encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::Status, r);
	std::vector<uint8_t> wdc = encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::ClearTarget, r);
	EXPECT(wfs.size() == 1 && wfs[0] == 0x00);
	EXPECT(wfc.size() == 1 && wfc[0] == 0x00);
	EXPECT(wds.size() == 1 && wds[0] == 0x00);
	EXPECT(wdc.size() == 1 && wdc[0] == 0x00);

	GuidedRecord ds; size_t cs = 99;
	EXPECT(decode_guided_field_group(GuidedMode::ReadFull, GuidedFieldGroup::Status, buf, sizeof(buf), ds, cs));
	EXPECT(cs == 0 && ds.launched);
	GuidedRecord dc; size_t cc = 99;
	EXPECT(decode_guided_field_group(GuidedMode::ReadApply, GuidedFieldGroup::ClearTarget, buf, sizeof(buf), dc, cc));
	EXPECT(cc == 0 && dc.target_cleared && dc.target_slot == 0xFFFF);

	std::printf("PASS guided_status_groups\n");
	return 0;
}

// decode handles READ modes only; a write mode must be rejected.
int test_read_rejects_write_mode() {
	const uint8_t body[18] = {0};
	GuidedRecord d; size_t consumed = 7;
	EXPECT(!decode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::TargetPos,
	                                  body, sizeof(body), d, consumed));
	EXPECT(consumed == 0);
	EXPECT(!decode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::Pos,
	                                  body, sizeof(body), d, consumed));
	std::printf("PASS guided_read_rejects_write_mode\n");
	return 0;
}

} // namespace

int main() {
	if (test_full_roundtrip()) return 1;
	if (test_delta_roundtrip()) return 1;
	if (test_status_groups()) return 1;
	if (test_read_rejects_write_mode()) return 1;
	std::printf("ALL nw_ingame_guided tests passed\n");
	return 0;
}
