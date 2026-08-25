// Simulation — the DEATH deploy screen's content feed and the dead player's
// medic call (D-HUD-19 residue b). The witnessed rules live in
// engine/runtime/world/deploy_screen_feed.h; this TU marshals the joiner's
// ClientState (the 0x0A sub-block-0 timers, the 0x6E wave groups, the 0x46
// roster names) and the authority's own facts into them, and routes the
// C2S 0x2E medic request the way the reload request already travels.
#include "simulation/nova_simulation_internal.h"

#include <npruntime/napi_np_server_ctx.h>
#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>
#include "rtxt/rtxt_string_file.h"

#include <world/deploy_screen_feed.h>
#include <world/spawn_select.h>

#include <algorithm>

using namespace godot;

namespace {

// The retail client medic-call cooldown: 310 ticks stamped at the send
// (retail: Input_HandleActionBinding case 217 @0x49b511 `dword_B76804 =
// 0x136`; decremented once per frame in Player_UpdatePerFrame @0x4de73e;
// cleared on the local death path @0x4b4d06, see docs/net/novaworld-net-re.md
// 0x2E).
constexpr int kMedicRequestCooldownTicks = 0x136;

} // namespace

bool Simulation::local_player_dead() const {
	// The one role-agnostic read of the local player's dead bit: the joiner's
	// recipient-local 0x0A health channel, the authority's entity flags.
	if (joiner_) {
		// The recipient-local 0x0A health tail (the client stores it as its own
		// Health) or the self record's dead bit (byte13 bit 0x02 -> Flags & 2 in
		// the local apply @0x4c1005).
		if (runtime_ == nullptr) return false;
		const opennova::netsim::ClientState &cs = runtime_->state();
		if (cs.local_health <= 0) return true;
		if (!runtime_->has_self_handle()) return false;
		const opennova::netsim::ClientEntityState *self = cs.find(runtime_->self_handle());
		return self != nullptr && self->state_flags_known && (self->state_flags & 0x02u) != 0;
	}
	if (!world_ || !world_->cached.local_player.valid()) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr &&
			((e->flags | e->engine_flags) & opennova::world::kEntityFlagDead) != 0;
}

bool Simulation::request_local_player_medic() {
	// The action gates (retail: case 217 @0x49b4b4..0x49b4da — in session, a
	// local entity, `Flags & 2`, the cooldown at zero).
	if (!runtime_ || !world_ || !world_->cached.local_player.valid()) return false;
	if (!local_player_dead()) return false;
	if (medic_request_cooldown_ticks_ != 0) return false;
	bool sent = false;
	if (joiner_) {
		sent = runtime_->queue_medic_request();
	} else if (host_owner_.serve_and_play) {
		// The listen host's own call rides its loopback client like the reload
		// request (nova_simulation_player_weapon.cpp): the server handler
		// broadcasts the 0x1E line to everyone including this client.
		opennova::MedicRequest request;
		request.entity_index = world_->cached.local_player.packed;
		host_loop_.client_send(opennova::c2s::MEDIC_REQUEST,
				opennova::encode_medic_request(request));
		sent = true;
	}
	if (sent) {
		medic_request_cooldown_ticks_ = kMedicRequestCooldownTicks;
		++medic_request_serial_;
	}
	return sent;
}

void Simulation::tick_local_medic_cooldown() {
	// (retail: Player_UpdatePerFrame @0x4de736..0x4de744 — one per frame while
	// nonzero; the local death path @0x4b4d06 zeroes it.)
	const bool dead = local_player_dead();
	if (dead && !local_dead_edge_seen_) medic_request_cooldown_ticks_ = 0;
	local_dead_edge_seen_ = dead;
	if (medic_request_cooldown_ticks_ > 0) --medic_request_cooldown_ticks_;
}

int Simulation::local_medic_request_cooldown_ticks() const {
	return medic_request_cooldown_ticks_;
}

int Simulation::local_medic_request_serial() const {
	return medic_request_serial_;
}

void Simulation::set_server_text(const String &p_medic_request_format) {
	// The rtxt "Server" table's STRSRV_MEDREQ format the host's medic
	// broadcast prints the caller's name into (Server_BroadcastMedicRequest
	// @0x515390, Lane 1's handler reads NapiNPServerCtx::medic_request_format).
	opennova::np::ServerTextTable text;
	text.medic_request_format = p_medic_request_format.utf8().get_data();
	opennova::np::set_server_text(ctx_, std::move(text));
}

