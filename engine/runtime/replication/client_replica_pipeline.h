#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <variant>

#include <net/npwire/ingame_decode.h> // EntityClass + WeaponReload (a per-family decode-header split candidate)
#include <net/npwire/emote_wire.h> // EmoteBroadcast (S2C 0x2D)

#include <runtime/replication/client_state.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/inmatch/session_transport.h>

namespace opennova::world {
class IRootMotionSource;
struct WeaponTable;
struct LiveRound;
} // namespace opennova::world

namespace opennova::terrain {
struct TerrainHeightField;
} // namespace opennova::terrain

namespace opennova::replication {

// The local client's decode pump: drains S2C datagrams off the loopback and folds
// them into a ClientState via the witnessed ingame_decode codec. This is the "local
// client decodes them via engine/net/novaworld/ingame_decode" half of the SP in-process
// listen server (ADR 0011). The same ClientState feeds the Godot present pass.
struct MedicVoiceRequest {};
// The two wire routes share health clearing but write different state before
// invoking the class callback. Keep the route through the native-world fold.
struct EntityDeathEvent {
    uint16_t entity_handle = 0xFFFF;
    int16_t death_anim_state_id = 0;
    int16_t hit_section = 0;
    bool item_state = false;
    // Entity_KillBySlotId's flags, the class callback's third argument: 0 for
    // the 0x26 kill, 1 (the silent death) for the 0x4E join-window kill
    // [orig: @0x42EC78 (0x26) / @0x4318b5 (0x4E)].
    int32_t kill_flags = 0;
};
// A chat line on channel 13 (local) names its sender's slot: the sender's
// person becomes the map's tracked target. The slot resolves at the effect
// pass, which owns the entities. [orig: Chat_DispatchToChannel @0x42B9CD..
// 0x42BA09 — slot+0x24, PlayerSlot_IsEntityInGame @0x434220,
// HUD_SetTrackedEntityTarget @0x59D050]
struct LocalChatSpeaker {
    uint8_t slot = 0;
};
// A tip event the client's own receive legs raise (hud/tip_system.h
// TipEvent): the spectator begin on the S2C 0x0A death-screen edge and on
// the own slot's S2C 0x4D. The effect pass queues it on the world's tip
// events. [orig: NapiNPClientMsg_0x00A @0x42ffd8..0x42ffdf;
// NapiNPClientMsg_HandleSpawnSlot @0x431857..0x43185e]
struct TipEventCommand {
    uint8_t event = 0;
};
using ClientEffectCommand = std::variant<PlaySoundCommand, MedicVoiceRequest,
        TrackedPlayerVoice, GameEventRecord, ExplosionEffectRecord, EntityDeathEvent,
        EmoteBroadcast, LocalChatSpeaker, ClientSquadEvent, TipEventCommand>;

class ClientReplicaPipeline {
public:
	using ItemClassResolution = std::optional<EntityClass>;
	using ItemClassResolver =
			std::function<ItemClassResolution(uint16_t)>;

	ClientReplicaPipeline();
	explicit ClientReplicaPipeline(std::function<EntityClass(uint16_t)> resolver);

	// Drain every pending S2C datagram and apply it to the held ClientState. Used for the
	// host-as-client loopback path (inner {tag,body} datagrams, no handshake).
	void pump(ISessionTransport &channel);

	// Fold ONE already-decoded inner S2C body (tag + body, no NWU/SCRK framing) into the
	// ClientState. The remote-joiner path calls this for each record of
	// JoinerConnection's inbound_reducer stream — the canonical, packet-ordered
	// applied stream (the per-family vectors are diagnostic views only);
	// pump() is the thin loopback loop over it. Exactly ONE of {pump,
	// apply-per-body} drives a given ClientState per frame (one fold path per
	// role) so frames_applied / seen_this_frame stay coherent.
	void apply(uint8_t tag, const std::vector<uint8_t> &body);

