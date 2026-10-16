// The replication seam's Delta Force: Xtreme 2 branches (docs/titles/dfx2-vs-jo-re.md §8), each
// against JO's over the same fan and fold:
//
//   DFX2-1   a co-op host's phase-3 S2C 0x0A carries the co-op checkpoint after the four
//            objective values (coop_checkpoint_slot, the world's DFX2-9 slot, 0xFFFF after the
//            round reset), and a
//            DFX2 joiner's view keeps it, its revision advancing on a change.
//   DFX2-9   a DFX2 joiner's 0x0F resets its checkpoint and takes each route record's done byte,
//            the last done record's slot its checkpoint; each new checkpoint a 0x0A carries marks
//            its route done through that marker. JO's discards the byte.
//   DFX2-10  S2C 0x66: a DFX2 joiner keeps the 0 and 3 (armory-only) entries, JO's the 0 and 2.
//
// [orig: NetPacket_WritePlayerState @0x4ffc2d..0x4ffc99; NapiNPClientMsg_0x00A @0x430361;
//  NapiNPClientMsg_HandleWeaponRestrictions @0x42d51c]
// [orig: NetPacket_WritePlayerState
//  @0x4fcd84..0x4fcd9b, NapiNPClientMsg_0x00A @0x431dee..0x431e62, NapiNPClientMsg_HandleWeaponRestrictions
//  @0x42f2ac (dfx2.exe)]

#include <base/gameprofile/gameprofile.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/replica_track.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/connection.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/world.h>

#include "fixtures/dfx2_wire_values.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;
namespace ns = opennova::replication;

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

// One phase-3 frame of a co-op host (g_GameType 0x30020) speaking `title`, to a remote recipient,
// its world's co-op checkpoint at `checkpoint`.
std::vector<uint8_t> phase3_frame(const TitleIdentity *title, uint16_t checkpoint = 0xFFFF) {
	world::World w;
	w.script.coop_checkpoint_slot = checkpoint;
	w.registry.configure_pool(0, 1);
	world::Entity recipient;
	recipient.kind = world::EntityKind::Organic;
	recipient.health = 150;
	const world::EntityHandle h = w.registry.spawn(0, recipient);
	w.script.subgoals.show_win = 2;
	ns::LoopbackChannel channel;
	ns::Connection conn{&channel, ns::TransportMode::Loopback, h, 0};
	conn.s2c_phase = 2; // the next counter value is phase 3
	(void)ns::emit_connection_s2c(w, conn, ns::snapshot_world(w), 0x30020u, 0, 0, 0, title);
	ns::Datagram dg;
	expect(channel.client_recv(dg) && dg.tag == s2c::PER_FRAME_UPDATE, "the phase-3 0x0A goes out");
	return dg.body;
}

void host_fan_writes_the_checkpoint() {
	expect(ns::coop_checkpoint_slot(world::World{}) == kCoopCheckpointNone,
	       "a fresh world's checkpoint is the round reset's 0xFFFF");
	const std::vector<uint8_t> dfx2_body = phase3_frame(dfx2());
	FrameUpdate fu;
	expect(decode_frame_update(dfx2_body.data(), dfx2_body.size(), ns::class_for_type_id, fu,
	               FrameObjectiveBody::ValuesAndCheckpoint) &&
	               fu.complete && fu.consumed == dfx2_body.size(),
	       "the DFX2 host's frame walks under DFX2's layout");
	expect(fu.flags2 == 3 && fu.objective.present && fu.objective.state[2] == 2 &&
	               fu.objective.has_coop_checkpoint && fu.objective.coop_checkpoint == 0xFFFFu,
	       "its objective body ends with the checkpoint 0xFFFF");
	const std::vector<uint8_t> jo_body = phase3_frame(jo());
	FrameUpdate jo_fu;
	expect(decode_frame_update(jo_body.data(), jo_body.size(), ns::class_for_type_id, jo_fu,
	               FrameObjectiveBody::Values) &&
	               jo_fu.complete && !jo_fu.objective.has_coop_checkpoint,
	       "JO's host writes the four values alone");
	expect(jo_body.size() + 4 == dfx2_body.size(), "the checkpoint is DFX2's four bytes");
	expect(phase3_frame(nullptr) == jo_body, "a host with no title writes JO's");
	const std::vector<uint8_t> marked_body = phase3_frame(dfx2(), 7);
	FrameUpdate marked;
	expect(decode_frame_update(marked_body.data(), marked_body.size(), ns::class_for_type_id, marked,
	               FrameObjectiveBody::ValuesAndCheckpoint) &&
	               marked.complete && marked.objective.has_coop_checkpoint &&
	               marked.objective.coop_checkpoint == 7u,
	       "the world's marked checkpoint slot rides the DFX2 frame");
	expect(phase3_frame(jo(), 7) == jo_body, "JO's host never writes it");
}

