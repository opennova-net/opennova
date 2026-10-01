// The death screen's spectate state over the replica rows: the target walk,
// the sub-mode cycle, the three spectator actions, the SPECTATORTARGET track,
// the destroyed-target re-pick and the S2C 0x75 fold.
// [orig: Spectator_CycleTarget_0 @0x52ac20; sub_52AFF0 @0x52aff0;
//  Input_HandleActionBinding cases 500..502 @0x49bd58..0x49bd9e;
//  Entity_TrySetMinimapTrackTarget @0x52abc0; Entity_Destroy @0x43e820;
//  NapiNPClientMsg_SetSpectatorMode @0x4259e0]

#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <string>
#include <vector>

namespace ns = opennova::replication;

namespace {

int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, m); ++failures; } } while (0)

constexpr uint16_t kLocal = 0x0002;

void add_player(ns::ClientReplicaPipeline &view, uint16_t handle, uint8_t flags = 0,
		bool player = true) {
	ns::ClientEntityState &row = view.state().upsert(handle);
	row.type_id = 0x14B9;
	row.cls = player ? opennova::EntityClass::Player : opennova::EntityClass::Infantry;
	row.state_flags = flags;
	row.state_flags_known = true;
}

ns::ClientReplicaPipeline make_view() {
	ns::ClientReplicaPipeline view;
	add_player(view, kLocal);
	add_player(view, 0x0000);
	add_player(view, 0x0001, 0x02); // dead
	add_player(view, 0x0004, 0x01); // hidden
	add_player(view, 0x0005);
	add_player(view, 0x0006, 0, false); // an NPC organic
	view.set_spectate_local_handle(kLocal);
	view.state().death_screen_active = true;
	return view;
}

std::vector<uint8_t> text_command(const std::string &text) {
	std::vector<uint8_t> body(text.begin(), text.end());
	body.push_back(0);
	return body;
}

} // namespace

