#include <runtime/replication/client_replica_pipeline.h>

#include "client_replica_body_arbitration.h"

#include <runtime/replication/entity_wire_bridge.h> // class_for_type_id (default resolver)
#include <runtime/terrain_query/height_field.h>        // remote-person terrain settle
#include <runtime/world/entity.h>              // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/infantry.h>            // IRootMotionSource + the anim flag/state tables
#include <runtime/world/world.h>               // exact mission PRNG seed
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/wire_handle.h>
#include <base/io/bam.h>                      // wrapped retail pitch chase

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <base/io/fixed.h>

namespace opennova::replication {


// ---- ClientReplicaPipeline -----------------------------------------------

ClientReplicaPipeline::ClientReplicaPipeline()
		: resolver_([](uint16_t tid) { return class_for_type_id(tid); }),
		  prng16_(world::World::kMissionPrng16Seed) {}

ClientReplicaPipeline::ClientReplicaPipeline(std::function<EntityClass(uint16_t)> resolver)
		: resolver_(std::move(resolver)),
		  prng16_(world::World::kMissionPrng16Seed) {}

void ClientReplicaPipeline::set_item_class_resolver(ItemClassResolver resolver) {
	item_resolver_ = std::move(resolver);
}

EntityClass ClientReplicaPipeline::classify(uint16_t type_id) const {
	// items.def first — the retail client's own dispatch source [orig: itemDef+356
	// @0x50f2e2]. It must outrank the 0x0D pool blanket: pool-1 holds no-callback
	// types too (an `ewep` emplacement), and sizing their header-only records as a
	// vehicle compact desyncs the whole frame after them.
	if (item_resolver_) {
		const ItemClassResolution resolution = item_resolver_(type_id);
		// A present Unknown is a catalogued unresolved/ambiguous definition. It
		// is terminal and intentionally reaches npwire's fail-closed width path.
		if (resolution.has_value()) return *resolution;
	}
	const auto it = learned_classes_.find(type_id);
	if (it != learned_classes_.end()) return it->second;
	return resolver_(type_id);
}

void ClientReplicaPipeline::apply(uint8_t tag, const std::vector<uint8_t> &body) {
	switch (tag) {
	case s2c::SESSION_CONFIG: { // field 3 = shared g_GameType
		SessionConfig config;
		if (decode_session_config(body.data(), body.size(), config))
			game_type_ = static_cast<uint32_t>(config.fields[3]);
		else
			++malformed_bodies_;
		break;
	}
	case s2c::FULL_PLAYER_INFO: { // extra = shared g_GameType
		FullPlayerInfo info;
		if (decode_full_player_info(body.data(), body.size(), info))
			game_type_ = info.extra;
		else
			++malformed_bodies_;
		break;
	}
	case s2c::PER_FRAME_UPDATE:
		apply_frame_update(body);
		break;
	case s2c::TEXT_COMMAND:
        apply_text_command(body);
        break;
	case s2c::WORLD_STATE_LOAD: {
		// The 0x0F's client-global fold modeled here: the deploy-map overlay is
		// zeroed, then armed from game_flags bit0 UNLESS the death screen is
		// already up. (The spawn pose / completion burst / waypoint legs live on
		// JoinerConnection; this reducer owns only the retained client globals.)
		// [orig: NapiNPClientMsg_0x00F — g_deploy_screen_active = 0 @0x42e2d8;
		//  `if (game_flags & 1) g_deploy_screen_active = !g_death_screen_active`
		//  @0x42e2f8]
		WorldStateLoad wsl;
		if (decode_world_state_load(body.data(), body.size(), wsl,
				game_type::is_waypoint_family(game_type_))) {
			// [orig: NapiNPClientMsg_0x00F @ 0x42E200, flag store @ 0x42E314] This flag only sets here.
			if (wsl.game_flags & 8u) state_.cease_fire = true;
			state_.deploy_overlay_active =
					(wsl.game_flags & 0x01u) != 0 && !state_.death_screen_active;
			// The trigger falling is what clears the open latch
			// [orig: the close-on-clear leg @0x5cac8e -> @0x54b954].
			if (!state_.deploy_overlay_active) state_.deploy_overlay_open_latch = false;
			state_.mark_changed();
		} else {
			++malformed_bodies_;
		}
		break;
	}
	case s2c::WEAPON_RELOAD: { // reload echo (same four-byte body as c2s::WEAPON_RELOAD_REQUEST)
		WeaponReload reload;
		size_t consumed = 0;
		if (decode_weapon_reload(body.data(), body.size(), reload, consumed) &&
		    consumed == body.size())
			pending_weapon_reloads_.push_back(reload);
		else
			++malformed_bodies_;
		break;
	}
	case s2c::TEAM_ASSIGN: {
		TeamAssign assign;
		size_t consumed = 0;
		if (decode_team_assign(body.data(), body.size(), assign, consumed))
			apply_team_assign(assign.entity_handle, assign.team);
		else
			++malformed_bodies_;
		break;
	}
	case s2c::EMPTY_SLOT_SWEEP: {
		DestroyEntityList destroyed;
		if (decode_destroy_entity_list(body.data(), body.size(), destroyed)) {
			for (uint16_t index : destroyed.pool0_indices)
				destroy_pool0_slot(index);
		} else {
			++malformed_bodies_;
		}
		break;
	}
	case s2c::ENTITY_DEATH: {
		// The host's per-death notify for every NON-PLAYER victim (the AI/item
		// leg of Entity_CheckAndProcessDeath) — a destructible's ONLY live death
		// channel beside 0x26; the load-stream 0x10/0x20 batches do not
		// re-stream after load. Retail resolves the pool row (slot < that
		// pool's capacity), zeroes Health, stamps the killer source, and runs
		// the class death callback with reason 4 — for a destructible item that
		// callback IS the local husk-swap/explosion chain the world twin runs
		// when this drains.
		// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — gates @0x42eba2/@0x42ebbb,
		//  Health = 0 @0x42ebd6, deathAnimStateId @0x42ebdf, cb(entity, 4, 0)
		//  @0x42ebf5; sender Entity_CheckAndProcessDeath @0x51b550 (msg 19)]
		EntityDeathRecord death;
		size_t consumed = 0;
		if (!decode_entity_death(body.data(), body.size(), death, consumed)) {
			++malformed_bodies_;
			break;
		}
		apply_entity_death(death.entity_handle, death.killer_source);
		break;
	}
	case s2c::DEATH_CAMERA_TARGET:
		apply_death_camera_target(body);
		break;
	case s2c::PLAYER_DOWNED_STATE:
		apply_player_downed_state(body);
		break;
	case s2c::KILL_SYNC: {
		// The SECOND client death route — the destructible deathCallback's own
		// authority resend rides this tag. Entity_KillBySlotId resolves the
		// slot the same way, zeroes Health, and runs the same cb(entity, 4,
		// flags); its not-already-dead gate is our world chain's husk gate.
		// [orig: NapiNPClientMsg_0x026 @0x42EC30 → Entity_KillBySlotId
		//  @0x42BCE0 — gates @0x42bcfc/@0x42bd15, Flags&2 gate @0x42bd2f,
		//  Health zero @0x42bd33, cb(entity, 4, flags) @0x42bd6a; the
		//  destructible resend Server_SendEntityStatePacket @0x509d70 via
		//  Entity_HandleDestructibleDeathEvent @0x440210]
		KillRecord kill;
		size_t consumed = 0;
		if (!decode_kill_record(body.data(), body.size(), kill, consumed)) {
			++malformed_bodies_;
			break;
		}
		apply_entity_death(kill.victim_slot,
				static_cast<int16_t>(kill.attacker));
		break;
	}
	case s2c::ENTITY_SPAWN_BATCH: // pool-0 organic spawn batch (§5.23)
		apply_organic_spawn(body);
		break;
	case s2c::POOL_SPAWN: // pool-1 entity spawn batch (§5.11)
		apply_pool_spawn(body);
		break;
	case s2c::STATIC_ENTITY_BATCH: // pool-2 (§5.9)
		apply_static_batch(body);
		break;
	case s2c::POOL3_SYNC: // pool-3 marker/waypoint sync batch (§5.12)
		apply_pool3_batch(body);
		break;
	case s2c::CAPTURE_ZONE_STATE:
		apply_capture_zone_overlay(body);
		break;
	case s2c::MINIMAP_OVERLAY:
		apply_minimap_overlay_batch(body);
		break;
	case s2c::END_ROUND_HEADER:
		apply_end_round_header(body);
		break;
	case s2c::END_ROUND_STATS: // §5.61 the chunked post-round stat board (0x56)
		apply_end_round_stats_chunk(body);
		break;
	case s2c::PLAYER_LIST: // §5.20 the Tab scoreboard (0x16)
		apply_player_list(body);
		break;
	case s2c::PLAYER_SYNC: // §5.21 the connection-slot roster (0x46)
		apply_player_sync(body);
		break;
	case s2c::GAME_EVENT: // §5.26 the kill/objective/medic feed lane (0x1E)
		apply_game_event(body);
		break;
	case s2c::CHAT_BROADCAST: // §5.52 the player-chat fan-out (0x14)
		apply_chat_broadcast(body);
		break;
	case s2c::ENTITY_ROUTED: // §5.36 sub-header + §5.15 guided body (0x44)
		apply_entity_routed(body);
		break;
	case s2c::DEPLOYED_ITEM: // live pool-1 placed-device spawn/update (§5.36)
		apply_deployed_item(body);
		break;
	case s2c::ENTITY_REMOVE: // live packed-handle retirement (0x12)
		apply_entity_remove(body);
		break;
	case s2c::OBJECTIVE_ENTITY_STATE: // flag/carryable pose + carry links (0x2F)
		apply_objective_entity_state(body);
		break;
	case s2c::SPAWN_WAVE_STATUS:
		apply_spawn_wave_status(body);
		break;
	case s2c::SCORE_DELTA_SOUND:
		apply_score_delta_sound(body);
		break;
	case s2c::SCRIPT_REMOTE_COMMAND: // the host VM's replicated WAC command
		apply_script_remote_command(body);
		break;
	default:
		// Game-start scalars and other non-entity tags this reducer
		// does not model.
		++unknown_tags_;
		break;
	}
}

std::vector<ClientRoundEvent> ClientReplicaPipeline::drain_round_events() {
	std::vector<ClientRoundEvent> out;
	out.swap(pending_round_events_);
	return out;
}

std::vector<ClientGameEvent> ClientReplicaPipeline::drain_game_events() {
	std::vector<ClientGameEvent> out;
	out.swap(pending_game_events_);
	return out;
}

std::vector<ClientChatLine> ClientReplicaPipeline::drain_chat_lines() {
	std::vector<ClientChatLine> out;
	out.swap(pending_chat_lines_);
	return out;
}

// [orig: GameMode_DispatchRemoteCommand @0x4F81E0 — the authority returns
//  before reading the body @0x4f8249; the operands decode by registry row and
//  the row's handler runs only when its flags carry 0x18 @0x4f8429]
void ClientReplicaPipeline::apply_script_remote_command(
		const std::vector<uint8_t> &body) {
	ScriptRemoteCommand command;
	size_t consumed = 0;
	if (!decode_script_remote_command(body.data(), body.size(), command, consumed)) {
		++malformed_bodies_;
		return;
	}
	if (authority_recipient_) return;
	pending_script_remote_commands_.push_back(std::move(command));
}

std::vector<ScriptRemoteCommand> ClientReplicaPipeline::drain_script_remote_commands() {
	std::vector<ScriptRemoteCommand> out;
	out.swap(pending_script_remote_commands_);
	return out;
}

std::vector<WeaponReload> ClientReplicaPipeline::drain_weapon_reloads() {
	std::vector<WeaponReload> out;
	out.swap(pending_weapon_reloads_);
	return out;
}

std::vector<EntityDeathRecord> ClientReplicaPipeline::drain_entity_deaths() {
	std::vector<EntityDeathRecord> out;
	out.swap(pending_entity_deaths_);
	return out;
}

void ClientReplicaPipeline::pump(ISessionTransport &channel) {
	Datagram dg;
	while (channel.client_recv(dg)) apply(dg.tag, dg.body);
}

namespace {
// The compact coarse heading the present rebuilds: yaw_byte = top 8 bits of the 32-bit
// engine BAM (present does `bam = yaw_byte << 24`). [simulation present.]
inline uint8_t yaw_byte_from_bam(int32_t bam) {
	return static_cast<uint8_t>(static_cast<uint32_t>(bam) >> 24);
}

inline int32_t chase_infantry_pitch(int32_t current, uint8_t target_byte) {
	const int32_t target = static_cast<int32_t>(
			static_cast<uint32_t>(target_byte) << 24);
	const int32_t delta = opennova::io::bam_sub(target, current);
	const int32_t step = opennova::io::bam_sar(
			opennova::io::bam_add(delta, 4), 3);
	return opennova::io::bam_add(current, step);
}
} // namespace

void ClientReplicaPipeline::apply_organic_spawn(const std::vector<uint8_t> &body) {
	OrganicSpawnBatch batch;
	if (!decode_organic_spawn_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: NapiNPClientMsg_0x0E family].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	for (const OrganicSpawnRecord &rec : batch.records) {
		if (!rec.has_body) continue;
		ClientEntityState *existing = state_.find(rec.slot_id);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		if (type_changed) state_.mark_topology_changed();
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		es.name = rec.entity_name;
		es.net_id = rec.net_id;
		es.spawn_tag = s2c::ENTITY_SPAWN_BATCH;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.orientation);
		es.heading_bam = rec.orientation;
		// Retail rows are born with the spawn orientation in EVERY heading
		// slot; seed the stage + promote target so the first compact's
		// unconditional +0x1A8 promote publishes the spawn orientation, not a
		// zero-initialized stage [orig: the spawn writes behind the @0x4C0320
		// promote].
		es.net_smooth_heading = rec.orientation;
		es.net_target_heading_bam = rec.orientation;
		es.heading_known = true;
		es.pitch_bam = 0; // organic spawn carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
		es.recoil_pitch = 0;
		es.team = rec.team;
		es.team_known = true;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

// The per-body-tick lean integrator, run once per client frame for every remote
// organic: the decay first, then the ramp from the latest wire bits — the same
// locally integrated model retail runs at both ends (only the bits replicate).
// The order is load-bearing: decay-then-ramp settles at ~±0x30000000 ≈ 67.5°,
// ramp-then-decay one ramp step short of it (~±0x2D000000).
// [orig: both legs of the single Entity_UpdateInfantryPlayerBody @0x4b40e0 pass —
//  decay lean -= (lean+8)>>4 @0x4b5c97, then the on-foot ramp @0x4b7dbf (bit 6 left
//  −0x3000000/tick) / @0x4b7dd6 (bit 7 right +0x3000000/tick)]
// The producer's ramp gates (alive/prone, and the seated ±0x1400000 variant) are not
// applied here: the decoded row carries no honest stance/seat state for them (D-INF-17).
void ClientReplicaPipeline::land_compact_pose(ClientEntityState &es, int32_t wx,
		int32_t wy, int32_t wz, bool has_heading, int32_t heading_bam,
		bool force_live_snap) {
	es.net_has_compact = true;
	if (!remote_motion_mode_ || force_live_snap || es.net_world_mover) {
		// Live snap: the host/SP roles (full-rate loopback; the authority never
		// interpolates, D-NET-89), the respawn edge [orig: live snap +
		// Entity_ResetToSpawnState @0x4C113C], and the vehicle dead-pose form.
		es.x = wx;
		es.y = wy;
		es.z = wz;
		if (has_heading) es.heading_bam = heading_bam;
		// Snap-mode players keep the live pitch mirrored from the wire byte so
		// presentation reads one field in both modes (the joiner's org2 chase
		// maintains it below).
		if (has_heading && es.cls == EntityClass::Player) {
			es.pitch_bam = static_cast<int32_t>(
					static_cast<uint32_t>(es.pitch_byte) << 24);
		}
		if (force_live_snap && remote_motion_mode_) {
			// Retail stages the target BEFORE the respawn branch snaps the live
			// pose [orig: LABEL_123 staging precedes the +0x24&2 snap @0x4C1109+]
			// — the staged target stays at the wire pose, so the next mover tick
			// sees a zero delta (deadband) instead of gliding anywhere.
			es.net_smooth_target[0] = wx;
			es.net_smooth_target[1] = wy;
			es.net_smooth_target[2] = wz;
			es.net_smooth_heading = has_heading ? heading_bam : es.heading_bam;
			es.net_target_heading_bam = es.net_smooth_heading;
			es.net_interp_steps = 0;
			es.net_interp_progress = 0;
		}
		return;
	}
	// STAGE-ONLY (net-re §5.38e): the compact read never writes the live pose of
	// an alive remote entity — it stages the target cluster and resets the
	// progress counter; tick_remote_motion moves the live pose.
	// [orig: case-2 @0x4C0FE4/EA/F0 + @0x4C0FD7/@0x4C0FF6 + @0x4C0FFC;
	//  infantry mode-2 incl. the targetHeading promote; vehicle mode-2 @0x4607D5+]
	es.net_smooth_target[0] = wx;
	es.net_smooth_target[1] = wy;
	es.net_smooth_target[2] = wz;
	if (has_heading) {
		if (es.cls == EntityClass::Infantry) {
			// org1: the PREVIOUS staged heading becomes the chase target — one
			// record behind the wire, UNCONDITIONALLY [orig: the +0x1A8
			// promote in @0x4C0320]. The spawn appliers seed the stage with
			// the spawn orientation (retail rows are born with it in every
			// heading slot), so a row's first compact promotes that — no
			// first-record special case exists in the witness.
			es.net_target_heading_bam = es.net_smooth_heading;
		}
		es.net_smooth_heading = heading_bam;
		if (es.cls == EntityClass::Player) {
			es.net_smooth_pitch = static_cast<int32_t>(
					static_cast<uint32_t>(es.pitch_byte) << 24);
		}
	}
	es.net_interp_progress = 0; // [orig: @0x4C0FFC — bucket +0x27E is NOT reset]
}

namespace {

// The org1 position-chase bucket [orig: Entity_UpdateInfantryAI ladder
// @0x4b9b0b-region — {3,4,5,8,16} at 0x2AAA/0x4000/0x5555/0x8000].
inline int16_t org1_bucket(int32_t dist) {
	if (dist < 0x2AAA) return 3;
	if (dist < 0x4000) return 4;
	if (dist < 0x5555) return 5;
	if (dist < 0x8000) return 8;
	return 16;
}

// The org2 position-chase bucket from the 2D horizontal distance — the ladder
// is verbatim from the binary INCLUDING the non-monotonic [0x6000,0x7000)->7
// step [orig: Entity_UpdateInfantryPlayerBody @0x4B44C4..0x4B4581].
inline int16_t org2_bucket(int32_t dist2d) {
	if (dist2d < 0x3000) return 6;
	if (dist2d < 0x4000) return 7;
	if (dist2d < 0x5000) return 8;
	if (dist2d < 0x6000) return 9;
	if (dist2d < 0x7000) return 7;
	if (dist2d < 0x8000) return 8;
	if (dist2d < 0xA000) return 10;
	if (dist2d < 0xC000) return 12;
	if (dist2d < 0xE000) return 14;
	if (dist2d < 0x10000) return 16;
	return 18;
}

constexpr double kRadPerBam = opennova::io::kRadiansPerBam;

// The caller-owned org gravity channel (the infantry.cpp world-motor twins)
// [orig: org2 vel_z -= 208 @0x4b7acf, clamp @0x4b7c77; org1 -= 416
//  @0x4bf7bf; terminal -32768].
constexpr int32_t kGravityStepPlayer = 208;
constexpr int32_t kGravityStep = 416;
constexpr int32_t kTerminalVelZ = -32768;

// The deck-ride (D-NET-196 replica tails): an org row follows its
// groundEntity's per-tick pose delta at mover top, before root motion and
// the settle — translation, the rotate-about-carrier, and the heading/roll
// adoption. The deltas read the CARRIER's live pose against its mover-entry
// savedLivePose stamp (Entity::saved_live_* — retail +0x80..+0x94), served
// together by the provider: the witnessed source pair, no rider-side copy.
// The rotate un-rotates the rider's carrier
// offset by the SAVED attitude with 2^-22-scaled NEGATED sines
// (dbl_7C57B0 = -4194304.0 — the inverse rotation) and re-rotates by the
// CURRENT attitude with positive sines, exactly the witnessed product
// order. [orig: org2 @0x4b52a0..0x4b5726 (z biased by capsule_bottom/2,
// restored @0x4b5649); org1 @0x4ba45d..0x4ba891; the standalone twin
// Entity_InterpolateFromParentDelta @0x4a8dc0]
void row_deck_ride(ClientEntityState &es,
                   const ClientReplicaPipeline::CarrierPoseProvider &carrier) {
	if (es.resolved_ground == 0xFFFF) return;
	ClientReplicaPipeline::CarrierPose cp;
	if (!carrier(es.resolved_ground, cp)) return;
	// The unmounted out-of-radius drop: 3D distance vs the carrier's bound
	// radius, saturated at 0x7FFF0000 before the int compare; beyond it the
	// ride AND the ground link drop [orig: @0x4b52a7..0x4b52ff — the
	// flt_7C19E0 = 2147418112.0 clamp, groundEntity = 0].
	{
		const double ddx = static_cast<double>(es.x - cp.pos[0]);
		const double ddy = static_cast<double>(es.y - cp.pos[1]);
		const double ddz = static_cast<double>(es.z - cp.pos[2]);
		double len = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
		if (len > 2147418112.0) len = 2147418112.0;
		if (static_cast<int32_t>(len) > cp.bound_radius) {
			es.resolved_ground = 0xFFFF;
			return;
		}
	}
	const int32_t dpx = cp.pos[0] - cp.saved_pos[0];
	const int32_t dpy = cp.pos[1] - cp.saved_pos[1];
	const int32_t dpz = cp.pos[2] - cp.saved_pos[2];
	const int32_t dyaw = io::bam_sub(cp.yaw, cp.saved_yaw);
	const int32_t dpitch = io::bam_sub(cp.pitch, cp.saved_pitch);
	const int32_t droll = io::bam_sub(cp.roll, cp.saved_roll);
	es.x += dpx;
	es.y += dpy;
	es.z += dpz;
	const int32_t rider_yaw_before = es.heading_bam;
	if (dyaw != 0 || dpitch != 0 || droll != 0) {
		auto q22c = [](int32_t bam) {
			return static_cast<int32_t>(
					std::cos(static_cast<double>(bam) * kRadPerBam) * io::kQ22One);
		};
		auto q22s = [](int32_t bam) {
			return static_cast<int32_t>(
					std::sin(static_cast<double>(bam) * kRadPerBam) * io::kQ22One);
		};
		auto q22s_neg = [](int32_t bam) {
			return static_cast<int32_t>(
					std::sin(static_cast<double>(bam) * kRadPerBam) * -io::kQ22One);
		};
		auto m22 = [](int32_t a, int32_t b) {
			return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
		};
		// The org2 z bias: rel_z is taken from the capsule mid, the halved
		// anim-frame bottom restored on writeback [orig: @0x4b53a1/0x4b5649];
		// the last advanced frame's bottom serves a transport row.
		const int32_t cb_half =
				es.rm_prev_bottom_live ? (es.rm_prev_bottom >> 1) : 0;
		// Retail's SHLs wrap in 32-bit registers; shift through unsigned so a
		// negative offset is not C++ UB.
		auto shl8 = [](int32_t v) {
			return static_cast<int32_t>(static_cast<uint32_t>(v) << 8);
		};
		const int32_t rel_x = shl8(es.x - cp.pos[0]) + 127;
		const int32_t rel_y = shl8(es.y - cp.pos[1]) + 127;
		const int32_t rel_z = shl8(es.z - cb_half - cp.pos[2]) + 127;
		// Un-rotate by the saved attitude (negated sines = the inverse)
		// [orig: @0x4b537b..0x4b5567].
		const int32_t cys = q22c(cp.saved_yaw), sys = q22s_neg(cp.saved_yaw);
		const int32_t cps = q22c(cp.saved_pitch), sps = q22s_neg(cp.saved_pitch);
		const int32_t crs = q22c(cp.saved_roll), srs = q22s_neg(cp.saved_roll);
		const int32_t rot_yaw_x = m22(rel_x, cys) - m22(rel_y, sys);
		const int32_t rot_yaw_y = m22(rel_x, sys) + m22(rel_y, cys);
		const int32_t rot_pitch_x = m22(rot_yaw_x, cps);
		const int32_t rot_pitch_cross = m22(rel_z, sps);
		const int32_t rot_pitch_z = m22(rot_yaw_x, sps) + m22(rel_z, cps);
		const int32_t rot_roll_x = m22(rot_yaw_y, crs) - m22(rot_pitch_z, srs);
		const int32_t rot_roll_z = m22(rot_yaw_y, srs) + m22(rot_pitch_z, crs);
		const int32_t unrot_xz = rot_pitch_x - rot_pitch_cross;
		// Re-rotate by the current attitude (positive sines), roll ->
		// pitch -> yaw [orig: @0x4b54ff..0x4b5653].
		const int32_t cyc = q22c(cp.yaw), syc = q22s(cp.yaw);
		const int32_t cpc = q22c(cp.pitch), spc = q22s(cp.pitch);
		const int32_t crc = q22c(cp.roll), src = q22s(cp.roll);
		const int32_t a = m22(rot_roll_x, src) + m22(rot_roll_z, crc);
		const int32_t b = m22(rot_roll_x, crc) - m22(rot_roll_z, src);
		const int32_t p = m22(unrot_xz, cpc) - m22(a, spc);
		const int32_t zp = m22(unrot_xz, spc) + m22(a, cpc);
		es.x = cp.pos[0] + ((m22(p, cyc) - m22(b, syc)) >> 8);
		es.y = cp.pos[1] + ((m22(p, syc) + m22(b, cyc)) >> 8);
		es.z = cp.pos[2] + cb_half + (zp >> 8);
	}
	// Heading/attitude adoption. Both motors add the carrier yaw delta to the
	// render heading and the body heading (org2 @0x4b5656..0x4b56c1, org1
	// @0x4ba842..0x4ba861; the mounted 0x1000-seat skip is seat-machinery a
	// transport row never reaches). org1 additionally drags its chase TARGET
	// (+0x1A8 @0x4ba867) and look Pitch (+0x14 @0x4ba88e); org2's bodyPitch
	// and torso/aim target adds have no row channels — named deferrals. Roll
	// adopts on both (+0x18 @0x4b5723/@0x4ba88b). The pitch/roll deltas are
	// rotated by the carrier-vs-rider relative yaw taken BEFORE the yaw add.
	if (dyaw != 0 || dpitch != 0 || droll != 0) {
		const int32_t rel_yaw = io::bam_sub(cp.yaw, rider_yaw_before);
		const double rr = static_cast<double>(rel_yaw) * kRadPerBam;
		const int32_t rc = static_cast<int32_t>(std::cos(rr) * io::kQ22One);
		const int32_t rs = static_cast<int32_t>(std::sin(rr) * io::kQ22One);
		auto m22 = [](int32_t a2, int32_t b2) {
			return static_cast<int32_t>((static_cast<int64_t>(a2) * b2) >> 22);
		};
		const int32_t pitch_d = m22(dpitch, rc) - m22(droll, rs);
		const int32_t roll_d = m22(dpitch, rs) + m22(droll, rc);
		es.heading_bam = io::bam_add(es.heading_bam, dyaw);
		es.rm_body_heading = io::bam_add(es.rm_body_heading, dyaw);
		es.roll_bam = io::bam_add(es.roll_bam, roll_d);
		if (es.cls == EntityClass::Infantry) {
			es.net_target_heading_bam =
					io::bam_add(es.net_target_heading_bam, dyaw);
			es.pitch_bam = io::bam_add(es.pitch_bam, pitch_d);
		}
	}
}

// The vehicle-family bucket [orig: Entity_UpdateWatercraftPhysics
// @0x48D480 (shared template) — {6,8,10,15,20,25,30}].
inline int16_t vehicle_bucket(int32_t dist) {
	if (dist < 0x2AAA) return 6;
	if (dist < 0x4000) return 8;
	if (dist < 0x5555) return 10;
	if (dist < 0x8000) return 15;
	if (dist < 0x10000) return 20;
	if (dist < 0x20000) return 25;
	return 30;
}

// The AIR-family bucket [orig: Entity_UpdateAircraftPhysics @0x490310
// interp — deadband 0x2AAA, {8,10,15,20,25,32}].
inline int16_t vehicle_air_bucket(int32_t dist) {
	if (dist < 0x4000) return 8;
	if (dist < 0x5555) return 10;
	if (dist < 0x8000) return 15;
	if (dist < 0x10000) return 20;
	if (dist < 0x20000) return 25;
	return 32;
}

// 3D / 2D distance of a 16.16 delta, clamped like retail's float->int path
// (flt_7C19E0 is the overflow clamp, not tuning).
inline int32_t dist_16_16(int64_t dx, int64_t dy, int64_t dz) {
	const double d = std::sqrt(double(dx) * double(dx) +
	                           double(dy) * double(dy) +
	                           double(dz) * double(dz));
	return d >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(d);
}

// Per-step delta with retail's signed half-add rounding: (d + N/2) / N via
// idiv truncation [orig: @0x4b9b2e / @0x4B459D / the family movers].
inline int32_t chase_step(int32_t d, int32_t n) {
	return io::bam_add(d, n >> 1) / n;
}

} // namespace

namespace {

// The org2 leg-chain chase applied to a decoded row: the LEGS chase the wire
// yaw and the body heading is their midpoint — the root delta rotates by the
// BODY, so a turning peer's feet lead its torso exactly as on the authority
// [orig: Entity_UpdateInfantryPlayerBody @0x4b4945..0x4b4ac1 — movement
// re-plant @0x4b49dd/@0x4b49e3; idle windows ((tick-32)&0x3F / tick&0x3F)
// @0x4b49ad..0x4b49e3; quarter-step clamp ±0x3000000 @0x4b49fb; twist
// ±0x30000000 vs the yaw @0x4b4a23; midpoint @0x4b4ab5. The authoritative
// sibling is infantry.cpp's player leg block — same constants, same shape.]
constexpr int32_t kRowLegChaseClamp = 0x3000000;
constexpr int32_t kRowLegTwistLimit = 0x30000000;
constexpr int32_t kRowLegReplantMin = 59652320;
constexpr int32_t kRowLegReplantSnap = 357913920;

inline int32_t row_abs_bam(int32_t v) { return io::bam_abs(v); }

// Disarm the row's root-motion channel: the next free-standing tick re-arms
// it fresh from the wire state (+ the live phase seed). Used by the mover
// freezes (dead/bit0/carried — retail applies the anim byte per record
// regardless of the mover skip [orig: @0x4c1153], so presentation must show
// the WIRE state while frozen), the respawn edge (the resume must not blend
// out of the pre-death primary), and type-change/handle-reuse edges.
void row_channel_disarm(ClientEntityState &es) {
	es.rm_state = -1;
	es.rm_prev_state = -1;
	es.rm_blend_weight = 1.0f;
	es.rm_blend_step = 0.0f;
	es.rm_prev_bottom_live = false;
	es.rm_leg_seeded = false;
}

void row_leg_chase(ClientEntityState &es, uint32_t key) {
	const int32_t yaw = es.heading_bam;
	if (!es.rm_leg_seeded) {
		es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = yaw;
		es.rm_leg_target[0] = es.rm_leg_target[1] = yaw;
		es.rm_body_heading = yaw;
		es.rm_leg_seeded = true;
	}
	if ((world::infantry_anim_flags(es.rm_state) & 0x1u) != 0) {
		es.rm_leg_target[1] = yaw;
		es.rm_leg_target[0] = yaw;
	} else {
		const int32_t dl = io::bam_sub(yaw, es.rm_leg_yaw[1]);
		if (row_abs_bam(dl) > kRowLegReplantMin &&
		    (row_abs_bam(dl) > kRowLegReplantSnap || ((key - 32) & 63u) == 0))
			es.rm_leg_target[1] = yaw;
		const int32_t dr = io::bam_sub(yaw, es.rm_leg_yaw[0]);
		if (row_abs_bam(dr) > kRowLegReplantMin &&
		    (row_abs_bam(dr) > kRowLegReplantSnap || (key & 63u) == 0))
			es.rm_leg_target[0] = yaw;
	}
	for (int leg = 0; leg < 2; ++leg) {
		const int32_t ldiff = io::bam_sub(es.rm_leg_target[leg], es.rm_leg_yaw[leg]);
		int32_t lstep = io::bam_sar(io::bam_add(ldiff, 2), 2);
		if (lstep > kRowLegChaseClamp) lstep = kRowLegChaseClamp;
		if (lstep < -kRowLegChaseClamp) lstep = -kRowLegChaseClamp;
		es.rm_leg_yaw[leg] = io::bam_add(es.rm_leg_yaw[leg], lstep);
		const int32_t twist = io::bam_sub(es.rm_leg_yaw[leg], yaw);
		if (twist > kRowLegTwistLimit)
			es.rm_leg_yaw[leg] = io::bam_add(yaw, kRowLegTwistLimit);
		else if (twist < -kRowLegTwistLimit)
			es.rm_leg_yaw[leg] = io::bam_sub(yaw, kRowLegTwistLimit);
	}
	es.rm_body_heading = io::bam_add(
			es.rm_leg_yaw[1],
			io::bam_sar(io::bam_sub(es.rm_leg_yaw[0], es.rm_leg_yaw[1]), 1));
}

// The row's AnimMap primary channel + root integration — retail's remote body
// runs the SAME machinery as the authority [orig: Entity_UpdateInfantryPlayerBody
// calls AnimMap_UpdateDualChannels @0x4B41DF; AnimChannel_InitFromParams blend
// init 0.1 / (1/15 on flag 0x400) @0x410640; Q22 rotation
// @0x4B41F0..0x4B4255; additive integration LAST @0x4B7CB4..0x4B7CEF;
// Entity_UpdateInfantryAI twin @0x4BF684..0x4BF6A2]. The kJumpLoop forward override is
// a retail ADM dump witness (root row 4756), not an IDA code claim. The wire
// state byte drives the channel; the player compact's phase byte seeds a fresh
// transition [orig: NetPacket_SerializePlayerState entity+0x377 store
// @0x4C11A6; AnimMap_UpdateEntity one-shot clear @0x40B7E4]. The
// airborne/drowning/ladder overrides ride the row's rm_entity_flags mirror of
// entity+0x24 — latched locally by the resolve and the edge/water channels
// below, never wire-carried, exactly retail's remote rows (world-wac-ai-re.md
// §29). Caller-owned gravity/vertical velocity is rm_vel_z
// [orig: Entity_UpdateInfantryPlayerBody vertical add @0x4B7CE0..0x4B7CEF,
// then resolver call @0x4B7CF4].
// The per-motor water/float channel, at the mover tail after the settle
// [orig: org2 @0x4b8020..0x4b8373; org1 @0x4bfae2..0x4bfca2]. Entry has the
// asymmetric hysteresis (submerge at head-under z + 0xA000 < water, leave at
// z >= water) and the CL/platform 0x100000 exemption; the splash/overlay
// edges are FX deferrals. The float latch is `(Flags & ~0x2000) | 0x8000` —
// swimming overrides airborne — and the not-submerged exit clears the float
// AND dive bits (0x208000).
void row_water_channel(ClientEntityState &es, int32_t z_post_integrate,
                       int32_t capsule_bottom, int32_t capsule_top,
                       int32_t water_z, bool has_water, uint32_t tick) {
	if (!has_water) {
		// No mission water plane (the EnvState 0 sentinel): the channel is
		// off, and any stale float/dive bits clear so the gravity gate can
		// never wedge on them.
		es.rm_entity_flags &= ~0x208000u;
		return;
	}
	const bool afloat = (es.rm_entity_flags & 0x8000u) != 0;
	const int32_t probe = io::bam_add(es.z, afloat ? 0 : 0xA000);
	if (probe >= water_z || (es.rm_entity_flags & 0x100000u) != 0) {
		es.rm_entity_flags &= ~0x208000u; // [orig: @0x4b8373 / @0x4bfc5c]
		return;
	}
	const int32_t cb_neg = capsule_bottom > 0 ? 0 : capsule_bottom;
	// The eye vertical (+0x74): a remote row derives it per tick from the anim
	// capsule — min(top - bottom, 0xD000), floored at 0x2000 (the lean tilt is
	// the local-lean channel's term, zero for an unleaning row)
	// [orig: the non-local arm @0x4b6984..0x4b6991; the floor @0x4b68e7].
	int32_t eye_h = capsule_top - capsule_bottom;
	if (eye_h > 0xD000) eye_h = 0xD000;
	if (eye_h < 0x2000) eye_h = 0x2000;
	if (es.cls == EntityClass::Player) {
		// The org2 buoyant-rise form, REMOTE arm: flat base -0x4C9 (the
		// surface bob AND the capsule-bottom term are local-player-only in
		// org2 [orig: @0x4b8063..0x4b8095 `-1225 - bob + v345` vs the else-arm
		// @0x4b80a5 `v347 = -1225`]) and no look-pitch dive term (gated
		// local-or-authority [orig: @0x4b80aa..0x4b8102]). cb_neg stays the
		// org1 target line's term below.
		const int32_t base = -0x4C9;
		const int32_t base_q = base >> 4;
		es.z = io::bam_add(es.z, (base_q < 0 ? -base_q : base_q) + 0x70);
		// The velocity-triplet drag [orig: @0x4b8124..0x4b8163].
		es.rm_vel_xy[0] -= (es.rm_vel_xy[0] + 16) >> 5;
		es.rm_vel_xy[1] -= (es.rm_vel_xy[1] + 16) >> 5;
		es.rm_vel_z -= (es.rm_vel_z + 16) >> 5;
		// Surface line: water + base/2 - eyeHeight/2, with the capsule-derived
		// eye above [orig: line @0x4b8146..0x4b8169].
		const int32_t surf = water_z + (base >> 1) - (eye_h >> 1);
		if (es.z >= surf) {
			es.rm_entity_flags &= ~0x200000u; // surfaced [orig: @0x4b8176]
			es.z = surf;
		} else if (es.z < surf - 0x2000 &&
		           (es.rm_entity_flags & 0x200000u) == 0) {
			es.rm_entity_flags |= 0x200000u; // the dive bit [orig: @0x4b81ef]
		}
	} else {
		// The org1 snap form: the float target quarter-chased from the
		// post-integrate z — gravity's and the resolver's z contributions
		// are DISCARDED while afloat (the tail rewrites z from the saved
		// pre-gravity value). The bob wave applies to every row.
		// [orig: target @0x4bfb2a..0x4bfb84 (sin(((x+y)>>12 + 4*tick)/256 *
		//  3.1) * 1224); the quarter-step tail @0x4bfc65..0x4bfc86]
		const int32_t wave_arg =
				((es.x + es.y) >> 12) + static_cast<int32_t>(tick) * 4;
		const int32_t bob = static_cast<int32_t>(
				std::sin(static_cast<double>(wave_arg) * 0.00390625 * 3.1) *
				1224.0);
		const int32_t target =
				io::bam_add(water_z, bob - (eye_h >> 1) - 0x4C9 + cb_neg);
		es.z = io::bam_add(z_post_integrate,
		                   (io::bam_sub(target, z_post_integrate) + 2) >> 2);
	}
	// The float latch (the splash/overlay edge is an FX deferral)
	// [orig: @0x4b8363 / @0x4bfc48].
	es.rm_entity_flags = (es.rm_entity_flags & ~0x2000u) | 0x8000u;
}

void row_root_motion_tick(ClientEntityState &es, world::IRootMotionSource &src,
                          const terrain::TerrainHeightField *terrain,
                          uint32_t key, bool is_self,
                          const ClientReplicaPipeline::ReplicaContactResolver *resolver,
                          const ClientReplicaPipeline::ReplicaBoundRadiusResolver *bound_resolver,
                          const ClientReplicaPipeline::ReplicaPeerSphere *peers,
                          int32_t peer_count, uint32_t tick, int32_t water_z,
                          bool has_water) {
	if (es.rm_adm_id < 0) return;
	// Channel state machine (the begin_body_transition mirror). The channel
	// chases the ARBITRATED current (+0x2BC, written per record by the fold's
	// @0x4c1153 apply — D-NET-209), never the raw coalesced wire byte; the
	// phase seed is ONE-SHOT per direct commit [orig: AnimMap_UpdateEntity
	// zeroes entity+0x377 after use @0x40B7E4]; the bottom-history slot
	// resets on EVERY update of climbs 32..35 and grenade deaths 176..179,
	// before the same-state fast path [orig: @0x40B607..0x40B645].
	int target_state = es.net_anim_current >= 0
			? static_cast<int>(es.net_anim_current)
			: static_cast<int>(es.anim_state_id);
	const bool bottom_reset_state =
			(target_state >= world::anim_state::kClimbIdle &&
			 target_state <= world::anim_state::kClimbIdle + 3) ||
			(target_state >= world::anim_state::kDeathGrenadeBase &&
			 target_state <= world::anim_state::kDeathGrenadeBase + 3);
	// The gait->stance transition insert [orig: AnimMap_UpdateEntity
	// @0x40b662..0x40b737]: with no deferred armed, a forward gait
	// committing to the crouch/prone walk first plays the run2crouch-family
	// clip and defers the real target — gated on the adm actually carrying
	// the transition clip (retail: table entry != entry 0; here: the state
	// resolves a track).
	if (es.net_anim_pending == 0 && es.rm_state >= 0 &&
			es.net_anim_current >= 0 && target_state != es.rm_state) {
		const int trans =
				world::gait_stance_transition_clip(es.rm_state, target_state);
		if (trans >= 0 && src.clip_length_ticks(es.rm_adm_id, trans, 0) >= 0) {
			es.net_anim_pending = static_cast<int16_t>(target_state);
			es.net_anim_pending_boundary = -1;
			es.net_anim_current = static_cast<int16_t>(trans);
			target_state = trans;
		}
	}
	if (es.rm_state < 0) {
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_prev_state = static_cast<int16_t>(target_state);
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		es.rm_prev_phase = es.rm_phase;
		es.rm_blend_weight = 1.0f;
		es.rm_blend_step = 0.0f;
		es.rm_prev_bottom_live = false;
	} else if (target_state != es.rm_state) {
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_prev_state = es.rm_state;
			es.rm_prev_phase = es.rm_phase;
		}
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		es.rm_blend_weight = 0.0f;
		es.rm_blend_step =
				(world::infantry_anim_flags(target_state) & 0x400u) != 0
						? (1.0f / 15.0f)
						: 0.1f;
	}
	// The clip-end deferred promotion [orig: the deferral arms the channel's
	// end-notify each tick (@0x40b7db/@0x40b7ad), AnimChannel_AdvancePlayback
	// latches it at the next loop wrap / one-shot end (@0x40b1ae/@0x40b18f),
	// and AnimMap promotes on the latched flag (@0x40b795/@0x40b7c3) — the
	// promoted retarget lands on the NEXT tick, as here]. The boundary is
	// armed lazily in the growing-phase convention; a queue behind an
	// already-finished one-shot (or a track-less state) promotes immediately —
	// the shipped hold-wedge safety, recorded inside D-NET-209.
	if (es.net_anim_pending != 0) {
		if (es.net_anim_pending_boundary < 0) {
			const int32_t len = src.clip_length_ticks(es.rm_adm_id, es.rm_state, 0);
			if (len <= 0) {
				es.net_anim_pending_boundary = es.rm_phase;
			} else if (src.clip_loops(es.rm_adm_id, es.rm_state)) {
				es.net_anim_pending_boundary = ((es.rm_phase / len) + 1) * len;
			} else {
				es.net_anim_pending_boundary = len;
			}
		}
		if (es.rm_phase >= es.net_anim_pending_boundary) {
			es.net_anim_current = es.net_anim_pending;
			es.net_anim_pending = 0;
			es.net_anim_pending_boundary = -1;
			// The promoted request starts at frame zero on its retarget —
			// retail zeroes the +0x377 seed every tick [orig: @0x40b7e4].
			es.net_anim_ratio_live = false;
		}
	}
	if (bottom_reset_state) es.rm_prev_bottom_live = false;
	// The legs keep chasing whatever the clip coverage is — the body heading
	// is presentation state, not clip state (a clipless wire state must not
	// freeze the torso mid-twist).
	if (es.cls == EntityClass::Player) row_leg_chase(es, key);
	world::RootMotionFrame frame;
	bool have = false;
	if (es.rm_blend_weight >= 1.0f) {
		int32_t phase = es.rm_phase;
		have = src.advance(es.rm_adm_id, es.rm_state, phase, frame);
		es.rm_phase = phase;
	} else {
		es.rm_blend_weight += es.rm_blend_step;
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_blend_weight = 1.0f;
			es.rm_blend_step = 0.0f;
		}
		int32_t pphase = es.rm_prev_phase, tphase = es.rm_phase;
		have = src.advance_blended(es.rm_adm_id, es.rm_prev_state, pphase,
		                           es.rm_state, tphase, es.rm_blend_weight,
		                           frame);
		es.rm_prev_phase = pphase;
		es.rm_phase = tphase;
	}
	if (!have) return;
	int32_t fwd = frame.dx, lat = frame.dy;
	if (es.rm_state == world::anim_state::kJumpLoop)
		fwd = 1024; // [data: retail ADM dump root row 4756]
	// The witnessed vertical: the capsule-bottom history delta replaces the
	// raw track dz while the slot is live [orig: AnimMap_UpdateEntity reads,
	// subtracts, and rewrites anim_slot[19] @0x40B88E..0x40B8A0].
	int32_t dz_eff = frame.dz;
	if (es.rm_prev_bottom_live)
		dz_eff = frame.capsule_bottom - es.rm_prev_bottom;
	es.rm_prev_bottom = frame.capsule_bottom;
	es.rm_prev_bottom_live = true;
	// Rotation heading: org2 = the leg-chased body heading loaded from
	// entity+0x8C [orig: Entity_UpdateInfantryPlayerBody @0x4B41E4, Q22 rotate
	// @0x4B41F0..0x4B4255]; org1 = the row's chased heading (retail pins org1
	// body == render heading). This tick's
	// freshly-chased body heading is used (retail consumes the same-tick
	// value — the leg chase runs earlier in the same body pass).
	int32_t move_heading = es.heading_bam;
	if (es.cls == EntityClass::Player) move_heading = es.rm_body_heading;
	else es.rm_body_heading = move_heading;
	// The own player's row is locally predicted world-side; its chase is the
	// 48/512 soft reconciliation only — no root add (risk-listed; retail's
	// local player integrates in its OWN motor, not the remote path). The
	// velocity term retail adds alongside the root (Position += root + vel)
	// is a named, caller-side deferral because rows carry no velocity state
	// [orig: Entity_UpdateInfantryPlayerBody root+velocity stores
	// @0x4B7CBF..0x4B7CEF, before resolver call @0x4B7CF4].
	if (is_self) return;
	const double rad = static_cast<double>(move_heading) *
	                   io::kRadiansPerBam;
	const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
	const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
	int32_t wx =
			static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
			static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
	int32_t wy =
			static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
			static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
	// The planar velocity maintenance [orig: org2 @0x4b78a8..0x4b79dc; org1
	// @0x4bf5cb..0x4bf61f]. org2 splits on the airborne bit: in air, the
	// optional MoveOrder-bit3 air-steer nudge (angle = look-yaw hi16 ·
	// 2π/65536 + dirpad · π/4, force ftol(cos/sin · −64.0), the parachute
	// straight-fall double-apply @0x4b7915..0x4b793d), the 63/64 damp, and
	// the ROOT PAIR ZEROED — airborne movement is momentum-owned
	// [orig: @0x4b7971..0x4b7975]; grounded (and org1 on every path): the
	// (7·v + 4) >> 3 decay with the |v| <= 8 snap to zero.
	if (es.cls == EntityClass::Player &&
	    (es.rm_entity_flags & 0x2000u) != 0) {
		if ((es.move_input & 0x08u) != 0) {
			const int32_t dirpad = static_cast<int32_t>(es.move_input & 0x07u);
			const double ang =
					static_cast<double>(
							static_cast<int16_t>(es.heading_bam >> 16)) *
							9.587371826171875e-05 + // [orig: dbl_7C9BC0]
					static_cast<double>(dirpad) * 0.7853975; // [orig: dbl_7C9BB0]
			const int32_t nx =
					static_cast<int32_t>(std::cos(ang) * -64.0); // flt_7C9BD8
			const int32_t ny = static_cast<int32_t>(std::sin(ang) * -64.0);
			es.rm_vel_xy[0] -= nx;
			es.rm_vel_xy[1] -= ny;
			if ((es.rm_entity_flags & 0x20u) != 0 && es.rm_vel_z <= -14336 &&
			    dirpad == 0) {
				es.rm_vel_xy[0] -= nx;
				es.rm_vel_xy[1] -= ny;
			}
		}
		es.rm_vel_xy[0] = static_cast<int32_t>(
				(static_cast<int64_t>(es.rm_vel_xy[0]) * 63) >> 6);
		es.rm_vel_xy[1] = static_cast<int32_t>(
				(static_cast<int64_t>(es.rm_vel_xy[1]) * 63) >> 6);
		wx = 0;
		wy = 0;
	} else {
		auto ground_decay = [](int32_t v) {
			v = static_cast<int32_t>((static_cast<int64_t>(v) * 7 + 4) >> 3);
			return (v >= -8 && v <= 8) ? 0 : v;
		};
		es.rm_vel_xy[0] = ground_decay(es.rm_vel_xy[0]);
		es.rm_vel_xy[1] = ground_decay(es.rm_vel_xy[1]);
	}

	// Root suppression flag channels: the float bit zeroes the vertical
	// channel, CL/ladder contact the horizontal pair. BOTH motors store true
	// zeros — the earlier "org2 writes the literal 1" reading mistook the
	// 0x8000 TEST-MASK load (`mov ebp, 8000h @0x4b7979`) for the stored
	// operand; the stores use the xor-zeroed scratch registers.
	// [orig: org2 stores edi, `xor edi, edi` @0x4b797e/@0x4b79b7,
	//  stores @0x4b7ab5/@0x4b7ac0-0x4b7ac4; org1 stores ebx,
	//  `xor ebx, ebx` @0x4bf600, stores @0x4bf671/@0x4bf67c-0x4bf680]
	if ((es.rm_entity_flags & 0x8000u) != 0) dz_eff = 0;
	if ((es.rm_entity_flags & 0x100000u) != 0) {
		wx = 0;
		wy = 0;
	}
	// Integrate: position takes momentum + root together [orig: org2
	// @0x4b7cbf..0x4b7cd2; org1 @0x4bf684..0x4bf6a2].
	es.x += es.rm_vel_xy[0] + wx;
	es.y += es.rm_vel_xy[1] + wy;
	es.z += dz_eff;
	// The org1 water tail re-bases from this value (gravity + resolver z are
	// discarded while afloat) [orig: the pre-gravity save @0x4bf6ba].
	const int32_t z_post_integrate = es.z;

	// The settle. With an embedder-provided contact resolver, this is the FULL
	// movement collision resolver at the witnessed caller order — the
	// caller-owned vertical velocity integrates first (org2 `vel -= 208;
	// pos += vel`, org1 `vel -= 416; pos += 2*vel`, terminal -32768), then the
	// resolver runs candidate-model contacts, push-out, person + replica-peer
	// repulsion, and the ground probe THROUGH candidate models; a non-positive
	// clearance lifts the row and zeroes the vertical velocity (the landing),
	// and the probe's hit lands in resolved_ground (retail's groundEntity).
	// [orig: vertical add @0x4B7CE0..0x4B7CEF then the resolver call
	// @0x4B7CF4 and lift @0x4B7CFE..0x4B7D0A (org2); @0x4BF7B8..0x4BF7FA
	// (org1); Entity_MovementCollisionResolver @0x4B2BD0; landing vel zero in
	// the shared tail]. The row's rm_entity_flags word rides the query both
	// ways — the resolver's latch sites and the caller's edge/water channels
	// share it (world-wac-ai-re.md §29). Without a resolver, the bounded
	// terrain-column subset below stands.
	if (resolver != nullptr && *resolver) {
		// Gravity skips while on a ladder/platform or afloat (the 0x108000
		// gate); the position add itself is unconditional — the witnessed
		// one-store folds vel into the root dz [orig: org2 gate @0x4b7ac8,
		// store @0x4b7cef; org1 gate @0x4bf7b8].
		if ((es.rm_entity_flags & 0x108000u) == 0) {
			es.rm_vel_z -= es.cls == EntityClass::Player ? kGravityStepPlayer
			                                             : kGravityStep;
			if (es.rm_vel_z < kTerminalVelZ) es.rm_vel_z = kTerminalVelZ;
		}
		es.z = io::bam_add(es.z, es.cls == EntityClass::Player
				? es.rm_vel_z : 2 * es.rm_vel_z);
		ClientReplicaPipeline::ReplicaContactQuery q;
		q.row_handle = es.handle;
		q.type_id = es.type_id;
		q.is_player_class = es.cls == EntityClass::Player;
		q.pos[0] = es.x;
		q.pos[1] = es.y;
		q.pos[2] = es.z;
		q.vel_xy[0] = es.rm_vel_xy[0] + wx; // the actual planar step — the
		q.vel_xy[1] = es.rm_vel_xy[1] + wy; // resolver's moving discriminant
		q.vel_z = es.rm_vel_z;
		q.capsule_bottom = frame.capsule_bottom;
		q.capsule_top = frame.capsule_top;
		q.source_bound_radius_q16 = bound_resolver != nullptr && *bound_resolver
				? (*bound_resolver)(es.type_id)
				: 0;
		q.anim_state_id = es.anim_state_id;
		q.anim_state_flags = world::infantry_anim_flags(es.anim_state_id);
		q.tick = tick;
		q.peers = peers;
		q.peer_count = peer_count;
		q.entity_flags = es.rm_entity_flags;
		const int32_t clearance = (*resolver)(q);
		es.x = q.pos[0];
		es.y = q.pos[1];
		es.z = q.pos[2];
		es.rm_vel_z = q.vel_z; // the idle skip band reverts + zeroes it
		es.rm_entity_flags = q.entity_flags;
		es.resolved_ground = q.out_ground;
		if (clearance <= 0) {
			es.z = io::bam_sub(es.z, clearance);
			es.rm_vel_z = 0;
			// Landing clears the airborne/swim bit (the landing sound is an
			// FX deferral) [orig: org2 @0x4b7f7c..0x4b7fa1; org1 landing
			// tail @0x4bf89f].
			es.rm_entity_flags &= ~0x2000u;
		} else if (clearance > 0xF000) {
			// The ledge/airborne edge: gate on !(Flags & 0x10A002) for the
			// player body (dead suppresses the whole edge) and 0x10A000 for
			// org1; carried force-clears and the airborne bit sets. The org2
			// 3/4 momentum carry and the local anim-31/47 stamps are named
			// deferrals — rows carry no slide velocity and the wire state
			// byte owns the channel. [orig: org2 @0x4b7e17..0x4b7e73; org1
			// @0x4bf8ae..0x4bf901]
			const uint32_t edge_mask =
					es.cls == EntityClass::Player ? 0x10A002u : 0x10A000u;
			if ((es.rm_entity_flags & edge_mask) == 0) {
				es.rm_entity_flags =
						(es.rm_entity_flags & ~0x40u) | 0x2000u;
				if (es.cls == EntityClass::Player) {
					// The org2 3/4 momentum carry into the velocity pair and
					// the STRAIGHT anim stamp (31, 47 while parachuting) —
					// the wire overwrites at the next record exactly as
					// retail's pending does [orig: carry @0x4b7e43..0x4b7e6d;
					// stamp @0x4b7e3f..0x4b7e61]. org1 keeps its clip on a
					// plain fall (the 47->31 ladder is parachute-gated
					// @0x4bf8d8).
					es.rm_vel_xy[0] += static_cast<int32_t>(
							(static_cast<int64_t>(wx) * 3) >> 2);
					es.rm_vel_xy[1] += static_cast<int32_t>(
							(static_cast<int64_t>(wy) * 3) >> 2);
					es.anim_state_id =
							(es.rm_entity_flags & 0x20u) != 0 ? 47 : 31;
				}
			}
		}
		row_water_channel(es, z_post_integrate, frame.capsule_bottom,
		                  frame.capsule_top, water_z, has_water, tick);
		return;
	}

	// Retail quantizes the final origin upward to the 0x1800 grid and probes
	// exactly 0x20000 downward. Terrain is accepted only inside that segment;
	// otherwise the segment end is the resolver's bounded fallback. Then only
	// non-positive signed foot clearance lifts the row. This ports the outdoor
	// terrain-column subset until decoded rows have candidate slices and the
	// high indoors flag required by the full model/contact resolver.
	// [orig: Entity_UpdateInfantryPlayerBody call @0x4B7CF4 and lift
	// @0x4B7CFE..0x4B7D0A; Entity_UpdateInfantryAI caller @0x4BF7FA;
	// Entity_MovementCollisionResolver probe/return @0x4B3D6E..0x4B3DA9;
	// raycast_entity_collision terrain window @0x413785..0x4137CB, reached by
	// Entity_RaycastGroundHeightAndObject @0x414320]
	if (terrain != nullptr && terrain->valid()) {
		const float world_x = static_cast<float>(es.x) / 65536.0f;
		const float world_z = -static_cast<float>(es.y) / 65536.0f;
		const int32_t terrain_ground = static_cast<int32_t>(
				terrain::height_field_height_world_bilinear(
						*terrain, world_x, world_z) *
				65536.0f);
		const int32_t probe_start = static_cast<int32_t>(
				(static_cast<uint32_t>(es.z) + 0x17FFu) & ~0x17FFu);
		int32_t resolved_ground = static_cast<int32_t>(
				static_cast<uint32_t>(probe_start) - 0x20000u);
		if (terrain_ground > resolved_ground &&
		    terrain_ground <= probe_start)
			resolved_ground = terrain_ground;
		// Retail's SUBs wrap in 32-bit registers. Route both differences through
		// the defined modular helper so an extreme fixed-point seam is not C++ UB.
		const int32_t foot_clearance = io::bam_sub(
				io::bam_sub(es.z, frame.capsule_bottom), resolved_ground);
		if (foot_clearance <= 0) es.z = io::bam_sub(es.z, foot_clearance);
	}
	// The bounded subset still runs the water channel — the float latch and
	// the per-motor surface hold are mover-tail behavior, not resolver
	// behavior (a resolver-less embedder with env water keeps swimmers at
	// the surface between records).
	row_water_channel(es, z_post_integrate, frame.capsule_bottom,
	                  frame.capsule_top, water_z, has_water, tick);
}

} // namespace