	// Advance the remote lean integrator one body tick (called once per client
	// frame). [orig: decay @0x4b5c97, then the ramp @0x4b7dbf/@0x4b7dd6]
	void tick_lean();
	void tick_arms_dip();
	// Wire handles whose compact record was dropped because its carrier could
	// not be resolved (the retail bail queues a C2S 0x0F entity request); the
	// embedding runtime drains and sends them once per frame.
	std::vector<uint16_t> drain_carrier_repair_requests();
	void tick_recoil();
	// Age the retained 0x40/0x6B banks by `elapsed` ticks. Persistent 0x10
	// slots do not age; the transient bank clears on expiry while the special
	// bank floors its lifetime at zero keeping the handle; live links re-arm
	// their slot's lifetime and handle and clear it when they lapse. Retail
	// runs it from the HUD pass's radar update with that update's elapsed
	// tick count, so the session's frame calls it (inmatch step_hud_radar
	// from Session's radar step) [orig: MapOverlay_UpdateTimers @0x5BFCE0,
	// called by Radar_UpdateContacts @0x59a9ce].
	void age_minimap_overlays(uint32_t elapsed);
	// Once per client tick: regular (non-special) markers refresh pose and
	// known from the decoded entity, mirroring retail's draw-time pool read.
	// [orig: Render_MinimapSlotBlip @0x5be4ac]
	void refresh_minimap_live_markers();
    using GuidedRoundResolver = std::function<world::LiveRound *(int16_t)>;
    void set_guided_round_resolver(GuidedRoundResolver resolver) { guided_round_resolver_ = std::move(resolver); }
    // Fire synchronously at the receive boundary, before a following 0x44.
    void set_round_receiver(std::function<void(const ClientRoundEvent &)> receiver) { round_receiver_ = std::move(receiver); }

	// JOINER role only (net-re §5.38e, D-NET-196): switch the 0x0A fold from
	// live-pose snap to smooth-target STAGING, and enable tick_remote_motion.
	// The host/SP roles keep the snap fold — their loopback view refreshes at
	// full rate and the authority never interpolates (D-NET-89).
	void set_remote_motion_mode(bool enabled) { remote_motion_mode_ = enabled; }

	// The replica contact-resolver seam (net-re §5.38e, D-NET-196): when the
	// embedding sim provides a resolver, each armed Player/Infantry row's
	// settle runs the FULL movement collision resolver — candidate-model
	// contacts and push-out, world-person + replica-peer repulsion, and the
	// ground probe THROUGH candidate models — in place of the bounded
	// terrain-column subset. Retail runs remote organics through the ordinary
	// org movers whose shared tail calls the resolver ungated
	// [orig: Entity_UpdateInfantryPlayerBody call @0x4B7CF4;
	//  Entity_UpdateInfantryAI @0x4BF7FA; resolver @0x4B2BD0]. The peers span
	// carries every live undead replica organic this tick (the resolver's
	// person-repulsion needs rows the world tables cannot see). Returns the
	// signed foot clearance (feet Z - resolved ground Z).
	struct ReplicaPeerSphere {
		uint16_t handle = 0xFFFF;
		int32_t x = 0, y = 0, z = 0;
		int32_t radius = 0;
	};
	struct ReplicaContactQuery {
		uint16_t row_handle = 0xFFFF;
		uint16_t type_id = 0;
		bool is_player_class = false; // org2 (player body) vs org1 (NPC) shape
		int32_t pos[3] = {};          // in/out, 16.16 mission space
		int32_t vel_xy[2] = {};       // this tick's planar root step (the
		                              // resolver's moving/full-update discriminant)
		int32_t vel_z = 0;            // in/out — the skip band reverts + zeroes
		int32_t capsule_bottom = 0;   // anim-frame capsule extents
		int32_t capsule_top = 0;
		int32_t source_bound_radius_q16 = 0; // exact entity+0 init result
		int32_t anim_state_id = 0;
		uint32_t anim_state_flags = 0;
		uint32_t tick = 0;
		const ReplicaPeerSphere *peers = nullptr;
		int32_t peer_count = 0;
		uint32_t entity_flags = 0;    // in/out — the row's retail-Flags mirror
		                              // (rm_entity_flags); the resolver's latch
		                              // sites read/write it as they do
		                              // ent->flags on a registry row
		uint16_t out_ground = 0xFFFF; // ground-probe hit (wire handle)
	};
	using ReplicaContactResolver = std::function<int32_t(ReplicaContactQuery &)>;
	void set_replica_contact_resolver(ReplicaContactResolver resolver) {
		replica_contact_resolver_ = std::move(resolver);
	}
	using ReplicaBoundRadiusResolver = std::function<int32_t(uint16_t type_id)>;
	void set_replica_bound_radius_resolver(ReplicaBoundRadiusResolver resolver) {
		replica_bound_radius_resolver_ = std::move(resolver);
	}

