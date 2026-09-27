// The role feeds — see role_feeds.h. [orig: NapiNPClientMsg_0x01D @0x430840;
// UI_UpdateDeathScreenContent @0x5536a0; HUD_DrawFriendlyTagsPass @0x5a4480]

#include <runtime/inmatch/role_feeds.h>

#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_roster_tags.h>
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of
#include <runtime/world/collision.h>

#include <algorithm>
#include <cstdio>

namespace opennova::inmatch {

bool local_death_screen_active(const RoleView &view) {
	return view.runtime != nullptr && view.runtime->state().death_screen_active;
}

bool local_player_dead(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr && view.runtime->local_player_dead();
	return view.kernel != nullptr && view.kernel->local.local_player_dead();
}

namespace {

// Authority: the Match clock; joiner: the folded 0x0A sub-block-1 copy
// [orig: g_RoundTimeRemaining @0x24C1958, the joiner store
// @0x430219..0x430235].
int32_t round_ticks_remaining(const RoleView &view) {
	const int32_t remaining = view.joiner
			? view.runtime->state().round_time_remaining_ticks
			: (view.kernel != nullptr ? view.kernel->world.match.remaining_ticks() : -1);
	return std::max(0, remaining);
}

// The local player's team the end-round overlay compares the winner against
// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0 reads byte_A85B48 @0x5b7f56].
// A joiner latches it from S2C 0x04 (ClientRuntime::assigned_team); the
// listen host's HostClient runtime never receives that record (the latch
// stays 0), so the authority reads its own player entity — retail's host
// latches the same value from its loopback 0x04 @0x425499.
int local_team_of(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr ? view.runtime->assigned_team() : 0;
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	return player != nullptr ? player->team : 0;
}

} // namespace

EndRoundSessionState end_round_session_state(const RoleView &view) {
	EndRoundSessionState v;
	if (view.runtime == nullptr) return v;
	const replication::ClientEndRoundStats &er = view.runtime->state().end_round;
	v.header_known = er.header_known;
	v.board_known = er.known;
	v.game_type = view.runtime->game_type();
	v.winner = static_cast<int>(er.header.winner_team);
	v.team_score_0 = static_cast<int>(er.header.team_score_0);
	v.team_score_1 = static_cast<int>(er.header.team_score_1);
	v.draw = er.header.draw != 0;
	v.my_index = static_cast<int>(er.header.player_index);
	v.round_ticks = round_ticks_remaining(view);
	v.death_screen = local_death_screen_active(view);
	v.local_team = local_team_of(view);
	// The team-mode arm stat.mnu's RADIO_TAB_* trio rides (the g_GameType
	// 0x10000 bit, base/gameprofile/game_type.h; the show callback's witness is
	// stat_screen_feed.h's).
	v.team_mode = game_type::is_team(view.runtime->game_type());
	// The round-cycle handoff's session half: the host's post-round linger
	// expiry closes the session [orig: Server_TickUpdate's drain sets
	// g_MissionExitReason = 3 @0x51db63 — the map cycle]; a joiner's session
	// dies with the host's exit.
	v.session_open = view.joiner ? !view.runtime->session_lost()
								 : view.host != nullptr && view.host->is_in_session != 0;
	return v;
}

hud::EndRoundOverlayInput end_round_overlay_input(const RoleView &view) {
	hud::EndRoundOverlayInput in;
	if (view.runtime == nullptr) return in;
	const replication::ClientEndRoundStats &er = view.runtime->state().end_round;
	in.game_type = view.runtime->game_type();
	in.draw = er.header.draw != 0;
	in.winner_team = er.header.winner_team;
	in.local_team = static_cast<uint8_t>(local_team_of(view));
	in.death_screen = local_death_screen_active(view);
	in.team_scores[0] = er.header.team_score_0;
	in.team_scores[1] = er.header.team_score_1;
	// The non-team 0x1D form's three named players + primary scores; empty
	// names take the ladder's name-less arms. [orig: the 0x1D commit
	// @0x430a70..0x430abb into byte_24C1A98/B7C/C60 + dword_24C1AD4/BB8/C9C]
	for (int i = 0; i < 3; ++i) {
		in.player_names[i] = er.header.player_names[i];
		in.player_scores[i] = er.header.player_scores[i];
	}
	// [orig: g_RoundTimeRemaining @0x24C1958 — the game-time line and the
	// timed/untimed arm picks read it on every role].
	in.round_time_remaining_ticks = round_ticks_remaining(view);
	return in;
}

BreathBarFacts breath_bar_facts(const RoleView &view) {
	BreathBarFacts out;
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		out.samples = cs.breath_samples;             // word_A85B7C
		out.spawn_success_gate = cs.end_round.header_known;
		if (view.joiner) out.breath_time = cs.breathtime;
	}
	// The authority's frame carries no sub-block 1 to its own loopback, and
	// its HUD reads the host's own named value [orig: g_WacVarBreathTime].
	if (!view.joiner && view.kernel != nullptr)
		out.breath_time = view.kernel->world.script.wac_values.breathtime;
	return out;
}

