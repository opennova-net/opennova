// The HUD panel feeds of the wire-up slice: the mounted-vehicle panel, the
// AAS zone status panel, the player-chat ring, and the session game type the
// shell's panel lanes gate on. Every witnessed rule lives in the engine
// (world/vehicle_panel_feed.h, world/lfp_feed.h, hud/feed_format.h); this TU is
// the Simulation seam that resolves the local player, the client runtime's
// zone-timer table and minimap banks, and the replica pipeline's chat drain
// for the HudOverlay setters (the set_scoreboard / fill_scoreboard_rows shape).

#include "simulation/simulation_internal.h"
#include "simulation/hud_view_records.h"

#include <runtime/hud/feed_format.h>
#include <runtime/hud/score_fanfare.h> // the 0x81 tone ladder
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/lfp_feed.h>
#include <runtime/world/vehicle_panel_feed.h>

using namespace sim_internal;

namespace godot {

Ref<VehiclePanelView> Simulation::get_vehicle_panel_view() const {
	// The panel describes the vehicle the local player rides — the attached
	// gun child re-roots to its parent vehicle, and every mount qualifies
	// (world::vehicle_panel_root carries the witness). The shell joins the
	// root's items.def sid to its VEHICLE_HUD block; the panel's only gate is
	// the block's interface texture [orig: HUD_DrawVehicleHealthBars
	// @0x5a4fd0 draws nothing without it, see docs/interface/hud-re.md].
	Ref<VehiclePanelView> out;
	out.instantiate();
	if (!kernel_->world.cached.local_player.valid()) return out;
	const opennova::world::Entity *local =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (local == nullptr) return out;
	const opennova::world::EntityHandle root_h =
			opennova::world::vehicle_panel_root(kernel_->world, *local);
	const opennova::world::Entity *root =
			root_h.valid() ? kernel_->world.registry.get(root_h) : nullptr;
	if (root == nullptr) return out;
	out->set_shown(true);
	out->set_item_id(root->item_id);
	return out;
}

bool Simulation::fill_vehicle_panel(const DefVehicleHudBlock &p_block,
		opennova::hud::HudVehiclePanelState &r_state) const {
	r_state.seats.clear();
	r_state.hull_health = 0;
	r_state.hull_max_health = 0;
	if (!kernel_->world.cached.local_player.valid()) return false;
	const opennova::world::Entity *local =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (local == nullptr) return false;
	const opennova::world::EntityHandle root_h =
			opennova::world::vehicle_panel_root(kernel_->world, *local);
	const opennova::world::Entity *root =
			root_h.valid() ? kernel_->world.registry.get(root_h) : nullptr;
	if (root == nullptr) return false;
	// The silhouette bands on the HULL's own health, not any rider's
	// [orig: the root's health band @0x5a50d1].
	r_state.hull_health = root->health;
	r_state.hull_max_health = root->health_max;
	opennova::world::fill_vehicle_panel_seats(kernel_->world, root_h,
			kernel_->world.cached.local_player, p_block, r_state.seats);
	return true;
}

bool Simulation::fill_lfp_zones(int p_local_team,
		std::vector<opennova::hud::HudLfpZone> &r_zones) {
	r_zones.clear();
	if (!kernel_ || !runtime_ || !kernel_->world.cached.local_player.valid()) return false;
	const opennova::world::Entity *local =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (local == nullptr) return false;
	// The zone-timer entry as the marker reads it: the client runtime's
	// 13-DWORD image of the retail shared timer list (present only once a
	// 0x6F value has arrived, which is when retail's CProximityList_FindEntryById
	// @0x598730 finds one), plus the two contest bytes the same message carries.
	const opennova::world::LfpZoneTimerLookup timer =
			[this](opennova::world::EntityHandle h, opennova::world::LfpZoneTimer &t) {
				const auto it = runtime_->zone_states().find(h.packed);
				if (it == runtime_->zone_states().end() || !it->second.has_value)
					return false;
				const auto &e = it->second.entry;
				t.team = e.mode_a;
				t.value = e.value_current;
				t.control = e.value_target;
				t.limit = e.value_limit;
				t.rate = e.value_rate;
				t.active = e.value_active;
				t.count_owner = e.contest_owner;
				t.count_other = e.contest_other;
				return true;
			};
	// The transient minimap slot's flag byte for the zone (+4 & 0xC0 gates the
	// marker; retail walks the 1160-slot transient bank @0x5a2517..0x5a256e).
	// The 0x6B ring slots land in the special bank here, so both are searched.
	const opennova::world::LfpCaptureFlagsLookup capture_flags =
			[this](opennova::world::EntityHandle h) -> uint8_t {
				const opennova::netsim::ClientMinimapState &map =
						runtime_->state().minimap;
				for (const auto &slot : map.transient) {
					if (slot.active && slot.handle == h.packed) return slot.flags;
				}
				for (const auto &slot : map.special) {
					if (slot.active && slot.handle == h.packed) return slot.flags;
				}
				return 0;
			};
	opennova::world::build_lfp_zones(kernel_->world, deploy_zone_registry(), *local,
			p_local_team, timer, capture_flags, r_zones);
	return true;
}

int64_t Simulation::get_session_game_type() const {
	// The in-match game type for EVERY role — the joiner's decoded header or
	// the HostClient view's own (retail g_GameType @0x24d2128); the AAS zone
	// panel and the Tab board key their arms on it.
	return runtime_ ? static_cast<int64_t>(runtime_->game_type()) : 0;
}

Ref<ScoreFeedback> Simulation::take_score_feedback() {
	// Revision-edge over the replica fold's 0x81 landing: every role's view
	// folds it (the host's own loopback included), so the edge is
	// role-agnostic [orig: NapiNPClientMsg_ScoreDeltaSound @0x42a0b0 runs on
	// every client, the listen host's own included].
	if (!runtime_) return Ref<ScoreFeedback>();
	const opennova::netsim::ClientState &cs = runtime_->state();
	if (cs.score_feedback.updates == score_feedback_updates_seen_) return Ref<ScoreFeedback>();
	score_feedback_updates_seen_ = cs.score_feedback.updates;
	const opennova::hud::ScoreTone tone =
			opennova::hud::score_delta_tone(cs.score_feedback.delta, cs.exp_fanfare);
	Ref<ScoreFeedback> out;
	out.instantiate();
	out->set_score(static_cast<int>(cs.score_feedback.score));
	out->set_delta(static_cast<int>(cs.score_feedback.delta));
	out->set_tone(String(opennova::hud::score_tone_set_name(tone)));
	return out;
}

TypedArray<ChatLineRow> Simulation::drain_chat_lines() {
	// The S2C 0x14 player-chat lines folded by the replica pipeline since the
	// last drain, each already routed by the witnessed channel table
	// (hud/feed_format.h): sink 0 = the SYSTEM ring, 1 = the CHAT ring,
	// 2 = the message queue (no ring), 3 = channel 3 (the unported third ring).
	TypedArray<ChatLineRow> out;
	if (!runtime_) return out;
	for (const opennova::netsim::ClientChatLine &line :
			runtime_->view().drain_chat_lines()) {
		Ref<ChatLineRow> d;
		d.instantiate();
		d->set_text(String::utf8(line.text.c_str()));
		d->set_argb(static_cast<int64_t>(
				opennova::hud::chat_channel_color(line.channel)));
		d->set_sink(static_cast<int>(
				opennova::hud::chat_channel_sink(line.channel)));
		d->set_channel(static_cast<int>(line.channel));
		out.push_back(d);
	}
	return out;
}

} // namespace godot
