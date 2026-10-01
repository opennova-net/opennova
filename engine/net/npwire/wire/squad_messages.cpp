#include <net/npwire/squad_messages.h>

#include <base/io/byte_reader.h>
#include <base/io/le.h>

namespace opennova {

namespace {

// The retail handlers' cursor is the format-parser contract: `p + n <= end ?
// read : 0`, the cursor advancing only on a read; a string runs to its NUL
// (strlen) with the cursor clamped to the body end
// [orig: e.g. NapiNPClientMsg_0x033 @0x425fb0..0x425fe8].
io::ByteReader reader(const uint8_t *body, size_t len) {
	return io::ByteReader(body, body != nullptr ? len : 0);
}

void put_cstr(std::vector<uint8_t> &out, const std::string &s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

} // namespace

std::vector<uint8_t> encode_waypoint_share(const WaypointShare &share) {
	// [orig: NetPacket_WriteTypeNameAndPosition @0x42b160 — [u8 type][cstr
	//  entity+244][i32 +4][i32 +8][i32 +12]]
	std::vector<uint8_t> out;
	out.push_back(share.target);
	put_cstr(out, share.name);
	io::append_u32_le(out, static_cast<uint32_t>(share.x));
	io::append_u32_le(out, static_cast<uint32_t>(share.y));
	io::append_u32_le(out, static_cast<uint32_t>(share.z));
	return out;
}

WaypointShare decode_waypoint_share(const uint8_t *body, size_t len) {
	// [orig: NapiNPServerMsg_HandleChatOrWhisper @0x51489c..0x51490f]
	io::ByteReader c = reader(body, len);
	WaypointShare out;
	out.target = c.read_u8();
	out.name = c.read_cstr();
	out.x = c.read_i32();
	out.y = c.read_i32();
	out.z = c.read_i32();
	return out;
}

std::vector<uint8_t> encode_waypoint_create(const WaypointCreate &create) {
	// [orig: WeaponOverlay_ActivateSlot @0x504aa0 — [cstr][i32][i32][i32][u8]]
	std::vector<uint8_t> out;
	put_cstr(out, create.name);
	io::append_u32_le(out, static_cast<uint32_t>(create.x));
	io::append_u32_le(out, static_cast<uint32_t>(create.y));
	io::append_u32_le(out, static_cast<uint32_t>(create.z));
	out.push_back(create.owner_index);
	return out;
}

WaypointCreate decode_waypoint_create(const uint8_t *body, size_t len) {
	// [orig: NapiNPClientMsg_0x033 @0x425fa0 — the name, x, y, the third dword
	//  skipped, the owner byte]
	io::ByteReader c = reader(body, len);
	WaypointCreate out;
	out.name = c.read_cstr();
	out.x = c.read_i32();
	out.y = c.read_i32();
	c.skip_if_available(4);
	out.owner_index = c.read_u8();
	return out;
}

std::vector<uint8_t> encode_entity_handle16(uint16_t handle) {
	// [orig: NetPacket_WriteEntityIndex16 @0x42b230]
	std::vector<uint8_t> out;
	io::append_u16_le(out, handle);
	return out;
}

uint16_t decode_entity_handle16(const uint8_t *body, size_t len) {
	// [orig: NapiNPServerMsg_0x04F @0x514a88; NapiNPClientMsg_0x07C @0x426020]
	io::ByteReader c = reader(body, len);
	return c.read_u16();
}

std::vector<uint8_t> encode_squad_join_request(uint8_t leader) {
	return {leader}; // [orig: NetPacket_SendWeaponSlotSwitch @0x42dc10]
}

uint8_t decode_squad_join_request(const uint8_t *body, size_t len) {
	io::ByteReader c = reader(body, len); // [orig: @0x5109c4..0x5109d4]
	return c.read_u8();
}

std::vector<uint8_t> encode_squad_join(const SquadJoin &join) {
	// [orig: NetPacket_WritePlayerChainLink @0x5106d0 — the link byte
	//  @0x510797, the member slot @0x5107A4]
	return {join.leader, join.member};
}

SquadJoin decode_squad_join(const uint8_t *body, size_t len) {
	// [orig: NapiNPClientMsg_HandleSquadJoin @0x425600..0x42563a]
	io::ByteReader c = reader(body, len);
	SquadJoin out;
	out.leader = c.read_u8();
	out.member = c.read_u8();
	return out;
}

std::vector<uint8_t> encode_squad_order_request(const SquadOrderRequest &order) {
	// [orig: NetPacket_WriteTypeCountStringAndArray @0x42b620 — [u8 type][u8
	//  count][cstr][count x u8]]
	std::vector<uint8_t> out;
	out.push_back(order.kind);
	out.push_back(static_cast<uint8_t>(order.targets.size()));
	put_cstr(out, order.text);
	out.insert(out.end(), order.targets.begin(), order.targets.end());
	return out;
}

SquadOrderRequest decode_squad_order_request(const uint8_t *body, size_t len) {
	// [orig: NapiNPServerMsg_HandleChatBroadcast @0x510b1b..0x510b62]
	io::ByteReader c = reader(body, len);
	SquadOrderRequest out;
	out.kind = c.read_u8();
	const uint8_t count = c.read_u8();
	out.text = c.read_cstr();
	for (uint8_t i = 0; i < count; ++i) out.targets.push_back(c.read_u8());
	return out;
}

std::vector<uint8_t> encode_squad_order(const SquadOrder &order) {
	// [orig: NetPacket_WriteByteAndCString @0x5107b0 — the byte @0x5107cd, then
	//  the string with its terminator]
	std::vector<uint8_t> out;
	out.push_back(order.kind);
	put_cstr(out, order.text);
	return out;
}

SquadOrder decode_squad_order(const uint8_t *body, size_t len) {
	// [orig: NapiNPClientMsg_0x072 @0x425710]
	io::ByteReader c = reader(body, len);
	SquadOrder out;
	out.kind = c.read_u8();
	out.text = c.read_cstr();
	return out;
}

std::vector<uint8_t> encode_fireteam_assign(const FireteamAssign &assign) {
	// [orig: NetPacket_WriteTypeCountAndArray @0x42b6c0]
	std::vector<uint8_t> out;
	out.push_back(assign.fireteam);
	out.push_back(static_cast<uint8_t>(assign.members.size()));
	out.insert(out.end(), assign.members.begin(), assign.members.end());
	return out;
}

FireteamAssign decode_fireteam_assign(const uint8_t *body, size_t len) {
	// [orig: NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c3c..0x510c6c]
	io::ByteReader c = reader(body, len);
	FireteamAssign out;
	out.fireteam = c.read_u8();
	const uint8_t count = c.read_u8();
	for (uint8_t i = 0; i < count; ++i) out.members.push_back(c.read_u8());
	return out;
}

std::vector<uint8_t> encode_fireteam_set(const FireteamSet &set) {
	return {set.member, set.fireteam}; // [orig: @0x510ca8..0x510cc8]
}

FireteamSet decode_fireteam_set(const uint8_t *body, size_t len) {
	io::ByteReader c = reader(body, len); // [orig: NapiNPClientMsg_0x073 @0x425770]
	FireteamSet out;
	out.member = c.read_u8();
	out.fireteam = c.read_u8();
	return out;
}

std::vector<uint8_t> encode_squad_recruit(const SquadRecruit &recruit) {
	return {recruit.recruiter, recruit.target}; // [orig: NetPacket_SendTeamChange @0x42dd00]
}

SquadRecruit decode_squad_recruit(const uint8_t *body, size_t len) {
	io::ByteReader c = reader(body, len); // [orig: @0x510d20..0x510d74]
	SquadRecruit out;
	out.recruiter = c.read_u8();
	out.target = c.read_u8();
	return out;
}

std::vector<uint8_t> encode_squad_recruited(uint8_t recruiter) {
	return {recruiter}; // [orig: @0x510d9a]
}

uint8_t decode_squad_recruited(const uint8_t *body, size_t len) {
	io::ByteReader c = reader(body, len); // [orig: NapiNPClientMsg_PlayerRecruited @0x4258b0]
	return c.read_u8();
}

std::vector<uint8_t> encode_go_code(const GoCode &code) {
	return {code.leader, code.code}; // [orig: NetPacket_SendVoteKick @0x42dd50]
}

GoCode decode_go_code(const uint8_t *body, size_t len) {
	// [orig: @0x510dc0..0x510e1d; NapiNPClientMsg_0x078 @0x425970]
	io::ByteReader c = reader(body, len);
	GoCode out;
	out.leader = c.read_u8();
	out.code = c.read_u8();
	return out;
}

std::vector<uint8_t> encode_punt_vote(uint8_t target) {
	return {target}; // [orig: NetPacket_WriteByte @0x5488ae]
}

uint8_t decode_punt_vote(const uint8_t *body, size_t len) {
	io::ByteReader c = reader(body, len); // [orig: NapiNPServerMsg_VoteKick @0x518f38]
	return c.read_u8();
}

} // namespace opennova