	// The deck-ride carrier seam (D-NET-196 replica tails): a row whose
	// contact resolve grounded it on an entity follows that carrier's
	// per-tick pose delta — translation plus the rotate-about-carrier —
	// exactly as retail's org movers ride groundEntity at mover top
	// [orig: org2 @0x4b52a0..0x4b5726; org1 @0x4ba45d..; the standalone twin
	//  Entity_InterpolateFromParentDelta @0x4a8dc0]. The embedding sim
	// resolves the probe's wire handle to the carrier's LIVE pose plus its
	// mover-entry savedLivePose stamp (Entity::saved_live_* — retail
	// +0x80..+0x94), and the ride reads (live - saved) per rider tick, the
	// witnessed source pair. Angles are BAM32; bound_radius is 16.16 (the
	// ride drops when the unmounted rider strays beyond it
	// [orig: @0x4b52a7..0x4b52ff]).
	struct CarrierPose {
		int32_t pos[3] = {};
		int32_t yaw = 0, pitch = 0, roll = 0; // BAM32
		int32_t saved_pos[3] = {};            // the mover-entry stamp
		int32_t saved_yaw = 0, saved_pitch = 0, saved_roll = 0;
		int32_t bound_radius = 0;             // 16.16
	};
	using CarrierPoseProvider = std::function<bool(uint16_t handle, CarrierPose &out)>;
	void set_carrier_pose_provider(CarrierPoseProvider provider) {
		carrier_pose_provider_ = std::move(provider);
	}

	// Mission water plane for the replica water/float channel (16.16;
	// has_water false = no water in this world). Fed per pump by the
	// embedder from the env state [orig: g_EnvWaterHeightFixed @ 0x26C6454].
	void set_water_z(int32_t z, bool has_water) {
		water_z_ = z;
		has_water_ = has_water;
	}

	// The per-class between-update mover, one call per 62.5 Hz logic tick after
	// the recv fold (retail order: Client_ProcessNetworkFrame first, entity
	// movers after). Chases each armed row's live pose/heading toward its staged
	// wire target with the witnessed per-class math:
	//  - Player rows: the org2 body-pass chase [orig: Entity_UpdateInfantryPlayerBody
	//    @ 0x4B40E0, chase @ 0x4B4470..0x4B46C0] — incl. the own-player soft
	//    position reconciliation (self_handle; bucket 48 moving / 512 still).
	//  - Infantry rows: the org1 motor fall-through [orig: Entity_UpdateInfantryAI
	//    @ 0x4b9a8c] + the promoted-heading quarter-step body chase [orig: @ 0x4be8fd].
	//  - Vehicle rows: the family chase template [orig: Entity_UpdateWatercraftPhysics
	//    @ 0x48D480 et al.]. Rows the embedding sim flags net_world_mover are
	//    instead predicted by the world-side family movers (world/vehicle_motor,
	//    all four families landed) and skipped here; the row-side chase remains
	//    for traitless vehicles and lib-only embedders.
	// No-op unless remote-motion mode is enabled.
	void tick_remote_motion(uint16_t self_handle);
	// Every armed player row's secondary channel for one tick (the advance,
	// then the 16-tick selection keyed on `tick`), the own row excepted.
	void tick_row_weapon_channels(uint16_t self_handle, uint32_t tick);

	// Recompose every carried row after its carrier's mover has completed. The
	// ordinary library-only tick_remote_motion path calls this as its final
	// phase. Embedders with a later world-side mover (the joiner vehicle
	// prediction seam) call it once more after mirroring those final carrier
	// poses back into ClientState. Keeping this phase explicit prevents pool
	// iteration order from leaving an earlier child one mover tick behind a
	// later carrier.
	// tick_sweep is true only from the per-tick mover call: it advances the
	// pure-client 128-tick stale-carrier sweep (C2S 0x0F for carrier + child,
	// local child destroy pending authority re-spawn) — record applies must
	// not double-run the cadence [orig: the @0x440d41..0x440e2f sweep].
	void refresh_carried_entities(bool tick_sweep = false);

