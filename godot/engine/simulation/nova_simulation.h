#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <mission/event_runtime.h>
#include <mission/promote.h>
#include <terrain/height_field.h>
#include <terrain/surface_type_map.h>
#include <wac/wac_system.h>

#include "wac/nova_wac_program.h"
#include <world/ai.h>
#include <world/collision.h>
#include <world/occlusion.h>
#include <world/player_input.h>
#include <world/player_look.h>
#include <world/player_spawn.h>
#include <world/player_view.h>
#include <world/weapon_fsm.h>
#include <world/weapon_inventory.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"
#include "simulation/infantry_root_motion.h"

#include "netsim/loopback_channel.h"          // host_loop_ (the host's own dcb-2 client)
#include "netsim/udp_session_transport.h"     // PeerLink::transport (the LAN per-peer transport)

#include <npwire/peer_addr.h>    // PeerAddr / PeerAddrHash
#include "network/nova_udp_pump.h"

#include <mission/bms.h>                      // bms::File (persisted so ctx_.mission outlives the match)
#include <npruntime/napi_np_server_ctx.h>     // NapiNPServerCtx / GameConfig / ConnectionMode / SocketMode
#include <npruntime/napi_np_protocol.h>       // HostAcceptEvent + the host owner-loop entry points
#include <npruntime/client_runtime.h>         // ClientRuntime (HostClient / Joiner roles)
#include <npruntime/host_session.h>           // HostOwner + host_session_pump (the shared host owner loop)

