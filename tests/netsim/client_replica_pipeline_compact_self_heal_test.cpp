// The compact 0x0A record's pre-apply consistency check: retail resolves the
// record's pool slot and, ahead of the class callback, requires a live entity
// whose itemDef id / defIndex match the wire type — otherwise it queues one
// C2S 0x0F entity request and runs the callback against a NULL target, so the
// record is consumed and NOTHING lands. A handle past its pool's capacity has
// no slot: consumed silently, no request. A compact record therefore never
// creates a row (only the spawn stream does) and never re-types one.
// [orig: NapiNPClientMsg_0x00A @0x42FEC0 — the check @0x4307B1..0x4307C4,
//  QueueReliableMessage(0x0F) @0x4307E9, the target nulled @0x4307F2]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kOtherPlayerType = 0x0777;
constexpr uint16_t kHandle = 0x0009;

using test_expect::expect;

nw::EntityClass classify(uint16_t type_id) {
	return (type_id == kPlayerType || type_id == kOtherPlayerType)
			? nw::EntityClass::Player
			: nw::EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint16_t handle, uint16_t type_id) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = type_id;
	r.cls = nw::EntityClass::Player;
	r.player.carrier_handle = 0xFFFF;
	r.player.pos_x_compressed = nw::network_compress_fixedpoint(1 << 16);
	r.player.pos_y_compressed = nw::network_compress_fixedpoint(2 << 16);
	r.player.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.player.yaw_byte = 0x40;
	r.player.anim_state_id = 62;
	r.player.anim_def_index = 0xFF;
	r.player.health_class_byte = 0x28;
	fu.records.push_back(r);
	return nw::encode_frame_update(fu);
}

// A record for a slot the spawn stream never filled: no row is created and
// exactly one 0x0F rides out, drained once.
bool run_unknown_handle_queues_repair_and_creates_nothing() {
	ns::ClientReplicaPipeline view(classify);
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(kHandle, kPlayerType));
	if (!expect(view.state().find(kHandle) == nullptr,
	            "a compact record never creates a row")) return false;
	const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
	if (!expect(repairs.size() == 1 && repairs[0] == kHandle,
	            "the unknown slot queues one 0x0F for its own handle")) return false;
	return expect(view.drain_carrier_repair_requests().empty(), "the request drains once");
}

// A row of another type (the slot was re-used): nothing lands on it, its type
// is not rewritten, and the 0x0F self-heal is queued.
bool run_type_mismatch_queues_repair_and_applies_nothing() {
	ns::ClientReplicaPipeline view(classify);
	view.state().upsert(kHandle).type_id = kOtherPlayerType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(kHandle, kPlayerType));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->type_id == kOtherPlayerType,
	            "a compact record never re-types a row")) return false;
	if (!expect(row->compact_revision == 0 && row->x == 0 && row->y == 0,
	            "the mismatched record lands nothing")) return false;
	const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
	return expect(repairs.size() == 1 && repairs[0] == kHandle,
	              "the mismatched slot queues one 0x0F for its own handle");
}

// The team-assign leg's pre-spawn row (type 0) is retail's itemDef-less slot:
// the same self-heal, not an apply.
bool run_typeless_row_queues_repair() {
	ns::ClientReplicaPipeline view(classify);
	view.apply_team_assign(kHandle, 2);
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(kHandle, kPlayerType));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->type_id == 0 && row->compact_revision == 0,
	            "the typeless row is left alone")) return false;
	const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
	return expect(repairs.size() == 1 && repairs[0] == kHandle,
	              "the typeless slot queues one 0x0F");
}

// The matching row folds as before, with no request.
bool run_matching_row_applies() {
	ns::ClientReplicaPipeline view(classify);
	view.state().upsert(kHandle).type_id = kPlayerType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(kHandle, kPlayerType));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->compact_revision == 1 &&
	                    row->x == (100 << 16) + nw::network_decompress_fixedpoint(
	                                                   nw::network_compress_fixedpoint(1 << 16)),
	            "a matching row folds the record")) return false;
	return expect(view.drain_carrier_repair_requests().empty(),
	              "a matching row queues no request");
}

// A slot index past the pool's capacity resolves to no slot at all: consumed
// silently, no row, no request [orig: `slot < g_PoolList[pool].capacity`].
bool run_out_of_capacity_handle_is_consumed_silently() {
	ns::ClientReplicaPipeline view(classify);
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(0x0FFF, kPlayerType));
	if (!expect(view.state().find(0x0FFF) == nullptr, "no row for a capacity miss"))
		return false;
	return expect(view.drain_carrier_repair_requests().empty(),
	              "a capacity miss queues no request");
}

// The case-2 def re-pick (D-NET-348): every remote player record looks its
// class's camouflage item up as a row and rebinds the body to it when the row
// differs; a dead record keeps the class it had; the next record of the old
// type then fails the check above and queues the 0x0F. A minted two-class
// desert table: class 5 -> 5305 (0x14B9), class 6 -> 5311 (0x14BF); the
// client's own handle answers no re-pick (its record lands on its own body).
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the class store
//  @0x4C11B4 (Entity_SetHealthFromDifficultyByte @0x4AD5A2), the dead arms
//  @0x4C102E / @0x4C1104, the climate pick @0x4C1248..0x4C129C, the compare
//  @0x4C12A5, the rebind @0x4C12B8..0x4C12D6]
constexpr uint16_t kClassSixType = 0x14BF;
constexpr uint16_t kSelfHandle = 0x0003;

