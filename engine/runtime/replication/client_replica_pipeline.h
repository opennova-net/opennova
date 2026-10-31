#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <variant>

#include <net/npwire/ingame_decode.h> // EntityClass + WeaponReload (a per-family decode-header split candidate)
#include <net/npwire/emote_wire.h> // EmoteBroadcast (S2C 0x2D)

#include <runtime/replication/client_state.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/world/physics_class_table.h>
#include <runtime/world/title_rules.h>
#include <runtime/inmatch/session_transport.h>

namespace opennova {
struct Protocol; // net/protocol/protocol.h
}

namespace opennova::inmatch {
class CharacterRegistry; // runtime/inmatch/character_registry.h
} // namespace opennova::inmatch

namespace opennova::world {
class IRootMotionSource;
class AnimVariantRings;
struct RootMotionFrame;
struct WeaponTable;
struct LiveRound;
struct HitRecord;
struct DeployedRoundCopy;
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
// A dead player record, in receive order with the 0x13s around it: its anim byte lands on
// the body the record names. The replica rows fold it in the decode; the effect pass hands
// the client's own body its share (world::player_body_dead_record), after any 0x13 the same
// receive carried ahead of it.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the dead arms @0x4C100B..0x4C1027 /
//  @0x4C10F1..0x4C10FB, the byte @0x4C0F23]
struct PlayerDeadRecord {
    uint16_t entity_handle = 0xFFFF;
    int16_t anim_state_id = 0;
};
// A chat line on channel 13 (local) names its sender's slot: the sender's
// person becomes the map's tracked target. The slot resolves at the effect
// pass, which owns the entities. [orig: Chat_DispatchToChannel @0x42B9CD..
// 0x42BA09 — slot+0x24, Entity_IsPersonType @0x434220,
// HUD_SetTrackedEntityTarget @0x59D050]
// S2C 0x24 "SETFLASH1 [n]": the lightning sequencer's timer A. The effect
// pass applies it to the world's weather. [orig: NapiNPClientMsg_HandleTextCommand
// @0x429ecf..0x429ef5 -> g_EnvLightningTimerA]
struct LightningTimerCommand {
    int32_t timer_a = 16;
};
// S2C 0x37: one door row's state from the host, applied by the effect pass to
// the world's door records. [orig: NapiNPClientMsg_HandleWeaponSlotAction
// @0x431250]
struct DoorRowUpdate {
    uint16_t entity_handle = 0;
    int32_t state = 0;  // the i16 wire word, sign-extended [orig: @0x43127e]
    uint8_t number = 0; // the row number; 0 never lands
};
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
// S2C 0x0F with the death screen up: the live HUD declutter level goes to the
// blank level (hud/hud_toggles.h hud_toggles_death_screen). The effect pass
// hands it to the HUD owner; a 0x0F with the death screen down (every live
// retail join's first 0x0A carries the death bit clear) leaves the level alone.
// [orig: NapiNPClientMsg_0x00F `cmp g_DeathScreenActive, 0` @0x42e3f5 /
//  @0x42e407 -> `mov g_HUDDetailLevel, 3` @0x42e412 ->
//  CRenderState_SetLayerVisibility(3) @0x42e41c]
struct HudDetailBlankCommand {};
// S2C 0x28: one co-op dialog line the authority's playback started. The
// effect pass hands it to the presentation with the local player's class, the
// locale the clip resolves by. [orig: NapiNPClientMsg_0x028 @0x425B40 ->
// Dialog_PlayByNameAndSlot(g_LocalPlayerEntity->playerClass, name, line)
// @0x425b94 (Dialog_PlayByNameAndSlot @0x44E3F0)]
struct DialogLineCommand {
    std::string dialog_name;
    int16_t line = 0;
};
using ClientEffectCommand = std::variant<PlaySoundCommand, MedicVoiceRequest,
        TrackedPlayerVoice, GameEventRecord, ExplosionEffectRecord, EntityDeathEvent,
        PlayerDeadRecord, EmoteBroadcast, LocalChatSpeaker, ClientSquadEvent, TipEventCommand,
        LightningTimerCommand, DoorRowUpdate, DialogLineCommand, HudDetailBlankCommand>;

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