namespace godot {

class NovaTerrainData;
class NovaObjectData;
class NovaSkeletalAnim;
class NovaItemDatabase;

// THE mission runtime binding: a thin host shell over the portable libs/world runtime.
// Owns one World + the three logic systems (WAC VM, BMS event evaluator, AI) and drives
// them through World::run_logic_tick — one logic tick per step(), the original's
// 62 Hz engine tick (current_tick in Game_ProcessMainFrame @0x5263f0). A host frame runs
// 0..N of those: the wall-clock accumulator lives in the driver (MissionRuntime.
// tick_realtime), faithful to Game_MainLoop @0x52b630. The per-system
// cadences live INSIDE the systems, as in the original: the WAC VM self-gates to every
// 62nd tick (sub_4F81A0 @0x4f81b1) and the BMS evaluator quarter-passes every 16th
// (Server_TickUpdate @0x51d7e0). Both the editor "Play the mission" preview and the game
// runtime go through this one path: promote a parsed BMS mission into the world
// (mission/promote.h), register the systems in the faithful order
// (mission/mission_systems.h), run a pre-mission pass, then tick. Entity transforms
// (mission space -> Godot space) and the part-anim phase are exposed for a scene/renderer
// to draw; host-presentation side effects (text/dialog/win) drain out of the World
// EffectLog each tick. Play/Pause/Step + Stop (snapshot/restore).
class NovaSimulation : public Node3D,
                       private opennova::world::ICollisionSectionMatrixProvider {
	GDCLASS(NovaSimulation, Node3D)

public:
	// Field layout of one entity record in get_present_snapshot()'s flat float buffer. ONE batched
	// PackedFloat32Array call replaces the per-entity scalar getters in the per-tick present loop
	// (the scalar getters box a Variant each; see feedback_dispatcher_callable_perf). Mirrored on the
	// GDScript side via these bound constants so the layout has a single source of truth (C++).
	// Rotation is emitted as mission-space degrees (pitch, yaw, roll) so the host builds the basis
	// through the one placer convention (MissionObjectPlacer.bms_to_godot_basis); position is already
	// in Godot space (x, z, -y). Pitch/roll are 0 today (yaw-only locomotion) — reserved for parity.
	enum PresentField {
		PF_KIND = 0,   // mission ItemType (3 = Organic), -1 if none
		PF_INDEX,      // index within its kind's list
		PF_BMS_ID,     // file entity id; host maps this to a placed node (primary key)
		PF_NET_ID,     // runtime SSN (WAC/BMS addressing)
		PF_POS_X,      // Godot-space position (mission (x,y,z) 16.16 -> (x, z, -y) units)
		PF_POS_Y,
		PF_POS_Z,
		PF_PITCH_DEG,  // mission-space rotation, degrees (0 today; reserved)
		PF_YAW_DEG,
		PF_ROLL_DEG,   // (0 today; reserved)
		PF_PHASE1,     // PANM channel 1 phase 0..65535
		PF_ACTIVE1,    // 1 when channel 1 has a live part-anim to render, else 0
		PF_PHASE2,     // PANM channel 2 phase
		PF_ACTIVE2,
		PF_BODY_ANIM_SLOT, // Entity.body_anim_slot (main-body .bad/.adm clip; consumed only by the deferred seam)
		PF_ANIM_STATE, // InfantryState.anim_state (full off_8135F0 state id; -1 when unavailable)
		PF_ANIM_PHASE_TICKS, // InfantryState.clip_phase, in IDA half-frame ticks
		PF_HIDDEN,     // 1 when the entity is hidden
		PF_ALIVE,      // 1 when alive
		PF_TYPE_ID,    // items.def runtime type id from the wire (0 = none); keys the joiner's wire avatars
		PF_WIRE_HANDLE,// (pool<<12)|slot wire handle from the decoded stream (joiner render key; 0 = none)
		// Final output of anim::compute_aim_overlay_angles. Presentation consumes
		// this result; it never repeats the mounted config selector.
		PF_AIM_OVERLAY_VALID,
		PF_AIM_BODY_PITCH_DEG,
		PF_AIM_BODY_YAW_DEG,
		PF_AIM_BODY_ROLL_DEG,
		PF_AIM_ANGLES, // nine contiguous (pitch,yaw,roll) triples, OverlayClass order
		PF_AIM_CLASS_STRIDE = 3,
		PF_STRIDE = PF_AIM_ANGLES + 9 * PF_AIM_CLASS_STRIDE
	};

	// Typed record returned by get_entity_effect_state_for_ssn(). Position is
	// already in Godot space; rotation remains mission Euler degrees so the
	// host applies the one MissionObjectPlacer basis conversion.
	enum EffectStateField {
		EFFECT_STATE_POSITION = 0,
		EFFECT_STATE_ROTATION_DEG,
		EFFECT_STATE_COUNT
	};

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	// World-object collision: the runtime models + per-tick proximity tables the AI
	// motor resolves against (world/collision.h). Reset per load; models re-registered
	// by resolve_collision_instances. ai_->collision points here (apply_collision_to_ai).
	opennova::world::CollisionWorld collision_world_;
	void apply_collision_to_ai();
	// Mission-lifetime collision graphic cache and host inputs. The initial
	// mission sweep and demand resolution for late-spawned players share these
	// exact model ids; repeated sweeps only attach instances and never duplicate
	// the model registry. MissionObjectPlacer is RefCounted, so retaining it also
	// keeps its object/ADM caches alive for a later RoundSim or F3 query.
	struct CollisionHuskPieceInfo {
		int32_t sections = 0;
		std::vector<opennova::world::Vec3> centers;
		float rest_min_z = 0.0f;
		float rest_max_z = 0.0f;
	};
	Ref<NovaItemDatabase> collision_item_db_;
	Ref<RefCounted> collision_placer_;
	std::unordered_map<std::string, int32_t> collision_model_by_graphic_;
	std::unordered_map<std::string, int32_t> collision_occlusion_by_graphic_;
	std::unordered_map<std::string, float> collision_radius_by_graphic_;
	std::unordered_map<std::string, CollisionHuskPieceInfo>
			collision_husk_pieces_by_graphic_;
	// Negative demand cache: one unresolved entity is attempted at most once per
	// mission unless the host explicitly asks for another full resolve sweep.
	std::unordered_map<uint16_t, uint64_t> collision_resolution_attempted_;
	// Only models with a sampler-live PANM track enter the Generic callback
	// path. Inert PANM rows remain on CollisionWorld's bit-exact Simple path.
	std::unordered_map<int32_t, Ref<NovaObjectData>> collision_pose_data_;
	// Organic/person collision is driven by the same ADM + canonical model-bone
	// table as rendering, but sampled synchronously from simulation state so
	// headless authority and F3 see the exact current pose. One source per entity:
	// different actors sharing a graphic can be on different clips/playheads.
	struct SkeletalCollisionSource {
		// Exact intact/main collision model this living ADM rig was built for.
		// A husk or alternate model on the same entity must use its own callback.
		int32_t model_id = -1;
		uint64_t registry_spawn_id = 0;
		Ref<NovaSkeletalAnim> anim;
		std::vector<int32_t> parents;
		std::vector<Transform3D> rest_global;
		PackedInt32Array overlay_classes;
	};
	std::unordered_map<uint16_t, SkeletalCollisionSource> collision_skeletal_sources_;
	// A non-negative value is the host's once-per-frame retail presentation
	// DWORD. Direct/headless simulations use deterministic logic time.
	int64_t panm_time_override_ms_ = -1;
	bool ensure_collision_instance(opennova::world::World &p_world,
			opennova::world::EntityHandle p_entity) override;
	bool build_section_matrices(opennova::world::World &p_world,
			opennova::world::EntityHandle p_entity, int32_t p_model_id,
			const opennova::world::CollisionMatrix &p_entity_world,
			const opennova::world::CollisionModel &p_model,
			std::vector<opennova::world::CollisionMatrix> &r_out) override;
	// Rendering occlusion: the portal/section-mask engine (world/occlusion.h) —
	// models attached alongside collision by resolve_collision_instances, the
	// portal weld run by occlusion_init_mission, per-frame masks/gates by
	// run_occlusion_frame. [docs/render/render-occlusion-re.md]
	opennova::world::OcclusionWorld occlusion_world_;
	// Per-frame entity render-gate verdicts (bms_id -> culled), rebuilt by
	// run_occlusion_frame; consumed via get_render_culled_bms_ids.
	std::vector<int32_t> occlusion_culled_bms_;
	std::unique_ptr<opennova::mission::BmsEventSystem> bms_;
	std::unique_ptr<opennova::wac::WacSystem> wac_;
	// The installed script program. Held as a Ref so it survives reset_world();
	// finish_load() re-applies it onto the fresh WacSystem each (re)load.
	Ref<NovaWacProgram> wac_program_;
	opennova::world::World::Snapshot baseline_; // play-start state, for Stop -> restore
	opennova::mission::PromoteResult promo_;
	bool loaded_ = false;
	bool playing_ = false;
	bool have_baseline_ = false;

	// --- in-match net runtime (P7, ADR 0009/0011): the SP / LAN host in-process listen server. OFF
	// by default, so the editor / non-net preview path is the direct AI-pool present, untouched. When
	// enabled (before load), bringup_host_runtime stands up the npruntime ctx_ + host_loop_ + runtime_
	// (declared in the P7 block below) and the present pass reads the client-decoded ClientState
	// (ADR 0011 Decision 1) instead of the AI pool. Server_TickUpdate owns the per-frame tick.
	bool listen_server_ = false;
	uint64_t last_sim_tick_us_ = 0;
	uint64_t last_net_tick_us_ = 0;
	mutable uint64_t last_present_snapshot_us_ = 0;
	mutable int last_present_entity_count_ = 0;

	// FollowOwner consumes the same wire-decoded pose as the present pass, but it
	// does so once per fixed tick inside a catch-up batch. Keep the identity index
	// native and generation-bound so GDScript does not rebuild the full PF_* buffer
	// plus four Dictionary indexes for every catch-up tick.
	struct PresentEffectPose {
		Vector3 position;
		Vector3 rotation_deg;
	};
	mutable bool present_effect_pose_cache_valid_ = false;
	mutable uint32_t present_effect_pose_cache_logic_tick_ = 0;
	mutable uint32_t present_effect_pose_cache_client_frame_ = 0;
	mutable const opennova::np::ClientRuntime *present_effect_pose_cache_runtime_ = nullptr;
	mutable std::unordered_map<uint16_t, PresentEffectPose> present_effect_poses_by_handle_;
	mutable std::unordered_map<int, uint16_t> present_effect_handles_by_bms_id_;
	mutable std::unordered_map<int, uint16_t> present_effect_handles_by_ssn_;
	mutable std::unordered_map<uint64_t, uint16_t> present_effect_handles_by_origin_;
	void invalidate_present_effect_pose_cache() const;
	void ensure_present_effect_pose_cache() const;
	PackedVector3Array cached_present_effect_state_for_handle(uint16_t p_handle) const;
	PackedVector3Array present_effect_state_for_handle(uint16_t p_handle) const;

	// --- co-op LAN host: a real UDP socket (NovaUdpPump) over the npruntime runtime. enable_host_listen
	// binds the socket (it implies the listen server); host_pump drives the owner loop, and
	// dispatch_event/admit_peer admit joiners + stream the named dcb-bearing 0x0C. host_session_config_
	// holds the GDScript-facing session options (the Dictionary getter + the §5.1 reactive-reply config
	// fed to configure_session_runtime). Sockets live here, the protocol/crypto in libs (ADR 0010).
	bool host_listen_ = false;
	Ref<NovaUdpPump> pump_;
	opennova::np::GameConfig host_session_config_; // the ONE consolidated server-state config (ADR 0013)
	uint16_t host_bind_port_ = 64220;                      // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's own player and folds
	// host_loop_ into runtime_; a DEDICATED host (false) runs the listen server with NO local player and
	// lets host_session_pump discard the loopback (step 5). Mirrors HostConfig.serve_and_play /
	// start_host_session's gating [orig: the §5.0 listen-host bring-up, SinglePlayer_StartMission @0x561af0].
	bool host_serve_and_play_ = true;
	uint32_t host_max_players_ = 16; // the lobby-advertised player cap; clamped host-side to the witnessed 1..65 [orig +0xC0]
	// The mission's raw terrain-tile (.til) file bytes, fed from the Godot host (which owns the resource
	// root) before load; copied into ctx_.terrain_til_data at bring-up so the initial-state burst streams
	// the S2C 0x45 terrain-tile load (phase 5). Empty => 0x45 faithfully skipped. [§5.37]
	std::vector<uint8_t> terrain_til_data_;
	// Build the PF_* present buffer from the client-decoded ClientState (runtime_->state()).
	PackedFloat32Array present_snapshot_from_client_view() const;

	// --- co-op LAN joiner: a pure non-authority np::ClientRuntime (Joiner role, built in enable_join /
	// finish_load; the runtime_ member is declared in the P7 block below). joiner_pump drives the
	// connect legs + the per-frame S2C->ClientState fold + the C2S 0x0C uplink over a dialed NovaUdpPump.
	// It runs run_logic_tick(false) for its own player L (a motor-driven pool-0 entity spawned at the
	// H-learned pose); remote entities render wire-direct (present + wire_present_pass). enable_join
	// turns it on; a sim is host XOR joiner. [orig: NapiNPClientMsg_0x00C @0x42E730 self name-match]
	bool joiner_ = false;
	bool joiner_started_ = false;          // ClientHello emitted (Idle -> Hello)
	bool joiner_local_spawned_ = false;    // L spawned at reached_in_match (one-shot guard)
	uint16_t joiner_self_wire_handle_ = 0; // H: stamped in the C2S 0x0C + present self-filter
	// Send one framed datagram to the dialed host (the joiner's send_datagram).
	void ship_to_host(const std::vector<uint8_t> &dg);
	// SelfSpawn (mission i32 16.16 + full BAM32 orientation) -> PlayerSpawn for L.
	opennova::world::PlayerSpawn spawn_from_self(const opennova::np::JoinerConnection::SelfSpawn &s) const;

	// Phase 2 (the moving player): the latest input from the host controller, applied to the
	// local player's AiEntity at the TOP of each frame (net-before-logic, ADR 0009). The
	// player then locomotes through the same infantry motor as an NPC. [net-re §5.38]
	opennova::world::PlayerInput player_input_{};
	void apply_player_input_pre_tick();
	// The mouse options + the sim-owned stance latches (the dword_B76484/dword_B76480
	// equivalents the 0x1D apply writes) — the host sends key EDGES and pixel deltas;
	// look angles and stance state live here. [orig: profile +0x590/+0x594; the
	// stance latches @ 0x501d1b/0x501d2d]
	opennova::world::PlayerLookSettings look_settings_{};
	int stance_latch_ = 0; // 0 stand, 1 crouch, 2 prone
	// Godot mouse motion is float; the original consumes whole center-lock pixels.
	// Carry the sub-pixel remainder between frames so slow motion is not lost.
	float look_px_accum_x_ = 0.0f;
	float look_px_accum_y_ = 0.0f;
	// The mounted def's run-gait class + ForceCrouch flag, mirrored per tick into the
	// infantry state like wpn_hold_kind [orig: AdmDefs +0xAC 'run_anim'; flags 0x40000].
	int weapon_run_anim_ = 0;
	bool weapon_force_crouch_ = false;

	// --- the local player's equipped-weapon action FSM (net-re §5.62) ------------------
	// The 12-state action queue on the equipped slot, pumped once per logic tick after the
	// world advances [orig: WeaponAction_ProcessAllEntities @ 0x542690 in the frame loop;
	// this port pumps the LOCAL player's slot only — D-WPN-6]. The host feeds the baked def
	// via set_local_player_weapon and per-frame trigger state via
	// set_local_player_weapon_input. Presentation outputs accumulate as ordered
	// per-tick records because several logic ticks can run per render frame; the
	// snapshot's monotonic serials remain diagnostics/rebuild state.
	opennova::world::WeaponFsmDef weapon_def_{};
	opennova::world::WeaponSlotState weapon_slot_{};
	bool weapon_active_ = false;
	bool weapon_fire_held_ = false;
	bool weapon_fire_pressed_ = false;
	bool weapon_reload_pressed_ = false;
	uint64_t weapon_play_serial_ = 0;
	String weapon_anim_key_;
	uint32_t weapon_anim_tick_ = 0;  // authoritative start tick for FP clip phase
	// The equipped .adm's per-slot VARIANT rings — multi-clip rows rotate round-robin.
	// The sim owns the ring heads exactly where the original keeps them (the weapon's
	// animState slot array +72): bake duration reads and play starts both SERVE the
	// head then ADVANCE it, and the play latches the served index for the host's clip
	// playback (both viewmodel parts follow one latch, so arms and gun never split).
	// [orig: Anim_GetDurationTicks @ 0x53ee10; AnimMap_PlayAnimBySlot @ 0x40bda0
	//  (+68 entry latch); ring build AnimMap_RegisterBoneNode @ 0x40c2d0]
	struct WeaponClipRing {
		PackedFloat32Array lengths;  // every variant's clip length (seconds), file order
		int head = 0;                // next variant to serve
	};
	std::vector<std::pair<String, WeaponClipRing>> weapon_clip_rings_;  // keys lowercased
	int weapon_anim_variant_ = 0;  // the play latch [orig: animState+68]
	WeaponClipRing *weapon_ring_for(const String &p_key_lower);
	// Serve-then-advance duration read; < 0 when the key has no ring.
	float weapon_ring_take_length(const char *p_key);
	// Serve-then-advance play take; returns the served variant index (0 for ringless).
	int weapon_ring_take_variant(const String &p_key);
	uint64_t weapon_fired_serial_ = 0;
	// Per-shooter tag-2 sequence. Unlike the presentation serial above, this
	// survives weapon remounts/switches and resets only with the mission/player
	// world [orig: word_B7C670; capture monotonic across adm changes].
	uint16_t local_round_sequence_ = 0;
	uint64_t weapon_dry_serial_ = 0;
	uint64_t weapon_reload_serial_ = 0;
	uint64_t weapon_unscope_serial_ = 0;
	uint64_t weapon_rescope_serial_ = 0;
	// The action-begin seam: serial + the started slot id; the state dict resolves
	// the started action's soundset/particle names for the host's sound/muzzle legs
	// [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860].
	uint64_t weapon_action_serial_ = 0;
	int weapon_action_started_ = -1;
	// The action-END seam: serial + the finished slot id; the state dict resolves the
	// finished action's soundsetend — the per-shot gunshot / reload-complete sound
	// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100].
	uint64_t weapon_action_end_serial_ = 0;
	int weapon_action_finished_ = -1;
	// One tick's presentation payload, copied while the mounted def and variant-ring
	// result are still authoritative. A host frame drains these records in tick order;
	// the tick stamp lets delayed clip starts resume at the correct playhead.
	struct PendingWeaponEvent {
		uint32_t tick = 0;
		Vector3 world_position;
		String anim_key;
		int anim_variant = 0;
		int action_started = -1;
		String action_soundset;
		String action_particle;
		String action_particle_userpoint;
		// The action-routing state after this tick's view promoter and before this
		// action's own unscope/rescope side effects. A render frame may drain several
		// ticks, so the final view snapshot cannot make this decision for every event.
		bool scope_settled = false;
		bool third_person = false;
		bool vehicle_attack_context = false;
		int action_finished = -1;
		String action_end_soundset;
		// The recoil-row DIRECT effect leg (casing eject / bolt smoke): the action id
		// plus its authored particle/userpoint. The host spawns it with no scope gate
		// and no live-handle suppression [orig: WeaponAction_Recoil @ 0x542dd0 gate
		// @ 0x542efa -> ActionSlot_SpawnEffect @ 0x542f64, param7=0].
		int action_effect = -1;
		String effect_particle;
		String effect_particle_userpoint;
		// A committed weapon switch: the newly equipped def's name — the host
		// reinstalls the viewmodel/FSM for it [orig: the mount's model re-resolve;
		// the equippedAdmIndex stamp @ 0x4dd727]. Empty = no switch this tick.
		String switch_to_weapon;
		// The switch-walk wrap-around deny [orig: PlaySoundOnDedicatedServer
		// (dword_24E08C4) @ 0x4e0354 — the deny sound seam].
		bool switch_denied = false;
	};
	std::vector<PendingWeaponEvent> pending_weapon_events_;
	float weapon_scope_max_mag_ = 0.0f; // def scope_max_mag (0 = key absent)
	// The mounted def's 3P body-channel kinds (special_hold / attack_anim; 0 = rifle)
	// plus the resolved AnimMap identity. The serial advances only when that AnimMap
	// changes; each InfantryState remembers which serial it observed, matching the
	// original's per-entity previous-held record and suppressing false dips between
	// differently named weapons that share one map.
	// [orig: AdmDefs +0/+0xA4/+0xA8; the +0x371 = 20 switch stamp @ 0x4b46f5].
	int weapon_hold_kind_ = 0;
	int weapon_attack_kind_ = 0;
	String weapon_anim_map_;
	uint64_t weapon_anim_map_serial_ = 0;
	void tick_local_player_weapon();

	// --- the local player's weapon slot pool + spawn kit + map rules -------------------
	// [orig: weaponSlotArrayBase @ 0xB75FD4 (780 x 100 B) + g_localAmmoPools @ 0xB75FE8 +
	//  restrictionData @ 0x24D4E00 + g_armoryWeaponAvailability @ 0x24D5600; the loadout
	//  grill 2026-07-18]
	opennova::world::WeaponInventory local_inventory_;
	bool local_inventory_valid_ = false;
	std::vector<opennova::world::WeaponKitEntry> spawn_kit_;
	// Distinguishes an EXPLICIT kit (even the armory's all-NONE empty one, which
	// leaves the table bare) from the never-set state that takes the WPN_M4AUTO
	// default [orig: the default literal @ 0x5246be applies only when neither the
	// mission entry nor the profile authored a buffer].
	bool spawn_kit_set_ = false;
	opennova::world::WeaponAvailability weapon_availability_;
	// A queued manual switch: the FSM plays SWITCHFROM/SWITCHRANK on the outgoing
	// weapon; its completion commits pending -> equipped [orig: MountWeaponSlot
	// @ 0x4dfa40 stamps g_pendingWeaponSlot + queues the action; the handler's
	// completion consumes it].
	bool weapon_switch_in_flight_ = false;
	// The next viewmodel install starts the FSM in SWITCHTO (the draw-in) instead of
	// idle — set by every switch commit and by the spawn mount
	// [orig: the switch chain runs switchfrom -> mount -> switchto].
	bool weapon_start_in_switchto_ = false;
	void commit_pending_weapon_switch();
	// Shared mount/deny routing for the category, cycle, and switchcategory walks
	// [orig: Player_MountWeaponSlot @ 0x4dfa40 / the deny play @ 0x4e0354].
	void handle_weapon_switch_outcome(const opennova::world::WeaponSwitchOutcome &p_out);
	// The witnessed input/stance gates packaged for the switch walks.
	opennova::world::WeaponSwitchGates local_weapon_switch_gates() const;
	// Player_InitPlayer's weapon leg [orig: @ 0x4e15f0]; shared by table load,
	// respawn, and the ACCEPT apply (which passes the freshly stored kit).
	void rebuild_local_player_loadout(bool p_select_spawn_default);

	// --- the local player's view state (ADS ease + 3P anchor chase) --------------------
	// Ticked at the world cadence immediately before the weapon pump, so camera lag and
	// the ADS settle promoter are render-rate independent and action effects observe the
	// same tick's promoted scope state [orig: Player_UpdatePerFrame call @ 0x42c18e
	// precedes WeaponAction_ProcessAllEntities call @ 0x526786].
	// The sim OWNS the engaged bit [orig: g_scopeEngaged @ 0x82CE94]: the host requests
	// toggles and reads the state; the FSM's unscope/rescope events flip it here.
	opennova::world::PlayerViewState player_view_{};
	void tick_local_player_view();
	// The host-sampled head-bone eye (mission space), the 3P anchor-chase target
	// [orig: ThirdPersonCamera_Update @ 0x437b70 target = Position + CameraOffset,
	//  the posed head bone]. The original computes CameraOffset sim-side from its
	//  bone matrices (@ 0x4b6bb3); hosted, the render skeleton lives host-side, so
	//  the host feeds its sample each frame (D-INF-18). Invalid -> Position + 1.0
	//  (the non-person bump [orig: @ 0x437e8f]).
	float local_eye_mission_[3] = {0.0f, 0.0f, 0.0f};
	bool local_eye_valid_ = false;

	// --- P7: the in-match runtime as a THIN ADAPTER over libs/npruntime ----------------
	// One in-match runtime funnels every path: the host/SP/editor-preview is the §5.0 mode-3
	// listen server (NapiNPServerCtx ctx_ + its own loopback client over host_loop_, driven by
	// the npruntime owner loop = Server_TickUpdate + tick_connections + handle_server_datagram);
	// the joiner is a non-authority np::ClientRuntime. The Godot net bindings stay PURE socket
	// pumps — all protocol/crypto/framing lives in libs (ADR 0009-0012, .agents/network.md).
	// host_loop_ MUST be declared before runtime_: the HostClient ClientRuntime holds a
	// non-owning reference into host_loop_, so the loopback has to outlive (and not move under)
	// the runtime.
	// The host state — ctx + per-peer transports + now_tick + serve_and_play — shared with the promoted
	// owner loop host_session_pump (libs/npruntime). MUST be declared before ctx_ (the alias) and before
	// host_loop_ (host_owner_.host_loopback points at host_loop_, set at bring-up). Replaces the old
	// ctx_/peers_/PeerLink members; admit_peer/dispatch_event moved into libs (np::, over host_owner_).
	opennova::np::HostOwner host_owner_;
	opennova::np::NapiNPServerCtx &ctx_ = host_owner_.ctx;    // alias: host only (is_authority)
	opennova::netsim::LoopbackChannel host_loop_;             // the host's own dcb-2 client; Server_TickUpdate's 0x0A target
	std::unique_ptr<opennova::np::ClientRuntime> runtime_;    // HostClient (host/SP) OR Joiner; the present-snapshot source
	opennova::bms::File mission_file_;                        // persisted so ctx_.mission outlives the match (the 0x0B burst body)
	std::string joiner_player_name_;                          // persisted for the Joiner runtime ctor on (re)load
	uint32_t now_tick_ = 0;                                   // the JOINER's per-frame clock (the host uses host_owner_.now_tick)
	// wire-id -> §5.10b replication class, built from items.def in resolve_item_traits.
	// The DECODE-side twin of the per-entity net_class_code stamp: the runtime's client
	// view sizes each inbound 0x0A tag-1 record by class, exactly as the retail client
	// dispatches via its own items.def serialize callback [orig: itemDef+356 @0x50f2e2].
	// Shared into the view's classifier lambda; survives per-load runtime rebuilds.
	std::shared_ptr<const std::unordered_map<uint16_t, opennova::EntityClass>> item_class_table_;
	// Install item_class_table_ on runtime_'s view (no-op until both exist). Called from
	// resolve_item_traits, finish_load (per-load runtime rebuild), and enable_join.
	void install_item_class_resolver();
	// Per-load host bring-up: mode 3 -> create_session(&host_loop_) -> configure_session_runtime
	// -> Server_InitNewRoundState -> the faithful host-player auto-spawn. Mirrors apps/nw_server.
	void bringup_host_runtime(const opennova::bms::File &file);
	// The per-frame host owner loop (recv-drain -> tick_connections -> Server_TickUpdate -> S2C
	// flush -> fold host_loop_ into ClientState). Socket legs gated on host_listen_ (pure SP has none).
	void host_pump();
	// The per-frame non-authority client loop (recv -> run_logic_tick(false) for L's motor ->
	// Client_ProcessNetworkFrame -> ship the C2S 0x0C; spawn L on the in-match edge).
	void joiner_pump();

	// Terrain the AI grounds on. We own copies of the host's depth buffer + 16x16 sector grid so
	// the portable TerrainHeightField's raw pointers outlive the source NovaTerrainData and survive
	// a reload (reset_world rebuilds ai_; apply_terrain_to_ai re-points it). Empty = no grounding.
	std::vector<uint16_t> terrain_heightmap_;
	std::vector<int> terrain_sector_grid_;
	opennova::terrain::TerrainHeightField terrain_field_;
	// Charmap (surface-type) raster copy + the sampler view the footstep pass
	// reads through world.surface_map [orig: Terrain_GetSurfaceTypeAtPosition
	// @ 0x606510]. Shares terrain_sector_grid_/origins with the height field.
	std::vector<uint8_t> surface_indices_;
	opennova::terrain::SurfaceTypeMap surface_map_;
	// SndProf.def text + water plane held for (re)application on reset_world.
	std::vector<uint8_t> sndprof_text_;
	int32_t env_water_z_q16_ = 0;
	void apply_terrain_to_ai();
	void apply_sound_state_to_world();

	// Anim-driven soldier locomotion: the .adm/.bad-backed root-motion source the infantry
	// motor integrates (world/infantry.h). Owned here so it survives reset_world; the fresh
	// ai_ is re-pointed at it like the terrain field. Empty = soldiers hold and stand.
	InfantryRootMotion infantry_anim_;
	void apply_root_motion_to_ai();
	std::vector<opennova::mission::ItemSeatSpec> item_seat_specs_;
	opennova::mission::PromoteOptions promote_options() const;

	void reset_world();
	// Shared post-promote wiring: load the BMS arrays, register the systems, run the
	// pre-mission pass, capture the restore baseline. Marks the sim loaded.
	void finish_load(const opennova::bms::File &file);
	void apply_host_session_mission_header(const opennova::bms::File &file);
	void refresh_host_accept_config();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaSimulation();

	// Load + promote the editor's live mission (the in-memory bms::File, including unsaved
	// edits). This is the editor-integration entry: simulate exactly what is on screen.
	bool load_from_mission_data(const Ref<NovaMissionData> &p_mission);
	// Load + promote a .bms mission from disk; false on parse failure.
	bool load_mission_file(const String &path);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const { return loaded_; }

	// Transport.
	void set_playing(bool p_playing) { playing_ = p_playing; }
	bool is_playing() const { return playing_; }
	// Advance exactly ONE 62 Hz logic tick — the original's engine tick. The per-system
	// dividers gate INSIDE the systems (the WAC VM self-gates to every 62nd tick, the BMS
	// evaluator quarter-passes every 16th), exactly where the original keeps them. Returns
	// false when no mission is loaded. Banking wall-clock and dispatching 0..N of these per
	// host frame is the driver's job (MissionRuntime.tick_realtime, the Game_MainLoop
	// @0x52b630 accumulator) — a host frame is NOT one tick.
	// [orig: Game_ProcessMainFrame @0x5263f0 (one current_tick++ @0x24c1968)]
	bool step();
	void restart();        // Stop: restore the play-start baseline (rewinds world + AI)

	// Turn the sim into an SP in-process listen server (ADR 0011): the host serializes
	// real entity state onto an in-process loopback (Server_TickUpdate's per-connection S2C
	// fan), the local client decodes it, and the present pass reads that decoded state. Call
	// BEFORE loading a mission — the next load stands up the npruntime host runtime. Disabling
	// reverts to the direct AI-pool present (the editor default).
	void enable_listen_server(bool p_enable);
	bool is_listen_server() const { return listen_server_; }

	// Feed the mission's raw terrain-tile (.til) file bytes so the listen host streams the S2C 0x45
	// terrain-tile load to joiners (climbs the client's g_loading_progress 5 -> 6; §5.37). The Godot
	// host owns the resource root, so it read_file()s the .til (named by the .trn tileinfo) and passes
	// the bytes here BEFORE loading the mission. Empty / not-called => 0x45 is faithfully skipped.
	void set_terrain_til_data(const PackedByteArray &p_til_bytes);

	// --- co-op LAN host (Increment C) ------------------------------------
	// Turn the sim into a co-op LAN HOST: bind a UDP listen socket on `p_port`
	// (0 = an OS-assigned ephemeral port) and accept joiners through the
	// witnessed session handshake, spawning each into the live World on join.
	// Implies enable_listen_server(true) — call BEFORE loading a mission.
	// Returns false if the socket can't bind.
	bool enable_host_listen(int p_port);
	bool is_host_listening() const { return host_listen_; }
	int get_host_listen_port() const;  // the bound UDP port (0 when not listening)
	int get_host_peer_count() const;   // joiners in handshake or admitted
	void configure_host_session(Dictionary p_options);
	Dictionary get_host_session_config() const;
	// Debug/test hook: directly admit a synthetic remote peer at a Godot-space
	// position, exercising the admit_peer + connection wiring without a live
	// socket handshake (the handshake itself is unit-tested in libs —
	// tests/novaworld/host_session_accept_test). Returns true if an entity was
	// spawned + bound. No-op unless host listening is on.
	bool admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team);