std::vector<StatScreenRow> end_round_rows(const RoleView &view, int tab) {
	// The PLAYER SLOT table retail walks is the roster every role's view folds
	// from 0x46 (name / clan / team). The local row comes from the 0x1D
	// header's board index, resolved to a connection slot below.
	std::vector<StatScreenRow> out;
	if (view.runtime == nullptr) return out;
	const replication::ClientState &cs = view.runtime->state();
	if (!cs.end_round.known) return out;
	std::vector<StatScreenPlayer> players;
	for (size_t i = 0; i < cs.roster.size(); ++i) {
		const replication::ClientRosterSlot &slot = cs.roster[i];
		if (!slot.bound) continue;
		StatScreenPlayer p;
		p.slot = static_cast<uint8_t>(i);
		p.team = slot.team;
		p.name = slot.name;
		p.squad = slot.clan;
		players.push_back(p);
	}
	// The header's player_index is the recipient's index into the FROZEN
	// (points-descending) board array, not a connection slot. Retail joins the
	// board by the row's stored slot id and highlights the row whose slot
	// matches board[player_index].slot; the same fold works for both roles
	// because the listen host consumes its own loopback 0x1D.
	int local_slot = -1;
	const int8_t header_index = cs.end_round.header.player_index;
	if (header_index >= 0 &&
			static_cast<size_t>(header_index) < cs.end_round.board.players.size())
		local_slot = cs.end_round.board.players[static_cast<size_t>(header_index)].slot;
	for (const StatScreenRow &r : stat_screen_rows(cs.end_round.board, players, false, local_slot)) {
		// The tab filter (stat_screen_row_visible carries the witness).
		if (stat_screen_row_visible(tab, r.team)) out.push_back(r);
	}
	return out;
}