	// S2C 0x5D empty-slot sweep: retire one RAW pool-0 slot index and everything
	// attached to it. The decoded view is the client's entity pool, so
	// Entity_Destroy's effect here is removing the ROW (not flagging it) —
	// omission from an 0x0A is deliberately not a despawn signal in this view, so
	// a retained row would keep blocking projectiles as a person proxy forever.
	// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 -> Pool_GetEntryUnchecked(0, idx)
	//  + Entity_Destroy]
	void destroy_pool0_slot(uint16_t pool0_index);

	// S2C 0x50 team assign leg 2: `entity->Team = team` for ANY pool 0..4 entity on
	// a non-authority client. Upserts so an assignment that precedes the entity's
	// spawn record is not lost (an unresolved row keeps type_id 0, which the render
	// and collision passes both skip).
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the team store @0x4319ee]
	void apply_team_assign(uint16_t handle, uint8_t team);
	// A player's identity pair (Flags & 0x100 rows): the NetId word the
	// character-slot lookup keys its avatar on and the animSlot byte, as S2C
	// 0x50 and 0x51 rebind them. An id the character registry does not hold
	// stays raw here; the presentation resolves it to the per-side default the
	// same way it resolves a 0x0C record's.
	// [orig: NapiNPClientMsg_TeamAssign @0x431b3a (animSlot) / @0x431b46
	//  (NetId), the MinimapSlot_HasEntity fallback @0x431b4d..0x431b77, the
	//  CharacterEntity rebind @0x431b8c..0x431b91]
	void apply_player_identity(uint16_t handle, uint16_t net_id, uint8_t anim_slot);
	// S2C 0x51: one entry of the host's team-change list (the C2S 0x29
	// continuation is the connection's).
	// [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0]
	void apply_team_change_confirm(const std::vector<uint8_t> &body);

	const ClientState &state() const { return state_; }
	ClientState &state() { return state_; }
	std::uint32_t frames_applied() const { return state_.frames_applied; }
	std::uint64_t revision() const { return state_.revision; }
	std::uint64_t topology_revision() const {
		return state_.topology_revision;
	}
	std::size_t unknown_tags() const { return unknown_tags_; }
	// Bodies whose tag IS handled but whose payload failed to decode; kept
	// apart from unknown_tags() so malformed known traffic is visible.
	std::size_t malformed_bodies() const { return malformed_bodies_; }

	// One-shot gameplay notifications surfaced by apply(). Draining keeps the
	// decoded ClientState persistent while preventing event replay on later frames.
	std::vector<ClientRoundEvent> drain_round_events();
	// S2C 0x1E game events folded by apply() — the kill/objective/medic feed
	// lane (client_replica_feed.cpp). Drained once per frame by the embedder,
	// which owns the roster names the lines are formatted against.
	std::vector<ClientGameEvent> drain_game_events();
	// S2C 0x14 chat lines folded by apply() — the player-chat lane
	// (client_replica_feed.cpp). Drained once per frame by the embedder, which
	// routes each line by the HUD channel table and posts it to its ring.
	std::vector<ClientChatLine> drain_chat_lines();
	// S2C 0x32 join/leave records folded by apply() (client_replica_feed.cpp),
	// drained once per frame by the embedder that owns gametext.
	std::vector<ClientGameText> drain_game_texts();
	std::vector<WeaponReload> drain_weapon_reloads();
	std::vector<ClientEffectCommand> drain_effect_commands();
	void post_chat_line(ClientChatLine line) {
		line.feed_order = next_feed_order_++;
		pending_chat_lines_.push_back(std::move(line));
	}
	// A ring line the client raises itself (the CMAP go code) takes the next
	// dispatch stamp, so it lands after every line already dispatched.
	uint32_t claim_feed_order() { return next_feed_order_++; }
	// A mission start lowers the round-over latch; a joiner's runtime lives
	// across its round-cycle reloads, so each load calls this
	// [orig: Game_StartMission @0x524a1f].
	void begin_mission() { state_.spawn_success_gate = false; }
	// S2C 0x23 WAC remote commands the fold accepted this frame; the embedding
	// role runs each registry row's handler (wac::run_remote_command) against
	// its world. A non-authority endpoint only: the retail handler returns
	// before decoding on the authority. [orig: GameMode_DispatchRemoteCommand
	// @0x4F81E0 — `!is_authority` @0x4f8249]
	std::vector<ScriptRemoteCommand> drain_script_remote_commands();
	// S2C 0x3F HUD relays the fold accepted this frame (the objective
	// notification, kind 0, and the mission-text chat relay, kind 1); the
	// joiner role replays each against its world.
	// [orig: NapiNPClientMsg_0x03F @0x42BB20]
	std::vector<ObjectiveNotification> drain_objective_notifications();

