// Simulation — the DEATH deploy screen's content feed and the dead player's
// medic call (D-HUD-19 residue b). The witnessed rules live in
// engine/runtime/world/deploy_screen_feed.h; this TU marshals the joiner's
// ClientState (the 0x0A sub-block-0 timers, the 0x6E wave groups, the 0x46
// roster names) and the authority's own facts into them, and routes the
// C2S 0x2E medic request the way the reload request already travels.
#include "simulation/simulation_internal.h"
#include "simulation/hud_view_records.h"
#include "simulation/deploy_rows.h" // the compiled SPAWNPOINTS_LIST row

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_chat.h> // Server_BroadcastDialogLine
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/spawn_select.h>

#include <algorithm>
#include <cstdio>

using namespace godot;

bool Simulation::local_player_dead() const {
	return opennova::inmatch::local_player_dead(role_view());
}

bool Simulation::request_local_player_medic() {
	if (!kernel_) return false;
	return active_role().request_medic();
}

int Simulation::local_medic_request_cooldown_ticks() const {
	return kernel_ ? kernel_->local.medic_request_cooldown_ticks : 0;
}

int Simulation::local_medic_request_serial() const {
	return kernel_ ? kernel_->local.medic_request_serial : 0;
}

void Simulation::set_server_text(const String &p_medic_request_format,
		const String &p_change_to_blue_format, const String &p_change_to_red_format) {
	// The rtxt "Server" table's formats the host's broadcasts print a player's
	// name into: STRSRV_MEDREQ (Server_BroadcastMedicRequest @0x515390) and
	// C2Blue / C2Red (NapiNPServerMsg_0x04D_ChangeTeam @0x51902E / @0x51909C).
	opennova::inmatch::ServerTextTable text;
	text.medic_request_format = opennova::to_std(p_medic_request_format);
	text.change_to_blue_format = opennova::to_std(p_change_to_blue_format);
	text.change_to_red_format = opennova::to_std(p_change_to_red_format);
	if (opennova::inmatch::NapiNPServerCtx *ctx = host_ctx())
		opennova::inmatch::set_server_text(*ctx, std::move(text));
}

bool Simulation::send_team_change_request() {
	return runtime_ != nullptr && runtime_->queue_team_change_request();
}

void Simulation::broadcast_dialog_line(const std::string &p_dialog, int p_line) {
	// The authority's co-op dialog line; a joiner has no host context.
	if (opennova::inmatch::NapiNPServerCtx *ctx = host_ctx())
		opennova::inmatch::Server_BroadcastDialogLine(*ctx, p_dialog, static_cast<int16_t>(p_line));
}

Ref<DeployStatus> Simulation::get_deploy_status(const Ref<RtxtStringFile> &p_gametext,
		const String &p_medic_key_label) {
	// The DEATH screen's STATIC facts for THIS client, off the role's replica
	// state and the authority's own facts (inmatch/role_feeds.h
	// deploy_screen_status carries the witnesses).
	Ref<DeployStatus> out;
	out.instantiate();
	out->assign(opennova::inmatch::deploy_screen_status(role_view(), deploy_zone_registry(),
			opennova::to_std(p_medic_key_label), game_text_lookup(p_gametext)));
	return out;
}

bool Simulation::fill_death_map_facts(opennova::hud::DeathMapFacts &r_out) {
	// The engine gather (inmatch/role_feeds.h death_map_facts) over the role's
	// replica view and the spawn-zone registry this sim builds per load.
	if (!kernel_) {
		r_out = opennova::hud::DeathMapFacts{};
		return false;
	}
	return opennova::inmatch::death_map_facts(role_view(), deploy_zone_registry(), r_out);
}

bool Simulation::is_death_shroud_revealed() const {
	return kernel_ && opennova::inmatch::death_shroud_revealed(role_view());
}

bool Simulation::deploy_refresh_due(int64_t p_prev_tick, int64_t p_tick) {
	return opennova::world::deploy_refresh_due(p_prev_tick, p_tick);
}

TypedArray<DeployListRow> Simulation::get_deploy_list_rows(const String &p_default_key,
		const String &p_default_home, const Dictionary &p_zone_names) {
	// The compiled SPAWNPOINTS_LIST: the engine builder runs both witnessed
	// loops over the zone rows this sim exposes (deploy_zone_rows), the team
	// colour tag, and the embedder-resolved WPNames strings.
	TypedArray<DeployListRow> out;
	if (!kernel_ || !is_joiner() || !runtime_) return out;
	opennova::world::DeployListInput in;
	// [orig: "<c4040FF>", or "<cFF2020>" when Team == 2 @0x553b1e..0x553b38]
	in.team_color_tag = runtime_->assigned_team() == 2 ? "<cFF2020>" : "<c4040FF>";
	in.default_key = opennova::to_std(p_default_key);
	in.default_home = opennova::to_std(p_default_home);
	in.zones = deploy_zone_rows();
	in.zone_name = [&p_zone_names](const std::string &key) {
		const String k = opennova::to_gd(key);
		if (p_zone_names.has(k)) return opennova::to_std(String(p_zone_names[k]));
		return key;
	};
	for (const opennova::world::DeployListRow &row : opennova::world::build_deploy_rows(in)) {
		Ref<DeployListRow> record;
		record.instantiate();
		record->assign(row);
		out.push_back(record);
	}
	return out;
}
