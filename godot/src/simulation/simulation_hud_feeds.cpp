// The HUD panel feeds of the wire-up slice: the mounted-vehicle panel, the
// AAS zone status panel, the player-chat ring, and the session game type the
// shell's panel lanes gate on. Every witnessed rule lives in the engine
// (world/vehicle_panel_feed.h, world/lfp_feed.h, hud/feed_format.h); this TU is
// the Simulation seam that resolves the local player, the client runtime's
// zone-timer table and minimap banks, and the replica pipeline's chat drain
// for the HudOverlay setters (the set_scoreboard / fill_scoreboard_rows shape).

#include "simulation/simulation_internal.h"
#include "simulation/hud_view_records.h"

#include <formats/mission/mission.h> // runtime type -> authored item ID
#include <runtime/hud/score_fanfare.h> // the 0x81 tone ladder
#include <runtime/hud/feed_format.h> // formatted_game_text_line (the 0x32 lines)
#include <runtime/inmatch/client_runtime.h> // queue_chat_message (the talk keys' send)
#include <runtime/inmatch/napi_np_server_ctx.h> // NetworkType (the NovaWorld talk gate)
#include <runtime/world/player_present.h> // kWeaponSwitchDenySoundset (the crew key's tone)
#include <runtime/world/script_sounds.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/lfp_feed.h>
#include <runtime/world/vehicle_panel_feed.h>

using namespace sim_internal;
using namespace opennova::def;

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
	opennova::world::VehiclePanelRoot v;
	v.shown = true;
	v.item_id = root->item_id + opennova::mission::kItemIdOffset;
	out->assign(v);
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
			kernel_->world.cached.local_player, p_block, r_state.seats,
			is_joiner() ? joiner_role_ : nullptr);
	return true;
}

bool Simulation::fill_lfp_zones(int p_local_team,
		std::vector<opennova::hud::HudLfpZone> &r_zones) {
	// The zone walk with the role's zone-timer image and minimap slot flags
	// (inmatch/role_feeds.h collect_lfp_zones carries the witnesses).
	return opennova::inmatch::collect_lfp_zones(role_view(), deploy_zone_registry(), p_local_team,
			r_zones);
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
	const opennova::replication::ClientState &cs = runtime_->state();
	if (cs.score_feedback.updates == net_.score_feedback_updates_seen) return Ref<ScoreFeedback>();
	net_.score_feedback_updates_seen = cs.score_feedback.updates;
	const opennova::hud::ScoreTone tone =
			opennova::hud::score_delta_tone(cs.score_feedback.delta, cs.exp_fanfare);
	opennova::hud::ScoreFeedbackView v;
	v.score = cs.score_feedback.score;
	v.delta = cs.score_feedback.delta;
	v.tone = tone;
	Ref<ScoreFeedback> out;
	out.instantiate();
	out->assign(v);
	return out;
}

TypedArray<ChatLineRow> Simulation::drain_chat_lines() {
	// The S2C 0x14 player-chat lines folded by the replica pipeline since the
	// last drain, each already routed by the witnessed channel table
	// (hud/feed_format.h): sink 0 = the SYSTEM ring, 1 = the CHAT ring,
	// 2 = the message queue (no ring), 3 = channel 3 (the unported third ring).
	TypedArray<ChatLineRow> out;
	if (!runtime_) return out;
	for (const opennova::replication::ClientChatLine &line :
			runtime_->view().drain_chat_lines()) {
		Ref<ChatLineRow> d;
		d.instantiate();
		d->assign(line);
		out.push_back(d);
	}
	return out;
}

opennova::hud::ChatEntryFacts Simulation::chat_entry_facts(uint32_t p_frame) const {
	// The NovaWorld network type: the authority's own transport mode, or a
	// joiner that joined through a NovaWorld APPID (a LAN join carries "0").
	const opennova::inmatch::NapiNPServerCtx *host = host_ctx();
	const bool novaworld = is_joiner()
			? net_.app_id != "0"
			: (host != nullptr &&
					host->transport_mode == opennova::inmatch::NetworkType::NovaWorld);
	return opennova::inmatch::chat_entry_facts(role_view(), novaworld, p_frame);
}

opennova::hud::ChatSendResult Simulation::send_chat_line(int p_dispatch, std::string &r_text,
		uint32_t p_frame) {
	const int channel = opennova::hud::chat_dispatch_channel(p_dispatch);
	if (runtime_ == nullptr || channel < 0) return opennova::hud::ChatSendResult::Refused;
	return runtime_->queue_chat_message(static_cast<uint8_t>(channel), r_text, p_frame);
}

void Simulation::raise_chat_denied_sound() {
	if (!kernel_) return;
	// The crew key's denied tone rides the interface channel like the other
	// DRY_CLAYSATCH plays (world/player_present.h).
	opennova::world::ScriptSoundEvent tone;
	tone.name = opennova::world::kWeaponSwitchDenySoundset;
	tone.kind = opennova::world::ScriptSoundEvent::Kind::Interface;
	kernel_->world.out.script_sounds.push_back(std::move(tone));
}

void Simulation::drain_game_text_lines(const opennova::hud::GameTextLookup &p_gametext,
		std::vector<std::string> &r_lines) {
	r_lines.clear();
	if (!runtime_) return;
	const uint32_t game_type = runtime_->game_type();
	for (const opennova::replication::ClientGameText &text : runtime_->view().drain_game_texts()) {
		std::string line = opennova::hud::formatted_game_text_line(
				text.subtype, text.text, text.team, game_type, p_gametext);
		if (!line.empty()) r_lines.push_back(std::move(line));
	}
}

} // namespace godot