// The chase tail the org2 and org1 legs share: the position step while the
// bucket runs, then the 512-progress cap with the starved idle force — a
// movement state parked past the progress cap walks its root motion forever,
// so retail reads AND writes the arbitration current (+0x2BC)
// [orig: @0x4b464f/@0x4b465f, g_animStateFlagsTable bit0 gate; the org1
//  twin is §5.38a cap 512 -> idle 43, the same shape].
static void row_chase_step_and_cap(ClientEntityState &es, int16_t progress) {
	if (progress < es.net_interp_steps) {
		es.x += es.net_smooth_target[0];
		es.y += es.net_smooth_target[1];
		es.z += es.net_smooth_target[2];
	}
	if (progress < 512) {
		es.net_interp_progress = progress + 1;
	} else if ((world::infantry_anim_flags(es.net_anim_current >= 0
						   ? es.net_anim_current
						   : es.anim_state_id) &
				   0x1u) != 0u) {
		es.net_anim_current = world::anim_state::kIdle;
		es.anim_state_id = world::anim_state::kIdle;
	}
}

void ClientReplicaPipeline::tick_remote_motion(uint16_t self_handle) {
	if (!remote_motion_mode_) return;
	const uint32_t rm_key = ++rm_tick_counter_;
	// The replica-peer sphere table for this tick's contact resolves — every
	// live organic replica row, one snapshot per tick (replica rows are
	// ordinary persons to the resolver's repulsion loop; the world person
	// tables cannot see ClientState rows). The dead-peer skip is the
	// witnessed +36&2 gate [orig: @0x4b3b8d]; bit-0 rows are frozen
	// carried-object/not-ready placeholders and sit out as our analogue.
	// Pipeline-owned scratch: this table is rebuilt every 62.5 Hz tick, so a
	// fresh heap vector per tick was pure allocator churn.
	std::vector<ReplicaPeerSphere> &contact_peers = contact_peer_scratch_;
	contact_peers.clear();
	if (replica_contact_resolver_) {
		contact_peers.reserve(state_.entities.size());
		for (const ClientEntityState &pe : state_.entities) {
			if (pe.cls != EntityClass::Player && pe.cls != EntityClass::Infantry)
				continue;
			if (pe.state_flags_known && (pe.state_flags & 0x03u) != 0u) continue;
			ReplicaPeerSphere p;
			p.handle = pe.handle;
			p.x = pe.x;
			p.y = pe.y;
			p.z = pe.z;
			p.radius = replica_bound_radius_resolver_
					? replica_bound_radius_resolver_(pe.type_id)
					: 0;
			contact_peers.push_back(p);
		}
	}
	// The deck-ride + root-motion pair every organic chase leg ends with: the
	// deck-ride runs at the witnessed mover position — after the chase, before
	// root motion — for every armed org row with a grounded carrier, clip or no
	// clip [orig: org2 ride @0x4b52a0 between the chase @0x4b4470 and the
	// integrate @0x4b7cbf].
	auto organic_chase_tail = [&](ClientEntityState &es, bool is_self) {
		if (!is_self && carrier_pose_provider_)
			row_deck_ride(es, carrier_pose_provider_);
		if (root_motion_ != nullptr)
			row_root_motion_tick(es, *root_motion_, remote_motion_terrain_,
			                     rm_key, is_self, &replica_contact_resolver_,
			                     &replica_bound_radius_resolver_,
			                     contact_peers.data(),
			                     static_cast<int32_t>(contact_peers.size()),
			                     rm_key, water_z_, has_water_);
	};
	for (ClientEntityState &es : state_.entities) {
		if (!es.net_has_compact) continue;
		const bool chased_class = es.cls == EntityClass::Player ||
		                          es.cls == EntityClass::Infantry ||
		                          es.cls == EntityClass::Vehicle;
		if (!chased_class) continue;

		// Carried rows skip their own chase; the post-mover phase below follows
		// the carrier attach after all carrier rows have advanced. Retail renders
		// a seat mount through the carrier attach each frame
		// [orig: the seat attach sets Flags 0x40, not bit0 —
		// @0x4946D0/@0x494752; bit0 belongs to carried OBJECTS and not-ready
		// rows, and is what the visible-entity collector skips @0x5C8CF4].
		// A carrier-owned row never falls back to its standalone chase. When a
		// newer compact sample switches to a carrier that is not present yet,
		// net_seat_valid is deliberately cleared so no stale local offset can be
		// reused; carrier_handle still records that the row is blocked on an
		// attachment. Hold its last world pose until a resolvable carried sample
		// (or an explicit free-standing sample) arrives.
		if (es.carrier_handle != wire_handle::kInvalid) continue;
		// The universal mover-skip: wire bit0 (carried-object/killed/not-ready
		// — NOT seat mounts, which stream 0x40) freezes the row at its staged
		// pose [orig: the Flags&1 early return @0x4b9a03 / the body-pass twin;
		// the bit rides the wire raw, §5.38e §5].
		if (es.state_flags_known && (es.state_flags & 0x01u) != 0u) continue;
		// A dead row holds its death pose until the respawn snap (D-NET-66);
		// vehicles mark the wreck with the dead-pose bit instead of bit 1.
		const uint8_t dead_bit = es.cls == EntityClass::Vehicle
				? kVehicleFlagDeadPose
				: static_cast<uint8_t>(world::kEntityFlagDead);
		if (es.state_flags_known && (es.state_flags & dead_bit) != 0u) continue;
		// A world-side family mover owns this row's motion (§5.38e B-facet: the
		// embedding sim stages, predicts, and mirrors back). The freezes above
		// run first so carried/not-ready/dead rows hold even when flagged.
		if (es.net_world_mover) continue;

		// Saved-live recapture, every tick [orig: @0x4b9a5f / each family head].
		es.net_saved_live_pose[0] = es.x;
		es.net_saved_live_pose[1] = es.y;
		es.net_saved_live_pose[2] = es.z;

		const bool is_self = es.handle == self_handle;
		const int64_t dx = int64_t(es.net_smooth_target[0]) - es.x;
		const int64_t dy = int64_t(es.net_smooth_target[1]) - es.y;
		const int64_t dz = int64_t(es.net_smooth_target[2]) - es.z;

		switch (es.cls) {
		case EntityClass::Player: {
			// The org2 body-pass chase [orig: Entity_UpdateInfantryPlayerBody
			// @0x4B4470..0x4B46C0]. Client heading/pitch divisor = 12.
			constexpr int32_t kOrg2HeadingDiv = 12;
			if (es.net_interp_progress == 0) {
				const int32_t dist = dist_16_16(dx, dy, dz);
				if (dist > 0x20000) {
					// Snap: position always; heading/pitch only for a non-self
					// row.
					es.x = es.net_smooth_target[0];
					es.y = es.net_smooth_target[1];
					es.z = es.net_smooth_target[2];
					if (!is_self) {
						es.heading_bam = es.net_smooth_heading;
						es.pitch_bam = es.net_smooth_pitch;
					}
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
					es.net_smooth_heading = 0;
					es.net_smooth_pitch = 0;
				} else if (dist < 0x2AAA) {
					// Position deadband — heading/pitch still chase.
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
					es.net_smooth_heading = chase_step(
							io::bam_sub(es.net_smooth_heading, es.heading_bam),
							kOrg2HeadingDiv);
					es.net_smooth_pitch = chase_step(
							io::bam_sub(es.net_smooth_pitch, es.pitch_bam),
							kOrg2HeadingDiv);
				} else {
					if (is_self) {
						// The own-player soft reconciliation: 48 moving / 512
						// still, position only [orig: @0x4B4490/@0x4B449E].
						es.net_interp_steps =
								(es.move_input & 0x08u) != 0u ? 48 : 512;
					} else {
						es.net_interp_steps =
								org2_bucket(dist_16_16(dx, dy, 0));
					}
					const int32_t n = es.net_interp_steps;
					es.net_smooth_target[0] = chase_step(int32_t(dx), n);
					es.net_smooth_target[1] = chase_step(int32_t(dy), n);
					int32_t step_z = chase_step(int32_t(dz), n);
					// Client vertical damping [orig: @0x4B4626/@0x4B4635].
					const int32_t adz =
							int32_t(dz < 0 ? -dz : dz);
					if (adz < 0x5555) step_z >>= 1;
					if (adz < 0x2AAA) step_z = 0;
					es.net_smooth_target[2] = step_z;
					es.net_smooth_heading = chase_step(
							io::bam_sub(es.net_smooth_heading, es.heading_bam),
							kOrg2HeadingDiv);
					es.net_smooth_pitch = chase_step(
							io::bam_sub(es.net_smooth_pitch, es.pitch_bam),
							kOrg2HeadingDiv);
				}
			}
			const int16_t progress = es.net_interp_progress;
			if (progress < kOrg2HeadingDiv && !is_self) {
				es.heading_bam = io::bam_add(es.heading_bam, es.net_smooth_heading);
				es.pitch_bam = io::bam_add(es.pitch_bam, es.net_smooth_pitch);
			}
			// The position step + progress cap + starved idle force
			// [orig: @0x4b464f/@0x4b465f] (row_chase_step_and_cap), then the
			// deck-ride/root-motion tail (organic_chase_tail).
			row_chase_step_and_cap(es, progress);
			organic_chase_tail(es, is_self);
			break;
		}
		case EntityClass::Infantry: {
			// The org1 motor fall-through [orig: @0x4b9a8c].
			if (es.net_interp_progress == 0) {
				const int32_t dist = dist_16_16(dx, dy, dz);
				if (dist > 0x20000) {
					// Snap is position-only for org1.
					es.x = es.net_smooth_target[0];
					es.y = es.net_smooth_target[1];
					es.z = es.net_smooth_target[2];
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
				} else if (dist < 0x2000) {
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
				} else {
					const int32_t n = org1_bucket(dist);
					es.net_interp_steps = static_cast<int16_t>(n);
					es.net_smooth_target[0] = chase_step(int32_t(dx), n);
					es.net_smooth_target[1] = chase_step(int32_t(dy), n);
					es.net_smooth_target[2] = chase_step(int32_t(dz), n);
				}
			}
			const int16_t progress = es.net_interp_progress;
			// The org1 position step + cap + starved idle force — the same
			// +0x2BC read/write [orig: §5.38a cap 512 -> idle 43;
			// @0x4b464f/@0x4b465f shape] (row_chase_step_and_cap).
			row_chase_step_and_cap(es, progress);
			// Heading: the promoted target chased with the org1 body
			// quarter-step — the witnessed (d + 2) >> 2 rounding, clamped
			// [orig: the body chase @0x4be8fd — (target - body + 2) >> 2 then
			// ±69273360/tick; the target is one record behind the wire
			// (§5.38e §1); the sibling world-side port is infantry.cpp's
			// body-yaw chase].
			{
				int32_t step = io::bam_sar(
						io::bam_add(io::bam_sub(es.net_target_heading_bam,
						                        es.heading_bam),
						            2),
						2);
				if (step > 69273360) step = 69273360;
				if (step < -69273360) step = -69273360;
				es.heading_bam = io::bam_add(es.heading_bam, step);
			}
			organic_chase_tail(es, is_self);
			break;
		}
		case EntityClass::Vehicle: {
			// The vehicle-family chase template [orig: Entity_UpdateWatercraftPhysics
			// @0x48D480 et al.] — runs alone for rows without a world-side
			// prediction mover (traitless vehicles, lib-only embedders); flagged
			// rows are predicted world-side and skipped above (§5.38e B-facet).
			// ONE shape, TWO witnessed constant sets: ground/water snap
			// 0x60000/0x20000 at reg>=293, deadband 0x2000, buckets
			// {6,8,10,15,20,25,30}; AIR snap 0xA0000 (0x20000 only when BOTH
			// received commands < 293), deadband 0x2AAA, buckets
			// {8,10,15,20,25,32} [orig: @0x48D480 / @0x490310] — selected by
			// the sim-stamped family so an air row without traits still
			// chases with its own family's constants.
			const bool air = es.net_air_family;
			if (es.net_interp_progress == 0) {
				const int32_t snap_threshold = air
						? ((es.vehicle_speed_reg < 293 &&
						    es.vehicle_lat_reg < 293) ? 0x20000 : 0xA0000)
						: (es.vehicle_speed_reg >= 293 ? 0x60000 : 0x20000);
				const int32_t dist = dist_16_16(dx, dy, dz);
				if (dist > snap_threshold) {
					es.x = es.net_smooth_target[0];
					es.y = es.net_smooth_target[1];
					es.z = es.net_smooth_target[2];
					es.heading_bam = es.net_smooth_heading;
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
					es.net_smooth_heading = 0;
				} else if (dist < (air ? 0x2AAA : 0x2000)) {
					// Position deadband — heading still steps toward the wire
					// euler [orig: the v46 < 0x2000 else-arm @0x48D480 zeroes
					// the target and still computes (d + 10) / 20; the air
					// deadband is 0x2AAA @0x490310].
					es.net_smooth_target[0] = 0;
					es.net_smooth_target[1] = 0;
					es.net_smooth_target[2] = 0;
					es.net_interp_steps = 0;
					es.net_smooth_heading =
							io::bam_add(
									io::bam_sub(es.net_smooth_heading,
									            es.heading_bam),
									10) /
							20;
				} else {
					const int32_t n = air ? vehicle_air_bucket(dist)
					                      : vehicle_bucket(dist);
					es.net_interp_steps = static_cast<int16_t>(n);
					es.net_smooth_target[0] = chase_step(int32_t(dx), n);
					es.net_smooth_target[1] = chase_step(int32_t(dy), n);
					es.net_smooth_target[2] = chase_step(int32_t(dz), n);
					es.net_smooth_heading =
							io::bam_add(
									io::bam_sub(es.net_smooth_heading,
									            es.heading_bam),
									10) /
							20;
				}
			}
			const int16_t progress = es.net_interp_progress;
			// Heading steps for exactly 20 ticks (the /20 divisor).
			if (progress < 20)
				es.heading_bam = io::bam_add(es.heading_bam, es.net_smooth_heading);
			if (progress < es.net_interp_steps) {
				es.x += es.net_smooth_target[0];
				es.y += es.net_smooth_target[1];
				es.z += es.net_smooth_target[2];
			}
			if (progress >= 128) {
				// Starvation: the speed register decays; progress freezes. The
				// register is the full int32 decompressed 16.16 value — retail
				// drains it signed and untruncated [orig: (v+64)>>7 drain
				// @0x48D480 interp tail; the signed < 293 compare on [177]].
				es.vehicle_speed_reg -= (es.vehicle_speed_reg + 64) >> 7;
			} else {
				es.net_interp_progress = progress + 1;
			}
			break;
		}
		default:
			break;
		}
	}
	// Seat mounts and persistent no-callback children are a post-mover phase:
	// all carrier rows above have reached this tick's live pose first. The
	// per-tick call also advances the pure-client stale-carrier sweep.
	refresh_carried_entities(/*tick_sweep=*/true);
}