	// Install the items.def-derived per-type classifier — the table the retail client
	// itself dispatches 0x0A records through (each type's serialize callback, seeded
	// from the *_function class tag at items.def load [orig: itemDef+356 dispatch
	// @0x50f2e2 / ItemList_FindIndexByTypeId]). Every present catalog entry
	// OUTRANKS the learned/heuristic chain in classify(): the 0x0D pool blanket
	// brands every pool-1 type Vehicle, which mis-sizes a no-callback item's
	// header-only record (e.g. an `ewep` emplacement) and desyncs the rest of the
	// frame. nullopt falls through to the learned map, then the phase-1 resolver;
	// a present Unknown is a known unresolved definition and fails closed.
	void set_item_class_resolver(ItemClassResolver resolver);
	// Install the embedder's items.def catalog, the one table the host's traits
	// sweep reads too. Its wire classes become the classifier above, and each
	// type's def facts (the def type +0x5C, the EWeap attrib +0x54 bit 0x20)
	// feed the destroy's refNum walk (erase_entity_tree). Without a catalog a
	// row has no def and the walk never runs. Null clears both.
	void set_item_catalog(std::shared_ptr<const ItemReplicationCatalog> catalog);

	// Inject the embedder's .adm root-motion source (JOINER role): armed rows
	// (rm_adm_id >= 0, stamped by the embedder) advance their own AnimMap
	// primary channel each tick and integrate the rotated root delta after the
	// chase — retail's remote-body dead reckoning [orig: the class movers run
	// the full body pass; root integration @0x4B7CB4 / @0x4BF684]. Null (the
	// lib-only embedders) = the truthful chase-only degradation.
	void set_root_motion_source(world::IRootMotionSource *source) {
		root_motion_ = source;
	}
	// The joiner's weapon table: the held record's special_hold the player
	// rows' secondary selection reads by the wire ADM index. Null = kind 0.
	void set_weapon_table(const world::WeaponTable *weapons) { weapon_table_ = weapons; }
	// The S2C 0x2D emote stamp on a decoded player row (a speaker with no
	// world twin): an armed row whose map authors emote_N takes it as its
	// secondary target (client_replica_weapon_channel.cpp). False = no stamp.
	bool stamp_row_emote(uint16_t handle, uint8_t emote);

	// Install the joiner's terrain column for the bounded remote-person
	// post-motion probe. The pipeline owns neither field nor backing buffers.
	// [orig: Entity_UpdateInfantryPlayerBody resolver call @0x4B7CF4;
	// Entity_UpdateInfantryAI caller @0x4BF7FA;
	// Entity_MovementCollisionResolver @0x4B2BD0]
	void set_remote_motion_terrain(
			const terrain::TerrainHeightField *terrain) {
		remote_motion_terrain_ = terrain;
	}

	// The phase-3 0x0A objective block has no on-wire discriminator. apply()
	// learns the shared g_GameType from S2C 0x08 field 3 / 0x7B `extra`;
	// replay/bootstrap callers may also seed it explicitly before a midstream 0x0A.
	void set_game_type(uint32_t game_type) { game_type_ = game_type; game_type_known_ = true; }
	uint32_t game_type() const { return game_type_; }
	// True once a folded 0x08 / 0x7B (or set_game_type) wrote the word.
	bool game_type_known() const { return game_type_known_; }