int main() {
	// The walk starts from the local player, skips the dead, hidden and
	// non-player rows, and wraps over the pool's used count.
	{
		ns::ClientReplicaPipeline view = make_view();
		view.spectate_cycle_target(1);
		CHECK(view.state().spectate_target == 0x0005, "local +1 skips the hidden slot 4");
		view.spectate_cycle_target(1);
		CHECK(view.state().spectate_target == 0x0000, "slot 6 is no player; the walk wraps to 0");
		view.spectate_cycle_target(-1);
		CHECK(view.state().spectate_target == 0x0005, "the reverse walk wraps back to 5");
		view.spectate_cycle_target(0);
		CHECK(view.state().spectate_target == 0xFFFF && view.state().death_screen_submode == 0,
				"direction 0 clears the target and the sub-mode");
	}
	// No candidate but the local player: target and sub-mode reset.
	{
		ns::ClientReplicaPipeline view;
		add_player(view, kLocal);
		add_player(view, 0x0003, 0x02);
		view.set_spectate_local_handle(kLocal);
		view.state().death_screen_submode = 1;
		view.spectate_cycle_target(1);
		CHECK(view.state().spectate_target == 0xFFFF && view.state().death_screen_submode == 0,
				"an empty walk drops back to the free sub-mode");
	}
	// The sub-mode cycle: free -> chase (picks a target) -> first person ->
	// free, and the actions' target steps only in chase / first person.
	{
		ns::ClientReplicaPipeline view = make_view();
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionNextTarget);
		CHECK(view.state().spectate_target == 0xFFFF, "the free sub-mode ignores the target rows");
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionCycleMode);
		CHECK(view.state().death_screen_submode == 1 && view.state().spectate_target == 0x0005,
				"entering chase with no target picks the next one");
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionNextTarget);
		CHECK(view.state().spectate_target == 0x0000, "Spectator Target + steps forward");
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionPrevTarget);
		CHECK(view.state().spectate_target == 0x0005, "Spectator Target - steps back");
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionCycleMode);
		CHECK(view.state().death_screen_submode == 2 && view.state().spectate_target == 0x0005,
				"first person keeps the target");
		view.spectate_action(ns::ClientReplicaPipeline::kSpectateActionCycleMode);
		CHECK(view.state().death_screen_submode == 0 && view.state().spectate_target == 0x0005,
				"the wrap to free keeps the (stale) target");
		view.spectate_cycle_mode(-1);
		CHECK(view.state().death_screen_submode == 2, "the reverse cycle wraps 0 -> 2");
	}
	// SPECTATORTARGET: an admitted player steps free -> chase; the local
	// player, a dead player and out-of-range handles are refused; the SU flag
	// admits a dead one.
	{
		ns::ClientReplicaPipeline view = make_view();
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SPECTATORTARGET\" \"5\""));
		CHECK(view.state().spectate_target == 0x0005 && view.state().death_screen_submode == 1,
				"the track sets the target and steps to chase");
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SPECTATORTARGET\" \"2\""));
		CHECK(view.state().spectate_target == 0x0005, "the local player is refused");
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SPECTATORTARGET\" \"1\""));
		CHECK(view.state().spectate_target == 0x0005, "a dead player is refused");
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SPECTATORTARGET\" \"20480\""));
		CHECK(view.state().spectate_target == 0x0005, "pool 5 is out of range");
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SU\" \"1\""));
		view.state().death_screen_submode = 2;
		view.apply(opennova::s2c::TEXT_COMMAND, text_command("\"SPECTATORTARGET\" \"1\""));
		CHECK(view.state().spectate_target == 0x0001 && view.state().death_screen_submode == 2,
				"the SU flag admits any typed entity and keeps a live sub-mode");
	}
	// The destroyed target re-picks from the local player before its row goes.
	{
		ns::ClientReplicaPipeline view = make_view();
		view.state().death_screen_submode = 1;
		view.spectate_cycle_target(1);
		CHECK(view.state().spectate_target == 0x0005, "the probe target");
		opennova::EntityRemove removal;
		removal.entity_handle = 0x0005;
		view.apply(opennova::s2c::ENTITY_REMOVE, opennova::encode_entity_remove(removal));
		CHECK(view.state().spectate_target == 0x0005,
				"the walk from the local player re-picks the dying row first, as retail's does");
		CHECK(view.state().find(0x0005) == nullptr, "the row is gone after the re-pick");
		// A dead target has no candidate left (slot 0 dead too): free again.
		view.state().find(0x0000)->state_flags = 0x02;
		removal.entity_handle = 0x0000;
		view.state().spectate_target = 0x0000;
		view.apply(opennova::s2c::ENTITY_REMOVE, opennova::encode_entity_remove(removal));
		CHECK(view.state().spectate_target == 0xFFFF && view.state().death_screen_submode == 0,
				"with no candidate left the re-pick resets to free");
	}
	// S2C 0x75: bit 0 is the death screen, the sub-mode and target clear, a
	// set bit grants the enemy tags.
	{
		ns::ClientReplicaPipeline view = make_view();
		view.state().death_screen_active = false;
		view.state().death_screen_submode = 2;
		view.state().spectate_target = 0x0005;
		view.apply(opennova::s2c::SPECTATOR_FLAGS, {0x01, 0x02});
		CHECK(view.state().death_screen_active && view.state().death_screen_submode == 0 &&
						view.state().spectate_target == 0xFFFF && view.state().enemy_tags_visible,
				"0x75 bit 0 opens the death screen and clears the spectate state");
		view.apply(opennova::s2c::SPECTATOR_FLAGS, {});
		CHECK(!view.state().death_screen_active && view.state().enemy_tags_visible,
				"a short 0x75 zero-fills and closes it, leaving the tag grant");
	}
	if (failures != 0) return 1;
	std::printf("client_replica_spectate_test OK\n");
	return 0;
}
