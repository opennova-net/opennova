// The body-anim transition PULSE (the tapped-prone-roll fix, live-witnessed
// 2026-07-27 in retail_validate_v1: a tapped roll rides the wire as state 41/42
// for a SINGLE 0x0A sample — the emitted byte is `pending ?: current` and flips
// the moment the follow-up state queues on the authority — while a HELD roll
// streams 41 for its full second).
//
// Retail applies each record's anim byte through the receive arbitration as it
// decodes [orig: @0x4c1153], so a one-sample pulse still starts the locked roll
// clip and the follow-up state queues behind it. Our snapshot seam coalesces
// several folded 0x0A datagrams into one presented byte, which silently dropped
// the pulse. NetClientView therefore latches the state a record OVERWRITES as
// `anim_state_pulse` (LAST transition wins: in a fold of [41, 48] the buried 41
// is the pulse; the 48 latched by the first transition was already presented
// and re-dispatching a presented state is a same-state no-op at the model), and
// presentation dispatches the pulse before the current state.

#include <netsim/net_client_view.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kPlayerHandle = 0x0067;

EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? EntityClass::Player : EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint8_t anim_state, uint8_t ratio) {
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;

	FrameUpdateRecord rec;
	rec.handle = kPlayerHandle;
	rec.type_id = kPlayerType;
	rec.cls = EntityClass::Player;
	rec.player.carrier_handle = 0xFFFF;
	rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
	rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
	rec.player.pos_z_compressed = network_compress_fixedpoint(0);
	rec.player.yaw_byte = 0x40;
	rec.player.anim_state_id = anim_state;
	rec.player.anim_channel_ratio = ratio;
	fu.records.push_back(rec);
	return encode_frame_update(fu);
}

} // namespace

int main() {
	ns::NetClientView view;
	view.set_item_class_resolver(&classify);

	bool ok = true;

	// First-ever sample: no pulse (the row's default 0 would read as the
	// anim_reset clip).
	view.apply(0x0A, player_frame(48, 0));
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	ok &= expect(es != nullptr, "player row decoded");
	if (es == nullptr) return 1;
	ok &= expect(es->anim_state_id == 48, "first sample applies");
	ok &= expect(es->anim_state_pulse == -1, "first sample never pulses");

	// A fold of [41, 48] between drains: the tapped roll. The buried 41 (and
	// ITS phase ratio) must survive as the pulse; the current byte is 48.
	view.apply(0x0A, player_frame(41, 6));
	view.apply(0x0A, player_frame(48, 60));
	ok &= expect(es->anim_state_id == 48, "fold coalesces to the latest state");
	ok &= expect(es->anim_state_pulse == 41,
	             "the buried transition survives as the pulse (last wins)");
	ok &= expect(es->anim_pulse_ratio == 6,
	             "the pulse carries the buried state's own phase ratio");

	// The presenter's drain is consume-once.
	view.state().clear_anim_pulses();
	ok &= expect(es->anim_state_pulse == -1, "drain clears the pulse");
	ok &= expect(es->anim_state_id == 48, "the current state survives the drain");

	// Steady state: an unchanged byte never re-arms the pulse.
	view.apply(0x0A, player_frame(48, 70));
	ok &= expect(es->anim_state_pulse == -1, "same-state records do not pulse");

	return ok ? 0 : 1;
}