bool collect_friendly_tags(const RoleView &view, std::vector<world::FriendlyTagSource> &out) {
	out.clear();
	if (view.kernel == nullptr) return false;
	world::World &w = view.kernel->world;
	const world::Entity *player = w.registry.get(w.cached.local_player);
	if (player == nullptr) return false;
	// The pass-level facts (retail g_DeathScreenActive / g_GameType): the
	// death screen bit is the client's local latch, the game type every role's
	// view carries.
	world::FriendlyTagPassContext ctx;
	ctx.death_screen = local_death_screen_active(view);
	ctx.game_type = view.runtime != nullptr ? view.runtime->game_type() : 0;
	// The session's rules word: a joiner's S2C 0x64 fixed block (+44), the
	// host's own mp_attributes. Bit 0x400 = the host option FriendlyTag 0.
	const uint32_t rules_word = (view.joiner && view.runtime != nullptr)
			? view.runtime->view().mp_attributes()
			: view.staged_mp_attributes;
	ctx.rules_no_friendly_tags = (rules_word & GameConfig::kMpAttribNoFriendlyTag) != 0;
	// The player walk's slot owner. On the authority the connection table IS
	// the player-slot table: each link's owned entity, revive window, and
	// medic-request latch (retail's PlayerSlot +0x24/+0x10/+0x2C).
	const NapiNPServerCtx *host = view.host;
	const world::PlayerSlotLookup authority_slot_lookup =
			[host](world::EntityHandle entity, world::PlayerSlotFacts &facts) {
				if (host == nullptr) return false;
				for (const NapiNPConnection &conn : host->np_protocol.connection_list) {
					if (conn.link.owned_entity != entity) continue;
					facts.revive_seconds = static_cast<uint8_t>(
							std::min<uint32_t>(conn.link.downed_revive_seconds, 0xFFu));
					facts.medic_request = conn.link.medic_request_active;
					return true;
				}
				return false;
			};
	if (!view.joiner) ctx.slot_lookup = &authority_slot_lookup;
	world::collect_friendly_tags(w, *player, out, ctx);
	if (view.joiner && view.runtime != nullptr && !ctx.rules_no_friendly_tags) {
		// A joiner's players are decoded rows, not World twins: the roster walk
		// over ClientState supplies them (replication/client_roster_tags.h).
		// Both walks compare with the local player's entity Team, not the S2C
		// latch; spawn_from_self seeds it before any 0x04/0x50 lands
		// [orig: g_LocalPlayerEntity+0x162 @0x5a455c / @0x5a3c71].
		const int32_t player_hp = w.tables.player.item_hp;
		replication::collect_roster_tags(view.runtime->state(),
				view.runtime->has_self_handle() ? view.runtime->self_handle() : 0xFFFFu,
				player->team, ctx.death_screen, ctx.game_type, out,
				[player_hp](uint16_t) { return player_hp; }, &w);
	}
	return true;
}