void joiner_view_keeps_the_checkpoint() {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_title(dfx2());
	view->set_game_type(0x30020u);
	view->apply(s2c::PER_FRAME_UPDATE, dfx2_wire::coop_objective_frame());
	expect(view->state().objective_show_win == 2 && view->state().coop_checkpoint == 0xFFFFu &&
	               view->state().coop_checkpoint_revision == 0,
	       "the capture's frame folds: its objective values, the checkpoint unchanged at 0xFFFF");
	FrameUpdate marked = dfx2_wire::coop_objective_update();
	marked.objective.coop_checkpoint = 7;
	view->apply(s2c::PER_FRAME_UPDATE, encode_frame_update(marked));
	expect(view->state().coop_checkpoint == 7 && view->state().coop_checkpoint_revision == 1,
	       "a new checkpoint is kept and its revision advances");
	view->apply(s2c::PER_FRAME_UPDATE, encode_frame_update(marked));
	expect(view->state().coop_checkpoint_revision == 1, "the same checkpoint is no change");
	auto jo_view = std::make_unique<ns::ClientReplicaPipeline>();
	jo_view->set_title(jo());
	jo_view->set_game_type(0x30020u);
	FrameUpdate jo_frame = dfx2_wire::coop_objective_update();
	jo_frame.objective.has_coop_checkpoint = false;
	jo_view->apply(s2c::PER_FRAME_UPDATE, encode_frame_update(jo_frame));
	expect(jo_view->state().objective_show_win == 2 && jo_view->state().coop_checkpoint == 0xFFFFu &&
	               jo_view->state().objective_updates_applied == 1,
	       "a JO view folds JO's four-value body");
}

void weapon_restrictions_keep_the_titles_armory_value() {
	// count 2: index 5 value 3, index 6 value 2
	const std::vector<uint8_t> body = {2, 5, 3, 6, 2};
	auto dfx2_view = std::make_unique<ns::ClientReplicaPipeline>();
	dfx2_view->set_title(dfx2());
	dfx2_view->apply(s2c::WEAPON_RESTRICTIONS, body);
	expect(dfx2_view->state().weapon_availability[5] == 3 &&
	               dfx2_view->state().weapon_availability[6] == 1,
	       "a DFX2 joiner keeps 3 (armory only) and drops 2");
	auto jo_view = std::make_unique<ns::ClientReplicaPipeline>();
	jo_view->set_title(jo());
	jo_view->apply(s2c::WEAPON_RESTRICTIONS, body);
	expect(jo_view->state().weapon_availability[5] == 1 &&
	               jo_view->state().weapon_availability[6] == 2,
	       "a JO joiner keeps 2 and drops 3");
}

// A co-op 0x0F body with three route records (slots 4, 5, 6; the first two done) and no
// location names. [orig: NapiNPClientMsg_0x00F @0x42e200]
std::vector<uint8_t> coop_world_state_load() {
	std::vector<uint8_t> b(4 + 12 + 6 + 1 + 128 * 4, 0);
	const uint8_t records[][5] = {{4, 0, 1, 0, 1}, {5, 0, 2, 0, 1}, {6, 0, 3, 0, 0}};
	b.push_back(3);
	b.push_back(0);
	for (const auto &r : records) b.insert(b.end(), r, r + 5);
	b.push_back(0);
	b.push_back(0);
	return b;
}

void world_state_folds_the_done_records() {
	const std::vector<uint8_t> body = coop_world_state_load();
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_title(dfx2());
	view->set_game_type(0x30020u);
	view->apply(s2c::WORLD_STATE_LOAD, body);
	const ns::ClientState &state = view->state();
	expect(state.world_state.waypoints.size() == 3 && state.world_state.waypoint_done_stored &&
	               state.world_state.waypoints[1].done == 1 && state.world_state.waypoints[2].done == 0,
	       "a DFX2 joiner's 0x0F keeps each record's done byte");
	expect(state.coop_checkpoint == 5u && state.coop_checkpoint_revision == 0,
	       "its checkpoint is the last done record's slot, no walk");
	auto jo_view = std::make_unique<ns::ClientReplicaPipeline>();
	jo_view->set_title(jo());
	jo_view->set_game_type(0x30020u);
	jo_view->apply(s2c::WORLD_STATE_LOAD, body);
	expect(jo_view->state().world_state.waypoints.size() == 3 &&
	               !jo_view->state().world_state.waypoint_done_stored &&
	               jo_view->state().coop_checkpoint == 0xFFFFu,
	       "a JO joiner's 0x0F keeps no checkpoint");

	// The joiner's track legs: the route rebuilt with the done bytes, then a new checkpoint
	// (slot 6 on the 0x0A) marks the route through it.
	world::World w;
	inmatch::ReplicaTrackSeen seen;
	inmatch::apply_replica_track(w, state, 0x30020u, true, seen);
	const std::vector<world::WaypointEntry> &route = w.script.waypoints.entries;
	expect(route.size() == 3 && route[0].done && route[1].done && !route[2].done,
	       "the DFX2 route takes the 0x0F's done bytes");
	FrameUpdate frame = dfx2_wire::coop_objective_update();
	frame.objective.coop_checkpoint = 5;
	view->apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
	expect(view->state().coop_checkpoint_revision == 0, "the 0x0F's checkpoint again is no change");
	frame.objective.coop_checkpoint = 6;
	view->apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
	expect(view->state().coop_checkpoint_revision == 1, "a new checkpoint");
	inmatch::apply_replica_track(w, view->state(), 0x30020u, true, seen);
	expect(route[2].done, "the new checkpoint's marker is marked done");

	world::World jo_world;
	inmatch::ReplicaTrackSeen jo_seen;
	inmatch::apply_replica_track(jo_world, jo_view->state(), 0x30020u, true, jo_seen);
	expect(jo_world.script.waypoints.entries.size() == 3 &&
	               !jo_world.script.waypoints.entries[0].done,
	       "the JO route keeps its own done flags");
}

} // namespace

int main() {
	host_fan_writes_the_checkpoint();
	joiner_view_keeps_the_checkpoint();
	weapon_restrictions_keep_the_titles_armory_value();
	world_state_folds_the_done_records();
	if (g_failures == 0) {
		std::printf("netsim_dfx2_replica: all checks passed\n");
		return 0;
	}
	std::printf("netsim_dfx2_replica: %d check(s) failed\n", g_failures);
	return 1;
}