void ClientReplicaPipeline::queue_carrier_repair(uint16_t handle) {
	for (uint16_t h : carrier_repair_requests_)
		if (h == handle) return;
	carrier_repair_requests_.push_back(handle);
}

std::vector<uint16_t> ClientReplicaPipeline::drain_carrier_repair_requests() {
	std::vector<uint16_t> out;
	out.swap(carrier_repair_requests_);
	return out;
}

void ClientReplicaPipeline::apply_pool_spawn(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	if (!decode_pool_spawn_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: NapiNPClientMsg_0x00D @ 0x432C40].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	for (const PoolSpawnRecord &rec : batch.records) {
		const world::EntityHandle spawn_handle{rec.slot_id};
		if (spawn_handle.pool() != 1 ||
				static_cast<std::size_t>(spawn_handle.slot()) >=
						world::retail_pool_capacity(1))
			continue;
		// A 0x0D spawn is pool-1 by construction — learn the type's 0x0A replication
		// class so the vehicle compact body decodes for it (see classify()).
		learned_classes_[rec.item_type_id] = EntityClass::Vehicle;
		ClientEntityState *existing = state_.find(rec.slot_id);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		uint32_t next_spawn_revision = existing != nullptr
				? existing->spawn_revision + 1u
				: 1u;
		if (next_spawn_revision == 0) next_spawn_revision = 1;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		if (type_changed) state_.mark_topology_changed();
		// Retail clears the complete 0x2B4-byte slot before applying every
		// 0x0D record. Replace the decoded row too: compact carrier/death/anim
		// and mover state belongs to the prior lifetime even when type matches.
		es = ClientEntityState{};
		es.handle = rec.slot_id;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		es.name = rec.entity_name;
		// The 0x0D handler memsets the full slot and has no entity+124
		// net-id field. Preserve retail's resulting zero, not ClientState's
		// unknown/sentinel default.
		es.net_id = 0;
		es.spawn_tag = s2c::POOL_SPAWN;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.heading_bam = rec.euler_z;
		// Born with the spawn orientation in every heading slot (see the
		// organic applier note).
		es.net_smooth_heading = rec.euler_z;
		es.net_target_heading_bam = rec.euler_z;
		es.heading_known = (rec.spawn_flags & 0x0001u) != 0;
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
		// Flag-gated values are zero in the decoded record when omitted.
		// Assign unconditionally: retail's receive slot is zero-initialized, so
		// omission denotes zero rather than "preserve the previous value".
		es.team = rec.team_byte;
		es.team_known = (rec.spawn_flags & 0x0010u) != 0;
		es.zone_number_rank = rec.zone_number_rank;
		es.zone_radius = rec.zone_radius;
		es.spawn_entity_flags = rec.entity_flags;
		es.spawn_section_mask = static_cast<uint32_t>(rec.section_mask);
		es.spawn_ammo_count = rec.bone_byte;
		es.spawn_ref_num = rec.alert_byte;
		es.spawn_sub_type = rec.action_byte;
		es.spawn_mount_mask = rec.seat_mask;
		for (std::size_t slot = 0; slot < rec.mount_handles.size(); ++slot)
			es.spawn_mount_handles[slot] = rec.mount_handles[slot];
		es.spawn_mount_handles[8] = rec.mount_handle_8;
		es.spawn_mount_handles[9] = rec.mount_handle_9;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.parent_handle = rec.parent_handle;
		es.target_handle = rec.target_handle;
		es.parent_pose_valid = false;
		es.state_flags = static_cast<uint8_t>(rec.entity_flags & 0xFFu);
		es.health_known = false;
		changed = true;
	}
	// 0x0D positions are absolute and parent rows normally precede their BFS
	// children. Resolve after the complete batch anyway, so record ordering is
	// not a hidden requirement.
	refresh_carried_entities();
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::erase_entity_tree(uint16_t root_handle) {
	// Retail destroys ONE row and DETACHES its dependents: Entity_Destroy
	// walks the occupant + mount handles through the vehicle detach and then
	// memsets only the target entity — a child attached to the removed row
	// survives with its parent link cleared until its own remove arrives.
	// [orig: Entity_Destroy @0x43e810 — occupant detach @0x43e9e9, per-mount
	//  detach loop @0x43ea38..0x43ea59, memset(entity, 0, 0x2B4) @0x43ea70]
	bool detached = false;
	for (ClientEntityState &entity : state_.entities) {
		if (entity.parent_handle != root_handle) continue;
		entity.parent_handle = wire_handle::kInvalid;
		entity.parent_pose_valid = false;
		detached = true;
	}
	const std::size_t before = state_.entities.size();
	state_.entities.erase(
			std::remove_if(state_.entities.begin(), state_.entities.end(),
					[&](const ClientEntityState &entity) {
						return entity.handle == root_handle;
					}),
			state_.entities.end());
	if (detached || state_.entities.size() != before)
		state_.mark_topology_changed();
}

// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 — the body carries RAW pool-0
//  indices, resolved with Pool_GetEntryUnchecked(0, idx), so the wire handle is
//  (0 << 12) | idx]
void ClientReplicaPipeline::destroy_pool0_slot(uint16_t pool0_index) {
	if (wire_handle::pool(pool0_index) != wire_handle::kPoolOrganic) return; // not a pool-0 slot index
	if (state_.find(pool0_index) == nullptr) return;
	erase_entity_tree(pool0_index);
}

// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the non-authority entity team store @0x4319ee]
void ClientReplicaPipeline::apply_team_assign(uint16_t handle, uint8_t team) {
	// Retail's gates: not the 0xFFFF sentinel, and the pool nibble must address one
	// of the five entity pools (@0x431910 header checks).
	const world::EntityHandle h{handle};
	if (!h.valid() || h.pool() >= world::kEntityPoolCount) return;
	ClientEntityState &entity = state_.upsert(handle);
	if (entity.team == team && entity.team_known) return;
	entity.team = team;
	entity.team_known = true;
	state_.mark_changed();
}

void ClientReplicaPipeline::refresh_carried_entities(bool tick_sweep) {
	std::vector<uint16_t> sweep_destroyed;
	// ClientState intentionally keeps its decoded rows public, so its general
	// find() contract must remain a derived linear lookup. This refresh owns a
	// stable vector for its whole eight-depth pass, though: build a disposable
	// first-row index here instead of rescanning a retail-sized world stream for
	// every carrier/parent edge. emplace preserves find()'s first-match behavior
	// if a caller has directly introduced duplicate handles.
	std::unordered_map<uint16_t, std::size_t> row_by_handle;
	row_by_handle.reserve(state_.entities.size());
	for (std::size_t i = 0; i < state_.entities.size(); ++i)
		row_by_handle.emplace(state_.entities[i].handle, i);
	auto find_row = [&](uint16_t handle) -> ClientEntityState * {
		const auto found = row_by_handle.find(handle);
		return found != row_by_handle.end()
				? &state_.entities[found->second]
				: nullptr;
	};

	// The item catalog and learned class table cannot change during this method.
	// Resolve each no-callback row's persistent STRUCTURAL CARRIER once, outside
	// the repeated pose-composition depths. Retail's 'ewep' class MOVE function
	// recomposes the child from groundEntity (entity+40) every tick — the slot
	// the 0x0D TARGET seeds — so a compact-less mounted gun rides its DRIVING
	// hull with no wire records of its own: the carrier-def seat bone
	// (carrierDef+532+subType) selects a model bone and the child adopts the
	// carrier-matrix-composed position + Euler set (or, with no resolvable
	// bone, the carrier pose verbatim).
	// [orig: move-fn table 'ewep' row @0x82abe0 -> Entity_UpdateTransformAndTurret
	//  @0x440ca0 — groundEntity read @0x440cbf, bone re-resolve @0x440cfd,
	//  carrier-matrix bone compose @0x44109d with position+Euler adoption
	//  @0x4410dd, no-bone verbatim carrier-pose adoption @0x4410ea..0x4411bc]
	// Without model bone tables at this layer, the recompose below carries the
	// rigid child-in-carrier pose captured from the 0x0D absolutes — the same
	// client-subset simplification the parent-follow path already pins.
	std::vector<uint16_t> persistent_carrier(state_.entities.size(), wire_handle::kInvalid);
	for (std::size_t i = 0; i < state_.entities.size(); ++i) {
		const ClientEntityState &child = state_.entities[i];
		if (child.target_handle == wire_handle::kInvalid && child.parent_handle == wire_handle::kInvalid)
			continue; // no carrier candidate — skip the catalog lookup
		if (classify(child.type_id) != EntityClass::NoNetworkCallback)
			continue;
		// The 0x0D TARGET is the structural carrier (groundEntity/+40) and
		// outranks any parent: retail's transform never reads +368.
		if (child.target_handle != wire_handle::kInvalid) {
			persistent_carrier[i] = child.target_handle;
			continue;
		}
		if (child.parent_handle == wire_handle::kInvalid) continue;
		// A POOL-0 parent on a no-callback child is the occupant/driver
		// back-reference, never a transform parent (live retail 0x0D witness,
		// 00TRg 2026-08-04: an OCCUPIED "50cal on 180 tripod" spawns with
		// parent=<its gunner's pool-0 handle>, while the gunner's own record
		// carries parent=<the gun> — composing both closes a mutual
		// seat/parent loop that ratchets the pair through the depth passes
		// (the reported climbing/spinning emplacements). The structural
		// carrier of a mounted-on-vehicle gun rides the record's separate
		// TARGET field, consumed above.
		// [orig: 0x0D store @0x433289 — entity+368 occupantEntity back-ref;
		//  target → groundEntity resolve @0x4332bc, store @0x4332d7]
		if (world::EntityHandle{child.parent_handle}.pool() == 0)
			continue;
		persistent_carrier[i] = child.parent_handle;
	}
	// Repeating the composition makes mixed seat/persistent-parent chains
	// independent of pool/vector ordering while preserving the promotion depth
	// cap. This method runs only after movers, so every lookup observes the
	// carrier's final live pose for this tick.
	for (int depth = 0; depth < 8; ++depth) {
		for (std::size_t child_index = 0;
		     child_index < state_.entities.size(); ++child_index) {
			ClientEntityState &child = state_.entities[child_index];
			// Compact-carried player/infantry/vehicle rows retain the latest
			// successfully resolved local sample. A missing carrier invalidates
			// the sample rather than leaving an offset that could attach to a
			// later handle reuse.
			if (child.net_seat_valid && child.carrier_handle != wire_handle::kInvalid) {
				const ClientEntityState *carrier = find_row(child.carrier_handle);
				if (carrier == nullptr) {
					child.net_seat_valid = false;
				} else {
					const WorldPose posed = network_transform_local_to_world(
							child.net_seat_local[0], child.net_seat_local[1],
							child.net_seat_local[2], carrier->x, carrier->y,
							carrier->z, static_cast<uint32_t>(carrier->heading_bam),
							static_cast<uint32_t>(carrier->pitch_bam),
							static_cast<uint32_t>(carrier->roll_bam));
					child.x = posed.x;
					child.y = posed.y;
					child.z = posed.z;
					if (child.net_seat_compose_yaw) {
						child.heading_bam = io::bam_add(
								carrier->heading_bam,
								static_cast<int32_t>(
										static_cast<uint32_t>(
												child.net_seat_local_yaw_byte)
										<< 24));
						child.yaw_byte = yaw_byte_from_bam(child.heading_bam);
					}
				}
			}

			if (persistent_carrier[child_index] == wire_handle::kInvalid) continue;
			// Only the addeweap/no-callback family rides this persistent
			// recompose: those children never receive compact motion samples,
			// so the load-time 0x0D target/parent relation is their only pose
			// source. A compact-sampled class (player/vehicle/infantry) moves
			// by its OWN records — its carrier composition happens per record
			// on the record's own carrier field — and its 0x0D parentHandle is
			// the occupantEntity/+368 DRIVER back-reference, never a transform
			// parent [orig: 0x0D store @0x433289; vehicle-compact carrier
			// compose @0x4608ce]. Recomposing such a row here glued the
			// vehicle to its spawn-time occupant — on non-COOP retail hosts,
			// "a vehicle follows the player around" (one per map, whichever
			// spawn record carried flag 0x0100).
			ClientEntityState *parent = find_row(persistent_carrier[child_index]);
			if (parent == nullptr) {
				// The pure-client stale-carrier sweep [orig: @0x440d41..
				// 0x440e2f]: after a 128-tick unresolvable run, request BOTH
				// rows over C2S 0x0F and locally destroy the child — the
				// authority's re-spawn re-materializes the pair. A later batch
				// may still provide the carrier inside the window, which
				// resets the run below.
				if (tick_sweep && depth == 0 &&
				    ++child.carrier_missing_ticks >= 128) {
					queue_carrier_repair(persistent_carrier[child_index]);
					queue_carrier_repair(child.handle);
					if (std::find(sweep_destroyed.begin(), sweep_destroyed.end(),
							child.handle) == sweep_destroyed.end())
						sweep_destroyed.push_back(child.handle);
				}
				continue;
			}
			child.carrier_missing_ticks = 0;
			// The dead-carrier leg. Retail's client HIDES the child in place —
			// carrier Flags & 2 -> child Flags |= 1, return, row persists
			// pending the authority's own destroy transaction [orig:
			// @0x440cdb..0x440cdd]. Our decoded view RETIRES the subtree
			// instead (the #403 substitute, kept deliberately): the hide is
			// presentation-equivalent (bit 0 = invisible), our authority
			// genuinely despawns the attachment on carrier death, and no
			// destroy transaction exists on this seam to mirror — an erased
			// row IS the authority truth here. The death signal stays the
			// known-zero health word only: the wire flags bit 1 is an
			// overloaded spawn/movement gate on 0x0D-fed rows, not a death
			// verdict (the loopback-identity pin).
			if (parent->health_known && parent->health_word == 0) {
				if (std::find(sweep_destroyed.begin(), sweep_destroyed.end(),
						child.handle) == sweep_destroyed.end())
					sweep_destroyed.push_back(child.handle);
				continue;
			}

			const int32_t parent_heading_bam = parent->heading_bam;
			if (!child.parent_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						child.x, child.y, child.z, parent->x, parent->y, parent->z,
						static_cast<uint32_t>(parent_heading_bam),
						static_cast<uint32_t>(parent->pitch_bam),
						static_cast<uint32_t>(parent->roll_bam));
				child.parent_local_x = local.x;
				child.parent_local_y = local.y;
				child.parent_local_z = local.z;
				child.parent_local_heading_bam =
						io::bam_sub(child.heading_bam, parent_heading_bam);
				child.parent_local_pitch_bam =
						io::bam_sub(child.pitch_bam, parent->pitch_bam);
				child.parent_local_roll_bam =
						io::bam_sub(child.roll_bam, parent->roll_bam);
				child.parent_pose_valid = true;
			}
			const WorldPose posed = network_transform_local_to_world(
					child.parent_local_x, child.parent_local_y, child.parent_local_z,
					parent->x, parent->y, parent->z,
					static_cast<uint32_t>(parent_heading_bam),
					static_cast<uint32_t>(parent->pitch_bam),
					static_cast<uint32_t>(parent->roll_bam));
			child.x = posed.x;
			child.y = posed.y;
			child.z = posed.z;
			child.heading_bam = io::bam_add(
					parent_heading_bam, child.parent_local_heading_bam);
			child.yaw_byte = yaw_byte_from_bam(child.heading_bam);
			child.pitch_bam = io::bam_add(
					parent->pitch_bam, child.parent_local_pitch_bam);
			child.roll_bam = io::bam_add(
					parent->roll_bam, child.parent_local_roll_bam);
		}
	}
	for (uint16_t handle : sweep_destroyed) erase_entity_tree(handle);
}