world::DeployScreenStatus deploy_screen_status(const RoleView &view,
		const world::SpawnZoneRegistry &zones, const std::string &medic_key_label,
		const hud::GameTextLookup &gametext) {
	int penalty = 0;
	int revive = 0;
	int hold = 0;
	int self_zone_index = -1;
	bool self_zone_numbered = false;
	int self_zone_countdown = 0;
	if (view.joiner && view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		penalty = cs.respawn_penalty_seconds;
		revive = cs.local_revive_seconds;
		hold = cs.spawn_hold_seconds;
		if (cs.spawn_waves.known && cs.spawn_waves.self_zone_handle != 0xFFFFu &&
				view.kernel != nullptr) {
			const world::EntityHandle zone{ cs.spawn_waves.self_zone_handle };
			self_zone_index = world::spawn_zone_index_of(zones, zone);
			if (const world::Entity *e = view.kernel->world.registry.get(zone))
				self_zone_numbered = e->zone_number != 0;
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle == cs.spawn_waves.self_zone_handle)
					self_zone_countdown = g.wave_countdown;
			}
		}
	}
	world::DeployStatusInput status_in;
	status_in.penalty_seconds = penalty;
	status_in.self_zone_index = self_zone_index;
	status_in.self_zone_numbered = self_zone_numbered;
	status_in.self_zone_countdown = self_zone_countdown;
	const world::DeployStatusLine line = world::build_deploy_status(status_in);
	world::DeployStaticsInput statics_in;
	statics_in.hold_seconds = hold;
	statics_in.revive_seconds = revive;
	// The +0x1E0 being-revived latch rides the joiner's ClientState (S2C 0x3A);
	// a listen host's own player has no revive sender ported yet, so it stays
	// clear there.
	statics_in.local_medic_reviving = view.joiner && view.runtime != nullptr
			? view.runtime->state().local_medic_reviving
			: false;

	world::DeployScreenStatus v;
	v.penalty_seconds = penalty;
	v.revive_seconds = revive;
	v.hold_seconds = hold;
	v.line = line;
	v.statics = world::deploy_statics_visibility(statics_in);
	v.medic_cooldown_ticks = view.kernel != nullptr
			? static_cast<int>(view.kernel->local.medic_request_cooldown_ticks)
			: 0;
	v.medic_request_serial =
			view.kernel != nullptr ? static_cast<int>(view.kernel->local.medic_request_serial) : 0;

	world::DeployInstructionsInput instructions;
	instructions.dead = local_player_dead(view);
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	if (player != nullptr) {
		instructions.team = player->team;
		instructions.player_name = player->display_name;
	}
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		instructions.game_type = view.runtime->game_type();
		instructions.permanent_death = cs.permanent_death;
		instructions.spectators_allowed = cs.spectators_allowed;
		instructions.check_secured_spawn = cs.deploy_check_secured_spawn;
		instructions.kill_announcement = cs.kill_announcement.text;
		instructions.round_ticks = cs.round_time_remaining_ticks;
		instructions.alive_players = cs.scoreboard.alive_player_count;
		const replication::ClientRosterSlot &roster = cs.roster[view.runtime->local_player_slot()];
		if (roster.bound) {
			instructions.player_name = roster.name;
			instructions.clan = roster.clan;
		}
		if (view.joiner) instructions.team = view.runtime->assigned_team();
		if (view.kernel != nullptr) {
			instructions.has_spawn_zones = !zones.empty();
			for (const world::EntityHandle handle : zones.entries) {
				const world::Entity *zone = view.kernel->world.registry.get(handle);
				if (zone == nullptr) continue;
				const auto found = view.runtime->zone_states().find(handle.packed);
				if (found == view.runtime->zone_states().end()) continue;
				const auto *replica = cs.find(handle.packed);
				const auto team = replica != nullptr && replica->team_known ? replica->team
																			: zone->team;
				const auto &entry = found->second.entry;
				if (team == instructions.team && entry.value_target >= entry.value_limit)
					instructions.has_full_team_spawn = true;
			}
		}
	}
	v.instructions = world::build_deploy_instructions(instructions, gametext);
	v.statics_text = world::deploy_statics_text(statics_in, medic_key_label, gametext);
	char zone_key[32];
	std::snprintf(zone_key, sizeof zone_key, "STRWPNAME%03d", line.zone_index + 1);
	const auto text = [&gametext](const char *section, const char *key, const char *fallback) {
		return hud::game_text(gametext, section, key, fallback);
	};
	v.respawn_text = world::deploy_status_text(line,
			text("Overlays", "STROVER_PENALTYTIMER", "Respawn penalty"),
			text("WPNames", zone_key, "Spawn Point"));
	return v;
}

