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
#include <net/npwire/ingame_message_id.h> // the C2S 0x14 / 0x13 menu-pick tags
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

PackedByteArray Simulation::take_tip_events() {
	PackedByteArray out;
	if (kernel_ == nullptr) return out;
	std::vector<uint8_t> &events = kernel_->world.out.tip_events;
	out.resize(static_cast<int64_t>(events.size()));
	for (size_t i = 0; i < events.size(); ++i) out.set(static_cast<int64_t>(i), events[i]);
	events.clear();
	return out;
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

bool Simulation::is_round_over() const {
	return kernel_ != nullptr && kernel_->world.match.outcome().ended;
}

opennova::hud::ChatEntryFacts Simulation::chat_entry_facts(uint32_t p_frame) const {
	// The NovaWorld network type (hud::ChatEntryFacts::novaworld): the
	// authority's own transport mode, or the network type the joiner's join
	// carried (JoinTarget::network_type).
	const opennova::inmatch::NapiNPServerCtx *host = host_ctx();
	const bool novaworld = is_joiner()
			? net_.join_network_type == opennova::inmatch::NetworkType::NovaWorld
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

void Simulation::send_go_code(int p_code) {
	if (runtime_ == nullptr || !kernel_ || p_code < 0 || p_code > 0xFF) return;
	runtime_->send_go_code(kernel_->world, static_cast<uint8_t>(p_code));
}

bool Simulation::send_voice_menu_pick(bool p_radio, int p_value) {
	return runtime_ != nullptr &&
			runtime_->queue_voice_menu_pick(p_radio ? opennova::c2s::RADIO_CALL_REQUEST
													: opennova::c2s::EMOTE_REQUEST,
					static_cast<int16_t>(p_value));
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

// This frame's ring-bound lines from the three S2C lanes, in the order their
// messages dispatched (hud::FeedPost carries the witness): the 0x1E game
// events folded to rows (the engine's feed_event_rows; the actor names
// resolve here against the decoded roster, a pool-0 INDEX on the wire being
// the handle (0<<12)|index, the driving slot's name with its registry clan
// tag first — replication::feed_actor_name) and resolved against gametext;
// the 0x14 chat lines routed by the channel table; the 0x32 lines formatted.
void Simulation::drain_feed_posts(const opennova::hud::GameTextLookup &p_gametext,
		bool p_mp_verbose, std::vector<opennova::hud::FeedPost> &r_posts) {
	r_posts.clear();
	if (!runtime_) return;
	opennova::replication::ClientState &cs = runtime_->state();
	const uint16_t self_handle =
			runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFF;
	const auto actor_of = [&cs](uint8_t index) -> opennova::hud::FeedActor {
		const auto *e = cs.find(static_cast<uint16_t>(index));
		return opennova::hud::FeedActor{
			opennova::replication::feed_actor_name(cs, index,
					e != nullptr ? e->display_name : std::string()),
			e != nullptr ? e->team : uint8_t{0}};
	};
	std::vector<opennova::hud::FeedEventInput> inputs;
	for (const opennova::replication::ClientGameEvent &ev : runtime_->drain_game_events()) {
		inputs.push_back({ ev.event_type, ev.attacker_index, ev.victim_index,
				ev.aux_index, ev.kind, ev.pos_x, ev.feed_order });
	}
	std::vector<opennova::hud::FeedRow> rows;
	const opennova::hud::FeedContext context{
		self_handle, p_mp_verbose, runtime_->game_type()};
	opennova::hud::feed_event_rows(inputs.data(), inputs.size(), context, actor_of, rows);
	for (const opennova::hud::FeedRow &row : rows) {
		std::string line = opennova::hud::feed_row_line(row, p_gametext);
		if (line.empty()) continue;
		r_posts.push_back({row.order, opennova::hud::ChatSink::System, row.color,
				std::move(line), row.announce});
	}
	for (const opennova::replication::ClientChatLine &line :
			runtime_->view().drain_chat_lines()) {
		if (line.text.empty()) continue;
		r_posts.push_back({line.feed_order, opennova::hud::chat_channel_sink(line.channel),
				opennova::hud::chat_channel_color(line.channel), line.text, false});
	}
	for (const opennova::hud::SquadFeedLine &line : runtime_->drain_squad_lines()) {
		opennova::hud::SquadFeedPost post = opennova::hud::squad_feed_post(line, p_gametext);
		if (!post.post) continue;
		r_posts.push_back({line.order,
				post.system_ring ? opennova::hud::ChatSink::System : opennova::hud::ChatSink::Chat,
				post.argb, std::move(post.text), false});
	}
	const uint32_t game_type = runtime_->game_type();
	for (const opennova::replication::ClientGameText &text : runtime_->view().drain_game_texts()) {
		std::string line = opennova::hud::formatted_game_text_line(
				text.subtype, text.text, text.team, game_type, p_gametext);
		if (line.empty()) continue;
		r_posts.push_back({text.feed_order, opennova::hud::ChatSink::System,
				opennova::hud::kGameTextLineColor, std::move(line), false});
	}
	opennova::hud::order_feed_posts(r_posts);
}

} // namespace godot