	// The 0x1D header-form discriminator's session half: retail reads
	// g_NapiNPCtx.is_in_session; a joiner is always in-session, while the
	// listen host's loopback replica passes world.mp_session (retail SP never
	// runs this client path at all). [orig: NapiNPClientMsg_0x01D @0x43086c]
	void set_mp_session(bool mp_session) { mp_session_ = mp_session; }
	// The listen host's own local client: its loopback 0x0A is the retail
	// header-only frame (anchor, flags1, phase byte, phase-0 block) and the fold
	// returns where retail's parser does [orig: NapiNPClientMsg_0x00A @0x430174].
	// Entities are presented from the host's own pools, never from this view.
	void set_authority_recipient(bool authority_recipient) {
		authority_recipient_ = authority_recipient;
	}
	bool authority_recipient() const { return authority_recipient_; }
	bool mp_session() const { return mp_session_; }

	// Recipient context for S2C 0x59's friend/foe item selection. Retail
	// compares the placing owner's team with the local player's team and lets
	// multiplayer attribute 0x8000 force the enemy presentation variant.
	void set_viewer_handle(uint16_t handle) { viewer_handle_ = handle; }
	uint16_t viewer_handle() const { return viewer_handle_; }
	void set_mp_attributes(uint32_t attributes) { mp_attributes_ = attributes; }
	uint32_t mp_attributes() const { return mp_attributes_; }
	// This client's own roster slot (retail g_LocalPlayerSlotId): the S2C 0x4D
	// fold tells its own slot's notice from another's [orig:
	// NapiNPClientMsg_HandleSpawnSlot @0x4317f0].
	void set_local_player_slot(uint8_t slot) { local_player_slot_ = slot; }

	// The death screen's spectate writers over the replica rows
	// (client_replica_spectate.cpp): the local player's own wire handle the
	// walk starts from and excludes (the embedder stamps it each frame), the
	// target walk (direction 0 clears target and sub-mode), the sub-mode cycle
	// and the SPECTATORTARGET track.
	// [orig: Spectator_CycleTarget_0 @0x52ac20; sub_52AFF0 @0x52aff0;
	//  Entity_TrySetMinimapTrackTarget @0x52abc0]
	void set_spectate_local_handle(uint16_t handle) { spectate_local_handle_ = handle; }
	void spectate_cycle_target(int direction);
	void spectate_cycle_mode(int direction);
	void spectate_track(uint16_t handle);
	// The death screen's three spectator actions by their dispatch codes: 500
	// cycles the sub-mode, 501 / 502 step the target +1 / -1 in a chase or
	// first-person sub-mode [orig: Input_HandleActionBinding cases 500
	// @0x49bd58, 501 @0x49bd67, 502 @0x49bd89; catalog rows 110..112].
	static constexpr int kSpectateActionCycleMode = 500;
	static constexpr int kSpectateActionNextTarget = 501;
	static constexpr int kSpectateActionPrevTarget = 502;
	void spectate_action(int code);

