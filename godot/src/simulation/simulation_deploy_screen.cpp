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
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include "rtxt/rtxt_string_file.h"

#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/spawn_select.h>

#include <algorithm>

using namespace godot;

bool Simulation::local_player_dead() const {
	// The one role-agnostic read of the local player's dead bit: the joiner's
	// replica (inmatch::ClientRuntime), the authority's entity flags (the kernel).
	if (joiner_) return runtime_ != nullptr && runtime_->local_player_dead();
	return kernel_ != nullptr && kernel_->local.local_player_dead();
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

void Simulation::set_server_text(const String &p_medic_request_format) {
	// The rtxt "Server" table's STRSRV_MEDREQ format the host's medic
	// broadcast prints the caller's name into (Server_BroadcastMedicRequest
	// @0x515390, Lane 1's handler reads NapiNPServerCtx::medic_request_format).
	opennova::inmatch::ServerTextTable text;
	text.medic_request_format = p_medic_request_format.utf8().get_data();
	opennova::inmatch::set_server_text(ctx_, std::move(text));
}

Ref<DeployStatus> Simulation::get_deploy_status() {
	// The DEATH screen's STATIC facts for THIS client [orig: the client
	// globals UI_UpdateDeathScreenContent @0x5536a0 reads — dword_A85B5C /
	// A85B60 / A85B68 from the 0x0A sub-block 0, word_A85BC0 + entity+538/548
	// from the 0x6E fold].
	int penalty = 0;
	int revive = 0;
	int hold = 0;
	int self_zone_index = -1;
	bool self_zone_numbered = false;
	int self_zone_countdown = 0;
	if (joiner_ && runtime_) {
		const opennova::replication::ClientState &cs = runtime_->state();
		penalty = cs.respawn_penalty_seconds;
		revive = cs.local_revive_seconds;
		hold = cs.spawn_hold_seconds;
		if (cs.spawn_waves.known && cs.spawn_waves.self_zone_handle != 0xFFFFu &&
				kernel_) {
			const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
			const opennova::world::EntityHandle zone{cs.spawn_waves.self_zone_handle};
			self_zone_index = opennova::world::spawn_zone_index_of(reg, zone);
			if (const opennova::world::Entity *e = kernel_->world.registry.get(zone)) {
				self_zone_numbered = e->zone_number != 0;
			}
			for (const opennova::SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle == cs.spawn_waves.self_zone_handle)
					self_zone_countdown = g.wave_countdown;
			}
		}
	}
	opennova::world::DeployStatusInput status_in;
	status_in.penalty_seconds = penalty;
	status_in.self_zone_index = self_zone_index;
	status_in.self_zone_numbered = self_zone_numbered;
	status_in.self_zone_countdown = self_zone_countdown;
	const opennova::world::DeployStatusLine line =
			opennova::world::build_deploy_status(status_in);
	opennova::world::DeployStaticsInput statics_in;
	statics_in.hold_seconds = hold;
	statics_in.revive_seconds = revive;
	statics_in.local_mounted = kernel_->local.view.mount.control_seat;
	const opennova::world::DeployStaticsVisibility statics =
			opennova::world::deploy_statics_visibility(statics_in);
	opennova::world::DeployScreenStatus v;
	v.penalty_seconds = penalty;
	v.revive_seconds = revive;
	v.hold_seconds = hold;
	v.line = line;
	v.statics = statics;
	v.medic_cooldown_ticks = kernel_ ? static_cast<int>(kernel_->local.medic_request_cooldown_ticks) : 0;
	v.medic_request_serial = kernel_ ? static_cast<int>(kernel_->local.medic_request_serial) : 0;
	Ref<DeployStatus> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String Simulation::get_deploy_status_text(const Ref<RtxtStringFile> &p_gametext) {
	// The STATIC_RESPAWN_MSG1 text: the engine's three sprintf arms over the
	// status line get_deploy_status computes, with the gametext strings
	// resolved here (GameText_GetString("Overlays", "STROVER_PENALTYTIMER") /
	// ("WPNames", "STRWPNAME%03d") @0x5536a0, their shipped fallbacks).
	const Ref<DeployStatus> status = get_deploy_status();
	const opennova::world::DeployStatusLine &line = status->value().line;
	auto game_text = [&p_gametext](const char *section, const String &key, const char *fallback) {
		if (!p_gametext.is_null() && p_gametext->has_string_in_section(section, StringName(key)))
			return p_gametext->get_string_in_section(section, StringName(key));
		return String(fallback);
	};
	const String penalty_label = game_text("Overlays", "STROVER_PENALTYTIMER", "Respawn penalty");
	String zone_name;
	if (line.kind == opennova::world::DeployStatusLine::Kind::Wave && line.numbered) {
		// "STRWPNAME%03d" over index + 1 (the key form deploy_screen_feed.h witnesses).
		zone_name = game_text("WPNames",
				String("STRWPNAME") + String::num_int64(line.zone_index + 1).pad_zeros(3),
				"Spawn Point");
	}
	return String::utf8(opennova::world::deploy_status_text(line,
			penalty_label.utf8().get_data(), zone_name.utf8().get_data()).c_str());
}

TypedArray<DeployListRow> Simulation::get_deploy_list_rows(const String &p_default_key,
		const String &p_default_home, const Dictionary &p_zone_names) {
	// The compiled SPAWNPOINTS_LIST: the engine builder runs both witnessed
	// loops over the zone rows this sim exposes (deploy_zone_rows), the team
	// colour tag, and the embedder-resolved WPNames strings.
	TypedArray<DeployListRow> out;
	if (!kernel_ || !joiner_ || !runtime_) return out;
	opennova::world::DeployListInput in;
	// [orig: "<c4040FF>", or "<cFF2020>" when Team == 2 @0x553b1e..0x553b38]
	in.team_color_tag = runtime_->assigned_team() == 2 ? "<cFF2020>" : "<c4040FF>";
	in.default_key = p_default_key.utf8().get_data();
	in.default_home = p_default_home.utf8().get_data();
	in.zones = deploy_zone_rows();
	in.zone_name = [&p_zone_names](const std::string &key) {
		const String k = String::utf8(key.c_str());
		if (p_zone_names.has(k)) return std::string(String(p_zone_names[k]).utf8().get_data());
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