void install_cammo_table(ns::ClientReplicaPipeline &view) {
	view.set_replica_player_cammo_resolver(
			[](uint16_t handle, uint8_t player_class, uint16_t &type_id) {
				if (handle == kSelfHandle) return false;
				type_id = player_class == 6 ? kClassSixType
				        : player_class == 5 ? kPlayerType
				                            : 0; // a class with no row: the first row
				return true;
			});
}

nw::EntityClass classify_with_six(uint16_t type_id) {
	return type_id == kClassSixType ? nw::EntityClass::Player : classify(type_id);
}

std::vector<uint8_t> player_frame_byte(uint16_t handle, uint16_t type_id,
		uint8_t health_class_byte, uint8_t state_flags) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = type_id;
	r.cls = nw::EntityClass::Player;
	r.player.carrier_handle = 0xFFFF;
	r.player.pos_x_compressed = nw::network_compress_fixedpoint(1 << 16);
	r.player.pos_y_compressed = nw::network_compress_fixedpoint(2 << 16);
	r.player.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.player.yaw_byte = 0x40;
	r.player.anim_state_id = 62;
	r.player.anim_def_index = 0xFF;
	r.player.state_flags = state_flags;
	r.player.health_class_byte = health_class_byte;
	fu.records.push_back(r);
	return nw::encode_frame_update(fu);
}

bool run_live_record_repicks_the_row_by_its_class() {
	ns::ClientReplicaPipeline view(classify_with_six);
	install_cammo_table(view);
	view.state().upsert(kHandle).type_id = kPlayerType;
	// Tier 2, class 6 on a class-5 row: the record applies, then rebinds.
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x26, 0));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->compact_revision == 1 &&
	                    row->type_id == kClassSixType &&
	                    row->cls == nw::EntityClass::Player,
	            "a live record of class 6 rebinds the class-5 row to class 6's camouflage row"))
		return false;
	if (!expect(view.drain_carrier_repair_requests().empty(), "the re-pick itself queues nothing"))
		return false;
	// The host still streams the old type: the check fails and asks once.
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x26, 0));
	row = view.state().find(kHandle);
	if (!expect(row->compact_revision == 1 && row->type_id == kClassSixType,
	            "the old type's next record lands nothing")) return false;
	const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
	return expect(repairs.size() == 1 && repairs[0] == kHandle,
	              "the old type's next record queues one 0x0F");
}

bool run_matching_class_keeps_the_row() {
	ns::ClientReplicaPipeline view(classify_with_six);
	install_cammo_table(view);
	view.state().upsert(kHandle).type_id = kPlayerType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x25, 0));
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x15, 0));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->compact_revision == 2 && row->type_id == kPlayerType,
	            "a record whose class picks the row's own type keeps it")) return false;
	return expect(view.drain_carrier_repair_requests().empty(), "and asks nothing");
}

bool run_dead_record_keeps_the_class_it_had() {
	ns::ClientReplicaPipeline view(classify_with_six);
	install_cammo_table(view);
	view.state().upsert(kHandle).type_id = kPlayerType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x25, 0));
	// A dead record (state bit 1) naming class 6: the class apply is skipped,
	// so the re-pick reads class 5 and keeps the row.
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x06, 0x02));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	if (!expect(row != nullptr && row->compact_revision == 2 && row->type_id == kPlayerType,
	            "a dead record re-picks by the class the body had")) return false;
	// Its class readers keep the live record's class too (entity+0x294 holds
	// the last stored nibble) [orig: @0x4C102E / @0x4C1104 jump past the class
	// store @0x4C11B4].
	return expect((row->health_class_byte & 0x0Fu) == 5u,
	              "a dead record leaves the row's class byte at the live record's class");
}

bool run_classless_row_repicks_the_first_row_and_self_is_left() {
	ns::ClientReplicaPipeline view(classify_with_six);
	install_cammo_table(view);
	view.state().upsert(kHandle).type_id = kPlayerType;
	view.state().upsert(kSelfHandle).type_id = kPlayerType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kHandle, kPlayerType, 0x28, 0));
	view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame_byte(kSelfHandle, kPlayerType, 0x26, 0));
	const ns::ClientEntityState *row = view.state().find(kHandle);
	const ns::ClientEntityState *self = view.state().find(kSelfHandle);
	if (!expect(row != nullptr && row->type_id == 0,
	            "a class with no camouflage row rebinds to the first row")) return false;
	return expect(self != nullptr && self->type_id == kPlayerType && self->compact_revision == 1,
	              "the client's own record is not re-picked here");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_live_record_repicks_the_row_by_its_class();
	ok &= run_matching_class_keeps_the_row();
	ok &= run_dead_record_keeps_the_class_it_had();
	ok &= run_classless_row_repicks_the_first_row_and_self_is_left();
	ok &= run_unknown_handle_queues_repair_and_creates_nothing();
	ok &= run_type_mismatch_queues_repair_and_applies_nothing();
	ok &= run_typeless_row_queues_repair();
	ok &= run_matching_row_applies();
	ok &= run_out_of_capacity_handle_is_consumed_silently();
	if (ok) std::printf("client_replica_pipeline_compact_self_heal: OK\n");
	return ok ? 0 : 1;
}