	// The local player's roster slot (entity+0x154, which the host stamps
	// from the slot id S2C 0x04 byte 17 carries): the slot
	// set_local_player_slot latched, whether or not its entity is bound.
	int local_roster_slot() const;
private:
	// A destroyed spectate target re-picks [orig: Entity_Destroy @0x43e820].
	void spectate_on_entity_removed(uint16_t handle);
	void apply_spectator_mode(const std::vector<uint8_t> &body); // 0x75
	void queue_carrier_repair(uint16_t handle);
	std::vector<uint16_t> carrier_repair_requests_;
	// Shared S2C 0x13 / 0x26 death fold (retail gates + row health + the
	// surfaced record). [orig: NapiNPClientMsg_EntityDeath @0x42EB50 /
	// Entity_KillBySlotId @0x42BCE0]
	void apply_entity_death(uint16_t handle_packed, int16_t value, bool item_state = false,
			int32_t kill_flags = 0);
	// S2C 0x4E: one page of the host's join-window kill list (the C2S 0x28
	// continuation is the connection's).
	// [orig: NapiNPClientMsg_HandleBatchKill @0x431870]
	void apply_batch_kill(const std::vector<uint8_t> &body);
	void apply_capture_zone_overlay(const std::vector<uint8_t> &body);
	void apply_minimap_overlay_batch(const std::vector<uint8_t> &body);
	// The death-screen folds live together in client_replica_death.cpp;
	// this remains one pipeline with no secondary client state or router.
	void apply_death_camera_target(const std::vector<uint8_t> &body);
	void apply_player_downed_state(const std::vector<uint8_t> &body);
	void apply_spawn_wave_status(const std::vector<uint8_t> &body);
	void apply_score_delta_sound(const std::vector<uint8_t> &body);
	void apply_script_remote_command(const std::vector<uint8_t> &body); // 0x23
	void apply_objective_notification(const std::vector<uint8_t> &body); // 0x3F
	// S2C 0x56 -- one chunk of the end-of-round stat board. Reassembles into
	// ClientState::end_round and decodes when the board completes.
	void apply_end_round_header(const std::vector<uint8_t> &body);
	void apply_end_round_stats_chunk(const std::vector<uint8_t> &body);
	void apply_frame_update(const std::vector<uint8_t> &body);
	// Load-time world-stream spawn/static batches (§5.2a) -> ClientState upsert. Each carries
	// ABSOLUTE world positions (no anchor) + the entity identity/type, so spawn-only entities
	// (statics/markers) and not-yet-moving organics are present before any 0x0A motion arrives.
	void apply_full_entity_spawn(const std::vector<uint8_t> &body); // 0x18 repair
	void apply_organic_spawn(const std::vector<uint8_t> &body); // 0x0C pool-0
	void apply_pool_spawn(const std::vector<uint8_t> &body);    // 0x0D pool-1
	void apply_static_batch(const std::vector<uint8_t> &body);  // 0x10 pool-2
	void apply_pool3_batch(const std::vector<uint8_t> &body);   // 0x20 pool-3
	// The live placed-device lifecycle (client_replica_placed_device.cpp).
	void apply_game_event(const std::vector<uint8_t> &body);   // 0x1E (the feed)
	void apply_weapon_restrictions(const std::vector<uint8_t> &body);
	void apply_text_command(const std::vector<uint8_t> &body);
	void apply_chat_broadcast(const std::vector<uint8_t> &body); // 0x14 (player chat)
	void apply_player_list(const std::vector<uint8_t> &body);  // 0x16 (the Tab board)
	void apply_player_sync(const std::vector<uint8_t> &body);  // 0x46 (its name join)
	// The command map's squad and waypoint legs (client_replica_squad.cpp).
	void apply_squad_join(const std::vector<uint8_t> &body);      // 0x71
	void apply_squad_order(const std::vector<uint8_t> &body);     // 0x72
	void apply_fireteam_set(const std::vector<uint8_t> &body);    // 0x73
	void apply_squad_recruited(const std::vector<uint8_t> &body); // 0x74
	void apply_go_code(const std::vector<uint8_t> &body);         // 0x78
	void apply_waypoint_create(const std::vector<uint8_t> &body); // 0x33
	void apply_destroy_entity(const std::vector<uint8_t> &body);  // 0x7C
	void apply_clan_roster(const std::vector<uint8_t> &body);  // 0x6A (the clan registry)
	void apply_visible_players(const std::vector<uint8_t> &body); // 0x4C (the slot pointer table)
	void apply_spawn_slot_notice(const std::vector<uint8_t> &body); // 0x4D (a player joined)
	// Whether a pool-0 handle is a player entity (the Flags 0x100 class bit):
	// a decoded Player row, or the entity a bound roster slot drives.
	bool is_player_entity(uint16_t handle) const;
	void apply_formatted_game_text(const std::vector<uint8_t> &body); // 0x32 (join/leave lines)
	void apply_entity_routed(const std::vector<uint8_t> &body); // 0x44 (guided, §5.15)
	void apply_deployed_item(const std::vector<uint8_t> &body); // 0x59 pool-1
	void apply_entity_remove(const std::vector<uint8_t> &body);  // 0x12
	void apply_objective_entity_state(const std::vector<uint8_t> &body); // 0x2F
	void erase_entity_tree(uint16_t root_handle);
	uint32_t begin_entity_lifetime(uint16_t handle);
	void discard_entity_notifications(uint16_t handle);
	// Land one decoded compact world sample on a row: live snap in snap mode /
	// on the forced edges (respawn, vehicle dead-pose); smooth-target staging +
	// interpProgress reset in remote-motion mode (§5.38e stage-only reads).
	void land_compact_pose(ClientEntityState &es, int32_t wx, int32_t wy,
	                       int32_t wz, bool has_heading, int32_t heading_bam,
	                       bool force_live_snap);