void ClientReplicaPipeline::apply_static_batch(const std::vector<uint8_t> &body) {
	StaticEntityBatch batch;
	if (!decode_static_entity_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: the 0x10 static handler].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	// The 0x10 record carries no slot id — the entity's slot is start_index + iteration index.
	for (size_t i = 0; i < batch.records.size(); ++i) {
		const StaticEntityRecord &rec = batch.records[i];
		const std::size_t slot = static_cast<std::size_t>(batch.start_index) + i;
		// Retail indexes pool 2 unchecked; its own capacity (1200) is the bound a
		// stock server can reach — a 1157-entity 01TR load page walked past a
		// 1024 clamp here and silently dropped the pool tail.
		// [orig: NapiNPClientMsg_0x010 @0x433400 — Pool_GetEntryUnchecked(2, idx)
		//  @0x433487; capacity EntityPool_Allocate @0x4421a2]
		if (slot >= world::retail_pool_capacity(2)) continue;
		const uint16_t handle = static_cast<uint16_t>(0x2000u | slot);
		if (rec.is_empty_slot) {
			const std::size_t before = state_.entities.size();
			state_.entities.erase(
					std::remove_if(state_.entities.begin(), state_.entities.end(),
							[handle](const ClientEntityState &row) {
								return row.handle == handle;
							}),
					state_.entities.end());
			if (state_.entities.size() != before) {
				state_.mark_topology_changed();
				++state_.world_stream_revision;
				changed = true;
			}
			continue;
		}
		ClientEntityState *existing = state_.find(handle);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		uint32_t next_spawn_revision = existing != nullptr
				? existing->spawn_revision + 1u
				: 1u;
		if (next_spawn_revision == 0) next_spawn_revision = 1;
		ClientEntityState &es = state_.upsert(handle);
		if (type_changed) state_.mark_topology_changed();
		// The retail 0x10 handler memsets the selected slot before itemType.
		es = ClientEntityState{};
		es.handle = handle;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		// Like 0x0D, the 0x10 handler clears entity+124 and never rewrites it.
		es.net_id = 0;
		es.spawn_tag = s2c::STATIC_ENTITY_BATCH;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.heading_bam = rec.euler_z;
		es.heading_known = (rec.field_flags & 0x0001u) != 0;
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
		es.team = rec.team_byte;
		es.team_known = (rec.field_flags & 0x0010u) != 0;
		es.spawn_entity_flags = rec.entity_flags;
		es.spawn_section_mask = static_cast<uint32_t>(rec.section_mask);
		es.spawn_ammo_count = rec.ammo_count;
		es.spawn_ref_num = rec.bone_a;
		es.spawn_sub_type = rec.bone_b;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.zone_number_rank = rec.weapon_byte;
		es.zone_radius = rec.attach_ref;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::apply_pool3_batch(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	if (!decode_pool3_sync_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: the 0x20 pool-3 handler].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	for (std::size_t i = 0; i < batch.records.size(); ++i) {
		const Pool3SyncRecord &rec = batch.records[i];
		// Retail indexes pool 3 directly from the 0x20 start index and record
		// ordinal, then stores netHandle into the already selected entity. The two
		// fields are deliberately not aliases. [orig: 0x20 handler @0x425C00,
		// Pool_GetEntryUnchecked(3, startIndex+i), netHandle store at entity+124]
		const std::size_t slot =
				static_cast<std::size_t>(batch.start_index) + i;
		if (slot >= world::retail_pool_capacity(3)) continue;
		const uint16_t handle = static_cast<uint16_t>(0x3000u | slot);
		if (rec.is_empty_slot) {
			const std::size_t before = state_.entities.size();
			state_.entities.erase(
					std::remove_if(state_.entities.begin(), state_.entities.end(),
							[handle](const ClientEntityState &row) {
								return row.handle == handle;
							}),
					state_.entities.end());
			if (state_.entities.size() != before) {
				state_.mark_topology_changed();
				++state_.world_stream_revision;
				changed = true;
			}
			continue;
		}
		ClientEntityState *existing = state_.find(handle);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		uint32_t next_spawn_revision = existing != nullptr
				? existing->spawn_revision + 1u
				: 1u;
		if (next_spawn_revision == 0) next_spawn_revision = 1;
		ClientEntityState &es = state_.upsert(handle);
		if (type_changed) state_.mark_topology_changed();
		// The retail 0x20 handler likewise memsets its complete selected slot.
		es = ClientEntityState{};
		es.handle = handle;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		es.net_id = rec.net_handle;
		es.spawn_tag = s2c::POOL3_SYNC;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(static_cast<int32_t>(rec.movement_val));
		es.heading_bam = static_cast<int32_t>(rec.movement_val);
		es.heading_known = (rec.flags_byte & 0x01u) != 0;
		es.pitch_bam = 0; // pool-3 sync carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
		es.team = rec.team_byte;
		es.team_known = (rec.flags_byte & 0x08u) != 0;
		es.spawn_entity_flags = 0;
		es.spawn_section_mask = 0;
		es.spawn_ammo_count = rec.ammo_count;
		es.spawn_bound_radius_q16 =
				static_cast<int32_t>(rec.orientation_val);
		es.spawn_ref_num = 0;
		es.spawn_sub_type = 0;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.zone_number_rank = 0;
		es.zone_radius = 0;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::apply_frame_update(const std::vector<uint8_t> &body) {
	FrameUpdate fu;
	// decode_frame_update leaves everything it walked in `fu` even on a short read,
	// so we apply whatever decoded cleanly (out.complete reflects a clean terminator).
	decode_frame_update(body.data(), body.size(),
	                    [this](uint16_t tid) { return classify(tid); }, fu,
	                    game_type::is_objective(game_type_), authority_recipient_);

	state_.anchor_x = fu.anchor_x;
	state_.anchor_y = fu.anchor_y;
	state_.anchor_z = fu.anchor_z;
	// The deploy-map overlay follows the host every frame — set AND cleared
	// by assignment, not edges [orig: NapiNPClientMsg_0x00A @0x42ff82 —
	// g_deploy_screen_active = (flags1 >> 1) & 1].
	state_.deploy_overlay_active = (fu.flags1 & 0x02u) != 0;
	if (!state_.deploy_overlay_active) state_.deploy_overlay_open_latch = false;
	// The death-screen edges on flags1 bit 0 [orig: @0x42ff88..0x43002b].
	{
		const bool bit = (fu.flags1 & 0x01u) != 0;
		if (bit && !state_.death_screen_active) {
			state_.death_screen_active = true;
			state_.death_screen_submode = 0;
			state_.enemy_tags_visible = true;
		} else if (!bit && state_.death_screen_active) {
			state_.death_screen_active = false;
			state_.enemy_tags_visible = false;
		}
	}
	if (fu.local_tail_present) {
		state_.local_health = fu.health;
		++state_.health_updates_applied;
	}
	if (fu.weapon.present) {
		// Phase 0 is the sole retail mirror of g_preround_delay_timer.
		// Retain it between phase cycles, exactly like the client global.
		// [orig: NapiNPClientMsg_0x00A @0x430064]
		state_.preround_delay_seconds = fu.weapon.preround_timer;
		// The DEATH screen's three slot timers ride the same sub-block
		// [orig: @0x430084 / @0x43009f / @0x4300c3].
		state_.respawn_penalty_seconds = fu.weapon.slot_state360;
		state_.local_revive_seconds = fu.weapon.slot_state368;
		state_.spawn_hold_seconds = fu.weapon.slot_state364;
	}
	if (authority_recipient_) {
		// The listen host's own frame carries nothing past the phase-0 block
		// [orig: NapiNPClientMsg_0x00A @0x430174]; the timer, environment,
		// objective, tail, and every entity/round come from the host's own
		// World on this role.
		++state_.frames_applied;
		state_.mark_changed();
		return;
	}
	if (fu.timer.present) {
		// The round clock: 62 x the wire's whole seconds, negative = untimed
		// -1. [orig: NapiNPClientMsg_0x00A @0x430219..0x430235 —
		//  g_round_time_remaining]
		state_.round_time_remaining_ticks = fu.timer.timer_seconds < 0
				? -1
				: 62 * static_cast<int32_t>(fu.timer.timer_seconds);
	}
	if (fu.objective.present) {
		state_.objective_won = static_cast<uint32_t>(fu.objective.state[0]);
		state_.objective_lost = static_cast<uint32_t>(fu.objective.state[1]);
		state_.objective_show_win = static_cast<uint32_t>(fu.objective.state[2]);
		state_.objective_show_lose = static_cast<uint32_t>(fu.objective.state[3]);
		++state_.objective_updates_applied;
	}
	if (fu.env.present) {
		state_.environment.present = true;
		state_.environment.fog_dist = fu.env.fog_dist;
		state_.environment.fog_accel = fu.env.fog_accel;
		state_.environment.tod_fixed = fu.env.tod_fixed;
		state_.environment.quake_ticks = fu.env.quake_ticks;
		state_.environment.cloud_scroll = fu.env.cloud_scroll;
		state_.environment.overcast = fu.env.overcast;
		state_.environment.rain_pct = fu.env.rain_pct;
		state_.environment.env_param = fu.env.env_param;
		++state_.environment.revision;
	}
	if (fu.passenger.present) {
		state_.mounted_ammo.present = true;
		state_.mounted_ammo.mount_handle = fu.passenger.mount_handle;
		state_.mounted_ammo.has_mount = fu.passenger.has_mount;
		state_.mounted_ammo.clip = fu.passenger.clip;
		state_.mounted_ammo.reserve = fu.passenger.reserve;
		++state_.mounted_ammo.revision;
	}

	// Tag 2 carries a fire origin and direction. Lift the compressed origin by
	// this frame's anchor now, while those transient coordinates are together.
	for (const RoundEventRecord &rec : fu.round_events) {
		ClientRoundEvent ev;
		ev.flags = rec.flags;
		ev.adm_index = rec.adm_index;
		ev.subtype = rec.subtype;
		ev.slot_byte = rec.slot_byte;
		ev.shooter_handle = rec.shooter_handle;
		ev.target_handle = rec.target_handle;
		ev.shot_seq = rec.shot_seq;
		ev.origin_x = fu.anchor_x + network_decompress_fixedpoint(rec.pos_x_compressed);
		ev.origin_y = fu.anchor_y + network_decompress_fixedpoint(rec.pos_y_compressed);
		ev.origin_z = fu.anchor_z + network_decompress_fixedpoint(rec.pos_z_compressed);
		ev.dir_yaw_bam = static_cast<int32_t>(
				static_cast<uint32_t>(rec.yaw_bam_high) << 16);
		ev.dir_pitch_bam = static_cast<int32_t>(
				static_cast<uint32_t>(rec.pitch_bam_high) << 16);
		pending_round_events_.push_back(ev);
	}

	state_.compact_records_applied += static_cast<std::uint32_t>(fu.records.size());
	for (ClientEntityState &e : state_.entities) e.seen_this_frame = false;
	struct PendingCarrierPose {
		uint16_t child_handle;
		uint16_t carrier_handle;
		uint16_t cx;
		uint16_t cy;
		uint16_t cz;
		uint8_t local_yaw_byte;
		// Player/infantry compacts carry a carrier-RELATIVE yaw byte; the
		// vehicle compact's orientation stays world-absolute even when its
		// position is carrier-local [orig: the read path stores the wire
		// eulerZ untransformed at entity+576 @0x4607f5 while the position
		// goes through Entity_TransformLocalToWorld @0x4608ce].
		bool compose_yaw;
	};
	std::vector<PendingCarrierPose> pending_carrier_poses;
	pending_carrier_poses.reserve(fu.records.size());

	// Fold in TWO sweeps so the witnessed child-before-carrier production
	// order still resolves: retail resolves a record's carrier against the
	// POOL, where the carrier exists from its spawn regardless of this
	// frame's record order — our row analog is created by the carrier's own
	// record, so a sweep-1 child whose carrier only appears later in the
	// same frame retries in sweep 2. A carrier absent after BOTH sweeps is
	// the retail bail: the WHOLE record drops and a C2S 0x0F entity request
	// is queued — nothing from the record lands [orig: vehicle resolve
	// @0x46085d, repair bail @0x4608ae..0x4608c1; the player op2 carrier
	// path @0x4c10d4]. Skipping only the position would half-apply
	// anim/flags/health from a sample retail never applied.
	std::vector<uint8_t> record_folded(fu.records.size(), 0u);
	for (int sweep = 0; sweep < 2; ++sweep)
	for (size_t rec_i = 0; rec_i < fu.records.size(); ++rec_i) {
		const FrameUpdateRecord &rec = fu.records[rec_i];
		if (record_folded[rec_i] != 0u) continue;
		if (rec.cls == EntityClass::NoNetworkCallback) {
			record_folded[rec_i] = 1u;
			continue;
		}
		uint16_t record_carrier = wire_handle::kInvalid;
		if (rec.cls == EntityClass::Player)
			record_carrier = rec.player.carrier_handle;
		else if (rec.cls == EntityClass::Vehicle)
			record_carrier = rec.vehicle.parent_slot_handle;
		else if (rec.cls == EntityClass::Infantry)
			record_carrier = rec.infantry.vehicle_slot_handle;
		if (record_carrier != wire_handle::kInvalid &&
				state_.find(record_carrier) == nullptr) {
			if (sweep == 0) continue; // the carrier may appear this frame
			queue_carrier_repair(record_carrier);
			record_folded[rec_i] = 1u;
			continue;
		}
		record_folded[rec_i] = 1u;
		ClientEntityState *existing = state_.find(rec.handle);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.type_id;
		ClientEntityState &es = state_.upsert(rec.handle);
		if (type_changed) state_.mark_topology_changed();
		// Capture the previous body-anim sample before the per-record clear: if
		// this record REPLACES it with a different state within one decode fold,
		// the old value becomes the transition PULSE presentation still has to
		// dispatch — retail applies each record's anim byte through the receive
		// arbitration as it decodes [orig: @0x4c1153], and a tapped prone roll
		// rides the wire for only 1-2 ticks (the byte is `pending ?: current`).
		// A row's first-ever organic sample never pulses (its default 0 would
		// read as the anim_reset clip).
		const bool prev_anim_sampled = es.cls == EntityClass::Player ||
		                               es.cls == EntityClass::Infantry;
		const uint8_t prev_anim_state = es.anim_state_id;
		const uint8_t prev_anim_ratio = es.anim_channel_ratio;
		if (es.type_id != rec.type_id && es.type_id != 0) {
			// A handle reused for a different type: the stamped adm, the
			// playing channel, and the arbitration FSM pair are the OLD
			// body's — re-resolve and re-arm.
			es.rm_adm_id = -2;
			row_channel_disarm(es);
			es.net_anim_current = -1;
			es.net_anim_pending = 0;
			es.net_anim_pending_boundary = -1;
			es.net_anim_ratio_live = false;
		}
		es.type_id = rec.type_id;
		es.cls = rec.cls;
		es.seen_this_frame = true;
		// Every compact record is a complete sample of these organic fields. Clear the
		// normalized row before class-specific assignment so dismounts and class changes
		// cannot retain a stale carrier/bone selector from an earlier frame.
		es.carrier_handle = wire_handle::kInvalid;
		es.mount_bone = 0;
		es.seat_type = 0;
		// Carrier identity and its resolved local pose form one atomic sample.
		// The second pass re-arms this only if the final carrier resolves.
		es.net_seat_valid = false;
		es.pitch_byte = 0;
		es.aim_yaw_byte = 0;
		es.anim_state_id = 0;
		es.anim_channel_ratio = 0;

		// Compact player and infantry records carry the authoritative organic
		// lifecycle bits. Retain the complete byte, while counting only known
		// dead -> alive edges so an initial alive spawn is not mistaken for a
		// respawn. This happens per decoded record rather than per render frame:
		// pump() may fold several queued 0x0A datagrams before Godot presents.
		bool has_state_flags = false;
		uint8_t state_flags = 0;
		if (rec.cls == EntityClass::Player) {
			has_state_flags = true;
			state_flags = rec.player.state_flags;
		} else if (rec.cls == EntityClass::Infantry) {
			has_state_flags = true;
			state_flags = rec.infantry.flags_byte;
		} else if (rec.cls == EntityClass::Vehicle) {
			// Vehicles carry the wire flags too; their dead marker is the
			// dead-pose/wreck bit (flags & 4), not the organic bit 1
			// [orig: the short-form select on flags_byte, §5.13].
			has_state_flags = true;
			state_flags = rec.vehicle.flags_byte;
		}
		const uint8_t dead_bit = rec.cls == EntityClass::Vehicle
				? kVehicleFlagDeadPose
				: static_cast<uint8_t>(world::kEntityFlagDead);
		bool respawned_this_record = false;
		bool row_was_dead = false;
		if (has_state_flags) {
			const bool was_known = es.state_flags_known;
			const bool was_dead = (es.state_flags & dead_bit) != 0u;
			const bool is_alive = (state_flags & dead_bit) == 0u;
			row_was_dead = was_known && was_dead;
			es.state_flags = state_flags;
			es.state_flags_known = true;
			if (was_known && was_dead && is_alive) {
				++es.respawn_revision;
				respawned_this_record = true;
			}
		}
		// The organic wire-dead position skip (LABEL_151). Vehicle wrecks are the
		// opposite: the dead-pose short form force-live-snaps the frozen pose.
		const bool wire_dead = has_state_flags &&
				rec.cls != EntityClass::Vehicle &&
				(state_flags & dead_bit) != 0u;

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor — or, for a CARRIER-LOCAL player record (vehicle/ground handle !=
		// 0xFFFF, D-NET-151), lift the local offset through the carrier's pose from this
		// view's own state [orig: op2 resolves the carrier from g_pool_list and runs
		// Entity_TransformLocalToWorld @0x4c10d4; a carrier with no itemDef DROPS the
		// record and queues a C2S 0x0F entity request — request plumbing an in-process
		// view does not need, so an unknown carrier just skips the position sample].
		// Carrier-local samples are queued for a second pass after every record has
		// updated the view. Production order is pool-0 child before pool-1 carrier.
		uint16_t cx = 0, cy = 0, cz = 0;
		bool skip_pos = false;
		// The record's decoded orientation target (BAM32). Players/infantry carry
		// the 8-bit yaw high byte; vehicles the 16-bit euler_z high half — the
		// wire precision each class actually has (§5.38e).
		int32_t heading_target = 0;
		bool has_heading_target = false;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.carrier_handle = rec.player.carrier_handle;
			es.mount_bone = rec.player.vehicle_bone;
			es.seat_type = rec.player.seat_type;
			es.pitch_byte = rec.player.pitch_byte;
			es.anim_state_id = rec.player.anim_state_id;
			es.anim_channel_ratio = rec.player.anim_channel_ratio;
			es.equipped_adm_index = rec.player.anim_def_index;
			es.state_flags = rec.player.state_flags;
			es.move_input = rec.player.move_input_byte;
			es.health_class_byte = rec.player.health_class_byte;
			if (rec.player.carrier_handle != wire_handle::kInvalid) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.player.carrier_handle, cx, cy, cz,
						rec.player.yaw_byte, /*compose_yaw=*/true});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.player.yaw_byte;
				heading_target = static_cast<int32_t>(
						static_cast<uint32_t>(rec.player.yaw_byte) << 24);
				has_heading_target = true;
			}
			break;
		case EntityClass::Vehicle:
			cx = rec.vehicle.pos_x_compressed;
			cy = rec.vehicle.pos_y_compressed;
			cz = rec.vehicle.pos_z_compressed;
			// The §5.13 compact's own parent field is the CARRIER (deck/ground
			// entity), consumed per record: a resolving parent composes THIS
			// record's vehicle-local position against the carrier's live pose,
			// an absent one takes the anchor-relative leg, and the stored
			// carrier ref is re-landed (nulled included) from every record
			// [orig: Entity_SerializeVehicleState read side — resolve
			// @0x46085d, local->world @0x4608ce, entity+40 (re)store
			// @0x460802]. The 0x0D spawn's parentHandle is a DIFFERENT slot —
			// occupantEntity/+368, a driver back-reference with no transform
			// semantics [orig: store @0x433289] — see
			// refresh_parented_pool_entities for the class gate that keeps it
			// out of this row's pose.
			// The carrier ref is consumed per record like the organic classes
			// (re-landed nulled included, D-NET-195) so the per-tick seat-follow
			// can ride a resolving deck carrier between records.
			es.carrier_handle = rec.vehicle.parent_slot_handle;
			if (rec.vehicle.parent_slot_handle != wire_handle::kInvalid) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.vehicle.parent_slot_handle, cx, cy, cz,
						0, /*compose_yaw=*/false});
				skip_pos = true;
			}
			es.yaw_byte = static_cast<uint8_t>(
					static_cast<uint16_t>(rec.vehicle.euler_z) >> 8);
			// The vehicle heading target keeps the wire's full 16-bit euler_z —
			// world-absolute even for carrier-local positions [orig: @0x4607f5].
			heading_target = static_cast<int32_t>(rec.vehicle.euler_z) * 65536;
			has_heading_target = true;
			es.health_word = rec.vehicle.health_word;
			es.health_known = true;
			// The raw speed register mirror ([177] source field) — gates the
			// fast-vehicle snap threshold and decays on starvation (§5.38e §4).
			if (!rec.vehicle.is_dead_pose) {
				es.vehicle_speed_reg =
						network_decompress_fixedpoint(rec.vehicle.weapon_aim_y);
				es.vehicle_steer_bam = static_cast<int32_t>(
						rec.vehicle.weapon_heading_bam) * 65536;
				es.vehicle_lat_reg =
						network_decompress_fixedpoint(rec.vehicle.weapon_aim_z);
			}
			// Live vehicle compacts omit entity+20/+24. Preserve the last full
			// spawn/dead-pose values until the short dead-pose form carries new
			// signed high words [orig: @0x460d4c/@0x460d52].
			if (rec.vehicle.is_dead_pose) {
				es.pitch_bam = static_cast<int32_t>(rec.vehicle.euler_x) * 65536;
				es.roll_bam = static_cast<int32_t>(rec.vehicle.euler_y) * 65536;
			}
			break;
		case EntityClass::Infantry:
			cx = rec.infantry.pos_x_compressed;
			cy = rec.infantry.pos_y_compressed;
			cz = rec.infantry.pos_z_compressed;
			es.carrier_handle = rec.infantry.vehicle_slot_handle;
			es.mount_bone = rec.infantry.seat_bone_idx;
			es.pitch_byte = rec.infantry.pitch_byte;
			es.aim_yaw_byte = rec.infantry.aim_yaw_byte;
			es.anim_state_id = rec.infantry.anim_byte;
			es.state_flags = rec.infantry.flags_byte;
			// The compact carries desired aim pitch (entity+0x2D0), not live
			// entity+0x14. Retail's remote gunner rebuilds the latter locally
			// with the same wrapped one-eighth chase as authoritative AI.
			// [orig: chase @0x4bef7b..0x4bef97]
			if (rec.infantry.vehicle_slot_handle != wire_handle::kInvalid &&
					rec.infantry.seat_bone_idx != 0) {
				es.pitch_bam = chase_infantry_pitch(
						es.pitch_bam, rec.infantry.aim_yaw_byte);
			}
			if (rec.infantry.vehicle_slot_handle != wire_handle::kInvalid) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.infantry.vehicle_slot_handle, cx, cy, cz,
						rec.infantry.yaw_byte, /*compose_yaw=*/true});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.infantry.yaw_byte;
				heading_target = static_cast<int32_t>(
						static_cast<uint32_t>(rec.infantry.yaw_byte) << 24);
				has_heading_target = true;
			}
			break;
		default:
			break; // unresolved/guided records are not compact motion samples
		}
		// Latch the overwritten body-anim state as this row's transition pulse.
		// LAST transition wins: in a fold of [41, 48] the 41 is the state being
		// buried (the 48 latched by the first transition was already presented
		// last frame, and re-dispatching a presented state is a same-state
		// no-op at the model). The presenter drains the pulse once per frame.
		// (The pulse survives as the DISARMED-row fallback + the legacy
		// publish rollback seam; armed rows now run the per-record
		// arbitration below — D-NET-209.)
		if (prev_anim_sampled &&
				(rec.cls == EntityClass::Player ||
						rec.cls == EntityClass::Infantry) &&
				es.anim_state_id != prev_anim_state) {
			es.anim_state_pulse = static_cast<int16_t>(prev_anim_state);
			es.anim_pulse_ratio = prev_anim_ratio;
		}
		// The per-record receive arbitration (the D-NET-209 closure): every
		// player/infantry record applies its anim byte through the retail FSM
		// pair as it decodes — several records folded between presents each
		// arbitrate in arrival order, exactly the @0x4c1153 shape.
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry) {
			apply_record_body_arbitration(es, es.anim_state_id,
					es.anim_channel_ratio,
					rec.cls == EntityClass::Player, wire_dead, row_was_dead,
					respawned_this_record);
            // [orig: NetPacket_SerializePlayerState @ 0x4C09C0, stance stores @ 0x4C11D7..0x4C1242]
            // The committed FSM state determines MoveOrder, including when
            // the just-received animation was deferred into the pending slot.
            if (rec.cls == EntityClass::Player) {
                const uint32_t flags = world::infantry_anim_flags(es.net_anim_current);
                es.net_stance_bits = static_cast<uint8_t>(
                        ((flags & 0x100u) ? 2 : 0) | ((flags & 0x200u) ? 1 : 0));
            }
		}
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry ||
				rec.cls == EntityClass::Vehicle) {
			es.heading_known = true;
			++es.compact_revision;
		}
		// Root-channel lifecycle at the organic freeze/respawn edges: a frozen
		// row (dead/bit0/carried) never root-ticks, so its channel is DISARMED
		// — presentation falls back to the per-record wire anim byte exactly
		// as retail applies it [orig: @0x4c1153] (the death/seat clips
		// dispatch); the respawn edge re-arms fresh so the resume never
		// blends out of the pre-death primary. (The one-shot phase seed now
		// rides the arbitration's direct-commit leg above — net_anim_ratio.)
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry) {
			const bool row_frozen =
					(has_state_flags &&
							(state_flags &
									(0x01u | world::kEntityFlagDead)) != 0u) ||
					es.carrier_handle != wire_handle::kInvalid;
			if (row_frozen || respawned_this_record) row_channel_disarm(es);
		}
		// A free-standing record clears any retained seat-local pose — the
		// relation is cleared before every record (D-NET-195); carrier-local
		// records atomically refresh it in the second pass below.
		// A vehicle wreck's short form re-lands the full frozen orientation;
		// treat it as the live-snap branch of the read [orig: the conditional
		// live stores @0x460930..0x460A50].
		const bool force_live_snap = respawned_this_record ||
				(rec.cls == EntityClass::Vehicle && rec.vehicle.is_dead_pose);
		if (!skip_pos && !(remote_motion_mode_ && wire_dead)) {
			// Retail's client read skips the position path entirely for a
			// wire-dead record [orig: the case-2 dead branch -> LABEL_151, no
			// position store]; the snap fold keeps its historical apply
			// (host/SP loopback rows refresh at full rate).
			const int32_t wx = fu.anchor_x + network_decompress_fixedpoint(cx);
			const int32_t wy = fu.anchor_y + network_decompress_fixedpoint(cy);
			const int32_t wz = fu.anchor_z + network_decompress_fixedpoint(cz);
			land_compact_pose(es, wx, wy, wz, has_heading_target, heading_target,
			                  force_live_snap);
		} else if (skip_pos && rec.cls == EntityClass::Vehicle &&
				has_heading_target) {
			// Carrier-local vehicle positions defer to the second pass, but the
			// wire euler stays world-absolute and lands LIVE: a deck-carried
			// vehicle is a carried OBJECT (the bit0-flagged attach class
			// @0x43C14A — distinct from seat mounts' 0x40) whose mover is
			// bit0-skipped, and the per-tick seat-follow owns its motion between
			// records, so there is no chase to consume a staged heading
			// [orig: the untransformed entity+576 store @0x4607f5]. Keep the
			// staged slot coherent for a later carrier-clear record.
			es.heading_bam = heading_target;
			es.net_smooth_heading = heading_target;
		}
	}

	// Resolve carrier-local children only after the complete frame has upserted and
	// updated every carrier. If the carrier is genuinely absent, leave the child's
	// prior world pose/yaw untouched, matching the retail record-drop path.
	for (const PendingCarrierPose &pending : pending_carrier_poses) {
		ClientEntityState *child = state_.find(pending.child_handle);
		const ClientEntityState *carrier = state_.find(pending.carrier_handle);
		// A fold may contain several records for one child. An earlier resolved
		// carrier must not overwrite a later unresolved switch/dismount.
		if (child == nullptr || carrier == nullptr ||
				child->carrier_handle != pending.carrier_handle)
			continue;
		// Compose against the carrier's LIVE (chased) pose — retail lifts through
		// the carrier entity's current +4..+0x18 block [orig: @0x4c10d4/@0x4608ce].
		const WorldPose w = network_transform_local_to_world(
				network_decompress_fixedpoint(pending.cx),
				network_decompress_fixedpoint(pending.cy),
				network_decompress_fixedpoint(pending.cz), carrier->x, carrier->y,
				carrier->z, uint32_t(carrier->heading_bam),
				uint32_t(carrier->pitch_bam), uint32_t(carrier->roll_bam));
		// Retain the seat-local offset for the per-tick carrier-follow: the
		// rider's rendered pose rides the carrier attach every frame in retail
		// [orig: the seat attach @0x4946D0/@0x494752]. The row is
		// live-snapped here (the recompose owns it from the next tick).
		child->net_seat_local[0] = network_decompress_fixedpoint(pending.cx);
		child->net_seat_local[1] = network_decompress_fixedpoint(pending.cy);
		child->net_seat_local[2] = network_decompress_fixedpoint(pending.cz);
		child->net_seat_local_yaw_byte = pending.local_yaw_byte;
		child->net_seat_compose_yaw = pending.compose_yaw;
		child->net_seat_valid = true;
		child->net_has_compact = true;
		child->x = w.x;
		child->y = w.y;
		child->z = w.z;
		// World yaw byte = carrier yaw + local yaw; BAM addition holds in the
		// 8-bit ring used by the compact view. Vehicle records keep their
		// world-absolute wire euler instead (compose_yaw false) [orig: the
		// untransformed entity+576 store @0x4607f5].
		if (pending.compose_yaw) {
			child->yaw_byte = uint8_t(carrier->yaw_byte + pending.local_yaw_byte);
			child->heading_bam = io::bam_add(
					carrier->heading_bam,
					static_cast<int32_t>(
							uint32_t(pending.local_yaw_byte) << 24));
		}
	}

	refresh_carried_entities();

	++state_.frames_applied;
	state_.mark_changed();
}

} // namespace opennova::replication
