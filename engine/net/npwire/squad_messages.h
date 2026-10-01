#pragma once

// The command map's squad and waypoint messages: the user waypoint share and
// delete, the squad join / recruit / fireteam / order / go-code legs, and the
// punt vote. Every reader here is the retail handler's own lenient cursor: a
// field the body is too short for reads 0 (a string reads up to its NUL or the
// body end) and the handler still acts on it, so a decoder never fails; an
// encoder writes exactly the retail writer's bytes.
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19) and
// docs/net/novaworld-net-re.md §4.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// C2S 0x17 — share one user waypoint: the target slot (0xFF = every
// subordinate of the sender) and the waypoint entity's name (+244) and
// position (+4/+8/+12).
// [orig: NetPacket_WriteTypeNameAndPosition @0x42b160 (writer, from
//  NetPacket_SendChatMessage @0x42ddc0 — the IDB name is a misnomer);
//  NapiNPServerMsg_HandleChatOrWhisper @0x514850 (reader)]
struct WaypointShare {
	uint8_t target = 0xFF;
	std::string name;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
};
std::vector<uint8_t> encode_waypoint_share(const WaypointShare &share);
WaypointShare decode_waypoint_share(const uint8_t *body, size_t len);

// S2C 0x33 — a shared waypoint for Waypoint_CreateForPlayer: the name, the
// position (the receiver skips the Z and re-samples the terrain) and the
// sender's pool-0 entity index.
// [orig: WeaponOverlay_ActivateSlot @0x504aa0 (the writer — a misnomer);
//  NapiNPClientMsg_0x033 @0x425fa0 (reader)]
struct WaypointCreate {
	std::string name;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint8_t owner_index = 0;
};
std::vector<uint8_t> encode_waypoint_create(const WaypointCreate &create);
WaypointCreate decode_waypoint_create(const uint8_t *body, size_t len);

// C2S 0x4F (a user waypoint deleted) and S2C 0x7C (the relay): one packed
// pool handle, 0xFFFF for an address outside every pool.
// [orig: NetPacket_WriteEntityIndex16 @0x42b230 (from NetPacket_SendEntityUpdate
//  @0x42de00); NapiNPServerMsg_0x04F @0x514a40; NapiNPClientMsg_0x07C @0x426020]
std::vector<uint8_t> encode_entity_handle16(uint16_t handle);
uint16_t decode_entity_handle16(const uint8_t *body, size_t len);

// C2S 0x43 — join a squad leader (the member's own slot id = leave).
// [orig: NetPacket_SendWeaponSlotSwitch @0x42dc10 (a misnomer: SendSquadJoin);
//  Server_HandleEntitySync @0x510990 (a misnomer: HandleSquadJoin)]
std::vector<uint8_t> encode_squad_join_request(uint8_t leader);
uint8_t decode_squad_join_request(const uint8_t *body, size_t len);

// S2C 0x71 — a member's squad leader (0xFF = none).
// [orig: NetPacket_WritePlayerChainLink @0x5106d0; NapiNPClientMsg_HandleSquadJoin
//  @0x425600]
struct SquadJoin {
	uint8_t leader = 0xFF;
	uint8_t member = 0;
};
std::vector<uint8_t> encode_squad_join(const SquadJoin &join);
SquadJoin decode_squad_join(const uint8_t *body, size_t len);

// C2S 0x44 — an order to one subordinate (kind 0) or a fireteam's members
// (kind 1); the empty text cancels.
// [orig: NetPacket_WriteTypeCountStringAndArray @0x42b620 (from
//  NetPacket_SendCommandType44 @0x42dc70); NapiNPServerMsg_HandleChatBroadcast
//  @0x510ae0 (the relay — a misnomer)]
struct SquadOrderRequest {
	uint8_t kind = 0;
	std::string text;
	std::vector<uint8_t> targets;
};
std::vector<uint8_t> encode_squad_order_request(const SquadOrderRequest &order);
// `count` is the wire count byte; `targets` holds that many reads (0 past the
// body end, like the relay's cursor).
SquadOrderRequest decode_squad_order_request(const uint8_t *body, size_t len);

// S2C 0x72 — one order line (kind 0 individual, 1 fireteam).
// [orig: NetPacket_WriteByteAndCString @0x5107b0; NapiNPClientMsg_0x072 @0x425710]
struct SquadOrder {
	uint8_t kind = 0;
	std::string text;
};
std::vector<uint8_t> encode_squad_order(const SquadOrder &order);
SquadOrder decode_squad_order(const uint8_t *body, size_t len);

// C2S 0x45 — assign the listed members to a fireteam (0 none, 1..3 A..C).
// [orig: NetPacket_WriteTypeCountAndArray @0x42b6c0 (from NetPacket_SendWeaponAction
//  @0x42dcc0); NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00]
struct FireteamAssign {
	uint8_t fireteam = 0;
	std::vector<uint8_t> members;
};
std::vector<uint8_t> encode_fireteam_assign(const FireteamAssign &assign);
FireteamAssign decode_fireteam_assign(const uint8_t *body, size_t len);

// S2C 0x73 — one member's fireteam.
// [orig: @0x510ca8..0x510cc8; NapiNPClientMsg_0x073 @0x425770]
struct FireteamSet {
	uint8_t member = 0;
	uint8_t fireteam = 0;
};
std::vector<uint8_t> encode_fireteam_set(const FireteamSet &set);
FireteamSet decode_fireteam_set(const uint8_t *body, size_t len);

// C2S 0x46 — recruit a player (the recruiter's own slot id, the target's).
// [orig: NetPacket_SendTeamChange @0x42dd00 (a misnomer: SendRecruit);
//  NapiNPServerMsg_HandleVoteKick @0x510d20 (a misnomer: the recruit relay)]
struct SquadRecruit {
	uint8_t recruiter = 0;
	uint8_t target = 0;
};
std::vector<uint8_t> encode_squad_recruit(const SquadRecruit &recruit);
SquadRecruit decode_squad_recruit(const uint8_t *body, size_t len);

// S2C 0x74 — the recruiter's slot id, to the recruited player.
// [orig: @0x510d9a..0x510db0; NapiNPClientMsg_PlayerRecruited @0x4258b0]
std::vector<uint8_t> encode_squad_recruited(uint8_t recruiter);
uint8_t decode_squad_recruited(const uint8_t *body, size_t len);

// C2S 0x4B and S2C 0x78 — a go code (0..5 UNIFORM..ZULU) from a leader.
// [orig: NetPacket_SendVoteKick @0x42dd50 (a misnomer: SendGoCode);
//  NapiNPServer_BroadcastPlayerProfileUpdate @0x510dc0 (a misnomer: the relay);
//  NetPacket_WriteTwoBytes; NapiNPClientMsg_0x078 @0x425970]
struct GoCode {
	uint8_t leader = 0;
	uint8_t code = 0;
};
std::vector<uint8_t> encode_go_code(const GoCode &code);
GoCode decode_go_code(const uint8_t *body, size_t len);

// C2S 0x3F — the punt vote's target slot.
// [orig: NetPacket_WriteByte from CMap_HandlePlayerListCallback @0x5488ae;
//  NapiNPServerMsg_VoteKick @0x518f10]
std::vector<uint8_t> encode_punt_vote(uint8_t target);
uint8_t decode_punt_vote(const uint8_t *body, size_t len);

} // namespace opennova