	// --- co-op LAN joiner (D.2) -------------------------------------------
	// Turn the sim into a co-op LAN JOINER: dial the host at `host_ip:port` and run
	// the witnessed in-match JOIN as a non-authority client. `player_name` rides the
	// ClientHello.co and is the key the host echoes into our organic-spawn record so
	// we self-identify (name-match) and learn our wire handle H. Call BEFORE loading
	// the mission (the next load arms the joiner frame path). Implies the client view;
	// a sim is host XOR joiner. Returns false if the socket can't be dialed.
	bool enable_join(const String &p_host_ip, int p_port, const String &p_player_name);
	bool is_joiner() const { return joiner_; }
	// True once the joiner has name-matched its organic-spawn record (self handle H known).
	bool is_joined_in_match() const;
	// The JoinerSession phase as an int (JoinerSession::Phase), -1 when not joining.
	int get_joiner_phase() const;
	// The learned wire handle H, 0 until in-match (debug / test).
	int get_joiner_self_handle() const;

	// --- the local player (ADR 0012; net-re §5.2b/§5.38) -------------------
	// Spawn the host's own player as an authoritative pool-0 entity at a Godot-space position
	// (yaw in mission degrees). Call AFTER a mission is loaded (the spawn needs the AI system
	// wired). Returns false if no mission is loaded or pool 0 is full. The player then runs
	// the infantry motor from input (set_player_input), not AI think.
	bool spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team);
	// Spawn the host's own player at the mission's player-START marker, selected the way the
	// original engine does — by game type, FARTHEST from the enemy set — NOT at any NPC's
	// position (net-re §5.2c). Single-player resolves the type-6002 start marker. Call AFTER a
	// mission is loaded. Returns: 1 = spawned at a real start marker; 0 = no start marker, spawned
	// at a safe fallback origin (never an NPC); -1 = failed (no mission / pool 0 full).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0]
	int spawn_local_player_at_start();
	// True once a local player has been spawned (World::cached.local_player valid).
	bool has_local_player() const;
	// The local player's wire handle ((pool<<12)|slot), 0 when none. The wire present pass
	// excludes it — the local player is drawn by LocalPlayerHost, not from the wire stream
	// (host: its own pool-0 player; joiner: L, never present in the wire stream anyway).
	int get_local_player_wire_handle() const;
	// Feed one frame of player input: the move keys + look yaw/pitch (mission degrees). Applied
	// to the player's body input at the top of the next frame. Movement keys + the lean
	// keys (Q/E, catalog ids 6/7) + jump; stance and look are SIM-owned state
	// (request_local_player_stance / add_local_player_look). There is no run key in the
	// original's catalog — running is the automatic forward-walk promotion in the body
	// selection [orig: @0x4b729d].
	void set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
	                      bool p_lean_left, bool p_lean_right, bool p_jump);
	// One frame of mouse pixels (screen sense: +x right, +y down) applied to the local
	// player's look through the witnessed integer pipeline: sens = setting << 11,
	// scoped zoom reduction, (px*sens+0x8000)>>16 per axis; yaw wraps; pitch clamps
	// +-80 deg with the up-limit +40 deg while prone. [orig: Input_ProcessMouseAxisBindings
	// @ 0x499680; axis cases 166/164 @ 0x4e109d/@ 0x4e0fed]
	void add_local_player_look(float p_dx_px, float p_dy_px);
	// Mouse options: sensitivity [1,511], default 128; Y invert (flipmouse, default off).
	// [orig: dword_24D207C / dword_24D2078; profile +0x590/+0x594; defaults @ 0x54bbc0]
	void set_local_player_mouse(int p_sensitivity, bool p_invert_y);
	// Stance SELECT request (0 stand / 1 crouch / 2 prone) — the 3-key semantics: each
	// key selects its stance, mutual exclusion at apply, REFUSED while the equipped
	// weapon has ForceCrouch (0x40000). Returns whether the stance changed. [orig:
	// input cases 169/170/172 @ 0x4e0d77.. -> C2S 0x1D ->
	// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
	bool request_local_player_stance(int p_stance);
	// The local player's authoritative position in Godot world space (for the follow camera);
	// Vector3() when no player is spawned.
	Vector3 get_local_player_position() const;
	// The local player's authoritative look yaw / pitch in mission degrees (for the first-person
	// camera). yaw = 90 - heading; pitch up positive. 0 when no player is spawned.
	float get_local_player_yaw_deg() const;
	float get_local_player_pitch_deg() const;
	// The local player's current/max health and team for the HUD, mirroring the original
	// per-frame HUD info. [orig: HUD_BuildEntityInfo @0x4b8440 — health ratio +92, team +374]
	int get_local_player_health() const;
	int get_local_player_max_health() const;
	int get_local_player_team() const;
	// Authoritative armory on-show state from the spawned entity. The class is
	// playerClass +0x294; the name resolves equipped AdmDef index +0x2B0.
	// [orig: Armory_ResolveSelectedClass @0x5642f0; Player_MountWeaponSlot @0x4dfa40]
	int get_local_player_class() const;
	String get_local_player_weapon_name() const;
	// The local player's canonical body-anim slot (BodyAnim; -1 when no player). The host
	// animates the 3rd-person avatar from this, mirroring how the present pass drives NPC models.
	int get_local_player_body_anim_slot() const;
	// The local player's full anim-state clip key ("anim_<name>", "" when no player). Carries
	// stance + jump the 8-slot BodyAnim enum can't (anim_idle_crouch / anim_jump_loop / ...); the
	// host plays it on the avatar via NovaObjectModel.play_body_clip for full stance fidelity.
	String get_local_player_anim_key() const;
	int get_local_player_anim_phase_ticks() const;
	// The local player's third-person aim-overlay state — the torso bend. Dictionary:
	//   valid: bool; aim_state: bool (anim-state flag 0x40 — the bend branch);
	//   body: Vector3 mission-euler degrees (pitch, yaw, roll) for the avatar node basis;
	//   angles: PackedVector3Array[9] mission-euler degrees per anim::OverlayClass.
	// The blends run in exact BAM int math [orig: Entity_BuildBoneTransformMatrices
	// @0x4b1290; docs/world/world-wac-ai-re.md §14]; the host converts each triple with
	// MissionObjectPlacer.bms_to_godot_basis (the single-sourced frame conversion) and
	// feeds NovaObjectModel.set_aim_overlay. Empty/invalid when no player.
	Dictionary get_local_player_aim_overlay() const;

	// --- the local player's equipped-weapon FSM (net-re §5.62) -------------
	// Install the equipped weapon: p_def is the NovaWeaponDatabase weapon dict (the
	// {actions, flags, clipsize, startrounds} slice is consumed) and p_clip_seconds
	// maps each .adm clip key to its VARIANT lengths in SECONDS — a
	// PackedFloat32Array in .adm file order (NovaSkeletalAnim.get_clip_variant_lengths;
	// a plain float is accepted as a single-variant convenience). The lengths seed the
	// per-slot rings and the Anim_InitActions bake consumes them ring-wise: one
	// serve-then-advance read per 'auto' delay field [orig: @ 0x541fa0;
	// Anim_GetDurationTicks @ 0x53ee10]. Resets the slot to a fresh idle with a full
	// magazine (retail bakes a def ONCE globally, so its rings persist across
	// re-equips; this per-equip reset rides the existing per-equip re-bake shape,
	// D-WPN-6 family).
	void set_local_player_weapon(const Dictionary &p_def, const Dictionary &p_clip_seconds);
	void clear_local_player_weapon();
	// Per-frame trigger state: fire held + edge, raw reload edge (the dispatch
	// gate runs sim-side) [orig: the binding-149/reload input dispatch,
	// Input_HandleActionBinding_0 @ 0x4e0420].
	void set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
	                                   bool p_reload_pressed);
	// The ADS toggle request: gated by the dispatcher rules (no toggle during
	// RELOAD/SWITCHFROM, def Flags & 3 required), flips the sim-owned engaged bit
	// and queues the scopeup/scopedown FSM states. Returns whether it toggled.
	// [orig: input case 6 @ 0x4e0420; Player_ToggleWeaponScope @ 0x4df0c0;
	//  WeaponSlot_TryQueueScopeUp @ 0x53f050 / ..ScopeDown @ 0x53f080]
	bool request_local_player_scope_toggle();
	// The host's camera mode, driving the fov suppression + anchor chase
	// [orig: g_camera_mode @ 0xA890C8].
	void set_local_player_camera_third_person(bool p_third_person);
	// The host-sampled head-bone eye (Godot space) for the 3P anchor chase; pass
	// valid=false when no skeleton sample exists (falls back to Position + 1.0).
	void set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid);
	// The view-state snapshot: {scope_engaged, scope_fraction, fov_h_deg,
	// tp_anchor (Godot space), tp_anchor_valid}. Read-only; ticked at 62.5 Hz.
	Dictionary get_local_player_view() const;
	// Horizontal -> vertical projection fov (degrees) through the aspect — the
	// ONE conversion both cameras use [orig: @ 0x58d900].
	static float fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect);
	// The waypoint-track snapshot for the HUD label: {show, count, current,
	// number, name_id, position (Godot space), done}. current is -1 with no
	// selection; number is the 1-based display index [orig: hudInfo+373 =
	// list index + 1 @ 0x4b88e8]. Read-only; the track advances in the world
	// tick. (docs/interface/hud-re.md §Waypoint HUD)
	Dictionary get_waypoint_hud_view() const;
	// The objectives-panel rows: an Array of {slot, text_id, shown, done} for
	// header slots 1..8, terminated at the first 0/255 win-condition id —
	// exactly the panel's row walk [orig: HUD_DrawWinConditions @0x5ba9e0..;
	// shown = show-win bit, done = won bit].
	Array get_objectives_view() const;
	// The FSM snapshot for the host: latest clip/action payloads, diagnostic serials,
	// ammo, kick, and the 3P body channel. Ordered presentation events drain through
	// drain_local_player_weapon_events(); the snapshot alone is not an event queue.
	Dictionary get_local_player_weapon_state() const;
	// Destructively drain the ordered presentation outputs accumulated since the
	// previous host frame. Each Dictionary encodes one PlayerWeaponEvent.
	Array drain_local_player_weapon_events();
	// Destructively drain the flight sim's resolved round impacts, each row already
	// mapped through the ammo effects_table to {position, direction, effect, sound}
	// [orig: Projectile_SpawnImpactEffect @ 0x4e9b80; world/round_sim.h RoundImpact].
	Array drain_round_impacts();

	// --- the local player's loadout: slot pool, spawn kit, map rules -------------------
	// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)
	// The spawn kit [orig: the 2048-B tuple buffer 'restrictionData' @ 0x24D4E00]:
	// rows {name, ammo_primary, ammo_secondary, flags} (values default -1). When
	// p_filter_by_availability, the kit is filtered through the availability table
	// with the knife fallback — the SP .bms promote leg [orig: Mission_LoadBMSFile
	// @ 0x40f7ae]; the armory/profile legs store unfiltered (the server validates).
	// An empty kit resets to the engine default {WPN_M4AUTO} [orig: @ 0x5246be].
	void set_spawn_loadout(const TypedArray<Dictionary> &p_kit, bool p_filter_by_availability);
	// The map weapon-availability rules [orig: g_armoryWeaponAvailability @ 0x24D5600]:
	// reset to all-allowed, then apply {name, value} pairs (the .mis item_availability
	// chunk shape; -1 maps to 3, sub-weapons inherit the parent's value)
	// [orig: build_item_restriction_table @ 0x54DDB0 name-list mode].
	void set_weapon_availability(const TypedArray<Dictionary> &p_pairs);
	// Availability by weapon name: 0 banned / 1 allowed / 2 armory-zone-only /
	// 3 mission-allowed; unknown names read 1. The armory UI filter term
	// [orig: populate_three_category_lists @ 0x566e6b nonzero test].
	int get_weapon_availability(const String &p_weapon_name) const;
	// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline
	// leg]: the accepted kit becomes the spawn kit, the slot pool refills from it
	// (sub-weapons expanded), pools reseed + clips recalc, and the equipped slot
	// re-selects. Rows whose weapon is availability-banned are refused (the server
	// 0x2F validation shape, availability 2 requires the armory zone the ACCEPT is
	// gated on anyway [orig: @ 0x515a4a]). Also stamps player_class when 5..9.
	bool apply_local_player_loadout(const TypedArray<Dictionary> &p_kit, int p_player_class);
	// Rebuild the local player's slot pool from the spawn kit and select the spawn
	// default — the Player_InitPlayer weapon leg [orig: @ 0x4e15f0: display list ->
	// table fill -> pool seed -> clip recalc -> SelectWeaponSlot(195) ->
	// SwitchToWeaponByHandle(195)]. Runs automatically after load_weapon_table; call
	// again on respawn.
	void respawn_local_player_loadout();
	// The category keys [orig: input actions 200-210 @ 0x4e1144 ->
	// Player_SwitchToWeaponByHandle((action-200)*65) @ 0x4e0170]. Category 1..9 =
	// the retail Knife/Sidearm/Primary/Flashbang/Frag/Smoke/Accessory/Detonator/
	// Medpack keys ('1'..'9').
	void request_local_player_weapon_category(int p_category);
	// Next/previous weapon [orig: input cases 212/214 -> Player_CycleWeaponSlot
	// @ 0x4dfe70, direction +1/-1].
	void request_local_player_weapon_cycle(int p_direction);
	// Inventory snapshot for hosts/tests: {equipped_combo, equipped_name, slots:
	// [{combo, name, clip}], pools: {class_name: rounds}, carry_flags}.
	Dictionary get_local_player_inventory() const;

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (NovaWacProgram). Applied now if
	// loaded and re-applied on every (re)load. Pass null to uninstall.
	void set_wac_program(const Ref<NovaWacProgram> &p_program);
	Ref<NovaWacProgram> get_wac_program() const { return wac_program_; }
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; inspect via get_wac_program().
	bool compile_and_set_wac(const PackedStringArray &p_sources);
	// { loaded, paused, runs, event_count, code_size } for transport/debug UI.
	Dictionary get_wac_state() const;
	// Last-frame microsecond counters for the runtime hot path. Allocates only when queried.
	Dictionary get_runtime_perf_counters() const;
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// The host fire-presentation drain: one Dictionary per round spawned since the
	// last call — {origin: Vector3 (godot), forward: Vector3 (godot, unit),
	// shooter_handle, is_local_player, ammo_index, sound_set, effect, mf_light} with
	// the ammo-def 'ai_launch'/'ai_launcheffect' names resolved. The fire present
	// pass plays/spawns per event, skipping the local player (whose action-slot
	// presentation is already ported). [orig: WeaponSlot_FireAndSpawnEffects
	// @0x53F440 — the firing host presents its own rounds inline at fire time;
	// world-wac-ai-re §17.4]
	Array drain_fire_presentation_events();

	// The sound-profile chain [orig: SoundProfile_LoadAll @ 0x527490 /
	// Entity_GetProfileSlotSound @ 0x528300]: feed SndProf.def text (VFS
	// bytes) — parsed into world.sound_profiles now and re-applied on
	// reset_world; per-entity bindings resolve in resolve_ai_weapons.
	void set_sound_profiles(const PackedByteArray &p_sndprof_text);
	// The mission water plane (godot Y units) the footstep water pick and the
	// landing legs compare feet against [orig: Env_WaterHeightFixed @ 0x26C6454].
	void set_water_z(double p_water_y);
	// Drain the per-tick slot-sound emissions (footsteps/foley/landing/screams):
	// one Dictionary per event — {set: String, pos: Vector3 (godot), handle,
	// slot} — played by the fire present pass at full volume
	// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20].
	Array drain_slot_sounds();

	// The live tracer TRAIL channels — the per-round point rings behind every streak,
	// framed per channel as [style_id, age, count, then count x (x, y, z, w)] in
	// godot space; the friendly/enemy style is already selected at spawn vs the local
	// team, and killed rounds' channels keep draining until empty. The fire present
	// pass builds the camera-facing ribbons from these.
	// [orig: the 256-channel pool g_TracerEmitterPool @ 0x2BF5270 — alloc
	// RoundData_SpawnRound @0x4ec774, append Projectile_UpdatePhysics (pre-move,
	// 1/tick), drain CEffectEmitterPool_Tick @0x5db830, draw
	// CEffectChannel_RenderRibbon @0x5db8a0; non-tracer rounds have no channel and
	// are invisible in flight (graphicModel zeroed @0x4ec900). The witness map lives
	// in world/tracer_trails.h.]
	PackedFloat32Array get_tracer_trails() const;

	// The destruction presentation drain (world/destruction.h; world-wac-ai-re
	// §24): {effects[], sounds[], husk_swaps[], debris_bursts[], glass_breaks[],
	// explosions_processed, items_destroyed}, godot-space positions, cleared on
	// read. Once per present, beside the fire drain.
	Dictionary drain_destruction_events();
	// The live death-piece pool as dictionaries {slot, generation, item_id,
	// section, type_index, scale, pos, heading, pitch, settled} — each piece renders as its single
	// husk-model section. [orig: DeathPiece_TickAll @0x57b900; §24]
	Array get_death_pieces() const;
	// Per-entity destruction diagnostics by bms_id (probe/F3 seam): health,
	// bound_radius, flags, traits presence — the damage chain's gate inputs.
	// (get_entity_debug is the AI-pool-index detail card; this one resolves by
	// the placed bms_id and carries the §24 gate fields.)
	Dictionary get_destruction_debug(int p_bms_id) const;

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

	// --- Read-only introspection (debug overlay / tooling) -----------------
	// The world's logic tick counter [orig: current_tick @0x24c1968]. The
	// pre-mission pass in finish_load already advanced it once, so a freshly
	// loaded mission reads 1 — consumers should track deltas, not absolutes.
	int64_t get_logic_tick() const;
	void set_panm_time_ms(int64_t p_time_ms);
	int64_t get_panm_time_ms() const;
	void debug_set_panm_time_ms(int64_t p_time_ms);
	// Whole-bank snapshots of the script variable stores (V0..V511 / G0..G255 /
	// M0..M15 [orig: dword_C6B240 / dword_C6BA40 / music bank]): ONE packed call
	// for a low-Hz overlay refresh instead of hundreds of boxed scalar reads.
	// Always bank-sized; all zeros when no mission is loaded.
	PackedInt32Array get_mission_variables_snapshot() const;
	PackedInt32Array get_global_variables_snapshot() const;
	PackedInt32Array get_music_variables_snapshot() const;
	// Globals (G#) round out the scalar var API (mission V# already bound).
	void set_global_variable(int index, int value);
	int get_global_variable(int index) const;
	// [i] = 1 when event i has fired (active latch + delay elapsed): the bulk
	// form of has_event_fired for an event readout. Empty when unloaded.
	PackedByteArray get_fired_events_snapshot() const;
	// Scalars-only detail card for ONE selected entity ({} when the index is
	// invalid). The key set is STABLE: a registry-despawned entity (scripted
	// remove) still carries every key, with typed defaults for the registry
	// half (kind/index -1, alive false, empty name, ...). Dictionary/String
	// allocation is fine at selected-entity-only low-Hz use; the per-tick
	// present loop has get_present_snapshot instead.
	Dictionary get_entity_debug(int p_index) const;
	// Probe seam: write an AI entity's health via the scripted-SETHP stores
	// (registry + motor copy) so in-game probes can shorten a fight.
	void debug_set_entity_health(int p_index, int p_hp);
	// Probe seam: teleport an AI entity (mission-space coords) through both
	// position stores, for probes defeated by mission geography.
	void debug_set_entity_position(int p_index, const Vector3 &p_mission_pos);
	// World-registry probe seams by SSN (pool-1 vehicles carry no AI brain and are
	// invisible to the AI-index seams): entity card + mission-space teleport.
	Dictionary get_world_entity_debug(int p_net_id) const;
	void debug_set_world_entity_position(int p_net_id, const Vector3 &p_mission_pos);
	// The D-AI-6 muzzle seam: per-frame posed gun-flash userpoint push from the
	// present layer, keyed by the row's PF_NET_ID / authored SSN (Godot-space
	// position; converted + stamped with the logic tick).
	void set_ai_muzzle_world(int p_net_id, const Vector3 &p_godot_pos);
	// Round-outcome card: {ended, winner_team, bluekills, greenkills, enemy_kills,
	// team_kills_by_others, friendly_kills_by_others, enemy_kills_by_others, humans}.
	// The sim-side end-of-round state + the SP kill-stat buckets the epilog score
	// screen and the WAC bluekills/greenkills builtins read (probe + HUD source).
	// [orig: g_spawn_success_gate @0x24c1928 / g_round_winning_team @0x24c1924 /
	// the 0xC846xx buckets]
	Dictionary get_round_outcome_debug() const;
	// Human-readable AI state name, "?" for the id gaps
	// [orig: Entity_LookupAIStateName @0x455cc0].
	static String ai_state_name(int p_state);
	// Infantry anim state id -> ADM clip key ("anim_<off_8135F0 name>"), empty for invalid gaps.
	static String infantry_anim_key(int p_state);

	// Entity query. The (kind, index) pair lets the editor map a sim entity back to its placed
	// mission record + its already-rendered node (MissionController._pickable).
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	int get_entity_index(int p_index) const;        // index within its kind's list
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw(int p_index) const;        // BAM heading -> radians
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for the editor remap)
	int get_entity_state(int p_index) const;        // AI state id (16 = GROUND_FOLLOWWP)
	int get_entity_net_id(int p_index) const;       // runtime SSN (WAC/BMS addressing), 0 if none
	// Empty when no LIVE registry entity owns p_ssn. Unlike the AI-indexed
	// getters, this includes non-AI pools and drops immediately on despawn.
	PackedVector3Array get_entity_effect_state_for_ssn(int p_ssn) const;
	// Compact client-view attachment lookups. These mirror get_present_snapshot's
	// self-filter, wire quantization, and yaw conversion exactly; empty means the
	// identity is absent from this tick's presented view.
	PackedVector3Array get_present_effect_state_for_ssn(int p_ssn) const;
	PackedVector3Array get_present_effect_state_for_wire_handle(int p_wire_handle) const;
	PackedVector3Array get_present_effect_state_for_bms_id(int p_bms_id) const;
	PackedVector3Array get_present_effect_state_for_origin(int p_kind, int p_index) const;
	int get_entity_bms_id(int p_index) const;       // file entity id; the host maps this to a placed node
	int get_entity_owner_connection_id(int p_index) const; // entity+0x78 dcb; the networked-player identity (D-NET-112)
	int get_entity_wire_handle(int p_index) const;  // (pool<<12)|slot — the per-entity wire identity
	// Godot-space positions of the entities the distant MODEL/depth-mask foliage
	// tier generates around: crouched or prone (MoveOrder stance bits 8-9) and
	// standing on terrain, not on another entity. Retail tests every visible
	// sector entity, but only infantry ever carry the stance bits.
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	// (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7]
	PackedVector3Array get_foliage_mask_anchor_positions() const;
	// Part-anim channel phase 0..65535 (PLAYPARTANIM); channel is 1 or 2. The host renders the
	// model part from this (the engine computes it; the host only reads it).
	int get_entity_part_anim_phase(int p_index, int channel) const;
	// True when channel has a live part-anim to render (rate set or phase moved off rest), so the
	// host only poses commanded channels and leaves untouched parts at their default.
	bool get_entity_part_anim_active(int p_index, int channel) const;
	// Entity.body_anim_slot: the main-body skeletal clip (.bad via .adm) the AI requested. Written
	// by EntityCommands::set_ssn_anim; consumed only by the host's deferred apply_body_anim seam
	// today (skeletal runtime not yet built — AnimMap_PlayAnimBySlot @0x40bda0 / off_8135F0). -1 =
	// none. (Distinct from world Entity.anim_slot = the retail +0x374 character-model selector.)
	int get_entity_body_anim_slot(int p_index) const;
	// True when the entity is flagged hidden (HideSingle / held). The present pass maps
	// (not hidden and alive) -> Node3D.visible.
	bool get_entity_hidden(int p_index) const;

	// ONE batched present snapshot for the per-tick render pass: a flat PackedFloat32Array of
	// get_entity_count() records, PF_STRIDE floats each, fields per the PresentField enum. Avoids the
	// ~10 Variant-boxed scalar getter calls per entity the present loop would otherwise make.
	PackedFloat32Array get_present_snapshot() const;
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the host's loaded NovaTerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). The editor preview and the
	// game runtime both call this once in MissionRuntime.setup() so they ground identically.
	void set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain);
	void set_item_seat_specs(const Array &p_specs);

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	// Wire the infantry root-motion source: resolve a model's .adm (e.g. "E_STAND.adm")
	// through the host's resource root and keep its clips' root tracks. Returns the number
	// of anim states with a usable clip (0 = nothing loaded; org1 soldiers then stand —
	// motion comes from clips, as in the original). Survives reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class NovaResourceRoot> &p_resource_root, const String &p_adm_name);
	int get_infantry_clip_count() const { return infantry_anim_.clip_count(0); }

	// Per-entity grounding: resolve every active infantry soldier's OWN model .adm (from its
	// items.def type id via the item database) and store its registry adm_id on the entity, so
	// each grounds + locomotes off its own clip rather than the shared default set. Idempotent;
	// call after load and again after spawning the local player.
	void resolve_infantry_adm_ids(const Ref<class NovaResourceRoot> &p_resource_root,
	                              const Ref<class NovaItemDatabase> &p_item_db);

	// Per-entity items.def trait resolution: stamp each live entity's is_ai_capable (AIData
	// attrib — gates the 0x0D AI-trailer, D-NET-97), net_class_code (§5.10b *_function class
	// tag -> the 0x0A serialize class; an unresolved/ewep item must NOT be serialized as a
	// vehicle or the client desyncs), and health_max/health (items.def hp = healthMax
	// [orig: Entity_InitFromItemDef @0x49e550]). Idempotent; call after load (and again after
	// spawning the local player).
	void resolve_item_traits(const Ref<class NovaItemDatabase> &p_item_db);

	// The D-AI-5 host weapon seed: stamp every AI entity's anim-fire round from its
	// items.def ammo_closeattack + clipsize (AiProfile::ammo_primary/clip_size — the
	// single-ammo stand-in for the entity+0x358..0x35B family, whose load-time
	// block-copy writer is unwitnessed; world-wac-ai-re §17.4/§17.7 item 1), and seed
	// the spawn magazine [orig: Entity_ResetToSpawnState @ 0x4b97a9 — word
	// entity+0x35C = itemDef+0x894]. Ammo NAMES resolve against the mission ammo
	// table, so call AFTER load_ammo_table; unresolved/absent leaves the NPC unarmed
	// (ammo_primary -1, the fire pass skips). Idempotent; returns armed-NPC count.
	int resolve_ai_weapons(const Ref<class NovaItemDatabase> &p_item_db);

	// World-object collision sweep: for each live entity with an items.def graphic,
	// load its .3di collision block (BVOL volumes + BPLN planes via the placer's
	// NovaObjectData cache), register one runtime model per graphic on the sim
	// collision world, and attach the per-entity instance. From then on the infantry
	// motor resolves against placed objects — wall push-out, standing on roofs,
	// hurt/ladder volumes, blink-box indoors [orig: Entity_ProcessCollisionAndPlatform-
	// Physics @0x4b2bd0 + the query set; docs/world/world-wac-ai-re.md §15; D-INF-3].
	// p_placer duck-types MissionObjectPlacer (object_data_for(graphic)). Returns the
	// instance count. Also attaches the render-occlusion portal models (buildings
	// whose graphic carries OVRT/OPLN/OFAC/OOBJ records) with their def bits.
	// Idempotent per load.
	int resolve_collision_instances(const Ref<class NovaItemDatabase> &p_item_db,
	                                Object *p_placer);

	// Mission-start portal init: register + weld + per-building flag stamp over
	// the attached occlusion models. Call once after resolve_collision_instances.
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission @ 0x525e11]
	void occlusion_init_mission();

	// The per-render-frame occlusion pipeline: building batch + portal slots +
	// occluder planes + the section-mask build + the per-entity render gates
	// (blink-hits + the outdoors three-ray latch). Camera in Godot space; fov_y in
	// degrees; fog/water in mission units; force_indoors mirrors the mission
	// attribute override [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> accum |= 2].
	// [orig: Terrain_CollectVisibleEntities @ 0x5c9160 steps 1-4 + the collector
	// gates]
	void run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
	                         double p_aspect, double p_near, double p_fog_dist_units,
	                         double p_water_z_units, bool p_force_indoors);

	// Frame results: [bms_id, visible<<32 | mask] pairs for every building the
	// occlusion frame touched (mask = section bits with forced-visible def bits
	// applied; bit N = COBJ section / render part N; bit 0 = exterior).
	PackedInt64Array get_building_visibility() const;
	// bms_ids of non-building entities the collector gates culled this frame.
	PackedInt32Array get_render_culled_bms_ids() const;
	bool occlusion_water_visible() const;
	bool occlusion_camera_indoors() const;

	// Read-only collision-world geometry for the F3 "Show collision" debug view:
	// { instances: [ { entity_handle, pos (Godot space), heading (mission yaw deg),
	//   volumes: [ { type, min_x..max_z (section-local units), corners:
	//   PackedVector3Array[8] (Godot world space, index bit0=max x / bit1=max y /
	//   bit2=max z in mission axes) } ] } ],
	//   player: { valid, position, points (PackedVector3Array[3]), radii
	//   (PackedFloat32Array[3]), capsule_bottom, capsule_top, foot_clearance } }.
	// Volumes are transformed through the SAME fixed-point path the resolver
	// queries use (CollisionWorld::debug_instances -> target_view ->
	// collision_matrix_from_heading), so the drawn boxes ARE what movement
	// resolves against. Output is capped: instances within 150u of the local
	// player (or the first 128 instances when no player is spawned).
	Dictionary get_collision_debug() const;

	// Read-only snapshot of the RoundSim debug ring for the F3 "Rounds" tab:
	// { tick, events: [ { tick, kind, kind_name, material, section, face,
	//   secondary_section, fallback, effect_tag, effect_tag_name, entity_handle,
	//   shooter_handle, ammo_index,
	//   husk, t, p0, p1, hit (Godot-space Vector3), entity_name } ] } — oldest
	// first, capped at RoundSim::kDebugTrailCap. Covers every resolved outcome
	// including face-miss fly-ons (the "why didn't that register" case).
	Dictionary get_round_debug() const;

	// The round hit-detection reality for the F3 hitbox view:
	// { entities: [ { entity_handle, pos, bound_radius, husk, has_faces,
	//   face_total, tris (PackedVector3Array, triangle list, Godot world),
	//   materials (PackedByteArray per tri), flags (PackedInt32Array per tri) } ],
	//   organics: [ { entity_handle, section, pos (sphere center, Godot),
	//   radius, authored_radius, masked, fallback } ] }.
	// Triangles are transformed in C++ through the SAME husk-aware
	// target_view + full-euler matrices the projectile raycast uses — the
	// drawn mesh IS the tested mesh. The payload is capped at 96 entities /
	// 24000 item faces within 80 u of the local player (face_total exposes
	// per-entity truncation). Organic posed/fallback spheres use the same range
	// and actor cap, omit the local avatar, and use the exact CollisionWorld
	// target matrices consumed by RoundSim.
	Dictionary get_hitbox_debug();

	// Diagnostic round injector: spawns one live round through the REAL
	// RoundSim::spawn (production velocity/tracer/trail path; owner = the local
	// player) from a Godot-space origin along a Godot-space direction, firing
	// the named ammo ("AMMO_556", "AMMO_M203_40MM_NADE", ...). The world tick
	// flies it and the F3 Rounds ring records the outcome — the pose-replay
	// probe's seam. Returns the round slot, -1 on bad ammo/full pool.
	int debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
	                      const String &p_ammo_name);

	// Read-only render-occlusion state for the F3 "Occlusion" debug tab:
	// { active, camera_indoors, exterior_visible, water_visible, local_blink_flags,
	//   counts: { instances, batched, visible, toc_culled, slots, window_groups,
	//   viewthru_groups, welds, culled_entities },
	//   buildings: [ { bms_id, pos (Godot), batched, visible, open_flagged,
	//   mask, has_open, has_windows, has_links, records, windows, portals,
	//   links } ] (capped 256),
	//   welds: [ { own_bms, own_section, other_bms, other_section } ] (capped 64) }.
	// Frame fields reflect the LAST run_occlusion_frame; before one runs the
	// batch is empty and masks default open.
	Dictionary get_occlusion_debug() const;

	// World-space portal-face geometry for the F3 "Show portal faces" 3D view:
	// { buildings: [ { bms_id, pos, visible, records: [ { type, section_a,
	//   section_b, pos, radius, glow, segments (PackedVector3Array a,b pairs) } ] } ] }.
	// Segments are each record's boundary outline — the OFAC edge words whose
	// low-15-bit shared-edge identity appears once (interior edges pair up and
	// drop, the same cancellation identity the occluder pass uses) — transformed
	// through the SAME render_matrix_from_pose path the engine's frame runs, then
	// mapped to Godot space. p_anchor (Godot) + p_range_units bound the sweep
	// per horizontal axis (range <= 0 = everything), capped at 128 buildings.
	Dictionary get_occlusion_portal_debug(const Vector3 &p_anchor, double p_range_units) const;

	// Local-player blink state [orig: g_LocalPlayerBlinkFlags @0x24C1934; entity Flags
	// 0x800000]. The render/audio hosts gate interior behavior on these.
	bool local_player_indoors() const;
	int local_player_blink_flags() const;

	// Sound-occlusion distance inflation for the audio host [orig:
	// Sound_ApplyOcclusionDistance @0x529970 — two LOS rays through terrain +
	// building solids; occluded sources sound farther]. Positions in Godot
	// world space; distance in/out 16.16.
	int64_t sound_occlusion_distance_q16(const Vector3 &listener_pos,
	                                     const Vector3 &source_pos, int64_t distance_q16,
	                                     int source_bms_id = 0);

	// The marched iris-exposure sampling (D-RLIT-2): three classification codes
	// for NovaWeatherCore.set_exposure_from_iris_samples — the camera ray runs
	// 8 units forward, clips against terrain, and samples at the end point and
	// two points marched back toward the camera in thirds. Per sample: a blink
	// hit classifies indoor (-1; -2 when the building carries no interior
	// data), else the outdoor sun level 8 minus one per blocked sun-occlusion
	// ray (three entity-only rays, 200 u toward the light, clip radii
	// -0x2000/-0x5000/-0x8000). Positions/directions in Godot world space.
	// Empty when no world is loaded (the caller falls back to the outdoor
	// sample). Residuals tracked on D-RLIT-2: the entity nearest-hit clip of
	// the camera ray, pool-1 dynamics in the sun rays, and the player-sector
	// entity-count ray gate.
	// [orig: compute_ambient_light_along_direction @ 0x5c7a00;
	//  terrain_sector_compute_lighting @ 0x5c7550;
	//  raycast_entity_collision @ 0x413760]
	PackedInt32Array compute_iris_samples(const Vector3 &cam_pos, const Vector3 &cam_forward,
	                                      const Vector3 &light_dir);

	// Loadout-zone gates for the host's armory key [orig: input action 218 opens
	// weapon.mnu WEAPON only while entity Flags & 0x400000 (a type-6 armory volume
	// contact), vehicle.mnu VEHICLE on Flags & 0x800 (type-11);
	// Input_HandleActionBinding @0x49b848/@0x49b858].
	bool local_player_in_armory_zone() const;
	bool local_player_in_vehicle_loadout_zone() const;

	// The USE-ITEM mount toggle: weapon-busy gate + the witnessed toggle
	// (deck best-seat / nearest-seat scan / seat-swap-or-detach). Returns true when a
	// mount, swap or dismount applied. [orig: Input_ProcessFrame @0x49d6dc ->
	// Entity_ToggleVehicleMount @0x436950]
	bool local_player_toggle_mount();

	// The floating attach labels around the local player, one Dictionary per label:
	// position (mission space, +0.1875 u lift applied), seat_type (world::SeatType,
	// 4 = armory point), armory (bool), nearest (bool, the full-bright highlight),
	// attach_text_key (the USEGUN weapon's attachtextid Overlays key, "" = absent ->
	// the STROVER_USEGUN default). Armory mode rides the zone flag; the can-fire
	// nearest-only gate models EquippedSlot presence + the ctrl/drvr seat reject.
	// [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 selection half;
	//  Player_CanFireWeapon @0x5cf780 — its camera/underwater legs are host state,
	//  unmodeled here: docs/interface/hud-re.md (D-HUD-11)]
	TypedArray<Dictionary> get_attach_labels() const;

	// Parse weapon.def from the resource root and install the armory table on the sim world
	// (world::World::weapons) — the server-side source for the 0x2F/0x5A loadout service, the
	// extended-uplink equipped-weapon gate, and the player-spawn WPN_M4AUTO default
	// (D-NET-141/143). [orig: Game_StartMission @0x5254bd -> WeaponDefs_LoadFile @0x5450A0,
	// right after AnimDef_InitAll @0x5254b3]. Idempotent; call after load.
	Error load_weapon_table(const Ref<class NovaResourceRoot> &p_resource_root,
	                        const String &p_name = "weapon.def");

	// Parse ammo.def and install the ballistics/damage table (world::World::ammo), then
	// resolve every armory entry's round_type to its ammo index — the authoritative round
	// sim's data feed (§5.60). Call after load_weapon_table.
	// [orig: Game_StartMission @0x52548a -> AmmoDef_LoadAll @0x40b0b0]
	Error load_ammo_table(const Ref<class NovaResourceRoot> &p_resource_root,
	                      const String &p_name = "ammo.def");

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaSimulation::PresentField);
VARIANT_ENUM_CAST(godot::NovaSimulation::EffectStateField);