	// Effective record classifier for the 0x0A event loop: the items.def table (when
	// installed) wins, then the class LEARNED from the world spawn stream, then the
	// injected resolver. The retail client classifies via each type's items.def
	// serialize callback [orig: itemDef+356 dispatch @0x50f2e2 /
	// ItemList_FindIndexByTypeId] — that is item_resolver_. Without an items table,
	// a 0x0D pool-1 spawn is the witnessed signal that a type replicates as a VEHICLE
	// (pool 1 = the vehicle pool), so the view records type->Vehicle there and decodes
	// the 15/21-B vehicle compact body for those types (a Player/Infantry misparse
	// would desync the whole record chain).
	EntityClass classify(uint16_t type_id) const;

	ClientState state_;
	world::IRootMotionSource *root_motion_ = nullptr;
	const world::WeaponTable *weapon_table_ = nullptr;
	const terrain::TerrainHeightField *remote_motion_terrain_ = nullptr;
	uint32_t rm_tick_counter_ = 0; // the leg re-plant window clock [orig: tick&63]
	bool remote_motion_mode_ = false;
	ReplicaContactResolver replica_contact_resolver_;
	// tick_remote_motion's per-tick replica-peer sphere table (reused capacity).
	std::vector<ReplicaPeerSphere> contact_peer_scratch_;
	ReplicaBoundRadiusResolver replica_bound_radius_resolver_;
	CarrierPoseProvider carrier_pose_provider_;
	int32_t water_z_ = 0;
	bool has_water_ = false;
	ItemClassResolver item_resolver_;                    // items.def table (authoritative)
	std::shared_ptr<const ItemReplicationCatalog> item_catalog_; // the items.def def facts
	// A row's def as the destroy reads it (entity+0x20): its wire type's
	// catalog profile, or null without a catalog or a definition.
	const ItemReplicationProfile *item_def(uint16_t type_id) const;
	std::function<EntityClass(uint16_t)> resolver_;      // phase-1 heuristic fallback
	std::unordered_map<uint16_t, EntityClass> learned_classes_;
	std::vector<ClientRoundEvent> pending_round_events_;
    GuidedRoundResolver guided_round_resolver_;
    std::function<void(const ClientRoundEvent &)> round_receiver_;
	std::vector<ClientGameEvent> pending_game_events_;
	std::vector<ClientChatLine> pending_chat_lines_;
	std::vector<ClientGameText> pending_game_texts_;
	// The dispatch stamp the ring-bound records take (ClientGameEvent::feed_order).
	uint32_t next_feed_order_ = 0;
	std::vector<WeaponReload> pending_weapon_reloads_;
	std::vector<ClientEffectCommand> pending_effect_commands_;
	std::vector<ScriptRemoteCommand> pending_script_remote_commands_;
	std::vector<ObjectiveNotification> pending_objective_notifications_;
	// Survives row deletion until this pipeline is destroyed.
	std::unordered_map<uint16_t, uint32_t> spawn_revisions_;
	std::size_t unknown_tags_ = 0;
	std::size_t malformed_bodies_ = 0;
	uint32_t game_type_ = 0;
	bool game_type_known_ = false;
	bool mp_session_ = false;
	bool authority_recipient_ = false;
	uint16_t viewer_handle_ = 0xFFFF;
	uint16_t spectate_local_handle_ = 0xFFFF;
	uint32_t mp_attributes_ = 0;
	uint8_t local_player_slot_ = 0;
	// Mission-seeded PRNG_Next16 stand-in shared by every decoded row in this
	// view. The body consumes one draw per person per tick even when recoil is
	// zero. Retail also has unrelated process-global consumers that this decoded
	// seam cannot honestly order against; algorithm and local call history after
	// the exact Game_StartMission seed remain bounded here.
	uint32_t prng16_ = 0;
};

} // namespace opennova::replication