	// The remote lean integrator, one body tick per client frame in retail's two
	// halves: the decay at the body pass's head, the held keys' ramp at its end,
	// so the pass's eye (tick_remote_motion) reads the decayed, unramped lean.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97, the
	//  eye @0x4B699F..0x4B6BAB, the ramp @0x4B7DBF / @0x4B7DD6]
	// The decay and the arms dip (with the torso roll) take the walk's own-body
	// split (tick_remote_motion's self_handle): a row the walk will park (every
	// pool-0 row and those after) takes them in its own visit, ahead of it, in
	// tick_remote_motion_through_self or tick_remote_motion_after_self, as retail
	// runs them inside each body's own update (D-NET-196).
	// The exits both organic body updates take ahead of their per-tick legs
	// (client_replica_body_ticks.cpp): Flags bit 0 and no animation channel end the update
	// ahead of its dual update; the starved chase ends it after the channel and the chase's
	// count. A leg ahead of the row's visit asks body_update_ends_ahead (the progress as the
	// chase will test it), a leg after the visit body_update_ended (the visit's latch).
	static bool body_update_exits_ahead_of_channel(const ClientEntityState &es);
	static bool body_update_ends_ahead(const ClientEntityState &es);
	static bool body_update_ended(const ClientEntityState &es);
	void tick_lean_decay(uint16_t self_handle = 0xFFFFu);
	void lean_decay_row(ClientEntityState &es);
	// Every visited row's ramp (a row the walk parked ramps after its visit).
	void tick_lean_ramp();
	void lean_ramp_row(ClientEntityState &es);
	void tick_arms_dip(uint16_t self_handle = 0xFFFFu);
	void arms_dip_row(ClientEntityState &es);
	// Wire handles whose compact record was dropped because its carrier could
	// not be resolved (the retail bail queues a C2S 0x0F entity request); the
	// embedding runtime drains and sends them once per frame.
	std::vector<uint16_t> drain_carrier_repair_requests();
	// Remote organic rows whose death edge fired in tick_remote_motion since
	// the last drain (the self row excluded: the local body's own edge
	// screams). The embedding runtime plays each one's scream.
	std::vector<uint16_t> drain_death_edges();
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
	// S2C 0x3E (empty): every overlay slot and every linked row cleared, the
	// last message of the host's join tail [orig: NapiNPClientMsg_0x03E
	// @0x4226D0 -> MapOverlay_ResetAllSlots @0x5BE8D0; Server_OnPlayerJoin @0x51aaee].
	void reset_minimap_overlays();
    using GuidedRoundResolver = std::function<world::LiveRound *(int16_t)>;
    void set_guided_round_resolver(GuidedRoundResolver resolver) { guided_round_resolver_ = std::move(resolver); }
    // Fire synchronously at the receive boundary, before a following 0x44.
    void set_round_receiver(std::function<void(const ClientRoundEvent &)> receiver) { round_receiver_ = std::move(receiver); }
    // The S2C 0x59 found leg over the embedder's round pool
    // (world::RoundSim::match_deployed_item), run where the record lands; true
    // when a slot matched. Unset, no slot matches.
    using DeployedRoundWalk =
            std::function<bool(const DeployedItemSpawn &, world::DeployedRoundCopy &)>;
    void set_deployed_round_walk(DeployedRoundWalk walk) { deployed_round_walk_ = std::move(walk); }

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
		// The row's lifetime and respawn (its spawn and respawn revisions): the resolver's
		// per-row state starts over with each, as retail zeroes the slot's idle throttle
		// (+0x373) at a spawn and at a respawn's reset (inmatch::replica_resolve_state)
		// [orig: memset(slot, 0, 0x388) @0x42E7F1; Entity_ResetToSpawnState @0x4B9656].
		uint32_t row_lifetime = 0;
		uint32_t row_respawn = 0;
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
		// The row's savedLivePose (+0x80..+0x88): its mover's entry stamp
		// (net_saved_live_pose), which the resolver's skip test, plane gating and
		// victim delta read [orig: Entity_UpdateInfantryPlayerBody @0x4B4190..0x4B419C;
		// Entity_UpdateInfantryAI @0x4B9A53..0x4B9A6E].
		int32_t saved_live_pos[3] = {};
		bool has_saved_live = false;
		// The row's CameraOffset (+0x6C..+0x74) as its mover stored it, the resolver's
		// eye point [orig: Entity_MovementCollisionResolver @0x4B2EE2..0x4B2EFB]: a
		// player row's (eye_offset, the body update's non-local arm); none for an
		// NPC row, whose eye the resolver derives (the org1 restamp at a rest lean).
		int32_t eye_offset[3] = {};
		bool has_eye = false;
		// The row's mounted latch: a row attached to a seat (its record names one, carrier_handle)
		// carries the attach's Flags 0x40, live or dead, which the row's Flags mirror does not;
		// the resolver then suppresses every model push force.
		// [orig: Entity_AttachToVehicleSlot @0x4946D0 - `or ecx, 40h` @0x494760;
		//  Entity_DetachFromVehicle @0x4355F0 - Flags &= ~0x40 @0x435910;
		//  Entity_MovementCollisionResolver @0x4B2BD0 - the latch @0x4B2D2B..0x4B2D34]
		bool mounted_latch = false;
	};
	using ReplicaContactResolver = std::function<int32_t(ReplicaContactQuery &)>;
	void set_replica_contact_resolver(ReplicaContactResolver resolver) {
		replica_contact_resolver_ = std::move(resolver);
	}
	using ReplicaBoundRadiusResolver = std::function<int32_t(uint16_t type_id)>;
	void set_replica_bound_radius_resolver(ReplicaBoundRadiusResolver resolver) {
		replica_bound_radius_resolver_ = std::move(resolver);
	}
	// The def fields the replica dead tail reads, resolved by wire type from
	// the embedder's items.def: deathtime (def+0x890), LeaveCorpse
	// (attrib & 0x400000) and whether the def names a decay effect
	// (particledeath, the +0x412 word). No resolver, or an unknown type, runs
	// no dead tail.
	struct ReplicaDeathTraits {
		int32_t deathtime_ticks = 0;
		bool leave_corpse = false;
		bool decay_effect = false;
	};
	using ReplicaDeathTraitsResolver =
			std::function<bool(uint16_t type_id, ReplicaDeathTraits &out)>;
	void set_replica_death_traits_resolver(ReplicaDeathTraitsResolver resolver) {
		replica_death_traits_resolver_ = std::move(resolver);
	}
	// A player record's def re-pick: the embedder answers the wire type of the
	// items row its class's camouflage item binds at the mission climate (the
	// row ItemList_FindIndexByTypeId finds, the first row for a miss), or false
	// where none binds (no charattr / items tables installed; the client's own
	// record, which picks by its local body's class word, with no body or def).
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the climate pick
	//  @0x4C1248..0x4C129C, ItemList_FindIndexByTypeId @0x4C129D]
	using ReplicaPlayerCammoResolver =
			std::function<bool(uint16_t handle, uint8_t player_class, uint16_t &type_id)>;
	void set_replica_player_cammo_resolver(ReplicaPlayerCammoResolver resolver) {
		replica_player_cammo_resolver_ = std::move(resolver);
	}
	// A def re-pick's row: the wire type of the items row it bound, and the record class that
	// type classifies as (a player record's re-pick, the mission start's reload).
	void retype_row(ClientEntityState &es, uint16_t type_id) const {
		es.type_id = type_id;
		es.cls = classify(type_id);
	}
	// The embedder's global hit record, which the 0x13's class callback reads
	// [orig: g_ProjectileHitRecord @0xB7C620 via Projectile_GetHitRecord
	// @0x4E7000]. No resolver, or a null record, reads as one with no round.
	using ReplicaHitRecordResolver = std::function<const world::HitRecord *()>;
	void set_replica_hit_record_resolver(ReplicaHitRecordResolver resolver) {
		replica_hit_record_resolver_ = std::move(resolver);
	}
	// The 0x26 / 0x4E kill's store of its section into that record, run at
	// the fold; the embedder gates it on its own entity (a typed, live row)
	// [orig: Entity_KillBySlotId @0x42BD29..0x42BD47].
	using ReplicaKillSectionStore = std::function<void(uint16_t handle, int16_t section)>;
	void set_replica_kill_section_store(ReplicaKillSectionStore store) {
		replica_kill_section_store_ = std::move(store);
	}
	// Each row the client's Entity_Destroy takes (erase_entity_tree, the root and each member
	// its refNum walk destroys), told to the embedder at the destroy's reference clears: the
	// joiner's HUD target cursor lets go of a decoded remote person's row
	// (world::LocalPlayer::note_wire_entity_destroyed) [orig: Entity_Destroy @0x43E810 -
	// `cmp g_HUDTargetCursorEntity, esi` @0x43E89C, the clear @0x43E8A4].
	using ReplicaDestroyObserver = std::function<void(uint16_t handle)>;
	void set_replica_destroy_observer(ReplicaDestroyObserver observer) {
		replica_destroy_observer_ = std::move(observer);
	}
	// A corpse whose dead tail reached 186 this tick: the embedder spawns the
	// def's decay effect at its body origin [orig: CEffectWorld_SpawnEmitterAtPosition
	// @0x4b4e39 / @0x4b9f36]; `release` marks an org1 corpse destroyed this
	// tick, whose held decay group the embedder releases [orig: Entity_Destroy
	// -> Entity_ReleaseEffectEmitter @0x43E8F5].
	struct ReplicaCorpseDecay {
		uint16_t handle = 0xFFFF;
		uint16_t type_id = 0;
		int32_t pos[3] = {};
		bool release = false;
	};
	std::vector<ReplicaCorpseDecay> drain_corpse_decays();
	// The organic rows whose respawn arm ran in this receive, by wire handle in record order: the
	// arm's reset releases the row's +0x1CC group inside the decode, ahead of the frame's entity
	// update, so the embedder stops each one at the receive's point and a hit emitter the update
	// spawns on the row survives (D-NET-1230); the reset's slice rebuild runs there too
	// (D-COL-22). [orig: NetPacket_SerializePlayerState
	// @0x4C09C0 - Entity_ResetToSpawnState called @0x4C113C; NetPacket_SerializeInfantryEntityState
	// @0x4C0320 - the call @0x4C05EC; Entity_ResetToSpawnState @0x4B9610 - the release
	// @0x4B97CB..0x4B97F5]
	std::vector<uint16_t> drain_respawn_releases();
	// A sound-profile slot one replica body's mover leg played this tick, at
	// the body's position when it played, with the identity the embedder
	// resolves it through: the row's items.def type and, for a player, its
	// packed avatar id (the wire net_id) that picks the female binding
	// [orig: Entity_GetProfileSlotSound @0x528300 ->
	// Entity_PlaySound3D_FullVolume @0x528E20].
	struct ReplicaSlotSound {
		uint16_t handle = 0xFFFF;
		uint16_t type_id = 0;
		uint16_t character_id = 0; // a player's net_id; 0 for an org1 body
		uint8_t slot = 0;
		int32_t pos[3] = {};
	};
	std::vector<ReplicaSlotSound> drain_slot_sounds();
	// One replica body's sound block on its tick half: the anim event word this tick's
	// channel update sampled, the frame's capsule bottom (the foot dip), the body's
	// position ahead of this tick's integrate and its link as last tick left it (the
	// on-entity pick), with the identity the embedder resolves the slots through
	// (world::wire_body_slot_sounds) [orig: Entity_UpdateInfantryPlayerBody - the parity
	// @0x4B76E6, the block @0x4B76F1..0x4B78A8; Entity_UpdateInfantryAI - @0x4BF144,
	// @0x4BF169..0x4BF2B0].
	struct ReplicaBodySoundWord {
		uint16_t handle = 0xFFFF;
		uint16_t type_id = 0;
		uint16_t character_id = 0; // a player's net_id; 0 for an org1 body
		bool on_entity = false;
		uint32_t events = 0;
		int32_t capsule_bottom = 0;
		int32_t pos[3] = {};
	};
	std::vector<ReplicaBodySoundWord> drain_body_sound_words();

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
	// The pool-0 walk runs a person row's bound move function, its +0x1C4 (the def's
	// move_function row): org1 and org2 are the chases below; the ewep row's update runs through
	// this seam over the row's words (the embedder's World holds the ground and the occupant),
	// which returns true when the update destroyed the row (its stale-ground leg), the row then
	// leaving after the walk; every other row runs nothing here (the null row skipped whole,
	// org0's and chld's bare returns, and the item rows ours does not run over a person).
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-0 walk's +0x1C4 call @0x4C2472;
	//  NapiNPClientMsg_0x00C @0x42E730 / NapiNPClientMsg_FullEntitySpawn @0x433780 - def+0x158
	//  into +0x1C4; Entity_UpdateTransformAndTurret @0x440CA0]
	using PersonEwepMover = std::function<bool(ClientEntityState &row)>;
	void set_person_ewep_mover(PersonEwepMover mover) { person_ewep_mover_ = std::move(mover); }
	// The bound move row of a person row's type, by the def's move_function; none without a
	// catalog entry (the row keeps its class's chase).
	std::optional<world::PhysicsClass> person_move_row(uint16_t type_id) const;
	// Whether the walk runs a decoded person row's bound move row in place of the org chases (a
	// pool-0 Player / Infantry row other than the joiner's own whose row is neither org1 nor org2).
	bool person_takes_item_row(const ClientEntityState &es, uint16_t self_handle) const;
	// The ride link's ground words: the embedder's entity at a ground handle,
	// its occupant (+0x170) and its own ground entity (+0x28) as wire
	// handles (0xFFFF for none); false where it has no entity there. A remote
	// person on a joiner is a decoded row, so the occupant's team is read from
	// the rows. [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 -
	//  @0x4B5EA9..0x4B5EF1]
	using GroundOccupantProvider =
			std::function<bool(uint16_t ground, uint16_t &occupant, uint16_t &ground_of_ground)>;
	void set_ground_occupant_provider(GroundOccupantProvider provider) {
		ground_occupant_provider_ = std::move(provider);
	}

	// Mission water plane for the replica water/float channel (16.16), compared
	// raw: retail keeps no dry sentinel (the word is 0 with no water authored).
	// Fed per pump by the embedder from the env state [orig:
	// g_EnvWaterHeightFixed @ 0x26C6454; Environment_InitDefaults @0x57C010,
	// the memset @0x57C011..0x57C01E].
	void set_water_z(int32_t z) { water_z_ = z; }

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
	// With the own-body split on (a joiner, whose own body serves its rings in the
	// World's entity pass), the walk visits the pool-1 rows and parks every pool-0
	// row (and the higher pools'), in two halves:
	// - the rows through the own row's slot, which tick_remote_motion_through_self
	//   visits from the World's entity pass, after its proximity build and ahead of
	//   its pool-0 walk (the embedder's World::before_pool0_walk);
	// - the rest, which tick_remote_motion_after_self visits once that walk ran the
	//   own body.
	// Each parked row takes its lean decay and arms dip ahead of its visit and its
	// selection and lean ramp after it. Until its visit, the World tick's readers of a
	// parked row (the round pass's guided inputs, wire actor and person body, the bone
	// raycast's pose) see it as its last tick left it, as retail's pool-1 walk and
	// projectile pass see every pool-0 body. after_self first finishes a middle half
	// the embedder never ran, and a parked half left over finishes at the next tick's
	// head.
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
	//  Weapon_UpdateAllProjectiles call @0x4C223A, the proximity build @0x4C240A, the
	//  pool-0 walk @0x4C2426..0x4C2472]
	void tick_remote_motion(uint16_t self_handle);
	void tick_remote_motion_through_self();
	void tick_remote_motion_after_self();
	// The look of each org1 row this tick's visit reached it on (rm_look_due). A live seated
	// row's seat branch: the Yaw chases the aim heading (+0x2EC) a quarter of the way, the step
	// clamped to +-0x2000000, the Pitch the aim pitch (+0x2D0) an eighth, then the Yaw is held
	// within 90 degrees of the seat's body heading (+0x8C) unless the carrier's config is 3, 4,
	// 5 or 7. A free-standing row's on-foot form (world::infantry_on_foot_look) after its body
	// chase, with the aim flag (+0x360) clear: a client stores only 0 there. Retail walks pool 1
	// (a gun's ai-fn tether and its class update's window store) before the pool-0 body, and the
	// body's recoil block precedes both branches, so the embedder calls this leg after its gun
	// channels and tick_recoil (a joiner: after tick_replica_emplaced_channels and
	// tick_remote_recoil). The body chase adds the same step to the Yaw as the recoil's
	// half-step does, so running the chase in the walk and the look here keeps retail's words.
	// [orig: Entity_UpdateInfantryAI @0x4B9910 - the recoil @0x4BE7FD..0x4BE84B, the body chase
	//  @0x4BE8FD..0x4BE931, the free-standing look @0x4BEB18..0x4BEBE7, the seat bone
	//  @0x4BED72 (+0x8C @0x4BEDB3), the seated look @0x4BEF57..0x4BEF97, the clamp
	//  @0x4BEF9A..0x4BEFED, the aim flag's client-reachable stores @0x4B9E30 / @0x4BA100 /
	//  @0x4BA319; Entity_ResetToSpawnState @0x4B9610 - @0x4B96B6; NapiNPClientMsg_0x00C
	//  @0x42E730 - the memset @0x42E7E9..0x42E7F1; Entity_AttachToBoneAndUpdateTransform
	//  @0x5463D0 (+0x8C @0x546620); Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk
	//  @0x4C2158..0x4C21E9 ahead of the pool-0 walk @0x4C2426..0x4C2472]
	void tick_org1_looks();
	void set_self_body_split(bool on) { self_body_split_ = on; }
	// The pool-1 interleave (a joiner, whose World walks pool 1 by slot): the walk parks
	// every pool-1 row too, and each takes its visit at its own slot of the World's pool-1
	// walk (the embedder's World::pool1_slot_leg calls visit_pool1_row), interleaved with
	// the World's movers in slot order. A compact-carried pool-1 row (a hull on a deck)
	// follows its carrier as it stands at the head of its own visit. A pool-1 row the
	// World's walk never reached the embedder visits as the walk ends
	// (unvisited_pool1_rows); one still parked then visits ahead of the parked pool-0 rows
	// (tick_remote_motion_through_self, or after_self where that never ran) (D-NET-196).
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
	//  Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 / @0x4C21E0 / @0x4C21E9;
	//  Entity_UpdatePool1Slot @0x4B8DD0 - the +0x1C4 mover @0x4B8E41..0x4B8E53]
	void set_pool1_interleave(bool on) { pool1_interleave_ = on; }
	// One parked pool-1 row's visit; the row, or null where the handle names no row the
	// walk parked (a World mover's row returns too: the World's mover moved it, its
	// visit returns at the mover test). A second call for the same row this tick is null.
	ClientEntityState *visit_pool1_row(uint16_t handle);
	// The parked pool-1 rows no slot leg has visited yet (slots past the World's pool-1
	// capacity), in walk order: the embedder visits them as the walk ends.
	std::vector<uint16_t> unvisited_pool1_rows() const;
	// The walk's order key (pool 1, pool 0, then the higher pools, each by slot) and
	// the split's line: the own row's key when the walk parks the rows past it,
	// else no line (UINT32_MAX).
	static uint32_t remote_walk_key(uint16_t handle);
	uint32_t remote_walk_split_key(uint16_t self_handle) const;
	// The first key the walk parks: pool 0's slot 0 under the split, else none
	// (UINT32_MAX).
	uint32_t remote_walk_park_key(uint16_t self_handle) const;

	// The S2C 0x70 table and its received flag. Retail's are process globals that
	// no session or mission start touches (their writers are the 0x70 handler and
	// the vehicle screen's first open), so the embedder hands in its process-lived
	// store (inmatch::ClientProcessState); null keeps this pipeline's own.
	// [orig: g_VehicleSpawnAvailabilityReceived 0xA81BB0 -- stores @0x429ACE /
	//  @0x429AE7 / @0x56425D; the table 0xA81BC0 and its count 0xA821B8 --
	//  @0x429AC8 / @0x429AE2]
	void use_vehicle_spawn_store(ClientVehicleSpawnAvailability *store) {
		vehicle_spawn_store_ = store;
	}
	// The S2C 0x82 score tracker's entries, the process's (ClientScoreTracker carries the
	// witness): the embedder hands in its process-lived store (inmatch::ClientProcessState);
	// null keeps this pipeline's own.
	void use_score_tracker_store(std::vector<ClientScoreTrackerEntry> *store) {
		score_tracker_store_ = store;
	}
	std::vector<ClientScoreTrackerEntry> &score_tracker_entries() {
		return score_tracker_store_ != nullptr ? *score_tracker_store_ : own_score_tracker_;
	}
	const std::vector<ClientScoreTrackerEntry> &score_tracker_entries() const {
		return score_tracker_store_ != nullptr ? *score_tracker_store_ : own_score_tracker_;
	}
	ClientVehicleSpawnAvailability &vehicle_spawn_availability() {
		return vehicle_spawn_store_ != nullptr ? *vehicle_spawn_store_ : own_vehicle_spawn_;
	}
	const ClientVehicleSpawnAvailability &vehicle_spawn_availability() const {
		return vehicle_spawn_store_ != nullptr ? *vehicle_spawn_store_ : own_vehicle_spawn_;
	}
	// The replica's tick clock, g_CurrentTick's stand-in: the leg re-plant
	// window, the radio age, the 16-tick weapon pass and the stale-carrier
	// sweep's 0x7F phase read it. Retail's main frame steps the word whether or
	// not its entity update runs (the pre-round and round-gate frames step it
	// too) and the mission start zeroes it, so the embedder steps it on a main
	// frame whose entity update is skipped (step_frame_tick), the entity update
	// steps it itself (tick_remote_motion), and the mission start's end zeroes
	// it (zero_frame_tick).
	// [orig: Game_StartMission @0x524360 (`mov g_CurrentTick, ebx` @0x525B9F);
	//  Game_ProcessMainFrame @0x5263F0 (the pause test @0x5265A0, `add
	//  g_CurrentTick, ebx` @0x5265B4, the entity gates @0x52672C..0x526742,
	//  Entity_UpdateAllEntities(g_CurrentTick) @0x526744..0x52674B);
	//  Entity_UpdateInfantryPlayerBody @0x4B40E0 (curTick @0x4B413E);
	//  Entity_UpdateTransformAndTurret @0x440CA0 (the read @0x440D37)]
	uint32_t frame_tick() const { return frame_tick_; }
	void step_frame_tick() { ++frame_tick_; }
	void zero_frame_tick() { frame_tick_ = 0; }
	// An armed player row's secondary channel, the own row excepted: the advance
	// runs at the head of that row's visit in the body walk, ahead of its
	// primary, as the dual update runs the secondary first (its ring serves
	// precede the primary's); the 16-tick selection keyed on `tick` follows the
	// pass for every row. [orig: AnimMap_UpdateDualChannels @0x40B8C0, the
	// secondary @0x40B908 before the primary @0x40B94E; the selection
	// @0x4b5d71..0x4b5ea3]
	void advance_row_weapon_channel(ClientEntityState &es, uint16_t self_handle);
	// Every visited row's selection (a row the walk parked waits for its visit).
	void select_row_weapon_channels(uint16_t self_handle, uint32_t tick);
	void select_row_weapon_channel(ClientEntityState &es, uint16_t self_handle, uint32_t tick);

	// Recompose every carried row after its carrier's mover has completed. The
	// ordinary library-only tick_remote_motion path calls this as its final
	// phase. Embedders with a later world-side mover (the joiner vehicle
	// prediction seam) call it once more after mirroring those final carrier
	// poses back into ClientState. Keeping this phase explicit prevents pool
	// iteration order from leaving an earlier child one mover tick behind a
	// later carrier.
	// tick_sweep is true only from the per-tick mover call: it runs the
	// pure-client stale-carrier sweep on the mover tick's 0x7F phase (C2S 0x0F
	// for carrier + child, local child destroy pending authority re-spawn) --
	// record applies must not run it [orig: Entity_UpdateTransformAndTurret
	// @0x440CA0, the sweep @0x440D2A..0x440E2F].
	void refresh_carried_entities(bool tick_sweep = false);
	// The refresh's scratch, kept across calls so a pass reuses its buckets and capacity: the
	// first-row index by handle and each row's persistent structural carrier.
	std::unordered_map<uint16_t, std::size_t> carried_row_index_;
	std::vector<uint16_t> carried_persistent_;

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
	// 0x50 and 0x51 rebind them. An id the installed character registry does not
	// hold becomes the first combo of the row's Team side (resolve_player_net_id,
	// D-NET-853).
	// [orig: NapiNPClientMsg_TeamAssign @0x431b3a (animSlot) / @0x431b46
	//  (NetId), the CAvatarDefs_HasPackedId fallback @0x431b4d..0x431b77, the
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
	// S2C 0x35 powerup weapon grants, in receive order; the joiner role
	// applies the ones naming its own player (inmatch/joiner_role.cpp).
	std::vector<WeaponPickupNotice> drain_weapon_pickups();
	std::size_t pending_weapon_pickup_count() const { return pending_weapon_pickups_.size(); }
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
	// A row's def as the client's handlers read it (entity+0x20): its wire
	// type's catalog profile, or null without a catalog or a definition.
	const ItemReplicationProfile *item_def(uint16_t type_id) const;
	// Install the embedder's character registry (its mounted Avatars.def, retail's
	// g_AvatarDefs, which exists before any join): a player row's NetId the table
	// lacks is restamped as the client's handlers restamp it
	// (resolve_player_net_id). The embedder keeps it alive; null (no table
	// mounted) passes every id through.
	void set_character_registry(const inmatch::CharacterRegistry *registry) {
		characters_ = registry;
	}

	// Inject the embedder's .adm root-motion source (JOINER role): armed rows
	// (rm_adm_id >= 0, stamped by the embedder) advance their own AnimMap
	// primary channel each tick and integrate the rotated root delta after the
	// chase — retail's remote-body dead reckoning [orig: the class movers run
	// the full body pass; root integration @0x4B7CB4 / @0x4BF684]. Null (the
	// lib-only embedders) = the truthful chase-only degradation.
	void set_root_motion_source(world::IRootMotionSource *source) {
		root_motion_ = source;
	}
	// The joiner World's per-.adm variant-ring heads (world::AiSystem::anim_rings):
	// every body of an .adm serves its channels' re-inits and loop wraps from the
	// one table, the client's own World body and every decoded row alike
	// [orig: AnimMap_LinkEntity @0x40BA10 links each body's slots to the entry's
	// one table (slot+0x48 = &entry+0x44 @0x40BA77); AnimMap_UpdateEntity
	// @0x40B737..0x40B778; AnimMap_AdvanceToNextAnim @0x40BDF0]. Null = every row
	// plays entry 0 (the lib-only embedders).
	void set_anim_variant_rings(world::AnimVariantRings *rings) { anim_rings_ = rings; }
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

	// The player body's slope conform on a replica row (row_body_conform): one
	// ground column at the row's position offset by (dx, dy), 0x4000 up and
	// 0x20000 down, through the row's candidate models as its movement resolve
	// reads them, and the def's attrib word by wire type. With no probe the
	// column is the terrain's alone (the world pass's terrain-only fallback);
	// with no attrib resolver the attrib reads 0.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the probes @0x4B6E41 /
	//  @0x4B6E5D / @0x4B6E98 / @0x4B6EB3 -> Entity_RaycastGroundHeight @0x4142C0;
	//  the def attrib test @0x4B6D99]
	struct ReplicaGroundColumn {
		uint16_t row_handle = 0xFFFF;
		uint16_t type_id = 0;
		int32_t pos[3] = {};
		int32_t dx = 0, dy = 0;
		uint32_t entity_flags = 0; // the row's retail-Flags mirror (rm_entity_flags)
	};
	using ReplicaGroundProbe = std::function<int32_t(const ReplicaGroundColumn &)>;
	void set_replica_ground_probe(ReplicaGroundProbe probe) {
		replica_ground_probe_ = std::move(probe);
	}
	using ReplicaDefAttribResolver = std::function<uint32_t(uint16_t type_id)>;
	void set_replica_def_attrib_resolver(ReplicaDefAttribResolver resolver) {
		replica_def_attrib_resolver_ = std::move(resolver);
	}
	// The def's second attrib word (ItemDef+0x58), by wire type: an org1 deck ride reads its
	// parent's IsTurret bit (0x1000) [orig: Entity_UpdateInfantryAI @0x4B9910 - `test dword ptr
	// [eax+58h], 1000h` @0x4BA823].
	void set_replica_def_attrib2_resolver(ReplicaDefAttribResolver resolver) {
		replica_def_attrib2_resolver_ = std::move(resolver);
	}
	// A person row's channel (its .adm registry id, rm_adm_id), by wire type: -1 for a type with
	// no usable model map, below -1 while the sources are unarmed. The 0x18 registers the
	// spawned body's channel in its handler, so the update after it runs with one
	// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780 - AnimMap_RegisterEntity @0x40BB60
	//  called @0x433DAA]; JoinerRole::resolve_row_adm_ids stamps every other row.
	using ReplicaAdmResolver = std::function<int(uint16_t type_id)>;
	void set_replica_adm_resolver(ReplicaAdmResolver resolver) {
		replica_adm_resolver_ = std::move(resolver);
	}
	// Whether a ground entity (a wire handle) is a vehicle (def type 1) in its crash state (its
	// +0x2EC byte): a player row on a ladder or under a canopy standing on one keeps its Yaw
	// unclamped [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - Entity_IsVehicle
	// @0x402BB0 called @0x4B4AE9, `cmp byte ptr [edi+2ECh], 0` @0x4B4AF2].
	using ReplicaGroundCrashedResolver = std::function<bool(uint16_t ground)>;
	void set_replica_ground_crashed_resolver(ReplicaGroundCrashedResolver resolver) {
		replica_ground_crashed_resolver_ = std::move(resolver);
	}
	// A seated row's seat type (+0x168: 1 sitex, 2 ctrlx, 3 UseGun, 5 drvrx), the
	// type of the carrier's seat whose bone the record names, which the attach
	// stamps from the carrier model's userpoint; 0 for none. The embedder answers
	// it by the carrier's wire type and the bone [orig: NetPacket_SerializePlayerState
	// @0x4C09C0 - the Entity_TryAttachOrDetach call @0x4C1317].
	using ReplicaSeatTypeResolver =
			std::function<uint8_t(uint16_t carrier_type_id, uint8_t bone)>;
	void set_replica_seat_type_resolver(ReplicaSeatTypeResolver resolver) {
		replica_seat_type_resolver_ = std::move(resolver);
	}
	// A seated row's seat carry: the frame of the carrier's seat bone the record
	// names, which a seated body's pass writes into its body heading (+0x8C), Roll
	// and savedLivePitch (+0x90) each tick. The embedder resolves it over its own
	// carrier and seat table; false = no frame (the row keeps its words).
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - Entity_GetBoneTransformAndOrientation
	//  @0x4B6510, +0x8C @0x4B654E, Roll @0x4B6572, +0x90 @0x4B6575]
	// The carrier def's mount config (+0x86C) rides along, filled whenever the carrier's
	// seat table resolves, the frame found or not: org1's seated look clamp reads it
	// [orig: Entity_UpdateInfantryAI @0x4B9910 - @0x4BEF9A..0x4BEFC3].
	struct ReplicaSeatFrame {
		int32_t heading = 0;
		int32_t pitch = 0;
		int32_t roll = 0;
		bool mount_config_valid = false;
		int32_t mount_config = 0;
	};
	using ReplicaSeatFrameResolver = std::function<bool(
			uint16_t carrier_handle, uint16_t carrier_type_id, uint8_t bone, ReplicaSeatFrame &out)>;
	void set_replica_seat_frame_resolver(ReplicaSeatFrameResolver resolver) {
		replica_seat_frame_resolver_ = std::move(resolver);
	}

	// The phase-3 0x0A objective block has no on-wire discriminator. apply()
	// learns the shared g_GameType from S2C 0x08 field 3 / 0x7B `extra`;
	// replay/bootstrap callers may also seed it explicitly before a midstream 0x0A.
	void set_game_type(uint32_t game_type) { game_type_ = game_type; game_type_known_ = true; }
	uint32_t game_type() const { return game_type_; }
	// True once a folded 0x08 / 0x7B (or set_game_type) wrote the word.
	bool game_type_known() const { return game_type_known_; }

	// The protocol the session speaks (net/protocol/protocol.h), whose edits to
	// the in-match bodies the folds follow: the 0x0A objective body's co-op
	// checkpoint (DFX2-1) and the 0x66 armory-only value (DFX2-10). Set before
	// the first fold; a fold with none aborts (protocol_in_match).
	void set_protocol(const Protocol *protocol) { protocol_ = protocol; }
	const Protocol *protocol() const { return protocol_; }
	// The rules the session's title runs (world::title_rules), whose joiner-side
	// knobs the folds follow: the 0x0F route's done records (DFX2-9). Unset until
	// the role sets them, as every rules holder is (world::require_title_rules).
	void set_rules(const world::TitleRules *rules) { rules_ = rules; }

	// The 0x1D header-form discriminator's session half: retail reads
	// g_NapiNPCtx.is_in_session; a joiner is always in-session, while the
	// listen host's loopback replica passes world.mp_session (single player's
	// own client takes its round end's 0x1D in the team form).
	// [orig: NapiNPClientMsg_0x01D @0x43086c]
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
	// The local body's in-place destroy (its ewep update's stale-ground leg, which a joiner's
	// role sees as the body's own C2S 0x0F): the destroy's tail stores 0xFF in its slot byte,
	// which local_body_slot reads until a record of the body's handle rebuilds it.
	// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the body's 0x0F @0x440E29, Entity_Destroy
	//  @0x440E2F; Entity_Destroy @0x43E810 - +0x154 = 0xFF @0x43EA78]
	void mark_local_body_destroyed() { local_body_slot_ = 0xFF; }

	// The death screen's spectate writers over the replica rows
	// (client_replica_spectate.cpp): the local player's own wire handle the
	// walk starts from and excludes (the embedder stamps it each frame), the
	// target walk (direction 0 clears target and sub-mode), the sub-mode cycle
	// and the SPECTATORTARGET track.
	// [orig: Spectator_CycleTarget_0 @0x52ac20; Spectator_CycleMode @0x52aff0;
	//  Entity_TrySetMinimapTrackTarget @0x52abc0]
	void set_spectate_local_handle(uint16_t handle) { spectate_local_handle_ = handle; }
	void spectate_cycle_target(int direction);
	// True when the cycle landed on the free sub-mode by the arm that then
	// places the local entity on the composed view pose (the forward wrap with
	// a target, any backward step to free) — that placement is the caller's
	// (inmatch::spectate_action) [orig: Spectator_CycleMode @0x52b043 / @0x52b080 ->
	// @0x52b082].
	bool spectate_cycle_mode(int direction);
	void spectate_track(uint16_t handle);
	// The death screen's three spectator actions by their dispatch codes: 500
	// cycles the sub-mode, 501 / 502 step the target +1 / -1 in a chase or
	// first-person sub-mode [orig: Input_HandleActionBinding cases 500
	// @0x49bd58, 501 @0x49bd67, 502 @0x49bd89; catalog rows 110..112].
	// Returns the cycle's placement verdict.
	static constexpr int kSpectateActionCycleMode = 500;
	static constexpr int kSpectateActionNextTarget = 501;
	static constexpr int kSpectateActionPrevTarget = 502;
	bool spectate_action(int code);

	// The local body's slot byte (g_LocalPlayerEntity+0x154), which the squad folds, the
	// command map's walks and sends, the join send's sound and the stat fill read
	// (client_replica_squad.cpp).
	int local_body_slot() const;
