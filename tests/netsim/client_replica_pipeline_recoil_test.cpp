// Pure-client recoil decay for decoded remote persons. Round receive stamps
// entity+0x380 before this body pass; the accumulator split and pitch drift are
// the same as the authoritative infantry kernel. Each compact yaw re-seeds a
// full local heading so the exact shared-stream yaw half-step survives until
// the next wire sample without becoming an invented wire field.
// [orig: RoundData_SpawnRound @0x4EC0D0; remote body update]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

constexpr uint16_t kPlayerType = 0x14B9;

nw::EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? nw::EntityClass::Player
	                             : nw::EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint16_t handle, uint8_t yaw_byte) {
	nw::FrameUpdate frame;
	frame.carried_handle = 0xFFFF;
	nw::FrameUpdateRecord record;
	record.handle = handle;
	record.type_id = kPlayerType;
	record.cls = nw::EntityClass::Player;
	record.player.carrier_handle = 0xFFFF;
	record.player.yaw_byte = yaw_byte;
	frame.records.push_back(record);
	return nw::encode_frame_update(frame);
}

bool expect(bool condition, const char *message) {
	if (!condition) std::printf("FAIL: %s\n", message);
	return condition;
}

} // namespace

int main() {
	ns::ClientReplicaPipeline view;
	ns::ClientEntityState &player = view.state().upsert(1);
	player.cls = nw::EntityClass::Player;
	player.recoil_pitch = 1 << 18;
	player.pitch_bam = 100;
	player.heading_bam = 100;
	ns::ClientEntityState &infantry = view.state().upsert(2);
	infantry.cls = nw::EntityClass::Infantry;
	infantry.recoil_pitch = 0x301;
	infantry.heading_bam = 200;
	ns::ClientEntityState &vehicle = view.state().upsert(0x1001);
	vehicle.cls = nw::EntityClass::Vehicle;
	vehicle.recoil_pitch = 1 << 18;

	view.tick_recoil();
	bool ok = true;
	ok = expect(view.state().find(1)->recoil_pitch == 245760,
	            "remote player loses half of the eighth-step") && ok;
	ok = expect(view.state().find(1)->pitch_bam == 4196,
	            "remote player pitch receives one eighth of that step") && ok;
	ok = expect(view.state().find(1)->heading_bam == 16484,
	            "mission-seeded first draw selects the positive yaw half-step") && ok;
	ok = expect(view.state().find(2)->recoil_pitch == 0,
	            "remote infantry snaps a signed remainder <= 0x300") && ok;
	ok = expect(view.state().find(2)->heading_bam == 248,
	            "zero-floor recoil still receives its PRNG-selected yaw half-step") && ok;
	ok = expect(view.state().find(0x1001)->recoil_pitch == (1 << 18),
	            "non-person decoded rows do not consume recoil") && ok;

	for (int tick = 0; tick < 512; ++tick) view.tick_recoil();
	ok = expect(view.state().find(1)->recoil_pitch == 0,
	            "remote recoil eventually reaches the retail snap floor") && ok;

	// PRNG_Next16 is one client-global stream and is consumed once for every
	// person body, even when its accumulator is zero. The sixth draw from the
	// exact Game_StartMission seed is even, selecting the positive yaw leg below.
	// These are retail-oracle literals, not a reimplementation in the test.
	ns::ClientReplicaPipeline order_view;
	for (uint16_t handle = 1; handle <= 6; ++handle) {
		ns::ClientEntityState &row = order_view.state().upsert(handle);
		row.cls = nw::EntityClass::Player;
		row.heading_bam = 1000;
		row.recoil_pitch = handle == 6 ? (1 << 18) : 0;
	}
	order_view.tick_recoil();
	ok = expect(order_view.state().find(1)->heading_bam == 1000,
	            "zero recoil consumes the shared draw without changing heading") && ok;
	ok = expect(order_view.state().find(6)->heading_bam == 17384,
	            "sixth shared draw selects the positive yaw half-step") && ok;

	// The local sub-byte heading must never become an invented wire field. A
	// compact update overwrites it from the real coarse yaw while retaining the
	// byte exactly for codec identity.
	ns::ClientReplicaPipeline wire_view;
	wire_view.set_item_class_resolver(&classify);
	// The spawn stream's row: a compact 0x0A never creates one
	// [orig: NapiNPClientMsg_0x00A @0x42FEC0, pre-apply check @0x4307B1..0x4307FA].
	wire_view.state().upsert(7).type_id = kPlayerType;
	wire_view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(7, 0x40));
	ns::ClientEntityState *wire_player = wire_view.state().find(7);
	ok = expect(wire_player != nullptr && wire_player->heading_bam == 0x40000000,
	            "compact yaw seeds the persistent full heading") && ok;
	if (wire_player != nullptr) {
		wire_player->recoil_pitch = 1 << 18;
		wire_view.tick_recoil(); // mission-seeded first draw is even: +0x4000
		ok = expect(wire_player->heading_bam == 0x40004000 &&
		                    wire_player->yaw_byte == 0x40,
		            "recoil changes full heading without mutating the wire byte") && ok;
		wire_view.apply(nw::s2c::PER_FRAME_UPDATE, player_frame(7, 0x40));
		ok = expect(wire_player->heading_bam == 0x40000000,
		            "the next compact yaw re-seeds full heading") && ok;
	}
	if (ok) std::printf("client_replica_pipeline_recoil: OK\n");
	return ok ? 0 : 1;
}