Dictionary Simulation::get_deploy_status() {
	// The DEATH screen's STATIC facts for THIS client (retail: the client
	// globals UI_UpdateDeathScreenContent @0x5536a0 reads — dword_A85B5C /
	// A85B60 / A85B68 from the 0x0A sub-block 0, word_A85BC0 + entity+538/548
	// from the 0x6E fold).
	Dictionary out;
	int penalty = 0;
	int revive = 0;
	int hold = 0;
	int self_zone_index = -1;
	bool self_zone_numbered = false;
	int self_zone_countdown = 0;
	if (joiner_ && runtime_) {
		const opennova::netsim::ClientState &cs = runtime_->state();
		penalty = cs.respawn_penalty_seconds;
		revive = cs.local_revive_seconds;
		hold = cs.spawn_hold_seconds;
		if (cs.spawn_waves.known && cs.spawn_waves.self_zone_handle != 0xFFFFu &&
				world_) {
			const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
			const opennova::world::EntityHandle zone{cs.spawn_waves.self_zone_handle};
			self_zone_index = opennova::world::spawn_zone_index_of(reg, zone);
			if (const opennova::world::Entity *e = world_->registry.get(zone)) {
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
	statics_in.local_mounted = player_view_.mount.control_seat;
	const opennova::world::DeployStaticsVisibility statics =
			opennova::world::deploy_statics_visibility(statics_in);
	out["penalty_seconds"] = penalty;
	out["revive_seconds"] = revive;
	out["hold_seconds"] = hold;
	out["queued_kind"] = static_cast<int>(line.kind);
	out["queued_zone_index"] = line.zone_index;
	out["queued_seconds"] = line.seconds;
	out["queued_numbered"] = line.numbered;
	out["show_psp_respawn"] = statics.psp_respawn;
	out["show_medic"] = statics.medic;
	out["medic_cooldown_ticks"] = medic_request_cooldown_ticks_;
	out["medic_request_serial"] = medic_request_serial_;
	return out;
}

String Simulation::get_deploy_status_text(const Ref<RtxtStringFile> &p_gametext) {
	// The STATIC_RESPAWN_MSG1 text: the engine's three sprintf arms over the
	// status line get_deploy_status computes, with the gametext strings
	// resolved here (GameText_GetString("Overlays", "STROVER_PENALTYTIMER") /
	// ("WPNames", "STRWPNAME%03d") @0x5536a0, their shipped fallbacks).
	const Dictionary status = get_deploy_status();
	opennova::world::DeployStatusLine line;
	line.kind = static_cast<opennova::world::DeployStatusLine::Kind>(
			static_cast<int>(status.get("queued_kind", 0)));
	line.seconds = static_cast<int>(status.get("queued_seconds", 0));
	line.numbered = static_cast<bool>(status.get("queued_numbered", false));
	line.zone_index = static_cast<int>(status.get("queued_zone_index", -1));
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

TypedArray<Dictionary> Simulation::get_deploy_list_rows(const String &p_default_key,
		const String &p_default_home, const Dictionary &p_zone_names) {
	// The compiled SPAWNPOINTS_LIST: the engine builder runs both witnessed
	// loops over the zone rows this sim exposes (get_deploy_spawn_zones), the
	// team colour tag, and the embedder-resolved WPNames strings.
	TypedArray<Dictionary> out;
	if (!world_ || !joiner_ || !runtime_) return out;
	opennova::world::DeployListInput in;
	// (retail: "<c4040FF>", or "<cFF2020>" when Team == 2 @0x553b1e..0x553b38)
	in.team_color_tag = runtime_->assigned_team() == 2 ? "<cFF2020>" : "<c4040FF>";
	in.default_key = p_default_key.utf8().get_data();
	in.default_home = p_default_home.utf8().get_data();
	const TypedArray<Dictionary> zones = get_deploy_spawn_zones();
	for (int i = 0; i < zones.size(); ++i) {
		const Dictionary z = zones[i];
		opennova::world::DeployZoneRow row;
		row.index = static_cast<int>(int64_t(z.get("param", 0))) - 1;
		row.letter = static_cast<char>('A' + row.index);
		row.name_key = String(z.get("name_key", "")).utf8().get_data();
		row.secured = bool(z.get("secured", false));
		row.wave_countdown = static_cast<uint16_t>(int64_t(z.get("wave_countdown", 0)));
		const Array occupants = z.get("occupants", Array());
		for (int k = 0; k < occupants.size(); ++k) {
			const Dictionary o = occupants[k];
			opennova::world::DeployOccupant occ;
			occ.handle = static_cast<uint16_t>(int64_t(o.get("handle", 0xFFFF)));
			occ.name = String(o.get("name", "")).utf8().get_data();
			occ.self = bool(o.get("self", false));
			row.occupants.push_back(occ);
		}
		in.zones.push_back(row);
	}
	in.zone_name = [&p_zone_names](const std::string &key) {
		const String k = String::utf8(key.c_str());
		if (p_zone_names.has(k)) return std::string(String(p_zone_names[k]).utf8().get_data());
		return key;
	};
	for (const opennova::world::DeployListRow &row : opennova::world::build_deploy_rows(in)) {
		Dictionary d;
		d["text"] = String::utf8(row.text.c_str());
		d["value"] = row.value;
		out.push_back(d);
	}
	return out;
}