bool collect_lfp_zones(const RoleView &view, const world::SpawnZoneRegistry &zones,
		int local_team, std::vector<hud::HudLfpZone> &out) {
	out.clear();
	if (view.kernel == nullptr || view.runtime == nullptr ||
			!view.kernel->world.cached.local_player.valid())
		return false;
	const world::World &w = view.kernel->world;
	const world::Entity *local = w.registry.get(w.cached.local_player);
	if (local == nullptr) return false;
	const ClientRuntime *runtime = view.runtime;
	// The zone-timer entry as the marker reads it.
	const world::LfpZoneTimerLookup timer = [runtime](world::EntityHandle h,
													  world::LfpZoneTimer &t) {
		const auto it = runtime->zone_states().find(h.packed);
		if (it == runtime->zone_states().end() || !it->second.has_value) return false;
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
	// The transient minimap slot's flag byte for the zone; the 0x6B ring slots
	// land in the special bank here, so both are searched.
	const world::LfpCaptureFlagsLookup capture_flags = [runtime](world::EntityHandle h) -> uint8_t {
		const replication::ClientMinimapState &map = runtime->state().minimap;
		for (const auto &slot : map.transient)
			if (slot.active && slot.handle == h.packed) return slot.flags;
		for (const auto &slot : map.special)
			if (slot.active && slot.handle == h.packed) return slot.flags;
		return 0;
	};
	world::build_lfp_zones(w, zones, *local, local_team, timer, capture_flags, out);
	return true;
}

hud::HudMapGridOrigin hud_map_grid_origin(const RoleView &view) {
	hud::HudMapGridOrigin v;
	v.present = view.kernel != nullptr && view.kernel->world.tables.map_grid_origin_present;
	v.x_q16 = v.present ? view.kernel->world.tables.map_grid_origin_x : 0;
	v.y_q16 = v.present ? view.kernel->world.tables.map_grid_origin_y : 0;
	if (!v.present && view.runtime != nullptr)
		v.present = replication::client_minimap_grid_origin(view.runtime->state(), v.x_q16, v.y_q16);
	return v;
}

bool joiner_deploy_hold_ready(const RoleView &view) {
	if (!view.joiner || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	return runtime.awaiting_deploy_pick() && runtime.has_self_handle() &&
			runtime.last_error().empty() && !runtime.session_lost();
}

bool joiner_in_match_ready(const RoleView &view, bool auto_deploy) {
	if (!view.joiner || view.runtime == nullptr || view.kernel == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	return view.kernel->world.cached.local_player.valid() && runtime.in_match() &&
			(!auto_deploy || !runtime.deployment_pick_pending());
}

bool deploy_zone_rows(const RoleView &view, const world::SpawnZoneRegistry &zones,
		std::vector<world::DeployZoneRow> &out) {
	out.clear();
	if (view.kernel == nullptr || !view.joiner || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const world::World &w = view.kernel->world;
	const uint8_t team = runtime.assigned_team();
	const replication::ClientState &cs = runtime.state();
	const uint16_t self_handle = runtime.has_self_handle() ? runtime.self_handle() : 0xFFFFu;
	for (size_t i = 0; i < zones.entries.size(); ++i) {
		const world::Entity *e = w.registry.get(zones.entries[i]);
		if (e == nullptr || !e->has_item_def || !e->is_spawn_point) continue;
		uint8_t effective_team = e->team;
		int32_t effective_control = e->zone_control;
		int32_t effective_limit = 0x10000;
		const auto live = runtime.zone_states().find(e->handle.packed);
		if (live != runtime.zone_states().end()) {
			// The DEATH list reads the value entry's team and exact value >= limit
			// gate. 0x53 is the separate timed-capture window; retaining an old
			// window after a later 0x6F must not overwrite this ownership channel.
			// [orig: UI_UpdateDeathScreenContent @0x5536a0; §5.49/§5.61]
			if (live->second.has_value) {
				effective_team = live->second.value.mode;
				effective_control = live->second.value.value_s;
				effective_limit = live->second.value.limit_s;
			}
		}
		if (effective_team != team) continue;
		world::DeployZoneRow row;
		row.index = static_cast<int>(i);
		row.letter = static_cast<char>('A' + static_cast<int>(i));
		char name_key[32];
		std::snprintf(name_key, sizeof(name_key), "STRWPNAME%03d", static_cast<int>(i) + 1);
		row.name_key = name_key;
		// The first-loop gate: a zone whose live timer entry sits below its limit
		// is NOT listed; no entry (or level >= limit) lists it. There is no zone-
		// number term (an earlier port carried one) — the retail list walk tests
		// only the timer entry's level against its limit and then the def's
		// spawn-zone attribute (engine record: deploy_screen_feed.h cites the
		// UI_UpdateDeathScreenContent list loop).
		row.secured = !(effective_control < effective_limit);
		// The 0x6E wave group on this zone: its countdown (entity+548) and the
		// queued members, named through the roster the way retail reads the
		// member entity's Name (the player entity's name IS the roster name)
		// [orig: dword_A85BC4[idx] / unk_A85CC4 @0x553cd0..0x553d8b, see world/deploy_screen_feed.h].
		if (cs.spawn_waves.known) {
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				row.wave_countdown = static_cast<uint16_t>(g.wave_countdown);
				for (uint16_t member : g.members) {
					world::DeployOccupant o;
					o.handle = member;
					std::string name;
					const world::EntityHandle mh{ member };
					for (const replication::ClientRosterSlot &slot : cs.roster) {
						if (slot.bound && slot.entity_slot == mh.slot() && mh.pool() == 0) {
							name = slot.name;
							break;
						}
					}
					if (name.empty()) {
						if (const replication::ClientEntityState *row_state = cs.find(member))
							name = row_state->display_name;
					}
					o.name = name;
					o.self = member == self_handle;
					row.occupants.push_back(o);
				}
			}
		}
		out.push_back(row);
	}
	return true;
}

namespace {

// The contained half of one drawn entity's lighting from its first blink hit
// (entity+0x1D0): the containing pool-2 building's identity and blink volume
// section, and for a PERSON the building's ItemDef+0x218 daylight: only the
// person wave loads the aux [orig: Terrain_RenderSectorEntitiesBySide
// @0x5c7da8..0x5c7db6 (Pool_GetEntryUnchecked(2, hit >> 20)) and
// @0x5c7f7a..0x5c7f93 (entry+0x20 -> ItemDef+0x218 when non-null)].
EntityLighting contained_lighting(const world::World &w, uint32_t first_hit, bool person) {
	EntityLighting out;
	out.interior = true;
	out.interior_section = world::BlinkAccum::hit_section(first_hit);
	const world::Entity *parent = w.registry.get(world::EntityHandle::make(
			2, world::BlinkAccum::hit_pool_entity_index(first_hit)));
	if (parent != nullptr) {
		out.interior_bms = parent->bms_id;
		if (person && parent->has_item_def) out.light_transfer = parent->light_transfer;
	}
	return out;
}

} // namespace

void EntityLightingFeed::collect(const RoleView &view, const int32_t sun_step_q16[3],
		const std::unordered_set<int32_t> &culled_bms,
		const std::unordered_set<int32_t> &culled_wire, int64_t layout_revision,
		std::vector<EntityLightingChange> &out) {
	out.clear();
	if (view.kernel == nullptr) return;
	mission::MissionKernel &kernel = *view.kernel;
	world::World &w = kernel.world;
	if (layout_revision_seen != layout_revision) {
		last_by_wire.clear();
		layout_revision_seen = layout_revision;
	}

	// One registry entity's context. Contained entities route through the
	// interior light group; the outdoor factor stays 1.0 [orig: the blink-ref
	// branch @0x5c74b7]. The +0x1C0 slice gate [orig: @0x5c6808] lives inside
	// the ray walk: an entity with no proximity-candidate slice blocks nothing
	// and holds quality 4 — statics never ray, and only slice candidates
	// (structures overlapping the entity's inflated bubble) can shade it.
	const auto entity_lighting = [&](const world::Entity &e) {
		if (e.blink_hits[0] != 0)
			return contained_lighting(w, e.blink_hits[0], e.item_type == 3);
		EntityLighting outdoor;
		const int blocked = kernel.collision.sun_visibility_blocked_rays(w, e, sun_step_q16);
		outdoor.quality = static_cast<uint8_t>(4 - blocked);
		return outdoor;
	};

	// The local player is a spawned entity (bms_id 0, outside the placed-node
	// walk); its quality feeds the presenter seam only.
	const world::Entity *local = w.registry.get(w.cached.local_player);
	local_quality = local != nullptr ? entity_lighting(*local).quality : 4;

	w.registry.for_each([&](const world::Entity &e) {
		// The pool-2 decorations and foliage are entity-collected, so the
		// entity wave lights them too; only the Building-type defs draw in the
		// building pass. [orig: Terrain_RenderSectorEntities walks
		// g_SectorEntityList — which Terrain_CollectVisibleEntities_0 fills from
		// the non-building statics — and sets each one's context
		// (Terrain_SetupEffectForEntity @ 0x5c7bf1); world::building_def_row]
		if ((e.kind == world::EntityKind::Building && world::building_def_row(e)) ||
				e.kind == world::EntityKind::Marker)
			return;
		// Only authored placements have a placed node addressed by BMS id.
		// Runtime-spawned rows can also carry a nonzero bms_id (players use
		// their net id), but the wire walk owns their rendering.
		if (e.bms_id == 0 || e.spawn_origin == world::kSpawnOriginNone) return;
		if (e.handle == w.cached.local_player) return;
		// Retail only lights a drawn entity; a culled one keeps its last
		// context until it renders again (the stack slot is simply never pushed).
		if (culled_bms.count(e.bms_id) != 0) return;
		const EntityLighting lighting = entity_lighting(e);
		const auto it = last_by_bms.find(e.bms_id);
		const EntityLighting last = it != last_by_bms.end() ? it->second : EntityLighting{};
		if (lighting == last) return;
		last_by_bms[e.bms_id] = lighting;
		out.push_back(EntityLightingChange{ false, e.bms_id, 0, lighting });
	});

	// Every rendered role consumes ClientState. Placed rows above continue to
	// address their placed node by BMS id; only rows without authored identity
	// reach the wire walk and therefore need a wire-handle lighting update.
	// On a joiner, pool-0 H must NEVER be cast to a local EntityHandle (H=0 and
	// L=0 can coexist); streamed pool-1 twins are allowed only after the type
	// check below. Host/SP rows use their authoritative exact-handle entity.
	// Cadence: like the registry walk above, the casts run per display frame.
	// Retail casts inside the sector render walk for every drawn entity every
	// frame; only the candidate SLICE the walker iterates refreshes on the
	// 17-tick arena edge, which CollisionWorld::build_tick_tables already
	// mirrors for wire rows [orig: Terrain_RenderSectorEntities @0x5c7bf1 /
	// Terrain_RenderSectorEntitiesBySide @0x5c7f9a -> Terrain_SetupEffectForEntity
	// @0x5c74a0 -> Entity_ComputeSunVisibility @0x5c6800 per frame; the slice
	// gate g_ProxSliceRefreshCounter >= 0x10 @0x4c240f ->
	// Entity_BuildProximityListsFromPools @0x4c2418, see
	// docs/render/render-lighting-re.md]. Throttling the casts themselves to
	// that cadence would hold a moving vehicle's sun factor stale for up to 16
	// ticks; the per-handle cache below only suppresses unchanged emits.
	if (!view.joiner) {
		// The host presents its own pools (D-NET-140 closed): the wire-rendered
		// rows are the runtime-spawned pool-0 organics and pool-1 dynamics with
		// no authored identity; placed rows went through the walk above.
		for (int pool = 0; pool <= 1; ++pool) {
			w.registry.for_each_in_pool(pool, [&](const world::Entity &e) {
				const uint16_t handle = e.handle.packed;
				if (e.item_id == 0 || e.handle == w.cached.local_player ||
						e.spawn_origin != world::kSpawnOriginNone) {
					last_by_wire.erase(handle);
					return;
				}
				// A hidden row is not drawn, so retail does not push a new stack
				// value; preserve the last emitted context (see the wire loop).
				if ((e.flags & 0x01u) != 0) return;
				const EntityClass cls = replication::entity_class_of(e);
				const bool person_source = pool == 0 &&
						(cls == EntityClass::Player || cls == EntityClass::Infantry);
				const bool dynamic_source = pool == 1 &&
						kernel.wire_collision_shape_for_type(static_cast<uint16_t>(e.item_id))
								.pool1_candidate_source_eligible;
				// A drawn row without a candidate slice never rays (quality 4)
				// but still takes the interior route when contained.
				EntityLighting lighting;
				if (e.blink_hits[0] != 0) {
					lighting = contained_lighting(w, e.blink_hits[0], e.item_type == 3);
				} else if (person_source || dynamic_source) {
					lighting = entity_lighting(e);
				}
				const auto it = last_by_wire.find(handle);
				const EntityLighting last = it != last_by_wire.end() ? it->second : EntityLighting{};
				if (lighting == last) return;
				last_by_wire[handle] = lighting;
				out.push_back(EntityLightingChange{ true, 0, handle, lighting });
			});
		}
	} else if (view.runtime != nullptr) {
		const ClientRuntime &runtime = *view.runtime;
		for (const replication::ClientEntityState &es : runtime.state().entities) {
			const uint16_t handle = es.handle;
			if (handle == world::EntityHandle::kInvalid || es.type_id == 0 ||
					(runtime.has_self_handle() && handle == runtime.self_handle())) {
				last_by_wire.erase(handle);
				continue;
			}
			// Retail only lights a drawn entity; a culled one keeps its last
			// context until it renders again (see the registry walk above).
			if (culled_wire.count(static_cast<int32_t>(handle)) != 0) continue;
			// A hidden row is not drawn, so retail does not push a new stack
			// value. Preserve the last emitted context: if it moves while hidden,
			// the first visible frame must compare against that retained material
			// state and emit the restoration instead of assuming the default.
			if (es.state_flags_known && (es.state_flags & 0x01u) != 0) continue;

			const world::EntityHandle h{ handle };
			const world::Entity *joiner_twin = nullptr;
			if (h.pool() != 0) {
				const world::Entity *candidate = w.registry.get(h);
				if (candidate != nullptr && static_cast<uint16_t>(candidate->item_id) == es.type_id)
					joiner_twin = candidate;
			}
			// The wire walk defers authored rows to their placed node (or static
			// batch). Do not repeat the same native ray query and cache an update
			// for a wire node that deliberately does not exist.
			if (h == w.cached.local_player ||
					(joiner_twin != nullptr && joiner_twin->spawn_origin != world::kSpawnOriginNone)) {
				last_by_wire.erase(handle);
				continue;
			}

			const bool person_source = h.pool() == 0 &&
					(es.cls == EntityClass::Player || es.cls == EntityClass::Infantry);
			const world::ResolvedCollisionShape shape = kernel.wire_collision_shape_for_type(es.type_id);
			const bool dynamic_source = h.pool() == 1 && shape.pool1_candidate_source_eligible;

			// The client's own +0x1D0 for the row: a pool-1 twin carries it; a
			// decoded source without one runs the client-side blink walk at its
			// decoded position [orig: Entity_BuildProximityList @0x4b3dc0 from
			// the client entity updates, e.g. Entity_UpdatePool1Slot @0x4b8e25].
			uint32_t first_hit = 0;
			if (joiner_twin != nullptr) {
				first_hit = joiner_twin->blink_hits[0];
			} else {
				const int32_t pos[3] = { es.x, es.y, es.z };
				world::BlinkAccum blink;
				kernel.collision.query_wire_blink_boxes_at_point(w, handle, pos,
						person_source || shape.item_type == 1 || shape.item_type == 3, blink);
				first_hit = blink.hits[0];
			}
			EntityLighting lighting;
			if (first_hit != 0) {
				lighting = contained_lighting(w, first_hit, person_source || shape.item_type == 3);
			} else if (person_source || dynamic_source) {
				const int blocked = kernel.collision.wire_sun_visibility_blocked_rays(w, handle,
						world::FixedVec3{ es.x, es.y, es.z }, shape.bbox_center_q16, sun_step_q16);
				lighting.quality = static_cast<uint8_t>(4 - blocked);
			}

			const auto it = last_by_wire.find(handle);
			const EntityLighting last = it != last_by_wire.end() ? it->second : EntityLighting{};
			if (lighting == last) continue;
			last_by_wire[handle] = lighting;
			out.push_back(EntityLightingChange{ true, 0, handle, lighting });
		}
	}
}

} // namespace opennova::inmatch