private:
	// A destroyed spectate target re-picks [orig: Entity_Destroy @0x43e820].
	void spectate_on_entity_removed(uint16_t handle);
	void apply_spectator_mode(const std::vector<uint8_t> &body); // 0x75
	void queue_carrier_repair(uint16_t handle);
	std::vector<uint16_t> carrier_repair_requests_;
	std::vector<uint16_t> death_edges_;
	std::vector<ReplicaCorpseDecay> corpse_decays_;
	std::vector<uint16_t> respawn_releases_;
	std::vector<ReplicaSlotSound> slot_sounds_;
	std::vector<ReplicaBodySoundWord> body_sound_words_;
	ReplicaDeathTraitsResolver replica_death_traits_resolver_;
	ReplicaPlayerCammoResolver replica_player_cammo_resolver_;
	ReplicaHitRecordResolver replica_hit_record_resolver_;
	ReplicaKillSectionStore replica_kill_section_store_;
	ReplicaDestroyObserver replica_destroy_observer_;
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
	// S2C 0x80 / 0x82, the score tracker (ClientScoreTracker carries the witness).
	void apply_score_tracker_reset(const std::vector<uint8_t> &body);
	void apply_score_tracker_time(const std::vector<uint8_t> &body);
	// S2C 0x05's score-tracker leg (ClientScoreTracker carries the witness).
	void apply_game_start_signal(const std::vector<uint8_t> &body);
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
	// A player row's NetId as the client's handlers store it (client_replica_lifecycle.cpp).
	uint16_t resolve_player_net_id(uint16_t net_id, uint8_t team) const;
	void apply_formatted_game_text(const std::vector<uint8_t> &body); // 0x32 (join/leave lines)
	void apply_entity_routed(const std::vector<uint8_t> &body); // 0x44 (guided, §5.15)
	void apply_deployed_item(const std::vector<uint8_t> &body); // 0x59 pool-1
	void apply_entity_remove(const std::vector<uint8_t> &body);  // 0x12
	void apply_objective_entity_state(const std::vector<uint8_t> &body); // 0x2F
	// A seated body's primary channel at its update's head (client_replica_motion.cpp): true
	// with this tick's frame when the channel has a track.
	bool advance_seated_channel(ClientEntityState &es, world::RootMotionFrame &frame);
	// A seated player row's mounted site (client_replica_motion.cpp): the seat carry (from
	// `seat`, its resolved seat frame, or none), a passenger's lean keys and the mounted eye
	// over `frame`, this tick's channel frame (or none).
	void tick_seated_body(ClientEntityState &es, const ReplicaSeatFrame *seat,
			const world::RootMotionFrame *frame);
	// A live seated org1 row's mounted tail on its eight-tick key: the movement resolver over
	// its channel's capsule (client_replica_motion.cpp).
	void resolve_seated_org1(ClientEntityState &es, const world::RootMotionFrame &frame,
			uint32_t rm_key);
	// One row's whole visit in tick_remote_motion's walk, and the walk's corpse
	// erase (client_replica_motion.cpp).
	void visit_remote_row(ClientEntityState &es, uint16_t self_handle, uint32_t rm_key);
	void erase_expired_remote_rows();
	void erase_entity_tree(uint16_t root_handle);
	// Entity_DropCarriedObject over a person carrier's row: the carried flag
	// leaves it and keeps the carrier's pose for the drop the client runs
	// (ClientWorldMaterializer). True when a flag was dropped.
	bool drop_carried_objective(uint16_t carrier_handle);
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
	world::AnimVariantRings *anim_rings_ = nullptr;
	const world::WeaponTable *weapon_table_ = nullptr;
	const terrain::TerrainHeightField *remote_motion_terrain_ = nullptr;
	uint32_t frame_tick_ = 0; // g_CurrentTick's stand-in (frame_tick())
	ClientVehicleSpawnAvailability own_vehicle_spawn_;             // use_vehicle_spawn_store
	ClientVehicleSpawnAvailability *vehicle_spawn_store_ = nullptr;
	std::vector<ClientScoreTrackerEntry> own_score_tracker_;         // use_score_tracker_store
	std::vector<ClientScoreTrackerEntry> *score_tracker_store_ = nullptr;
	bool remote_motion_mode_ = false;
	ReplicaContactResolver replica_contact_resolver_;
	// tick_remote_motion's per-tick replica-peer sphere table (reused capacity).
	std::vector<ReplicaPeerSphere> contact_peer_scratch_;
	// tick_remote_motion's walk order over state_.entities (reused capacity).
	std::vector<size_t> walk_order_scratch_;
	// A parked row: its handle and its index in state_.entities when parked, re-checked
	// against the handle at its visit (a row the walk's first half erased moved).
	struct ParkedRow {
		uint16_t handle = 0;
		size_t index = 0;
	};
	// The parked row's entry now: at its index while that still holds it, else by handle.
	ClientEntityState *parked_row(const ParkedRow &parked);
	// The own-body split (set_self_body_split): the rows the walk parked through the
	// own row's slot (middle_rows_) and past it (parked_rows_), each in walk order,
	// and the tick they wait in.
	bool self_body_split_ = false;
	std::vector<ParkedRow> middle_rows_;
	std::vector<ParkedRow> parked_rows_;
	// The list a visit walks: a parked list is swapped into it, so the list takes back the
	// visit's emptied buffer with its capacity (pool1_visit_ likewise for pool1_rows_).
	std::vector<ParkedRow> parked_visit_;
	std::vector<uint16_t> pool1_visit_;
	uint16_t parked_self_ = 0xFFFFu;
	uint32_t parked_tick_ = 0;
	// The pool-1 interleave (set_pool1_interleave): the pool-1 rows the walk parked, in
	// walk order, and by slot (the handle's low twelve bits) each one's index in
	// state_.entities when parked plus one, 0 once visited (re-checked against its handle
	// at its visit), with the count still parked: a slot's own entry, as the World's walk
	// reaches each slot (D-NET-1346).
	bool pool1_interleave_ = false;
	std::vector<uint16_t> pool1_rows_;
	std::vector<size_t> pool1_parked_;
	size_t pool1_parked_count_ = 0;
	// The pool-1 rows the World's walk never reached, ahead of the parked pool-0 rows.
	void finish_pool1_rows();
	// The walk's expired corpses (handle, the def names a decay), erased after
	// each half's visits.
	std::vector<std::pair<uint16_t, bool>> remote_motion_expired_;
	// One parked half's visits (tick_remote_motion_through_self / _after_self).
	void visit_parked_rows(std::vector<ParkedRow> &rows);
	// A compact-carried row that follows its carrier between records: a resolved
	// seat-local sample on a carrier, except a world-mover vehicle, which the
	// embedding sim predicts (its row publishes the mirrored pose).
	static bool follows_carrier_seat(const ClientEntityState &child);
	bool deck_ride_yaw_adds(const ClientEntityState &row) const;
	// One such row's follow: its pose from its carrier row's live pose and its
	// seat-local sample; a carrier row that is gone invalidates the sample.
	static void follow_carrier_seat(ClientEntityState &child, const ClientEntityState *carrier);
	ReplicaBoundRadiusResolver replica_bound_radius_resolver_;
	ReplicaGroundProbe replica_ground_probe_;
	ReplicaDefAttribResolver replica_def_attrib_resolver_;
	ReplicaDefAttribResolver replica_def_attrib2_resolver_;
	ReplicaAdmResolver replica_adm_resolver_;
	ReplicaGroundCrashedResolver replica_ground_crashed_resolver_;
	ReplicaSeatTypeResolver replica_seat_type_resolver_;
	ReplicaSeatFrameResolver replica_seat_frame_resolver_;
	CarrierPoseProvider carrier_pose_provider_;
	PersonEwepMover person_ewep_mover_;
	GroundOccupantProvider ground_occupant_provider_;
	int32_t water_z_ = 0;
	ItemClassResolver item_resolver_;                    // items.def table (authoritative)
	std::shared_ptr<const ItemReplicationCatalog> item_catalog_; // the items.def def facts
	const inmatch::CharacterRegistry *characters_ = nullptr; // the embedder's Avatars.def
	std::function<EntityClass(uint16_t)> resolver_;     // phase-1 heuristic fallback
	std::unordered_map<uint16_t, EntityClass> learned_classes_;
	std::vector<ClientRoundEvent> pending_round_events_;
    GuidedRoundResolver guided_round_resolver_;
    std::function<void(const ClientRoundEvent &)> round_receiver_;
    DeployedRoundWalk deployed_round_walk_;
	std::vector<ClientGameEvent> pending_game_events_;
	std::vector<ClientChatLine> pending_chat_lines_;
	std::vector<ClientGameText> pending_game_texts_;
	// The dispatch stamp the ring-bound records take (ClientGameEvent::feed_order).
	uint32_t next_feed_order_ = 0;
	std::vector<WeaponReload> pending_weapon_reloads_;
	std::vector<WeaponPickupNotice> pending_weapon_pickups_;
	std::vector<ClientEffectCommand> pending_effect_commands_;
	std::vector<ScriptRemoteCommand> pending_script_remote_commands_;
	std::vector<ObjectiveNotification> pending_objective_notifications_;
	// Survives row deletion until this pipeline is destroyed.
	std::unordered_map<uint16_t, uint32_t> spawn_revisions_;
	std::size_t unknown_tags_ = 0;
	std::size_t malformed_bodies_ = 0;
	uint32_t game_type_ = 0;
	const Protocol *protocol_ = nullptr; // set_protocol
	const world::TitleRules *rules_ = world::unset_title_rules(); // set_rules
	bool game_type_known_ = false;
	bool mp_session_ = false;
	bool authority_recipient_ = false;
	uint16_t viewer_handle_ = 0xFFFF;
	uint16_t spectate_local_handle_ = 0xFFFF;
	uint32_t mp_attributes_ = 0;
	uint8_t local_player_slot_ = 0;
	// The local body's slot byte as its last record or its in-place destroy stored it; -1 while
	// no record of the viewer's handle has landed (local_body_slot reads the latched slot).
	int16_t local_body_slot_ = -1;
	// Mission-seeded PRNG_Next16 stand-in shared by every decoded row in this
	// view. The body consumes one draw per person per tick even when recoil is
	// zero. Retail also has unrelated process-global consumers that this decoded
	// seam cannot honestly order against; algorithm and local call history after
	// the exact Game_StartMission seed remain bounded here.
	uint32_t prng16_ = 0;
};

} // namespace opennova::replication
