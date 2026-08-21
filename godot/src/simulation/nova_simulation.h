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
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4i.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mission/event_runtime.h>
#include <mission/promote.h>
#include <hud/hud_frame.h> // HudVehiclePanelState / HudLfpZone (the panel feed seams)
#include <hud/hud_minimap.h>
#include <playersav/weapon_sav.h> // weapon.sav: the per-side profile class + kit pages
#include <terrain_query/height_field.h>
#include <terrain_query/surface_type_map.h>
#include <wac/wac_system.h>

#include "wac/nova_wac_program.h"
#include <def/def.h> // the retained weapon.def parse (S6b)
#include <simassets/adm_clip_index.h> // the equipped rig's clip lengths (S6b)
#include <world/player_loadout.h> // the moved loadout cluster (S7b, ADR 0028)
#include <world/player_weapon.h> // the moved equipped-weapon cluster (S7a, ADR 0028)
#include <world/present_rows.h> // the engine-owned PF_* present-row layout (ADR 0031)
#include <simassets/collision_resolve.h> // the collision/occlusion resolution sweep (ADR 0031)
#include <simassets/sim_collision_pose.h> // the engine-side pose provider (S3, ADR 0028)
#include <simassets/sim_model_cache.h> // the sim's own .3di source (ADR 0028)
#include <npruntime/mission_session.h>
#include <world/ai.h>
#include <world/tick_accumulator.h>
#include <world/collision.h>
#include <world/occlusion.h>
#include <world/player_input.h>
#include <world/player_look.h>
#include <world/player_spawn.h>
#include <world/player_view.h>
#include <world/round_sim.h> // the hit-zone damage tables (re-exported statics)
#include <world/spawn_select.h>
#include <world/weapon_fsm.h>
#include <world/weapon_inventory.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"
#include <simassets/adm_root_motion.h> // the engine-side IRootMotionSource (ADR 0028)

#include "netsim/loopback_channel.h"          // host_loop_ (the host's own dcb-2 client)
#include "netsim/item_replication_catalog.h" // canonical items.def replication traits
#include "netsim/client_world_materializer.h" // header-only joiner pools 1..3
#include "netsim/udp_session_transport.h"     // PeerLink::transport (the LAN per-peer transport)

#include <npwire/peer_addr.h>    // PeerAddr / PeerAddrHash
#include "network/nova_udp_pump.h"

#include <mission/bms.h>                      // bms::File (persisted so ctx_.mission outlives the match)
#include <npruntime/napi_np_server_ctx.h>     // NapiNPServerCtx / GameConfig / ConnectionMode / SocketMode
#include <npruntime/napi_np_protocol.h>       // HostAcceptEvent + the host owner-loop entry points
#include <npruntime/client_runtime.h>         // ClientRuntime (HostClient / Joiner roles)
#include <npruntime/host_session.h>           // HostOwner + host_session_pump (the shared host owner loop)
#include <npruntime/joiner_world_bridge.h>    // the joiner's per-frame world<->net bridge (S10a)

#include "simulation/nova_mission_session_values.h"

namespace opennova::hud {
struct ScoreboardEntry; // hud/hud_scoreboard.h — the Tab-board drawer row
}

namespace godot {

class TerrainData;
class ObjectData;
class SkeletalAnim;
class ItemDatabase;
class AvatarDatabase;
class ResourceRoot;

// The Godot adapter for one portable MissionSession tick target. It owns the
// World and logic systems (WAC VM, BMS evaluator, AI); MissionSession owns
// lifecycle, input retention, fixed cadence, and terminal outcomes. One target
// advance is the original's 62 Hz engine tick (current_tick in
// Game_ProcessMainFrame @0x5263f0), while advance_session_frame runs 0..N of
// those, faithful to Game_MainLoop @0x52b630. The per-system
// cadences live INSIDE the systems, as in the original: the WAC VM self-gates to every
// 62nd tick (WacScript_AdvanceTick @0x4f81b1) and the BMS evaluator quarter-passes every 16th
// (Server_TickUpdate @0x51d7e0). MainGame/GameWorld is the sole live owner for
// this path; focused tests and non-gameplay tools may instantiate it directly:
// promote a parsed BMS mission into the world
// (mission/promote.h), register the systems in the faithful order
// (mission/mission_systems.h), run a pre-mission pass, then tick. Entity transforms
// (mission space -> Godot space) and the part-anim phase are exposed for a scene/renderer
// to draw; presentation side effects (text/dialog/win) drain out of the World
// EffectLog each tick. Runtime transport and fixture teardown use the same
// play/pause/step/restart surface.
class Simulation : public Node3D,
                       private opennova::np::MissionTickTarget,
                       private opennova::world::ICollisionSectionMatrixProvider,
                       private opennova::world::IMountedPoseProvider {
	GDCLASS(Simulation, Node3D)

public:
	enum JoinTerrainTilState {
		JOIN_TERRAIN_TIL_ABSENT = 0,
		JOIN_TERRAIN_TIL_RECEIVING = 1,
		JOIN_TERRAIN_TIL_COMPLETE = 2,
		JOIN_TERRAIN_TIL_INVALID = 3,
	};

	// Field layout of one entity record in get_present_snapshot()'s flat float
	// buffer. The layout is OWNED by the engine — world/present_rows.h carries
	// the enum with its field documentation — and this class enum re-exports
	// every entry with the engine's values so GDScript reads it as bound
	// constants (BIND_ENUM_CONSTANT needs a class-scope enum; the assignments
	// make drift impossible). Add new fields engine-side first, then mirror
	// the entry here.
	enum PresentField {
		PF_KIND = opennova::world::PF_KIND,
		PF_INDEX = opennova::world::PF_INDEX,
		PF_BMS_ID = opennova::world::PF_BMS_ID,
		PF_NET_ID = opennova::world::PF_NET_ID,
		PF_POS_X = opennova::world::PF_POS_X,
		PF_POS_Y = opennova::world::PF_POS_Y,
		PF_POS_Z = opennova::world::PF_POS_Z,
		PF_PITCH_DEG = opennova::world::PF_PITCH_DEG,
		PF_YAW_DEG = opennova::world::PF_YAW_DEG,
		PF_ROLL_DEG = opennova::world::PF_ROLL_DEG,
		PF_PHASE1 = opennova::world::PF_PHASE1,
		PF_ACTIVE1 = opennova::world::PF_ACTIVE1,
		PF_PHASE2 = opennova::world::PF_PHASE2,
		PF_ACTIVE2 = opennova::world::PF_ACTIVE2,
		PF_BODY_ANIM_SLOT = opennova::world::PF_BODY_ANIM_SLOT,
		PF_ANIM_STATE = opennova::world::PF_ANIM_STATE,
		PF_ANIM_PHASE_TICKS = opennova::world::PF_ANIM_PHASE_TICKS,
		PF_ANIM_SOURCE_STATE = opennova::world::PF_ANIM_SOURCE_STATE,
		PF_ANIM_SOURCE_PHASE_TICKS = opennova::world::PF_ANIM_SOURCE_PHASE_TICKS,
		PF_ANIM_BLEND_WEIGHT = opennova::world::PF_ANIM_BLEND_WEIGHT,
		PF_ANIM_REMOTE_REQUEST = opennova::world::PF_ANIM_REMOTE_REQUEST,
		PF_ANIM_STATE_PULSE = opennova::world::PF_ANIM_STATE_PULSE,
		PF_ANIM_PULSE_TICKS = opennova::world::PF_ANIM_PULSE_TICKS,
		PF_WPN_ANIM_STATE = opennova::world::PF_WPN_ANIM_STATE,
		PF_WPN_PHASE_TICKS = opennova::world::PF_WPN_PHASE_TICKS,
		PF_WPN_SOURCE_STATE = opennova::world::PF_WPN_SOURCE_STATE,
		PF_WPN_SOURCE_PHASE_TICKS = opennova::world::PF_WPN_SOURCE_PHASE_TICKS,
		PF_WPN_BLEND_WEIGHT = opennova::world::PF_WPN_BLEND_WEIGHT,
		PF_WPN_VARIANT = opennova::world::PF_WPN_VARIANT,
		PF_WPN_SOURCE_VARIANT = opennova::world::PF_WPN_SOURCE_VARIANT,
		PF_HIDDEN = opennova::world::PF_HIDDEN,
		PF_LOCAL_VIEW_SUPPRESSED = opennova::world::PF_LOCAL_VIEW_SUPPRESSED,
		PF_ALIVE = opennova::world::PF_ALIVE,
		PF_RESPAWN_REVISION = opennova::world::PF_RESPAWN_REVISION,
		PF_TYPE_ID = opennova::world::PF_TYPE_ID,
		PF_WIRE_HANDLE = opennova::world::PF_WIRE_HANDLE,
		PF_CHARACTER_ID = opennova::world::PF_CHARACTER_ID,
		PF_CARRIER_HANDLE = opennova::world::PF_CARRIER_HANDLE,
		PF_AIM_OVERLAY_VALID = opennova::world::PF_AIM_OVERLAY_VALID,
		PF_AIM_BODY_PITCH_DEG = opennova::world::PF_AIM_BODY_PITCH_DEG,
		PF_AIM_BODY_YAW_DEG = opennova::world::PF_AIM_BODY_YAW_DEG,
		PF_AIM_BODY_ROLL_DEG = opennova::world::PF_AIM_BODY_ROLL_DEG,
		PF_AIM_ANGLES = opennova::world::PF_AIM_ANGLES,
		PF_AIM_CLASS_STRIDE = opennova::world::PF_AIM_CLASS_STRIDE,
		PF_EMPLACED_CONTROLS_VALID = opennova::world::PF_EMPLACED_CONTROLS_VALID,
		PF_EWEAP_GUNYAW = opennova::world::PF_EWEAP_GUNYAW,
		PF_EWEAP_GUNPITCH = opennova::world::PF_EWEAP_GUNPITCH,
		PF_VEHICLE_MOTION_VALID = opennova::world::PF_VEHICLE_MOTION_VALID,
		PF_VEHICLE_STEERING = opennova::world::PF_VEHICLE_STEERING,
		PF_VEHICLE_SPEED = opennova::world::PF_VEHICLE_SPEED,
		PF_VEHICLE_ROTOR = opennova::world::PF_VEHICLE_ROTOR,
		PF_VEHICLE_TAIL_ROTOR = opennova::world::PF_VEHICLE_TAIL_ROTOR,
		PF_VEHICLE_WHEELS = opennova::world::PF_VEHICLE_WHEELS,
		PF_TEX_TEAM_VALID = opennova::world::PF_TEX_TEAM_VALID,
		PF_TEX_TEAM = opennova::world::PF_TEX_TEAM,
		PF_ZONE_CTRL_VALID = opennova::world::PF_ZONE_CTRL_VALID,
		PF_TEAMSWING = opennova::world::PF_TEAMSWING,
		PF_LFP_CAMPPERCENT_VALID = opennova::world::PF_LFP_CAMPPERCENT_VALID,
		PF_LFP_CAMPPERCENT = opennova::world::PF_LFP_CAMPPERCENT,
		PF_WORLD_HEAT_GLOW_VALID = opennova::world::PF_WORLD_HEAT_GLOW_VALID,
		PF_WORLD_HEAT_GLOW = opennova::world::PF_WORLD_HEAT_GLOW,
		PF_RIGHT_HAND_COLLAPSED = opennova::world::PF_RIGHT_HAND_COLLAPSED,
		PF_HELD_WEAPON_ADM = opennova::world::PF_HELD_WEAPON_ADM,
		PF_HELD_WEAPON_PITCH_DEG = opennova::world::PF_HELD_WEAPON_PITCH_DEG,
		PF_HELD_WEAPON_YAW_DEG = opennova::world::PF_HELD_WEAPON_YAW_DEG,
		PF_HELD_WEAPON_ROLL_DEG = opennova::world::PF_HELD_WEAPON_ROLL_DEG,
		PF_HELD_WEAPON_HAND_FRAME = opennova::world::PF_HELD_WEAPON_HAND_FRAME,
		PF_SECTION_MASK_VALID = opennova::world::PF_SECTION_MASK_VALID,
		PF_SECTION_MASK_LO = opennova::world::PF_SECTION_MASK_LO,
		PF_SECTION_MASK_HI = opennova::world::PF_SECTION_MASK_HI,
		PF_STRIDE = opennova::world::PF_STRIDE
	};

	// Typed record returned by get_entity_effect_state_for_ssn(). Position is
	// already in Godot space; rotation remains mission Euler degrees so the
	// the shell applies the one MissionObjectPlacer basis conversion.
	enum EffectStateField {
		EFFECT_STATE_POSITION = 0,
		EFFECT_STATE_ROTATION_DEG,
		EFFECT_STATE_COUNT
	};

	// Seat-bone type codes, surfaced so GDScript reads ONE source — the values
	// are pinned to engine/runtime/world's SeatType by static_assert in the .cpp.
	// [orig: Entity_FindBestSeatSlot @0x4351f0 classifies "sitex"->1,
	// "ctrlx"->2, "UseGun"->3, armory scan ->4 @0x436417, "drvrx"->5]
	enum SeatCode {
		SEAT_NONE = 0,
		SEAT_PASSENGER = 1,
		SEAT_CONTROLLER = 2,
		SEAT_GUNNER = 3,
		SEAT_ARMORY_POINT = 4,
		SEAT_DRIVER = 5,
	};

	// local_player_blink_flags() letter bits GDScript gates the render passes
	// on — mirrors world/collision.h kBlinkIndoorsBit/kBlinkWaterOffBit
	// (static_asserts in nova_simulation_occlusion.cpp pin them).
	enum BlinkFlag {
		BLINK_INDOORS = 0x2,
		BLINK_WATER_OFF = 0x8,
	};

	// The WAC/AI attach-to-seat command ids (world.h SeatSelectionMode maps
	// them to seat filters). [orig: command 123 = sitex only, 124 = reject
	// ctrlx, 125 = any seat — the Entity_RequestVehicleAttach command gates]
	enum MountCommand {
		MOUNT_COMMAND_PASSENGER_ONLY = 123,
		MOUNT_COMMAND_SKIP_CONTROLLER = 124,
		MOUNT_COMMAND_ANY_SEAT = 125,
	};

	// The equipped-weapon FSM action ids, re-exported with the engine's values
	// (world/weapon_fsm.h weapon_action carries the witness; assignment from
	// the engine enum makes drift impossible).
	enum WeaponAction {
		WEAPON_ACTION_IDLE = opennova::world::weapon_action::kIdle,
		WEAPON_ACTION_EMPTY_IDLE = opennova::world::weapon_action::kEmptyIdle,
		WEAPON_ACTION_FIRE = opennova::world::weapon_action::kFire,
		WEAPON_ACTION_RECOIL = opennova::world::weapon_action::kRecoil,
		WEAPON_ACTION_RELOAD = opennova::world::weapon_action::kReload,
		WEAPON_ACTION_EMPTY = opennova::world::weapon_action::kEmpty,
		WEAPON_ACTION_SWITCH_TO = opennova::world::weapon_action::kSwitchTo,
		WEAPON_ACTION_SWITCH_FROM = opennova::world::weapon_action::kSwitchFrom,
		WEAPON_ACTION_SWITCH_RANK = opennova::world::weapon_action::kSwitchRank,
		WEAPON_ACTION_SCOPE_UP = opennova::world::weapon_action::kScopeUp,
		WEAPON_ACTION_SCOPE_DOWN = opennova::world::weapon_action::kScopeDown,
		WEAPON_ACTION_OVERHEATED = opennova::world::weapon_action::kOverheated,
		WEAPON_ACTION_COUNT = opennova::world::weapon_action::kCount,
	};

	// The stance vocabulary request_local_player_stance consumes — the engine's
	// InfantryState::Stance values (world/infantry.h carries the witness; the
	// C2S 0x1D apply's mutual-exclusion latch is cited at the request site).
	enum Stance {
		STANCE_STAND = static_cast<int>(
				opennova::world::InfantryState::Stance::kStand),
		STANCE_CROUCH = static_cast<int>(
				opennova::world::InfantryState::Stance::kCrouch),
		STANCE_PRONE = static_cast<int>(
				opennova::world::InfantryState::Stance::kProne),
	};

	// Collision-face flag bits (world/collision.h kFaceFlag* carries the
	// witness) — the hitbox debug view's face styling reads them.
	enum FaceFlag {
		FACE_FLAG_BOTH_SIDES = opennova::world::kFaceFlagBothSides,
		FACE_FLAG_NEVER_HIT = opennova::world::kFaceFlagNeverHit,
		FACE_FLAG_DOUBLE_SIDED = opennova::world::kFaceFlagDoubleSided,
	};

	// Bounding-volume type codes (world/collision.h bvol_type carries the
	// witness; the letter names are the Super OED manual's volume suffixes).
	enum BvolType {
		BVOL_CONTACT_MARKER = opennova::world::bvol_type::kContactMarker,
		BVOL_LADDER_CL = opennova::world::bvol_type::kLadderCL,
		BVOL_ARMORY_CA = opennova::world::bvol_type::kArmoryCA,
		BVOL_VEHICLE_VC = opennova::world::bvol_type::kVehicleVC,
		BVOL_BLINK_BB = opennova::world::bvol_type::kBlinkBB,
		BVOL_DOOR_CD = opennova::world::bvol_type::kDoorCD,
		BVOL_CHANGE_TEAM_CT = opennova::world::bvol_type::kChangeTeamCT,
		BVOL_VEHICLE_LOADOUT = opennova::world::bvol_type::kVehicleLoadout,
		BVOL_VEHICLE_EXT = opennova::world::bvol_type::kVehicleExt,
		BVOL_FLAG_CF = opennova::world::bvol_type::kFlagCF,
		BVOL_DAMAGE_HIGH_DH = opennova::world::bvol_type::kDamageHighDH,
		BVOL_DAMAGE_MEDIUM_DM = opennova::world::bvol_type::kDamageMediumDM,
		BVOL_DAMAGE_LOW_DL = opennova::world::bvol_type::kDamageLowDL,
	};

	// Occlusion portal-face record type bytes (world/occlusion.h kOccRec*
	// carries the witness) — the occlusion debug view's type styling.
	enum OccRecordType {
		OCC_REC_OCCLUDER = opennova::world::kOccRecOccluder,
		OCC_REC_OPEN = opennova::world::kOccRecOpen,
		OCC_REC_WINDOW = opennova::world::kOccRecWindow,
		OCC_REC_PORTAL = opennova::world::kOccRecPortal,
		OCC_REC_WELDED_LINK = opennova::world::kOccRecWeldedLink,
	};

	// Engine-domain scalar re-exports (each value's witness lives at its
	// engine home; assignment from the engine constant makes drift impossible).
	enum {
		// One full PLAYPARTANIM sweep (16.16 1.0) — the phase/ACTIVE domain
		// bound for the debug pages (world/ai.h kPartAnimPhaseOne).
		PART_ANIM_PHASE_ONE = opennova::world::kPartAnimPhaseOne,
		// The "no local record" wire-handle sentinel (world/entity.h
		// EntityHandle::kInvalid).
		INVALID_WIRE_HANDLE = opennova::world::EntityHandle::kInvalid,
		// The epilog/debrief ESC-less exit timeout in ticks (world/world.h
		// kEpilogExitTimeoutTicks; epilog_exit_timeout_seconds() derives).
		EPILOG_EXIT_TIMEOUT_TICKS = opennova::world::kEpilogExitTimeoutTicks,
		// The item-effect attach scan reads only a model's first 16 userpoints
		// (threedi_3di3.h THREEDI_USER_POINT_SCAN_LIMIT; pinned by
		// static_assert in nova_simulation_bind.cpp).
		ITEM_USER_POINT_SCAN_LIMIT = 16,
		// The retail signed-16 storage domain entity health lives in
		// (world/entity.h kRetailI16Min/Max).
		ENTITY_HEALTH_MIN = opennova::world::kRetailI16Min,
		ENTITY_HEALTH_MAX = opennova::world::kRetailI16Max,
		// The horizontal default camera fov, degrees (world/player_view.h
		// kPlayerCameraFovHDeg; the static_assert in the bind TU pins the
		// integral mirror against the engine float).
		DEFAULT_PLAYER_FOV_H_DEG = 80,
		// Header {version, stride, row_count} for the retained minimap rows.
		// v3 appends the client-resolved draw policy: {policy_flags (bit0
		// rotate, bit1 footprint), half_x_q16, half_y_q16, floor_px}; v4
		// appends {medic} — the local-team charattr Medic bit the map marker
		// pass draws the red-cross plate for (retail: draw_entity_labels_and_markers
		// @0x5a49e0 — AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3 under the
		// local-team gate @0x5a4ac6/@0x5a4acf, see docs/interface/hud-re.md).
		HUD_MINIMAP_SNAPSHOT_VERSION = 4,
		HUD_MINIMAP_HEADER_SIZE = 3,
		HUD_MINIMAP_STRIDE = 17,
	};

	// Spawn-origin provenance (world/entity.h): (kind << 24) | (index &
	// 0xFFFFFF), kSpawnOriginNone = none. The decoded halves of NONE are the
	// present rows' "no local record" kind/index sentinels. Fixed uint32_t so
	// SPAWN_ORIGIN_NONE binds positive.
	enum : uint32_t {
		SPAWN_ORIGIN_NONE = opennova::world::kSpawnOriginNone,
		SPAWN_ORIGIN_KIND_NONE =
				static_cast<uint32_t>(opennova::world::kSpawnOriginKindNone),
		SPAWN_ORIGIN_INDEX_NONE =
				static_cast<uint32_t>(opennova::world::kSpawnOriginIndexNone),
	};

	// The spawn-origin pack/decode helpers, re-exported for GDScript
	// composition (world/entity.h carries the packing contract).
	static int64_t spawn_origin_pack(int p_kind, int p_index) {
		return static_cast<int64_t>(opennova::world::spawn_origin_pack(
				static_cast<uint32_t>(p_kind), static_cast<uint32_t>(p_index)));
	}
	static int spawn_origin_kind(int64_t p_origin) {
		return opennova::world::spawn_origin_kind(
				static_cast<uint32_t>(p_origin));
	}
	static int spawn_origin_index(int64_t p_origin) {
		return opennova::world::spawn_origin_index(
				static_cast<uint32_t>(p_origin));
	}

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	// World-object collision: the runtime models + per-tick proximity tables the AI
	// motor resolves against (world/collision.h). Reset per load; models re-registered
	// by resolve_collision_instances. ai_->collision points here (apply_collision_to_ai).
	opennova::world::CollisionWorld collision_world_;
	void apply_collision_to_ai();
	// The shell input the sweep reads (its retained items.def rows feed the
	// engine resolve). RefCounted, so retaining it also keeps its object/ADM
	// caches alive for a later RoundSim or F3 query.
	Ref<ItemDatabase> collision_item_db_;
	// The sim's own asset source (ADR 0028): the mounted root pinned for its
	// index lifetime + the parse-once model cache the collision/occlusion/
	// radius extraction reads. Render caches stay render-only. Mutable: it is
	// a parse-on-miss CACHE — the const present path resolves the joiner's
	// attachment models through it (one mounted matrix path, S4b).
	Ref<ResourceRoot> asset_root_;
	mutable opennova::simassets::SimModelCache sim_models_;
	// Portable mission lifecycle and cadence. During one advance call the Godot
	// adapter holds a single typed tick sink so presentation consumes every
	// catch-up tick before the next simulation tick.
	opennova::np::MissionSession mission_session_;
	Callable session_tick_sink_;
	int64_t frame_net_us_ = 0;
	int64_t frame_sim_us_ = 0;
	int64_t frame_sink_us_ = 0;
	opennova::np::MissionSessionRole configured_session_role() const;
	bool begin_session_load();
	void complete_session_load();
	void fail_session_load(const char *p_message);
	bool advance_world_tick();
	void restore_world_baseline();
	opennova::np::TickOutcome advance_mission_tick(
			const opennova::np::TickInput &p_input) override;
	bool reset_mission_to_baseline(opennova::np::SessionError &r_error) override;
	void close_mission() override;
	// The mission-lifetime collision graphic caches + the negative demand
	// cache, engine-owned (simassets::CollisionResolveState, ADR 0031); the
	// registry sweep and the joiner's wire ghosts share one implementation.
	opennova::simassets::CollisionResolveState collision_resolve_;
	// Wire-side collision resolution for decoded pool-1 movers: runtime type id
	// -> {model id, bound radius}, sharing the by-graphic caches above. -1 model
	// with 0 radius latches an unresolvable type so it is attempted once.
	struct WireCollisionShape {
		int32_t model_id = -1;
		float bound_radius = 0.0f;
	};
	std::unordered_map<uint16_t, WireCollisionShape> wire_collision_shape_by_type_;
	// A non-negative value is the shell's once-per-frame retail presentation
	// DWORD. Direct/headless simulations use deterministic logic time.
	int64_t panm_time_override_ms_ = -1;
	// The engine-side collision pose provider (S3b full, ADR 0028) — the ONE
	// section-matrix source. It poses from the sim's own parse-once models and
	// .adm rigs over the installed asset root; a world without a root has no
	// model source and every query is a counted decline (the legacy
	// render-bound builder is gone).
	opennova::simassets::SimCollisionPoseProvider collision_pose_native_;
	// Post-A/B native-path health counters (the cutover retired the divergence
	// stats; these keep declines observable): cumulative queries/declines on the
	// two native-authoritative pose paths. A production decline is the
	// masked-failure signal the soak/probes gate on (debug_native_pose_stats).
	uint64_t collision_native_queries_ = 0;
	uint64_t collision_native_declines_ = 0;
	uint64_t mounted_native_queries_ = 0;
	uint64_t mounted_native_declines_ = 0;
	bool ensure_collision_instance(opennova::world::World &p_world,
			opennova::world::EntityHandle p_entity) override;
	bool build_section_matrices(opennova::world::World &p_world,
			opennova::world::EntityHandle p_entity, int32_t p_model_id,
			const opennova::world::CollisionMatrix &p_entity_world,
			const opennova::world::CollisionModel &p_model,
			std::vector<opennova::world::CollisionMatrix> &r_out) override;
	// The mounted-pose resolver (S4, ADR 0028) is native-only: the engine-side
	// resolver over the sim's own parse is the sole host-authority path.
	bool resolve_mounted_pose(opennova::world::World &p_world,
			const opennova::world::Entity &p_carrier,
			const opennova::world::Seat &p_seat,
			opennova::world::MountedPose &r_out) override;
	bool resolve_mounted_pose_native(opennova::world::World &p_world,
			const opennova::world::Entity &p_carrier,
			const opennova::world::Seat &p_seat,
			opennova::world::MountedPose &r_out);
	// The native mounted-pose model sources: type id -> the spec's graphic
	// name (SeatSpecExtraction::graphic_by_type), resolved through the sim
	// cache AT QUERY TIME. The cache frees its parses whenever set_asset_root
	// switches the index, so this table stores the graphic name and never a
	// raw pointer (a retained pointer here was a use-after-free across root
	// switches). Kept across reset_world — the table installs before mission
	// promotion; boot wires the asset root before the steps run, so the cache
	// serves every query.
	std::unordered_map<int32_t, std::string> mounted_pose_native_graphics_;
	// Rendering occlusion: the portal/section-mask engine (world/occlusion.h) —
	// models attached alongside collision by resolve_collision_instances, the
	// portal weld run by occlusion_init_mission, per-frame masks/gates by
	// run_occlusion_frame. [docs/render/render-occlusion-re.md]
	opennova::world::OcclusionWorld occlusion_world_;
	// Per-frame entity render-gate verdicts (bms_id -> culled), rebuilt by
	// run_occlusion_frame; consumed via get_render_culled_bms_ids.
	std::vector<int32_t> occlusion_culled_bms_;
	// Delta baselines for the render-occlusion apply path: what the shell last
	// applied, so steady frames emit nothing. Cleared on world reset and via
	// reset_occlusion_apply_baseline() (the occlusion A/B seam re-arms a full
	// re-emit).
	std::unordered_map<uint32_t, int64_t> occl_apply_building_last_;
	std::vector<int32_t> occl_apply_culled_last_;
	// Per-entity sun-visibility quality (bms_id -> 1..4) last emitted to the
	// shell, plus the local player's current quality for the presenter seam.
	// Unlisted entities are quality 4 (factor 1.0), the node default.
	std::unordered_map<int32_t, uint8_t> sun_quality_last_;
	uint8_t local_sun_quality_ = 4;
	// The pool-2 building a packed blink hit names, as a bms_id (0 = none).
	int blink_hit_owner_bms_id(uint32_t p_hit) const;
	std::unique_ptr<opennova::mission::BmsEventSystem> bms_;
	std::unique_ptr<opennova::wac::WacSystem> wac_;
	// The installed script program. Held as a Ref so it survives reset_world();
	// finish_load() re-applies it onto the fresh WacSystem each (re)load.
	Ref<WacProgram> wac_program_;
	opennova::world::World::Snapshot baseline_; // runtime-start state, for restart/teardown
	opennova::wac::WacSystem::RuntimeState wac_baseline_;
	opennova::mission::PromoteResult promo_;
	// The last boot_mission resolution decisions (S9 soak surface).
	struct MissionBootDebug {
		int32_t text_source = 0; // mission::MissionTextSource
		int64_t text_size = 0;
		std::string infantry_adm;
		std::vector<opennova::mission::PromoteOptions::AiProfileRow> aip_rows;
	};
	MissionBootDebug boot_debug_;
	// Resource-install invariant only. Public lifecycle is
	// mission_session_.state(); this prevents partially constructed worlds from
	// serving data while Loading/Failed transitions are in flight.
	bool world_installed_ = false;
	bool defer_session_load_completion_ = false;
	bool have_baseline_ = false;
	bool have_wac_baseline_ = false;

	// --- in-match net runtime (P7, ADR 0009/0011): the SP / LAN host in-process listen server. OFF
	// by default, so an explicit non-network fixture uses the direct AI-pool present. When
	// enabled (before load), bringup_host_runtime stands up the npruntime ctx_ + host_loop_ + runtime_
	// (declared in the P7 block below) and the present pass reads the client-decoded ClientState
	// (ADR 0011 Decision 1) instead of the AI pool. Server_TickUpdate owns the per-frame tick.
	bool listen_server_ = false;
	uint64_t last_sim_tick_us_ = 0;
	uint64_t last_net_tick_us_ = 0;
	// The two halves of run_occlusion_frame: the portal/section build
	// (OcclusionWorld::build_frame) and the per-entity render-gate probe loop.
	uint64_t last_occlusion_build_us_ = 0;
	uint64_t last_occlusion_probe_us_ = 0;
	// One opt-in gate for every native runtime timer/counter. Retail play keeps
	// this false; F3 Stats and the manual probe share the public ownership seam.
	bool runtime_profiling_enabled_ = false;
	mutable uint64_t last_present_snapshot_us_ = 0;
	mutable int last_present_entity_count_ = 0;
	// Exact identity/order of the most recently returned PF_* buffer. Dynamic
	// values (pose, animation, visibility) deliberately do not participate:
	// GDScript row plans may keep their offsets while reading fresh values.
	struct PresentRowIdentity {
		int32_t wire_handle = 0;
		int32_t type_id = 0;
		int32_t bms_id = 0;
		int32_t kind = -1;
		int32_t index = -1;

		bool operator==(const PresentRowIdentity &p_other) const {
			return wire_handle == p_other.wire_handle &&
			       type_id == p_other.type_id &&
			       bms_id == p_other.bms_id &&
			       kind == p_other.kind &&
			       index == p_other.index;
		}
	};
	mutable std::vector<PresentRowIdentity> present_layout_;
	mutable uint64_t present_layout_revision_ = 0;

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
	// A missing owner is also stable for one decoded-client epoch. Remember
	// misses so stale effect attachments cannot turn lazy lookup into one full
	// entity scan per fixed tick/query. Positive caches remain authoritative
	// when another alias materializes the same row.
	mutable std::unordered_set<uint16_t> present_effect_missing_handles_;
	mutable std::unordered_set<int> present_effect_missing_bms_ids_;
	mutable std::unordered_set<int> present_effect_missing_ssns_;
	mutable std::unordered_set<uint64_t> present_effect_missing_origins_;
	void invalidate_present_effect_pose_cache() const;
	void ensure_present_effect_pose_cache() const;
	bool cache_present_effect_pose(
			const opennova::netsim::ClientEntityState &p_entity_state) const;
	PackedVector3Array cached_present_effect_state_for_handle(uint16_t p_handle) const;
	PackedVector3Array present_effect_state_for_handle(uint16_t p_handle) const;

	// --- co-op LAN host: a real UDP socket (UdpPump) over the npruntime runtime. enable_host_listen
	// binds the socket (it implies the listen server); host_pump drives the owner loop, and
	// dispatch_event/admit_peer admit joiners + stream the named dcb-bearing 0x0C. host_session_config_
	// holds the GDScript-facing session options (the Dictionary getter + the §5.1 reactive-reply config
	// fed to configure_session_runtime). Sockets live here, the protocol/crypto in libs (ADR 0010).
	bool host_listen_ = false;
	Ref<UdpPump> pump_;
	opennova::np::GameConfig host_session_config_; // the ONE consolidated server-state config (ADR 0013)
	uint16_t host_bind_port_ = 64220;                      // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's own player and folds
	// host_loop_ into runtime_; a DEDICATED host (false) runs the listen server with NO local player and
	// lets host_session_pump discard the loopback (step 5). Mirrors HostConfig.serve_and_play /
	// start_host_session's gating [orig: the §5.0 listen-host bring-up, SinglePlayer_StartMission @0x561af0].
	bool host_serve_and_play_ = true;
	uint32_t host_max_players_ = 16; // the lobby-advertised player cap; clamped host-side to the witnessed 1..65 [orig +0xC0]
	// The mission's raw terrain-tile (.til) file bytes, fed from the Godot shell (which owns the resource
	// root) before load; copied into ctx_.terrain_til_data at bring-up so the initial-state burst streams
	// the S2C 0x45 terrain-tile load (phase 5). Empty => 0x45 faithfully skipped. [§5.37]
	std::vector<uint8_t> terrain_til_data_;
	// MissionText briefing values parsed from the mounted <mission>.bin (or the
	// medmssn.bin fallback) before load. These remain the RTXT's original cp1252
	// bytes so the S2C 0x7E payload is byte-exact for localized text.
	bool mission_text_loaded_ = false;
	std::string mission_briefing3_;
	std::string mission_briefing2_;
	// Numeric suffix -> original cp1252 MissionText [Locations] value.
	// bringup_host_runtime resolves these against type-2044 BMS markers in
	// spawn order for the S2C 0x0F deploy-map label block.
	std::unordered_map<int32_t, std::string> mission_location_texts_;
	// Numeric suffix -> [PeopleNames] STRNAME%03i value; promote resolves an
	// entity's authored display name (D-HUD-20) from its BMS name_index here
	// (retail: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a; the witnessed
	// resolve lives engine-side in promote.cpp).
	std::unordered_map<int32_t, std::string> mission_people_names_;
	// Build the PF_* present buffer from the client-decoded ClientState (runtime_->state()).
	PackedFloat32Array present_snapshot_from_client_replicas() const;

	// --- co-op LAN joiner: a pure non-authority np::ClientRuntime (Joiner role, built in enable_join /
	// finish_load; the runtime_ member is declared in the P7 block below). joiner_pump drives the
	// connect legs + the per-frame S2C->ClientState fold + the C2S 0x0C uplink over a dialed UdpPump.
	// It runs run_logic_tick(false) for its own player L (a motor-driven pool-0 entity spawned at the
	// H-learned pose); remote entities render wire-direct (present + wire_present_pass). enable_join
	// turns it on; a sim is host XOR joiner. [orig: NapiNPClientMsg_0x00C @0x42E730 self name-match]
	bool joiner_ = false;
	// The joiner's per-frame world<->net bridge (S10a, ADR 0028): the frame
	// sequence, its latches (started/spawned/redeploy/tripwire), the
	// wire-header materializer, and the per-replica resolver state all live in
	// engine/net/npruntime. This binding supplies the shell legs as PumpHooks.
	opennova::np::JoinerWorldBridge joiner_bridge_;
	// The shell-asset leg of the bridge's materialize phase: rebuild the
	// collision/occlusion/trait/seat caches for the changed streamed rows.
	void on_replica_world_changed(
			const opennova::netsim::ClientWorldSyncResult &p_sync);
	// The env-gated ~1 Hz tripwire print (the bridge owns the sampled state).
	void print_joiner_net_diagnostic_sample();
	// Input-latch resets + adm resolution at L's spawn/redeploy edges.
	void on_joiner_local_player_spawned(int32_t p_look_heading_bam);
	void on_joiner_local_player_redeployed(int32_t p_look_heading_bam);
	// Retail authenticates with one packed Avatars.def selection for each side.
	// GameWorld resolves the active profile before enable_join; retain it here
	// because a direct-loaded join rebuilds ClientRuntime in finish_load.
	opennova::np::CharacterJoinVars join_character_vars_{};
	bool join_character_vars_set_ = false;
	// The listen host's own type-2 connection consumes the same profile shape,
	// installed before its authoritative player spawn.
	opennova::np::CharacterJoinVars local_character_vars_{};
	bool local_character_vars_set_ = false;
	// Explicit resource-corpus identity for the retail anti-cheat 0x30/0x31
	// sources. Empty keeps safe silence. Retained across direct-load runtime
	// rebuilds just like the character and charattr profile data.
	std::string join_integrity_profile_id_;
	// The install root the joiner's JOIN VERSIONCRCSTRING checksum reads its
	// loose expansion/<name>/version.txt from (D-NET-166). Empty keeps the
	// golden "0". Retained across direct-load runtime rebuilds.
	std::string join_expansion_version_root_;
	// The live environment owner consumes each decoded phase-2 edge once. The
	// ClientState revision is monotonic for one ClientRuntime; fresh runtimes
	// reset this cursor with their other receive-side cursors.
	uint32_t joiner_environment_revision_seen_ = 0;
	// Last authoritative S2C 0x5A grant installed into the local slot pool.
	// Requests may rebuild optimistically, but only a newer host grant becomes
	// the durable spawn/respawn kit.
	uint64_t joiner_applied_loadout_revision_ = 0;
	// The shell applies the profile kit/class right after runtime setup — on a
	// joiner that is BEFORE L exists (L spawns on the name-match). The class
	// latch lives on the world-typed loadout aggregate
	// (local_loadout_.pending_player_class); L's spawn block stamps it with
	// the equipped weapon, the same Player_InitPlayer-time arm the host's own
	// spawn performs. [orig: Player_InitPlayer weapon leg @ 0x4e15f0]
	// Send one framed datagram to the dialed host (the joiner's send_datagram).
	void ship_to_host(const std::vector<uint8_t> &dg);

	// Phase 2 (the moving player): the latest input from the host controller, applied to the
	// local player's AiEntity at the TOP of each frame (net-before-logic, ADR 0009). The
	// player then locomotes through the same infantry motor as an NPC. [net-re §5.38]
	opennova::world::PlayerInput player_input_{};
	void apply_player_input_pre_tick();
	// One live verdict for body pose and attach-label selection. The caller
	// supplies the current local body so the query cannot accidentally consume
	// the previous tick's aimed_shot_available cache.
	// Retail witness: Player_CanFireWeapon @0x5cf780.
	bool local_player_can_fire_weapon(
			const opennova::world::AiEntity *p_body) const;
	// Retail's held-weapon draw gate, local-player branch — the weapon model is shown
	// iff the soldier may fire it. [orig: Entity_CanFireWeapon @ 0x4dcb10]
	bool local_held_weapon_visible(const opennova::world::Entity &p_entity) const;
	// Retail has one input-owned entity view (the mouse accumulators ARE the
	// entity yaw/pitch). An authoritative attach snap or the ladder legs (the
	// CL alignment chase, the ±120° clamp, the post-ladder pitch restore) can
	// write the split world/AI copy during the logic tick, so mirror any
	// sim-written view value back into the host latch before the next pre-tick
	// input write can restore the old look.
	void sync_local_mounted_input_heading();
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
	// The M-cycle map mode + the two radar zooms — the engine-side state
	// machine carries the retail lifecycle (cycle, zoom routing, spawn
	// reset, the dead-player clear); this class only routes requests and
	// tick edges into it (witness at hud::HudMapControl).
	opennova::hud::HudMapControl hud_map_control_;

	// --- the local player's equipped-weapon action FSM (net-re §5.62) ------------------
	// The 12-state action queue on the equipped slot, pumped once per logic tick after the
	// world advances [orig: WeaponAction_ProcessAllEntities @ 0x542690 in the frame loop;
	// this member owns the LOCAL player's slot. World::run_logic_tick separately pumps
	// occupied UseGun parent slots for NPC gunners in that same global phase]. The host
	// feeds the baked def via set_local_player_weapon and per-frame trigger state via
	// set_local_player_weapon_input. Presentation outputs accumulate as ordered
	// per-tick records because several logic ticks can run per render frame; the
	// snapshot's monotonic serials remain diagnostics/rebuild state.
	// The whole moved equipped-weapon state (S7a, ADR 0028): def/slot/
	// rings/serials/UseGun/PowerThrow/presentation events live in
	// engine/runtime/world (world/player_weapon.h); this binding marshals
	// installs, inputs, drains, and the two wire request records.
	opennova::world::LocalPlayerWeapon local_weapon_;
	using LocalUseGunSwitch = opennova::world::LocalUseGunSwitch;
	// The retained weapon.def parse (S6b, ADR 0028): the by-name install reads
	// rows from here so the ACCEPT chain needs no shell dictionary. Freed on
	// reload and in the destructor.
	DefWeaponsFile weapon_defs_ = {};
	bool weapon_defs_loaded_ = false;
	// The equipped rig's per-key clip variant lengths (the native replacement
	// for the render skeletal's clip_seconds feed).
	opennova::simassets::AdmClipIndex weapon_clip_index_;
	// The UseGun borrow + slot selection moved to world/player_weapon.h
	// (S7a); these inline wrappers keep the family's call sites unchanged.
	opennova::world::WeaponSlotState *active_local_weapon_slot() {
		return world_ ? opennova::world::active_local_weapon_slot(
				*world_, local_weapon_) : &local_weapon_.slot;
	}
	const opennova::world::WeaponSlotState *active_local_weapon_slot() const {
		return world_ ? opennova::world::active_local_weapon_slot(
				*world_, local_weapon_) : &local_weapon_.slot;
	}
	bool local_usegun_switch_is_instant() const {
		return world_ != nullptr && opennova::world::
				local_usegun_switch_is_instant(*world_, local_weapon_);
	}
	void sync_local_usegun_weapon_transition() {
		if (world_) opennova::world::sync_local_usegun_weapon_transition(
				*world_, local_weapon_);
	}
	void commit_local_usegun_weapon_switch() {
		if (world_) opennova::world::commit_local_usegun_weapon_switch(
				*world_, local_weapon_);
	}
	void queue_local_usegun_weapon_switch(bool p_same_category) {
		if (world_) opennova::world::queue_local_usegun_weapon_switch(
				*world_, local_weapon_, p_same_category);
	}
	void install_local_player_weapon(const Dictionary &p_def,
	                                 const Dictionary &p_clip_seconds,
	                                 bool p_preserve_slot_state,
	                                 bool p_allow_same_weapon_rebake);
	// The Dictionary/def-row feeders both build the world install payload.
	static opennova::world::WeaponInstallData install_data_from_dict(
			const Dictionary &p_def, const Dictionary &p_clip_seconds);
	void tick_local_player_weapon();

	// --- the local player's weapon slot pool + spawn kit + map rules -------------------
	// [orig: weaponSlotArrayBase @ 0xB75FD4 (780 x 100 B) + g_localAmmoPools @ 0xB75FE8 +
	//  restrictionData @ 0x24D4E00 + g_armoryWeaponAvailability @ 0x24D5600; the loadout
	//  grill 2026-07-18]
	opennova::world::WeaponInventory local_inventory_;
	bool local_inventory_valid_ = false;
	// The world-typed loadout aggregate (S7b): the availability table, the
	// resident spawn kit (retail's restrictionData — spawn_kit_set
	// distinguishes an EXPLICIT kit, even the armory's all-NONE empty one,
	// from the never-set state that takes the WPN_M4AUTO default [orig: the
	// default literal @ 0x5246be]), and the pre-spawn class latch. The rules
	// over it live in world/player_loadout.h; this binding converts
	// dictionaries and routes the joiner wire submissions.
	opennova::world::LocalPlayerLoadout local_loadout_;
	// The mission's loadout/availability chunks, stashed at finish_load for the
	// weapon-table load to promote through the witnessed SP-vs-net gate —
	// retail reads the chunks with the catalog already loaded
	// (Game_StartMission parses weapon.def @ 0x5254b3 before
	// Mission_LoadBMSFile runs); our mission loads first, so the promotion
	// waits for load_weapon_table. [orig: Mission_LoadBMSFile @ 0x40F4E0]
	std::vector<std::pair<std::string, int32_t>> mission_availability_rows_;
	std::vector<opennova::world::WeaponKitEntry> mission_kit_rows_;
	// The ACTIVE player weapon profile record — retail's g_charSelClass slot: two
	// side blocks (blue/red), each carrying the class byte that selects both the wire
	// class and one of five 2048-byte kit pages, plus the single-player page.
	// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0 reads five 0x1080C records;
	//  Game_StartMission @0x525767..@0x525836 selects the block and the page]
	// Seeded from the shipped defaults so it is NEVER empty even when weapon.sav is
	// absent [orig: PlayerProfile_InitDefaults @0x54bb40 — class 8 both sides,
	// WPN_M4AUTO/WPN_AK47AUTO pages].
	opennova::playersav::Record weapon_profile_ =
			opennova::playersav::make_defaults().slots[0];
	bool weapon_profile_loaded_ = false;
	// The switch commit/outcome/gates moved to world/player_weapon.h (S7a);
	// wrappers keep the family's call sites unchanged.
	void commit_pending_weapon_switch() {
		if (world_) opennova::world::commit_pending_weapon_switch(
				*world_, local_weapon_,
				local_inventory_valid_ ? &local_inventory_ : nullptr);
	}
	void handle_weapon_switch_outcome(
			const opennova::world::WeaponSwitchOutcome &p_out) {
		if (world_) opennova::world::handle_weapon_switch_outcome(
				*world_, local_weapon_,
				local_inventory_valid_ ? &local_inventory_ : nullptr, p_out);
	}
	opennova::world::WeaponSwitchGates local_weapon_switch_gates() const {
		return world_ ? opennova::world::local_weapon_switch_gates(
						*world_, local_weapon_,
						local_inventory_valid_ ? &local_inventory_ : nullptr)
				: opennova::world::WeaponSwitchGates{};
	}
	// Player_InitPlayer's weapon leg [orig: @ 0x4e15f0]; shared by table load,
	// respawn, and the ACCEPT apply (which passes the freshly stored kit).
	void rebuild_local_player_loadout(bool p_select_spawn_default);
	// Copies the assigned side's profile page into the resident kit buffer
	// (local_loadout_.spawn_kit) in a live session — retail's single restrictionData [orig: Game_StartMission
	// @0x525813; re-run per side by NapiNPClientMsg_TeamAssign @0x431a9a]. False when
	// not in a session, before the catalog exists, or when the page resolves empty.
	bool seed_session_kit_from_profile();
	// Re-copies the page when the SIDE the team selector names stops matching the side
	// the resident buffer came from — the S2C 0x04 latch arriving after the catalog, or
	// a later S2C 0x50 reassignment [orig: byte_A85B48 @0x425499 / @0x4319db].
	bool reseed_session_kit_on_side_change();
	// Which side the resident kit buffer was last copied from (-1 = never seeded).
	int weapon_profile_seeded_side_ = -1;
	// Push the submission content (class + ADM rows + equipped combo) into the joiner
	// runtime's 0x2F loadout-submission seam. No-op for hosts/SP.
	void push_joiner_loadout_kit();
	// Re-entry latch: rebuild_local_player_loadout re-pushes the seam once the live
	// equipped combo has settled, so the push must never drive a rebuild back.
	bool pushing_joiner_loadout_kit_ = false;
	bool apply_local_player_loadout_impl(
			const TypedArray<Dictionary> &p_kit, int p_player_class,
			bool p_submit_joiner_request);
	// The typed-rows core of the apply (the dict overload converts, the 0x5A
	// grant path feeds np::kit_from_authoritative_grant's rows directly).
	bool apply_local_player_loadout_rows(
			std::vector<opennova::world::WeaponKitEntry> p_kit,
			int p_player_class, bool p_submit_joiner_request);
	// Fold the latest authoritative S2C 0x5A grant into the local slot pool at
	// the same recv-before-actions boundary as the retail handler.
	void apply_joiner_authoritative_loadout();
	// The deploy/spawn-zone registry (letters/pick-index space), built lazily per
	// load [orig: Entity_BuildSpawnZoneList @0x43EAE0].
	const opennova::world::SpawnZoneRegistry &deploy_zone_registry();
	opennova::world::SpawnZoneRegistry deploy_zone_registry_;
	bool deploy_zone_registry_built_ = false;
	// Copy each accepted kit row's fourth value into the retail per-ammo
	// shooter damage-class table (1 = x0.9, 2 = x1.1).
	void sync_local_player_damage_classes();

	// --- the local player's view state (ADS ease + 3P anchor chase) --------------------
	// Ticked at the world cadence immediately before the weapon pump, so camera lag and
	// the ADS settle promoter are render-rate independent and action effects observe the
	// same tick's promoted scope state [orig: Player_UpdatePerFrame call @ 0x42c18e
	// precedes WeaponAction_ProcessAllEntities call @ 0x526786].
	// The sim OWNS the engaged bit [orig: g_scopeEngaged @ 0x82CE94]: the host requests
	// toggles and reads the state; the FSM's unscope/rescope events flip it here.
	opennova::world::PlayerViewState player_view_{};
	// The FP viewmodel motion-lead tracker (per render frame) and the local
	// entity's per-62.5Hz-tick movement delta it samples.
	opennova::world::PlayerViewMotionLead fp_motion_lead_{};
	float local_tick_delta_[3] = {0.0f, 0.0f, 0.0f};
	float local_tick_prev_pos_[3] = {0.0f, 0.0f, 0.0f};
	bool local_tick_prev_valid_ = false;
	// The binocular toggle seeds one fixed-radius random aim displacement. It
	// survives movement/death/third-person suppression until the raw toggle drops.
	float binocular_yaw_offset_deg_ = 0.0f;
	float binocular_pitch_offset_deg_ = 0.0f;
	void reset_local_player_view_effects();
	void refresh_local_player_view_effects();
	void tick_local_player_view();
	// The dead-player map-mode clear, run once per advanced tick (witness at
	// hud::HudMapControl::on_local_player_dead).
	void tick_hud_map_death_gate();

	// --- P7: the in-match runtime as a THIN ADAPTER over engine/net/npruntime ----------------
	// One in-match runtime funnels every live path: the host/SP game is the §5.0 mode-3
	// listen server (NapiNPServerCtx ctx_ + its own loopback client over host_loop_, driven by
	// the npruntime owner loop = Server_TickUpdate + tick_connections + handle_server_datagram);
	// the joiner is a non-authority np::ClientRuntime. The Godot net bindings stay PURE socket
	// pumps — all protocol/crypto/framing lives in libs (ADR 0009-0012, .agents/network.md).
	// host_loop_ MUST be declared before runtime_: the HostClient ClientRuntime holds a
	// non-owning reference into host_loop_, so the loopback has to outlive (and not move under)
	// the runtime.
	// The host state — ctx + per-peer transports + now_tick + serve_and_play — shared with the promoted
	// owner loop host_session_pump (engine/net/npruntime). MUST be declared before ctx_ (the alias) and before
	// host_loop_ (host_owner_.host_loopback points at host_loop_, set at bring-up). Replaces the old
	// ctx_/peers_/PeerLink members; admit_peer/dispatch_event moved into libs (np::, over host_owner_).
	opennova::np::HostOwner host_owner_;
	opennova::np::NapiNPServerCtx &ctx_ = host_owner_.ctx;    // alias: host only (is_authority)
	opennova::netsim::LoopbackChannel host_loop_;             // the host's own dcb-2 client; Server_TickUpdate's 0x0A target
	std::unique_ptr<opennova::np::ClientRuntime> runtime_;    // HostClient (host/SP) OR Joiner; the present-snapshot source
	// Retail loads this process-scoped table from charattr.def before joining.
	// Keep the byte image outside ClientRuntime so a direct mission load can
	// reinstall it when finish_load rebuilds an as-yet-unstarted joiner.
	opennova::np::CharAttrChallengeTable charattr_challenge_table_{};
	bool charattr_challenge_loaded_ = false;
	opennova::bms::File mission_file_;                        // persisted so ctx_.mission outlives the match (the 0x0B burst body)
	std::string joiner_player_name_;                          // persisted for the Joiner runtime ctor on (re)load
	// One immutable items.def catalog supplies both the authoritative entity stamp
	// and the decoded-client record-width resolver. The callback codec, physical
	// motion family, and allocation inputs remain independent traits.
	std::shared_ptr<const opennova::netsim::ItemReplicationCatalog>
			item_replication_catalog_;
	Ref<ItemDatabase> item_replication_catalog_db_;
	uint64_t item_replication_catalog_revision_ = 0;
	// Mission-scoped source for the authoritative half of the same contract.
	// World::restore rewinds registry entities to the pre-trait promotion baseline,
	// so restart reapplies this database before rebuilding decoded client replicas.
	Ref<ItemDatabase> item_traits_db_;
	// Install the catalog resolver on runtime_'s view (no-op until both exist). Called from
	// resolve_item_traits, finish_load (per-load runtime rebuild), and enable_join.
	void install_item_class_resolver();
	// Install or clear the retained boot charattr table on the current Joiner runtime.
	void install_charattr_challenge_table();
	// Copy the per-class ATTRIBUTES words into World::class_attribute_flags -- the
	// joiner's live table when one exists (S2C 0x41 mutates it in receive order),
	// else the boot copy. Runs at world creation, at every table install, and
	// after each net pump (retail: AnimMap_IsSlotActive @0x4125e0 reads the one
	// g_CharAttr table CharAttr_LoadFromDef @0x412140 fills and the 0x41 arm
	// AnimMap_SetSlotProperty @0x412890 clears; see docs/interface/hud-re.md).
	void sync_class_attribute_flags();
	// Install the retained retail player-profile join block on the current runtime.
	void install_character_join_vars();
	// Install or clear the explicitly selected retail integrity corpus profile.
	void install_join_integrity_profile();
	// Install the retained JOIN-checksum install root (D-NET-166).
	void install_expansion_version_root();
	// Per-load host bring-up: mode 3 -> create_session(&host_loop_) -> configure_session_runtime
	// -> Server_InitNewRoundState -> the faithful host-player auto-spawn. Mirrors apps/nw_server.
	void bringup_host_runtime(const opennova::bms::File &file);
	// Route the listen host's socketless gameplay C2S through the same message
	// dispatcher as remote connections before Server_TickUpdate drains 0x0C.
	void drain_host_client_gameplay_requests();
	// The per-frame host owner loop (local loopback gameplay + recv-drain -> tick_connections ->
	// Server_TickUpdate -> S2C flush -> fold host_loop_ into ClientState). Socket legs gated on
	// host_listen_ (pure SP has none).
	void host_pump();
	// The per-frame non-authority client loop, now the bridge's pump (S10a):
	// this binding builds the PumpContext/PumpHooks and delegates. The
	// pre-mission preload pump shares the bridge's hello latch + clock.
	// Deposit received framed datagrams for this frame's recv pump.
	void joiner_deposit_inbound();
	void joiner_pump();
	// Wire-side authored-shape resolution for one decoded runtime type id
	// (items.def graphic -> the shared by-graphic collision model cache).
	WireCollisionShape wire_collision_shape_for_type(uint16_t type_id);

	// Terrain the AI grounds on. We own copies of the shell's depth buffer + 16x16 sector grid so
	// the portable TerrainHeightField's raw pointers outlive the source TerrainData and survive
	// a reload (reset_world rebuilds ai_; apply_terrain_to_ai re-points it). Empty = no grounding.
	std::vector<uint16_t> terrain_heightmap_;
	std::vector<int> terrain_sector_grid_;
	opennova::terrain::TerrainHeightField terrain_field_;
	// Charmap (surface-type) raster copy + the sampler view the footstep pass
	// reads through world.surface_map [orig: Terrain_GetSurfaceTypeAtPosition
	// @ 0x606510]. Shares terrain_sector_grid_/origins with the height field.
	std::vector<uint8_t> surface_indices_;
	opennova::terrain::SurfaceTypeMap surface_map_;
	// The placed-tile surface override (D-SND-15): the mission .til entries
	// plus the tileset's .TSD-fed tile-index -> surface table, both resolved
	// engine-side (terrain_query surface_tiles.h — the witnesses live there).
	// Owned here like the heightmap so world.surface_map's raw pointers
	// survive reset_world; the zero table is retail's no-.TSD default.
	std::vector<opennova::terrain::SurfaceTileEntry> surface_tiles_;
	std::array<uint8_t, 256> tile_surface_table_{};
	// SndProf.def text + water plane held for (re)application on reset_world.
	std::vector<uint8_t> sndprof_text_;
	int32_t env_water_z_q16_ = 0;
	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	std::vector<CharacterSexRow> character_sex_rows_;
	void apply_terrain_to_ai();
	void apply_sound_state_to_world();
	void apply_character_traits_to_world();

	// Anim-driven soldier locomotion: the .adm/.bad-backed root-motion source the infantry
	// motor integrates (world/infantry.h). Owned here so it survives reset_world; the fresh
	// ai_ is re-pointed at it like the terrain field. Empty = soldiers hold and stand.
	opennova::simassets::AdmRootMotion infantry_anim_;
	// Per-entity ADM resolution is a spawn-time invariant, not a one-shot mission-load
	// sweep: joiner-local and host-admitted players are attached to the AI pool after
	// MissionPresentation's initial call. Retain the resolver inputs and advance this
	// high-water mark whenever AiSystem gains entries (its attach storage is append-only).
	void resolve_client_row_adm_ids();
	std::unordered_map<uint16_t, int> client_row_adm_by_type_;
	Ref<ResourceRoot> infantry_adm_resource_root_;
	Ref<ItemDatabase> infantry_adm_item_db_;
	int infantry_adm_resolved_ai_count_ = 0;
	void apply_root_motion_to_ai();
	void reset_infantry_adm_ids();
	void resolve_new_infantry_adm_ids();
	static void resolve_infantry_adm_before_server_tick(void *p_context);
	std::vector<opennova::mission::ItemSeatSpec> item_seat_specs_;
	// The two witnessed .aip profile speeds per ai_textfile, fed to
	// PromoteOptions before promotion (see promote.h AiProfileRow).
	std::vector<opennova::mission::PromoteOptions::AiProfileRow> ai_profiles_;
	// The shared install tail (both install orders): sort for the per-frame
	// binary search, stamp turret clamps, refresh live pool-1 rows, and re-sync
	// the header-only materializer image.
	void finalize_installed_seat_specs();
	void refresh_item_seat_spec(opennova::world::Entity &p_entity);
	// Resolve each spec's turret clamp window from its primary weapon's
	// weapon.def rows. Called from BOTH install orders (specs-then-table and
	// table-then-specs); all-zero = not authored, no clamp.
	void stamp_seat_spec_turret_limits();
	opennova::mission::PromoteOptions promote_options() const;

	void reset_world();

public:
	// The authority network-environment mirrors — the weather device drives
	// these natively (Weather.advance_world_driven / run_mission_start_boundary).
	void set_network_environment(int64_t p_fog_target_q16,
	                             int64_t p_fog_current_q16,
	                             int64_t p_fog_accel_clamp,
	                             int64_t p_tod_fixed24,
	                             int64_t p_tod_advance_per_tick,
	                             int64_t p_quake_ticks,
	                             int64_t p_cloud_scroll_rate_target,
	                             int64_t p_rain_pct_current_q16,
	                             int64_t p_overcast_blend_q16,
	                             int64_t p_precipitation_kind);
	void advance_network_environment_tick();
	void initialize_network_environment_mission_start();

private:
	// Shared post-promote wiring: load the BMS arrays, register the systems, run the
	// pre-mission pass, capture the restore baseline. Marks the sim loaded.
	void finish_load(const opennova::bms::File &file);
	// Stash the mission's loadout/availability chunks (plain world types) for
	// load_weapon_table to promote through the engine's SP-vs-net gate
	// (world/player_loadout.h). [orig: Mission_LoadBMSFile @ 0x40F4E0]
	void stash_mission_loadout_rules(const opennova::bms::File &p_file);
	void apply_host_session_mission_header(const opennova::bms::File &file);
	void refresh_host_accept_config();

protected:
	static void _bind_methods();

public:
	Simulation();
	// A dying joiner sim ships the retail goodbye burst before the socket drops — retail sends
	// its disconnect packets from the connection teardown that Destroy also runs, so freeing the
	// sim (ESC abort, watchdog abort, return-to-menu) must not leak an admitted peer on the host.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0, called by Destroy]
	~Simulation() override;

	// Load + promote an in-memory bms::File. This remains a narrow fixture/tooling seam;
	// ONED gameplay launches only from a saved loose .bms through load_mission_file().
	bool load_from_mission_data(const Ref<MissionData> &p_mission);
	// S9 (ADR 0028): the ordered mission boot — engine/runtime/mission
	// runtime_boot owns the sequence + the file-resolution policy; this entry
	// supplies the step bodies over the existing feeds. The shell composes
	// role bring-up before it and presentation after it. Returns OK or
	// ERR_CANT_OPEN (mission missing / load failed; the sequence aborted).
	int64_t boot_mission(const Ref<MissionData> &p_mission,
			const Ref<ResourceRoot> &p_resource_root,
			const Ref<ItemDatabase> &p_item_db,
			const Ref<TerrainData> &p_terrain,
			const PackedByteArray &p_terrain_til, const String &p_wac_basename,
			const String &p_infantry_adm, const String &p_mission_file_basename,
			bool p_playable);
	// The boot's native resolution decisions, for the shell's S9 assert-equal
	// soak (text source/size, .aip rows, the effective adm name).
	Dictionary get_mission_boot_debug() const;
	// Load + promote a .bms mission from disk; false on parse failure.
	bool load_mission_file(const String &path);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const;
	int get_session_state() const {
		return static_cast<int>(mission_session_.state());
	}
	String get_session_error() const {
		return String::utf8(mission_session_.last_error().message.c_str());
	}

	// Transport.
	bool is_playing() const {
		return mission_session_.state() ==
				opennova::np::MissionSessionState::Running;
	}
	// Advance exactly ONE 62 Hz logic tick — the original's engine tick. The per-system
	// dividers gate INSIDE the systems (the WAC VM self-gates to every 62nd tick, the BMS
	// evaluator quarter-passes every 16th), exactly where the original keeps them. Returns
	// false when the session cannot take a direct local/test tick. Banking wall
	// clock and dispatching 0..N ticks per render frame belongs to MissionSession
	// (the Game_MainLoop @0x52b630 accumulator) — a render frame is NOT one tick.
	// [orig: Game_ProcessMainFrame @0x5263f0 (one current_tick++ @0x24c1968)]
	bool step();

	// Turn the sim into an SP in-process listen server (ADR 0011): the host serializes
	// real entity state onto an in-process loopback (Server_TickUpdate's per-connection S2C
	// fan), the local client decodes it, and the present pass reads that decoded state. Call
	// BEFORE loading a mission — the next load stands up the npruntime host runtime. Disabling
	// reverts to the direct AI-pool present used by explicit non-network fixtures.
	void enable_listen_server(bool p_enable);
	bool is_listen_server() const { return listen_server_; }

	// Feed the mission's raw terrain-tile (.til) file bytes so the listen host streams the S2C 0x45
	// terrain-tile load to joiners (climbs the client's g_loading_progress 5 -> 6; §5.37). The Godot
	// shell owns the resource root, so it read_file()s the .til (named by the .trn tileinfo) and passes
	// the bytes here BEFORE loading the mission. Empty / not-called => 0x45 is faithfully skipped.
	void set_terrain_til_data(const PackedByteArray &p_til_bytes);
	// Feed the raw RTXT mission string table before load. Native lookup avoids a
	// UTF-8 round-trip and resolves briefing2's retail briefing fallback.
	void set_mission_text_data(const PackedByteArray &p_rtxt_bytes);
	// Overlay the active game-type scoring row from retail's loose VERSION 40
	// score.ini. Returns false for absent, malformed, or unsupported data.
	bool set_score_config_data(const PackedByteArray &p_score_ini_bytes);

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
	// tests/npruntime/handshake_server_test, the P2 retarget of the retired
	// novaworld host_session_accept_test). Returns true if an entity was
	// spawned + bound. No-op unless host listening is on.
	bool admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team);

	// --- co-op LAN joiner (D.2) -------------------------------------------
	// Turn the sim into a co-op LAN JOINER: dial the host at `host_ip:port` and run
	// the witnessed in-match JOIN as a non-authority client. `player_name` rides the
	// game ClientAuth.NA and is the key the host echoes into our organic-spawn record so
	// we self-identify (name-match) and learn our wire handle H. Call BEFORE loading
	// the mission (the next load arms the joiner frame path). Implies client replicas;
	// a sim is host XOR joiner. Returns false if the socket can't be dialed.
	bool enable_join(const String &p_host_ip, int p_port, const String &p_player_name);
	bool is_joiner() const { return joiner_; }
	// True while a live net session owns this sim: the world tick is the ONLY
	// pump for the session socket, so the Play/Step/Stop transport locks out
	// (retail multiplayer has no pause; a stopped listen host reaps every
	// joiner at cs_dir0.timeout_ms [orig: CNapiNetwork_Init @ 0x4ca4a0]).
	// The single home for the rule — F3 transport, MCP, and the ESC pause all
	// read this predicate.
	bool is_transport_locked() const { return joiner_ || host_listen_; }
	// The fixed logic-tick quantum (1/62.5 s) — the ONE cadence constant,
	// re-exported from the engine accumulator for GDScript composition.
	static double tick_dt() { return opennova::world::TickAccumulator::kTickDt; }
	// The tick cadence as a rate, and wall-clock ms -> whole logic ticks —
	// re-exports of the engine tick home (world/tick_accumulator.h carries
	// the current_tick witness).
	static double ticks_per_second() { return opennova::world::kTicksPerSecond; }
	static int ticks_from_ms(int64_t p_ms) {
		return opennova::world::ticks_from_ms(p_ms);
	}
	// The epilog/debrief ESC-less exit timeout in seconds, derived from the
	// engine tick constants (world/world.h kEpilogExitTimeoutTicks).
	static double epilog_exit_timeout_seconds() {
		return opennova::world::kEpilogExitTimeoutTicks *
				opennova::world::TickAccumulator::kTickDt;
	}

	// --- Portable session frame (ADR 0035) --------------------------------
	// The input and outcomes are typed values. The one temporary tick sink keeps
	// per-tick Godot presentation synchronous during catch-up without installing
	// a persistent callback bus; GameFramePipeline orders concrete devices around
	// this call.
	Ref<MissionFrameOutcome> advance_session_frame(
			const Ref<MissionFrameInput> &p_input,
			const Callable &p_tick_sink = Callable());
	Ref<MissionFrameOutcome> step_session_frame(
			const Ref<MissionFrameInput> &p_input,
			const Callable &p_tick_sink = Callable());
	bool pause_session();
	bool resume_session();
	bool reset_session();
	void fail_session(const String &p_reason);
	void close_session();
	// Last frame's spans: {tick_us, sim_us, present_us, effects_us, net_us,
	// did_tick, ticks} — the probe/F3 accounting seam.
	Dictionary get_session_perf() const;
	// Set the per-side character ids/classes/avatar bytes carried by ClientAuth.
	// Must be called before enable_join; later runtime rebuilds retain the values.
	void set_join_character_profile(const Dictionary &p_profile);
	void set_local_character_profile(const Dictionary &p_profile);
	// Select a registered retail resource-corpus profile for S2C 0x30/0x31.
	// Empty clears it; an unknown id also clears it and returns false.
	bool set_join_integrity_profile(const String &p_profile_id);
	// The install root whose loose expansion/<name>/version.txt feeds the JOIN
	// VERSIONCRCSTRING checksum (D-NET-166). Empty keeps the golden "0".
	// Retained across runtime rebuilds like the character/integrity data.
	void set_join_expansion_version_root(const String &p_game_root);
	// Load the process-scoped anti-cheat CHARACTER table before the first join
	// network pump. Missing/empty charattr.def is soft and leaves all rows inactive,
	// matching Game_Run's continue-after-error behavior.
	bool load_charattr_challenge(
			const Ref<class ResourceRoot> &p_resource_root);
	// Ship the 0x46 ClientGoodBye burst now (idempotent; joiner-only no-op otherwise). The
	// destructor calls this too, so explicit calls are only needed when the socket must close
	// before the sim is freed.
	void leave_net_session();
	// Retail connects before constructing the wire-header world: drive only the socket/session
	// legs until the terminal pre-world sync marker has been received and ACKed
	// (S2C 0x7B identifies the mission earlier), then resume the same connection
	// after the caller has constructed it. No World tick or gameplay uplink runs here.
	void set_join_world_ready(bool p_ready);
	// Freeze the renderer's unique loaded, non-foliage .3DI definition count
	// into the joiner's C2S 0x3D paging seam. GameWorld calls this once after
	// mission/render model setup and before revealing the loaded world; later
	// S2C entity spawns and presentation loads deliberately cannot mutate it.
	void finalize_loaded_model_challenge_snapshot();
	bool poll_join_preload();
	bool is_join_preload_ready() const;
	// The joiner's admission-FSM stage name (diagnostics: the shell's post-load join
	// watchdog names the stage a stalled join is parked in). Empty when not joining.
	String get_join_admission_stage() const;
	bool has_join_mission() const;
	String get_join_server_name() const;
	String get_join_mission_name() const;
	String get_join_mission_file() const;
	// Consume the latest decoded S2C 0x0A phase-2 state once per receive
	// revision. Empty means no new authoritative sample.
	Dictionary take_join_environment_update();
	// Exact pre-world payloads retained by the joiner from retail's initial
	// state stream. The mission header is exactly 616 bytes when available. TIL
	// bytes are exposed only in COMPLETE; the explicit state distinguishes a
	// valid omitted 0x45 from a partial or malformed stream.
	PackedByteArray get_join_mission_header() const;
	int64_t get_join_terrain_til_state() const;
	PackedByteArray get_join_terrain_til() const;
	String get_join_expansion() const;
	int64_t get_join_game_type() const;
	String get_join_error() const;
	// Session loss: empty while healthy, else a player-facing reason the shell surfaces the
	// way it surfaces a join failure. Two causes — the host's explicit close (the punt
	// channel) and in-match silence past the reap window. Retail exits the mission with a
	// mapped exit reason here and shows no in-world dialog.
	// [orig: the cs_dir0.timeout_ms = 120000 reap installed by CNapiNetwork_Init @0x4ca4a0
	//  and the punt record CNapiNPConnection_HandleDescriptionPacket @0x621ae0, both ->
	//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
	String get_session_loss_reason() const;
	// The same edge as a state test rather than a presentation string: in-world surfaces
	// (the deploy screen) need to know the session is gone, not what to tell the player.
	bool is_session_lost() const;
	// True once the joiner has name-matched its organic-spawn record and received the
	// applicable deployment release (self handle H known and gameplay uplink enabled).
	bool is_joined_in_match() const;
	// Monotonic initial-admission boundary: policy + both initial loadout grants
	// have landed. Deploy-pick state is intentionally exposed separately.
	bool is_join_initial_admission_complete() const;
	// The per-second joiner trace is deliberately opt-in for release play. Set
	// OPENNOVA_NET_DIAGNOSTICS=1 to emit it. The snapshot remains available so
	// tests/debug UI can distinguish a real ordered gap from ordinary idle traffic.
	bool is_joiner_network_diagnostics_enabled() const;
	Dictionary get_joiner_network_diagnostics() const;
	// Player-paced deployment (the deploy-map screen; net-re §5.61/§5.0d). True while
	// the join owes the player a deployment pick or awaits the host's release of one —
	// the shell shows the DEATH deploy screen and the join watchdog stops (the
	// remaining transitions are player-paced).
	bool is_join_deploy_pick_pending() const;
	// The DEATH screen's SPAWNPOINTS_LIST rows: {param:int, letter:String,
	// name_key:String} per team-owned secured deploy zone, letters/names keyed by the
	// spawn-zone registry index. Row 0 (the Default Spawn, param 0) is the shell's.
	// [orig: UI_UpdateDeathScreenContent @0x5536a0]
	TypedArray<Dictionary> get_deploy_spawn_zones();
	// Send the player's deploy pick: 0 = default spawn (0xFFFF), 65534 = auto team
	// spawn (0xFFFE), else the 1-based registry index resolved to its entity handle.
	// Re-picks while awaiting the release match retail (the host silently drops an
	// invalid pick and the screen stays). [orig: Input_HandleActionBinding case 12]
	bool send_deployment_pick(int p_param);
	// The joiner's server-assigned team — the S2C 0x04 tail-byte latch the deploy
	// screen colors/filters by [orig: byte_A85B48]. 0 when not joining.
	int get_join_assigned_team() const;
	// The class-availability policy for the active session. A joiner reads the
	// retail host's S2C 0x76; an authority exposes its configured writer source.
	int get_class_allow_mask() const;
	// The JoinerConnection phase as an int (JoinerConnection::Phase), -1 when not joining.
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
	// The local player's wire identity ((pool<<12)|slot). Packed zero is valid: callers that
	// need presence use has_local_player()/the runtime's has_self_handle() instead of a sentinel.
	// The host returns its pool-0 player; a joiner returns H, the host-assigned identity that its
	// wire-present pass excludes while LocalPlayerPresenter draws the distinct local motor entity L.
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
	// Raw engine heading (BAM32) for the heading-up spinmap.
	int64_t get_local_player_heading_bam() const;
	// Radar zoom: positive = radarout (x1.15 toward 0x100000), negative =
	// radarin (x0.85 toward 4096), zero = the spawn reset (the witnessed step
	// lives in hud::spinmap_zoom_step).
	int request_hud_radar_zoom(int p_direction);
	int get_hud_radar_zoom_q16() const;
	// map_toggle's 0->2->3->0 cycle (witness at HudMinimapInput::map_mode).
	int request_hud_map_cycle();
	int get_hud_map_mode() const;
	int get_hud_big_zoom_q16() const;
	// Mission attrib bit5 (AttribFlags::RotateMap180) rotates the gameplay
	// map 180 degrees; the witness rides HudMinimapInput::flip_180.
	bool get_hud_map_flip_180() const;
	// The local player's authoritative look yaw / pitch in mission degrees (for the first-person
	// camera). yaw = 90 - heading; pitch up positive. 0 when no player is spawned.
	float get_local_player_yaw_deg() const;
	float get_local_player_pitch_deg() const;
	// The local player's current/max health and team for the HUD, mirroring the original
	// per-frame HUD info. [orig: HUD_BuildEntityInfo @0x4b8440 — health ratio +92, team +374]
	int get_local_player_health() const;
	int get_local_player_max_health() const;
	// The gamemus Var7 projection (world/music_vars.h carries the witness).
	int get_local_player_health_percent() const;
	int get_local_player_team() const;
	// The packed Avatars.def character id (npwire/character_id.h) the authority
	// stamped on the local player — entity+0x15C on the host's own spawn, the
	// named 0x0C record's id on a joiner (also before L exists); 0 = none yet.
	int get_local_player_character_id() const;
	// Authoritative armory on-show state from the spawned entity. The class is
	// playerClass +0x294; the name resolves equipped AdmDef index +0x2B0.
	// [orig: Armory_ResolveSelectedClass @0x5642f0; Player_MountWeaponSlot @0x4dfa40]
	int get_local_player_class() const;
	String get_local_player_weapon_name() const;
	// The local player's canonical body-anim slot (BodyAnim; -1 when no player). The shell
	// animates the 3rd-person avatar from this, mirroring how the present pass drives NPC models.
	int get_local_player_body_anim_slot() const;
	// The local player's full anim-state clip key ("anim_<name>", "" when no player). Carries
	// stance + jump the 8-slot BodyAnim enum can't (anim_idle_crouch / anim_jump_loop / ...); the
	// shell plays it on the avatar via ObjectModel.play_body_clip for full stance fidelity.
	String get_local_player_anim_key() const;
	// The sim-owned stance latch (0 stand, 1 crouch, 2 prone) — the
	// dword_B76484 prone-latch equivalent the render-slot drape gate reads
	// (retail: RenderSlot_DrawAllDrapes @0x5d6e81 reads
	// g_PlayerStanceProneLatch, see docs/render/render-lighting-re.md).
	int get_local_player_stance_latch() const { return stance_latch_; }
	// The HUD stance icon index (0 stand / 1 crouch / 2 prone) from the sim's
	// authoritative stance state [orig: HUD_BuildEntityInfo @0x4b860c —
	// entity+300 flags 0x200=crouch -> 1, 0x100=prone -> 2]. The witnessed
	// source is the body state, never the anim clip name.
	int get_local_player_stance() const;
	// Read-only F3 card: the local player's water/eye classification — the
	// exact terms the recoil/spread/aimed-shot gates consume. Dictionary:
	//   body_z, eye_height, eye_z, water_z: float mission units
	//   water_authored, drowning, eye_below_water, submerged, in_air: bool
	//   stance: int (0 stand / 1 crouch / 2 prone)
	// Empty while no local player resolves.
	Dictionary get_local_player_body_debug() const;
	int get_local_player_anim_phase_ticks() const;
	String get_local_player_anim_source_key() const;
	int get_local_player_anim_source_phase_ticks() const;
	float get_local_player_anim_blend_weight() const;
	// The local player's third-person aim-overlay state — the torso bend. Dictionary:
	//   valid: bool; aim_state: bool (anim-state flag 0x40 — the bend branch);
	//   body: Vector3 mission-euler degrees (pitch, yaw, roll) for the avatar node basis;
	//   angles: PackedVector3Array[9] mission-euler degrees per anim::OverlayClass.
	// The blends run in exact BAM int math [orig: Entity_BuildBoneTransformMatrices
	// @0x4b1290; docs/world/world-wac-ai-re.md §14]; the shell converts each triple with
	// MissionObjectPlacer.bms_to_godot_basis (the single-sourced frame conversion) and
	// feeds ObjectModel.set_aim_overlay. Empty/invalid when no player.
	Dictionary get_local_player_aim_overlay() const;
	// The third-person held-weapon model name for an ADM index (weapon.def gfx3).
	String get_weapon_third_person_model(int p_adm_index) const;

	// --- the local player's equipped-weapon FSM (net-re §5.62) -------------
	// Install the equipped weapon: p_def is the WeaponDatabase weapon dict (the
	// {actions, flags, clipsize, startrounds} slice is consumed) and p_clip_seconds
	// maps each .adm clip key to its VARIANT lengths in SECONDS — a
	// PackedFloat32Array in .adm file order (SkeletalAnim.get_clip_variant_lengths;
	// a plain float is accepted as a single-variant convenience). The lengths seed the
	// per-slot rings and the Anim_InitActions bake consumes them ring-wise: one
	// serve-then-advance read per 'auto' delay field [orig: @ 0x541fa0;
	// Anim_GetDurationTicks @ 0x53ee10]. A normal install is a real mount and
	// resets the personal slot unless p_preserve_slot_state selects an already-live
	// UseGun parent/personal slot.
	void set_local_player_weapon(const Dictionary &p_def, const Dictionary &p_clip_seconds,
	                             bool p_preserve_slot_state = false);
	// The production mount (S6b, ADR 0028): find the row in the RETAINED
	// weapon.def parse, bake the FSM def from it, and seed the clip rings from
	// the rig's own .adm through the sim's mounted index — one step at ACCEPT
	// time, no shell dictionary and no render dependency [orig: the ACCEPT
	// chain rebuilds the slot table + mounts with no render dependency —
	// WeaponSlotTable_LoadAllFromDefs @ 0x5414e0 + Player_MountWeaponSlot
	// @ 0x4dfa40; Anim_InitActions @ 0x541fa0 bakes the delays]. Returns false
	// when the name is not in the retained table (caller keeps the current
	// weapon, mirroring the armory guard). The Dictionary pair above survives
	// as the GUT synthetic-def seam and retires with S7a.
	bool install_local_player_weapon_by_name(const String &p_weapon_name,
	                                         bool p_preserve_slot_state = false);
	// Render-side late binding of .adm clip lengths for the already-mounted def.
	// This is the only path allowed to preserve a same-name live action slot and
	// queued presentation [orig: FP model resolve @ 0x4ded60 is not a mount].
	void rebake_local_player_weapon(const Dictionary &p_def,
	                                const Dictionary &p_clip_seconds,
	                                bool p_preserve_slot_state = false);
	void clear_local_player_weapon();
	void set_local_player_first_person_model_available(bool p_available);
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
	// Retail action 26 (default B): toggles the persistent binocular request.
	// The effective raised/view bits are derived each tick from movement, life,
	// round-end, and camera mode. Returns the new requested state; false also
	// represents a refused toggle.
	bool request_local_player_binoculars_toggle();
	// Retail action 41 (default N), deliberately independent of the mission's
	// EnableNVG night-semantics flag. Returns the new active state.
	bool request_local_player_nvg_toggle();
	// Retail actions 56/57 (default +/-), available even while NVG is off.
	// Returns the clamped gain in [0,4].
	int request_local_player_nvg_gain(int p_delta);
	// The view actions' chase preference (view1st/viewwithgun -> false,
	// viewchase -> true) [orig: g_camera_third_person_selected @ 0xA860DF]; the
	// camera mode itself is RESOLVED per tick by the arbiter from the
	// preference and the seat (world/player_view.h player_view_resolve_mode).
	void set_local_player_third_person_selected(bool p_selected);
	// The debug menu's on-foot third person — stock JO never resolves it
	// (net-re §5.39, the onhook debug affordance); never a gameplay key.
	void set_local_player_debug_third_person(bool p_enabled);
	// The shell-sampled head-bone eye (Godot space) for the 3P anchor chase; pass
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
	// Header {version, stride, row_count}, followed by rows {bank, handle, x,
	// y, z, heading_bam, icon, argb, flags, source, remaining_ticks,
	// entity_known, policy_flags, half_x_q16, half_y_q16, floor_px}.
	// Coordinates are mission 16.16; the policy tail is resolved from the
	// LOCAL entity's def class exactly where retail resolves it (witness at
	// world::minimap_blip_draw_policy). No native pointers escape.
	PackedInt32Array get_hud_minimap_snapshot() const;
	// Static footprint polygons for footprint-class markers (buildings/zones
	// with marker models): {version=1, count} then per row {handle,
	// fill_argb, fill_value_count, xy_q16..., edge_value_count, xy_q16...}.
	// Baked once per world from the OOBJ occlusion ground-slice mesh transformed
	// by the entity pose (witness at world::minimap_footprint_from_occlusion).
	PackedInt32Array get_hud_minimap_footprints() const;

private:
	// get_hud_minimap_snapshot cache: the retained banks bump
	// ClientMinimapState::revision, and every other input — entity resolves,
	// per-row draw policies, the restored local row — advances only with the
	// world logic tick, so the display frames between 62 Hz ticks reuse the
	// built array instead of re-walking the 1160 retained slots.
	mutable PackedInt32Array minimap_snapshot_cache_;
	mutable uint64_t minimap_snapshot_revision_ = 0;
	mutable uint64_t minimap_snapshot_tick_ = 0;
	mutable uint16_t minimap_snapshot_local_handle_ = 0xFFFF;
	mutable bool minimap_snapshot_valid_ = false;

public:
	// { present: bool, position: Vector3 } — the type-2043 grid-origin marker.
	Dictionary get_hud_map_grid_origin() const;
	// The objectives-panel rows: an Array of {slot, text_id, shown, done} for
	// header slots 1..8, terminated at the first 0/255 win-condition id —
	// exactly the panel's row walk [orig: HUD_DrawWinConditions @0x5ba9e0..;
	// shown = show-win bit, done = won bit].
	Array get_objectives_view() const;
	// The FSM snapshot for the shell: latest clip/action payloads, diagnostic serials,
	// ammo, kick, and the 3P body channel. Ordered presentation events drain through
	// drain_local_player_weapon_events(); the snapshot alone is not an event queue.
	Dictionary get_local_player_weapon_state() const;
	// Destructively drain the ordered presentation outputs accumulated since the
	// previous render frame. Each Dictionary encodes one PlayerWeaponEvent.
	Array drain_local_player_weapon_events();
	// Destructively drain the flight sim's resolved round impacts, each row already
	// mapped through the ammo effects_table to {position, direction, effect, sound}
	// [orig: Projectile_SpawnImpactEffect @ 0x4e9b80; world/round_sim.h RoundImpact].
	Array drain_round_impacts();
	// Drain this frame's folded S2C 0x1E game events as feed rows — one per
	// line the original would post to its message feed (retail: the 0x426270
	// handler). Each row carries the actor NAMES (resolved here, where the
	// decoded roster lives), the canned-message key (plus the camp rows'
	// WPNames level key), and the witnessed line color; the embedder resolves
	// the keys against gametext and calls the format helpers below.
	// Suppressed types (the LFP result set + the tip-only 58) never appear.
	Array drain_feed_events();
	// The folded Tab board's HEADER (netsim ClientScoreboard counts + the
	// session strings): known/team_mode/timed, the witnessed players count
	// (accepted rows minus the spectator trailer, netsim::scoreboard_header),
	// in_game/spectators, game_type, server and mission names. The rows no
	// longer round-trip through script — HudOverlay pulls them natively via
	// fill_scoreboard_rows.
	Dictionary get_scoreboard() const;
	// The native Tab-board row handoff: fills the drawer's entries via the
	// netsim projection (netsim::project_scoreboard — wire order, the server
	// sorts and the client never re-sorts). NOT ClassDB-bound; HudOverlay
	// calls it through this typed seam. Returns false (rows cleared) when no
	// runtime exists.
	bool fill_scoreboard_rows(
			std::vector<opennova::hud::ScoreboardEntry> &r_rows) const;
	// The mounted-vehicle panel (hud/hud_vehicle_panel.h, world/vehicle_panel_feed.h):
	// {shown, item_id} — the panel's root vehicle (the attached gun child
	// re-roots to its parent), whose items.def sid the shell joins to its
	// VEHICLE_HUD block. Read-only.
	Dictionary get_vehicle_panel_view() const;
	// The native panel handoff (NOT ClassDB-bound; HudOverlay::set_vehicle_panel
	// calls it): the hull's health band + one row per authored seat pair
	// (occupancy, rider health, the seat-select digit, the own seat). Returns
	// false (rows cleared) when the local player rides nothing.
	bool fill_vehicle_panel(const DefVehicleHudBlock &p_block,
			opennova::hud::HudVehiclePanelState &r_state) const;
	// The AAS zone status panel feed (world/lfp_feed.h), NOT ClassDB-bound:
	// one HudLfpZone per spawn-zone list entry joined with the client
	// runtime's zone-timer entry and the zone's transient minimap slot flags.
	bool fill_lfp_zones(int p_local_team,
			std::vector<opennova::hud::HudLfpZone> &r_zones);
	// The in-match game type for every role (joiner header / HostClient view).
	int64_t get_session_game_type() const;
	// The S2C 0x14 player-chat lines since the last drain, each routed by the
	// witnessed channel table: [{text, argb, sink, channel}] where sink 0 =
	// the SYSTEM ring, 1 = the CHAT ring, 2 = the message queue, 3 = channel 3.
	Array drain_chat_lines();
	// Substitute actor names into a canned template (retail: Chat_FormatMessage
	// @0x422C60): the STRCND48 bonus re-compose when `extra` names the local
	// player, then $A/$B sequential case-insensitive replace-all. Exposed so
	// the string lookup can live with the string table while the substitution
	// rule stays in engine C++.
	String format_feed_line(const String &p_template, const String &p_attacker,
			const String &p_victim, const String &p_extra,
			const String &p_bonus_template) const;
	// Compose a camp line — the template's %s takes the level's WPNames string
	// (retail: the case-59/60 sprintf @0x427327/@0x42736B).
	String format_feed_camp_line(const String &p_template,
			const String &p_wpname) const;

	// --- the local player's loadout: slot pool, spawn kit, map rules -------------------
	// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)
	// The spawn kit [orig: the 2048-B tuple buffer 'restrictionData' @ 0x24D4E00]:
	// rows {name, ammo_primary, ammo_secondary, flags} (values default -1). When
	// p_filter_by_availability, the kit is filtered through the availability table
	// with the knife fallback — the SP .bms promote leg [orig: Mission_LoadBMSFile
	// @ 0x40f7ae]; the armory/profile legs store unfiltered (the server validates).
	// An empty kit resets to the engine default {WPN_M4AUTO} [orig: @ 0x5246be].
	void set_spawn_loadout(const TypedArray<Dictionary> &p_kit, bool p_filter_by_availability);
	// True only after a mission/profile explicitly supplied a spawn kit; the
	// WPN_M4AUTO engine fallback created by load_weapon_table leaves this false.
	bool has_explicit_spawn_loadout() const { return local_loadout_.spawn_kit_set; }
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
	// Commit the profile class without replacing a mission-authored weapon kit.
	bool set_local_player_class(int p_player_class);
	// Load the player's weapon profile (weapon.sav) from an ABSOLUTE filesystem path.
	// This is a save file, not a mounted PFF/loose resource, so it is read through
	// FileAccess rather than the resource root. Header gate: magic "FPBC" + version
	// "0211", then five 0x1080C profile-slot records; slot 0 becomes the active
	// record and its class bytes are clamped to [5,9]. A missing or malformed file is
	// NOT fatal — the shipped defaults stay installed and an Error is returned so the
	// caller can warn. [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0 (the
	// "expansion\\<g_ExpansionName>\\weapon.sav" path build @0x54f68c..@0x54f6b7);
	// the session-start clamp apply_session_settings_to_globals @0x5516ab]
	Error load_weapon_profile(const String &p_path);
	// Read slot 0's two character headers (raw bytes, no session clamp) without
	// requiring a live Simulation. Returns {error, loaded, blue, red}; each side
	// carries player_class, avatar_a (nationality id), avatar_b (division id),
	// and avatar_packed. This is the menu boot seam over the same five-record
	// file as load_weapon_profile().
	static Dictionary read_weapon_profile_summary(const String &p_path);
	// Persist PLAYER_INFO's ACCEPT snapshot into active profile slot 0:
	// `profile.player_class` (5..9) is written to BOTH side blocks and each
	// non-empty `profile.side_profiles[side]` {avatar_a, avatar_b, avatar_packed}
	// to its own block — playersav::update_avatar_selection carries the
	// save_player_info_from_dialog witness. The other four slots and every kit
	// page survive; the file is replaced atomically.
	// ERR_INVALID_PARAMETER when the snapshot carries no committable side.
	static Error save_weapon_profile_selection(const String &p_path,
			const Dictionary &p_profile);
	// The profile file's path RULE relative to the mount root (playersav
	// weapon_sav_relpath): with an active expansion retail looks ONLY under
	// "expansion/<name>/", never the root [orig: the path build
	// @0x54f68c..@0x54f6b7]. Static so shell path assembly stays a join.
	static String weapon_profile_relpath(const String &p_expansion_name);
	// The FP viewmodel submit spec {gun, arms, adm, show_arms} (simassets
	// fp_viewmodel_spec [orig: Player_RenderFirstPersonViewModel @0x4ded60;
	// the emplaced arms omission @0x4dedc7]). `character_arms` is the local
	// player's resolved combo arms graphic (retail's CharacterEntity arms model,
	// the ONLY arms source — weapon.def gfx1a/gfx1b are discarded tokens);
	// has_def=false is the bring-up path; an empty gun on a resolved def means
	// submit no FP gun.
	// The witnessed viewmodel placement units, re-exported from engine
	// simassets/fp_viewmodel_spec.h.
	static double weapon_def_pos_scale();
	static Vector3 viewmodel_fallback_pos_units();
	static Vector3 viewmodel_fallback_tpos_units();
	static Vector3 viewmodel_fallback_rot_bias_deg();
	static double viewmodel_pass_near_z();
	static String viewmodel_bringup_fallback_weapon();
	// Player-view calibration re-exports (world/player_view.h).
	static double player_eye_min_above_position();
	static double player_non_person_eye_bump();
	static int player_head_bone_index();
	static double player_aim_project_range();
	// The witnessed person hit-zone -> damage-multiplier table and the
	// Landable seat-branch bone leg (world/round_sim.h carries the witness;
	// the engine 6.0 seat leg is the truth the debug views mirror).
	static double hit_zone_damage_multiplier(int p_section) {
		return opennova::world::hit_zone_damage_multiplier(p_section);
	}
	static double seat_hit_bone_damage_multiplier(int p_bone) {
		return opennova::world::seat_hit_bone_damage_multiplier(p_bone);
	}
	// The per-axis portal-slot collection range, world units
	// (world/occlusion.h kPortalSlotCollectRadius).
	static double portal_slot_collect_radius() {
		return opennova::world::kPortalSlotCollectRadius;
	}
	// The mission coordinate domain in world units (world/geom.h — the signed
	// 16.16 carrier span the debug/edit fields clamp to).
	static double mission_coord_min() {
		return opennova::world::kMissionCoordMinUnits;
	}
	static double mission_coord_max() {
		return opennova::world::kMissionCoordMaxUnits;
	}
	static Dictionary fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
			const String &p_character_arms, const String &p_animadm, int p_flags);
	// Read-only view of the active profile record for the shell's status copy:
	// {loaded, blue: {player_class, avatar_a, avatar_b, avatar_packed, kit: [names]},
	//  red: {...}}. The kit array is the SELECTED page — the one the class byte picks.
	Dictionary get_weapon_profile_summary() const;
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
	// Canonical, unexpanded current tuples for the armory host. Retail preselects
	// visible parent rows from g_armoryLoadoutBufferByClass, never from the expanded
	// weaponSlotArrayBase [orig: populate_ammo_type_combo_boxes @ 0x564930].
	TypedArray<Dictionary> get_local_player_loadout() const;

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (WacProgram). Applied now if
	// loaded and re-applied on every (re)load. Pass null to uninstall.
	void set_wac_program(const Ref<WacProgram> &p_program);
	Ref<WacProgram> get_wac_program() const { return wac_program_; }
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; inspect via get_wac_program().
	bool compile_and_set_wac(const PackedStringArray &p_sources);
	// Retail executes the freshly installed WAC once before the 255-tick
	// environment settle. Host/standalone authority only; idempotent per load.
	bool run_mission_start_wac();
	// Replace the early post-BMS restore point with the fully settled play-start
	// state, including WAC temporal/RNG state.
	void seal_mission_start_baseline();
	// { loaded, paused, runs, event_count, code_size } for transport/debug UI.
	Dictionary get_wac_state() const;
	// Last-frame microsecond counters for the runtime hot path. Allocates only when queried.
	Dictionary get_runtime_perf_counters() const;
	// One opt-in seam for native sim/net/present/occlusion timings and
	// projectile collision attribution. Disabled by default so ordinary play
	// performs no native profiling clock reads or timing-counter writes.
	void set_runtime_profiling_enabled(bool p_enabled);
	bool is_runtime_profiling_enabled() const {
		return runtime_profiling_enabled_;
	}
	// Allocation-free last-tick trace sampling for the F3 hot path. Vector
	// lanes are times=(terrain, static, dynamic, person),
	// counts=(calls, static survivors, dynamic survivors, person survivors),
	// and faces=(static, dynamic).
	Vector4i get_last_projectile_trace_times_us() const;
	Vector4i get_last_projectile_trace_counts() const;
	Vector2i get_last_projectile_trace_faces() const;
	// Allocation-free int forms of the same last-frame counters, for per-frame
	// sampling by the F3 frame-stats board (a Dictionary per frame would churn).
	int64_t get_last_sim_tick_us() const { return static_cast<int64_t>(last_sim_tick_us_); }
	int64_t get_last_net_tick_us() const { return static_cast<int64_t>(last_net_tick_us_); }
	int64_t get_last_present_snapshot_us() const {
		return static_cast<int64_t>(last_present_snapshot_us_);
	}
	int64_t get_last_occlusion_build_us() const {
		return static_cast<int64_t>(last_occlusion_build_us_);
	}
	int64_t get_last_occlusion_probe_us() const {
		return static_cast<int64_t>(last_occlusion_probe_us_);
	}
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// The shell fire-presentation drain: one Dictionary per round spawned since the
	// last call — {origin: Vector3 (godot), forward: Vector3 (godot, unit),
	// shooter_handle, is_local_player, ammo_index, sound_set, effect, mf_light} with
	// the ammo-def 'ai_launch'/'ai_launcheffect' names resolved. The fire present
	// pass plays/spawns per event, skipping the local player (whose action-slot
	// presentation is already ported). [orig: WeaponSlot_FireAndSpawnEffects
	// @0x53F440 — the firing host presents its own rounds inline at fire time;
	// world-wac-ai-re §17.4]
	Array drain_fire_presentation_events();

	// The fire-sound legs on the logic clock (world/fire_sound.h): the shell
	// stamps the camera listener each frame before the tick batch, and drains
	// the ready one-shots ({set, pos, source_bms_id} rows) each present.
	// [orig: listener_pos @ 0x24D6630; Sound_TickPendingSlots @ 0x529310]
	void set_sound_listener(const Vector3 &p_listener_godot);
	Array drain_fire_sounds();

	// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=fwd,
	// Y=left, Z=up) from raw weapon.def pos/tpos units — the /256 blend +
	// NoCardSwitch suppression run in world/player_view (S8), then the
	// per-frame motion lead (the damped movement-delta tracker) and the 0x500
	// z drop when the viewport frames 4:3 or narrower — the rig samples the
	// viewport SIZE (device work) and the 3w<=4h rule itself is the engine's
	// (world/player_view.h player_view_narrow_aspect). The rig maps view axes
	// onto its camera frame. [orig: Player_UpdateFirstPersonCamera @ 0x4dd380
	// — lead @ 0x4dd4f2..0x4dd56c, narrow-aspect drop @ 0x4dd571]
	Vector3 local_player_viewmodel_bias_view_units(
			const Vector3 &p_pos_raw_units, const Vector3 &p_tpos_raw_units,
			int p_viewport_w, int p_viewport_h);

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

	// Queue this frame's REMOTE-body footsteps and foley for one wire row.
	// Runs only for wire-RENDERED bodies (a joiner's remote rows, a listen
	// host's admitted players); the authority tick's sound pass never reaches
	// net-snapped peers, so each drawn body has exactly one source. The
	// applier supplies the row's identity, the wire-driven clip playhead span
	// it just crossed, and its world pose; the witnessed consume itself is
	// the portable world::wire_body_slot_sounds (world/wire_body_sound.h),
	// fed through the same SoundSlotEvent drain the authority bodies use
	// (retail: the org1/org2 sound blocks, see docs/audio/lwf-dbf-sound-re.md).
	void present_wire_body_sounds(int p_type_id, int p_character_id,
			int p_wire_handle, int p_carrier_handle, int p_anim_state,
			int p_from_phase, int p_to_phase, const Vector3 &p_pos);
	// Drain persistent entity-attached emitter registrations. Producers refresh
	// a keyed (source_spawn_id, lane) intent; the audio layer expands `set` into
	// LWF layers and owns keep-alive, spatial ranking, and physical voices.
	// Rows are {source_spawn_id, handle, source_bms_id, pos, lane, slot,
	// lifetime, emitted_tick, pitch_q16, volume_q8_8, source_only, set}.
	// [orig: SoundEmitter_Register @0x529270]
	Array drain_sound_emitters();

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

	// The in-flight round glows: one row per active round whose ammo authors
	// `light_move` — {id (presentation generation), pos (godot space), radius,
	// color}. The presenter's light pool spawns a permanent (mode 1) light per
	// id, follows it per tick, and despawns dropped ids (retail:
	// RoundData_SpawnRound @0x4ec8da spawn, the per-tick follow @0x4eaa9f,
	// Projectile_ReleaseEffects clear — witness map on
	// engine/runtime/renderer/light_scene.h).
	Array get_round_glow_rows() const;

	// The styled ribbon compile over trail rows (renderer/tracer_frame.h owns
	// the witnessed style tables and the camera-facing build
	// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0]). Static so the present
	// pass and stub-sim tests share the one native seam:
	// {additive: {positions, colors}, alpha: {positions, colors}, channels} —
	// each family one triangle-strip vertex run (channels joined by degenerate
	// pairs), ready for verbatim ImmediateMesh upload.
	static Dictionary compile_tracer_ribbons(const PackedFloat32Array &rows,
			const Vector3 &camera);

	// The destruction presentation drain (world/destruction.h; world-wac-ai-re
	// §24): {effects[], sounds[], husk_swaps[], debris_triangles, glass_points,
	// explosions_processed, items_destroyed}, godot-space positions, cleared on
	// read. Once per present, beside the fire drain.
	Dictionary drain_destruction_events();
	// The live death-piece pool as dictionaries {slot, generation, item_id,
	// section, type_index, scale, pos, heading, pitch, settled} — each piece renders as its single
	// husk-model section. [orig: DeathPiece_TickAll @0x57b900; §24]
	Array get_death_pieces() const;
	// Per-entity destruction diagnostics by bms_id (probe/F3 seam): health,
	// bound_radius, flags, traits presence, KZ/bridge-DEAD anchors — the damage
	// chain's gate inputs.
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
	// The S11 dual-publish seam (D-NET-209): true (default) presents armed
	// replica rows from the simulation-arbitrated channel directly; false
	// routes them through the legacy remote-request publish (model-side FSM).
	// This toggles ONLY the PUBLISH path — the per-record receive arbitration
	// and the deferred/insert channel work in the fold run either way, so the
	// flag is a presentation A/B, not a full pre-S11 rollback.
	// Native pose-path health: cumulative queries/declines for the collision
	// provider and the mounted resolver, plus the installed mounted model
	// sources. The soak gates on declines == 0 — the A/B divergence stats this
	// replaces were retired with the cutover.
	Dictionary debug_native_pose_stats() const;
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
	// (registry + motor copy) so in-game probes can shorten a fight. Returns
	// ERR_UNAVAILABLE without a live sim, ERR_INVALID_PARAMETER for a missing
	// AI index, and OK only after both authoritative mirrors are mutated.
	Error debug_set_entity_health(int p_index, int p_hp);
	Error debug_crew_vehicle(int p_occupant_ssn, int p_vehicle_ssn);
	void set_local_player_eye_offset(const Vector3 &p_offset_godot, bool p_valid);
	bool local_player_fp_weapon_hidden() const;
	Error debug_crew_local_player(int p_vehicle_ssn);
	// Probe seam: teleport an AI entity (mission-space coords) through both
	// position stores, for probes defeated by mission geography. Uses the same
	// truthful Error contract as debug_set_entity_health.
	Error debug_set_entity_position(int p_index, const Vector3 &p_mission_pos);
	// World-registry probe seams by SSN (pool-1 vehicles carry no AI brain and are
	// invisible to the AI-index seams): entity card + mission-space teleport.
	Dictionary get_world_entity_debug(int p_net_id) const;
	// The decoded joiner-side client row for one wire handle — the ClientState
	// twin of get_world_entity_debug (which reads the materialized registry):
	// exactly what the wire carried and the fold retained, before presentation.
	// Empty when not a joiner or the handle has no row.
	Dictionary get_client_entity_debug(int p_handle) const;
	void debug_set_world_entity_position(int p_net_id, const Vector3 &p_mission_pos);
	// Exact-slot parity probe: set the authoritative MountSlot words on a
	// world entity so a real UDP phase-8 sample can prove receiver application.
	Error debug_set_world_entity_weapon_ammo(int p_net_id, int p_clip,
	                                         int p_reserve);
	// Land the local player at an exact F3-dumped pose (probe seam). Returns
	// ERR_UNAVAILABLE until the complete local-player subject exists.
	Error debug_teleport_local_player(const Vector3 &p_mission_pos, float p_yaw_deg,
			float p_pitch_deg);
	// The D-AI-6 muzzle seam: per-frame posed bullet fire-origin userpoint push from the
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
	// The adjacent retail transition-arbitration flags table (off_8139E8).
	static int64_t infantry_anim_flags(int p_state);
	// The queue gate over two of those flag words (world/infantry.h) — the one
	// rule the netsim record fold and the presenter body FSM both apply.
	static bool remote_body_state_defers(int64_t p_current_flags, int64_t p_next_flags);

	// Entity query. The (kind, index) pair lets the shell map a sim entity back to
	// its promoted mission record and already-rendered node.
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	int get_entity_index(int p_index) const;        // index within its kind's list
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw(int p_index) const;        // BAM heading -> radians
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for shell remap)
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
	int get_entity_bms_id(int p_index) const;       // file entity id; the shell maps this to a placed node
	int get_entity_owner_connection_id(int p_index) const; // entity+0x78 dcb; the networked-player identity (D-NET-112)
	int get_entity_wire_handle(int p_index) const;  // (pool<<12)|slot — the per-entity wire identity
	// Godot-space positions of the entities the distant MODEL/depth-mask foliage
	// tier generates around: crouched or prone (MoveOrder stance bits 8-9) and
	// standing on terrain, not on another entity. Retail tests every visible
	// sector entity, but only infantry ever carry the stance bits.
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	// (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7]
	PackedVector3Array get_foliage_mask_anchor_positions() const;
	// PF_PHASE/PF_ACTIVE jointly encode one exact signed dword: PHASE carries
	// low16 and ACTIVE carries high16+1 (zero means unpublished). This avoids
	// float32 precision loss in the otherwise-float presentation snapshot.
	static int32_t decode_present_part_anim_phase(
			const PackedFloat32Array &p_snapshot, int p_base, int p_channel);
	// Raw signed part-anim channel dword (PLAYPARTANIM); channel is 1 or 2.
	// Ordinary sweeps occupy 0..0x10000, while zero-time wrapping states are
	// preserved. The shell renders the model part from this value.
	int get_entity_part_anim_phase(int p_index, int channel) const;
	// True when the authority publishes this semantic channel. This is an
	// ownership predicate, not a movement predicate: owned endpoints, including
	// zero, must overwrite a prior pose. Channel 1 is suppressed by
	// ItemDefAttrib 0x1000; channel 2 is unconditional.
	bool get_entity_part_anim_active(int p_index, int channel) const;
	// Entity.body_anim_slot: the main-body skeletal clip (.bad via .adm) the AI requested. Written
	// by EntityCommands::set_ssn_anim; consumed only by the shell's deferred apply_body_anim seam
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
	// Revision for the exact ordered identity layout of the most recently
	// returned snapshot. Pose-only changes keep this stable.
	int64_t get_present_layout_revision() const {
		return static_cast<int64_t>(present_layout_revision_);
	}
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the shell's loaded TerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). GameWorld
	// and direct test/tooling fixtures call this through MissionPresentation.setup().
	void set_terrain_height_field(const Ref<TerrainData> &p_terrain);
	// S16 (ADR 0028): the seat/mount table installs through the NATIVE
	// extractor (simassets::extract_item_seat_specs) over the retained def
	// rows + the sim's own model parses. Seeds are full 1xxxxx def ids; the
	// extractor walks authored addeweap children transitively. The shell
	// GDScript extraction + its Dictionary install seam are gone — proven
	// equivalent live before the cutover (29/29 specs identical, 0 mismatches
	// on retail 00TRg, 2026-08-07).
	void install_native_seat_specs(const Ref<class ItemDatabase> &p_item_db,
			const std::vector<int> &p_seed_item_ids);
	// The joiner prewarm's wire-type install (a header-only join has no
	// mission-body table): seeds = streamed type ids + kItemIdOffset,
	// replacing the whole installed table exactly like the boot install.
	// False when the item db or the sim's asset root is missing.
	bool install_seat_specs_for_type_ids(
			const Ref<class ItemDatabase> &p_item_db,
			const PackedInt32Array &p_type_ids);

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	// Wire the infantry root-motion source: resolve a model's .adm (e.g. "E_STAND.adm")
	// through the shell's resource root and keep its clips' root tracks. Returns the number
	// of anim states with a usable clip (0 = nothing loaded; org1 soldiers then stand —
	// motion comes from clips, as in the original). Survives reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class ResourceRoot> &p_resource_root, const String &p_adm_name);
	int get_infantry_clip_count() const { return infantry_anim_.clip_count(0); }

	// Per-entity grounding: resolve every active infantry soldier's OWN model .adm (from its
	// items.def type id via the item database) and store its registry adm_id on the entity, so
	// each grounds + locomotes off its own clip rather than the shared default set. The
	// resolver inputs are retained so players spawned later receive their ADM automatically.
	void resolve_infantry_adm_ids(const Ref<class ResourceRoot> &p_resource_root,
	                              const Ref<class ItemDatabase> &p_item_db);

	// Per-entity items.def trait resolution: stamp each live entity's is_ai_capable (AIData
	// attrib — gates the 0x0D AI-trailer, D-NET-97), net_class_code (§5.10b *_function class
	// tag -> the 0x0A serialize class; an unresolved/ewep item must NOT be serialized as a
	// vehicle or the client desyncs), and health_max/health (items.def hp = healthMax
	// [orig: Entity_InitFromItemDef @0x49e550]). Idempotent; call after load (and again after
	// spawning the local player).
	void resolve_item_traits(const Ref<class ItemDatabase> &p_item_db);

	// The D-AI-5 host weapon seed: stamp every AI entity's anim-fire round from its
	// items.def ammo_closeattack + clipsize (AiProfile::ammo_primary/clip_size — the
	// single-ammo stand-in for the entity+0x358..0x35B family, whose load-time
	// block-copy writer is unwitnessed; world-wac-ai-re §17.4/§17.7 item 1), and seed
	// the spawn magazine [orig: Entity_ResetToSpawnState @ 0x4b97a9 — word
	// entity+0x35C = itemDef+0x894]. Ammo NAMES resolve against the mission ammo
	// table, so call AFTER load_ammo_table; unresolved/absent leaves the NPC unarmed
	// (ammo_primary -1, the fire pass skips). Idempotent; returns armed-NPC count.
	int resolve_ai_weapons(const Ref<class ItemDatabase> &p_item_db);
	// Install the packed Avatars.def character-sex registry used by the
	// portable sound-profile selector. Retained across reset_world; returns the
	// number of unique packed character ids installed.
	int set_character_avatar_database(
			const Ref<class AvatarDatabase> &p_avatar_db);

	// World-object collision sweep: for each live entity with an items.def graphic,
	// load its .3di collision block (BVOL volumes + BPLN planes via the placer's
	// ObjectData cache), register one runtime model per graphic on the sim
	// collision world, and attach the per-entity instance. From then on the infantry
	// motor resolves against placed objects — CB wall push-out, standing on roofs,
	// hurt/CA/BB triggers, and the CL ladder legs (frame extraction, entry gate,
	// alignment chase, climb states, exits) [orig: collision resolver @0x4b2bd0
	// + the query set; docs/world/world-wac-ai-re.md §15; D-INF-3].
	// Returns the instance count. Also attaches the render-occlusion portal
	// models (buildings whose graphic carries OVRT/OPLN/OFAC/OOBJ records)
	// with their def bits. Idempotent per load. Model extraction reads the
	// sim's own SimModelCache through the installed asset root
	// (set_asset_root; ADR 0028) — a rootless sim attaches nothing.
	int resolve_collision_instances(const Ref<class ItemDatabase> &p_item_db);
	// Install the mounted root the SIMULATION resolves assets through — the
	// engine-side mirror of the render mount. Pins the root's index for the
	// sim-model cache (ADR 0028).
	void set_asset_root(const Ref<class ResourceRoot> &p_root);

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
	// Delta form of get_building_visibility(): only pairs whose packed value
	// changed since the last call, so the shell applies changes instead of
	// re-walking the whole building set every frame.
	PackedInt64Array get_building_visibility_changes();
	// Delta form of get_render_culled_bms_ids():
	// [n_added, ids..., n_removed, ids...] since the last call.
	PackedInt32Array get_render_culled_changes();
	// Per-entity sun-visibility factor feed (D-RLIT-3): pairs
	// [bms_id, quality 1..4] whose quality changed since the last call. The
	// shell maps quality through sun_visibility_factor into
	// ObjectModel.set_entity_lighting_context. The local player's quality is
	// computed but never emitted here — the presenter reads it via
	// get_local_player_sun_quality() so the FP parts can keep their witnessed
	// exemption while the third-person body dims.
	PackedInt64Array get_entity_sun_visibility_changes(const Vector3 &p_light_dir);
	int get_local_player_sun_quality() const { return local_sun_quality_; }
	// Quality (1..4) -> the effectScale the render-state stack multiplies —
	// engine-owned so the mapping has ONE writer (renderer::
	// sun_visibility_factor; retail: Entity_ComputeSunVisibility @0x5c6800,
	// stack write @0x5c7fa5, see docs/render/render-lighting-re.md). Both
	// presentation consumers (the occlusion sun feed and the local-player
	// body) call this instead of re-deriving the 0.25 step.
	float sun_quality_factor(int p_quality) const;
	// The present pass's visibility intent for one placed entity — the
	// occlusion release edge lands a node on the sim's CURRENT visibility so a
	// hidden entity never flashes for a frame.
	bool entity_present_visible(int p_bms_id) const;
	// Forget the applied-state baselines: the next delta call re-emits the
	// full frame state (the occlusion A/B seam and shell cache resets use it).
	void reset_occlusion_apply_baseline();
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
	// Read-only F3 rows for the placed throwable devices: entity_handle,
	// item_id, team, pos (mission units), yaw/pitch/roll_deg (the exact stick
	// pose), parent_handle + parent_live, arm_delay_ticks, think, health.
	Array get_throwable_debug() const;
	// Per-frame visual snapshot of item-modeled throwables: tracer-cadence flying
	// rounds with a TrcrID model plus placed devices. Entries: {key, item_id, pos (godot),
	// rotation_deg (pitch, yaw, roll — placer convention)}; the enemy-team item
	// swap follows the viewer team [orig: the S2C 0x59 dual TrcrID words +
	// the spawner's team pick @ 0x4ec79b; world-wac-ai-re §27].
	Array get_throwable_visuals() const;

	// The impact-scar draw list for ScarPresenter (nova_simulation_scars.cpp):
	// World::scars compiled through renderer::compile_scar_draws with the shell's
	// camera (Godot space), fog distance and the combined terrain light colour
	// (Env_TerrainLightCombined — EnvFile.combine_terrain_light(sun, sky)).
	// { vertices (PackedVector3Array, Godot axes; world space for shared-ring
	//   batches, SECTION-LOCAL for entity-ring batches), uvs, colors,
	//   batch_owner/texture/section/flags(bit0 entity_local, bit1 building)/
	//   first/count, batch_bms_id, batch_spawn_origin, strip_names,
	//   slots_live, slots_culled, rings_leased }. Empty without a world.
	Dictionary get_scar_draw_list(const Vector3 &p_camera_godot, float p_fog_distance,
			const Color &p_terrain_light) const;
	// The Scar_RenderCache owner gate over OcclusionWorld's section masks and
	// the entity's blink-box quad (see nova_simulation_scars.cpp).
	bool scar_owner_visible(uint16_t p_owner_packed) const;

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
	// The F3 entity picker: one plain geometric trace_projectile segment
	// (terrain / water / static + dynamic CFAC / person bone spheres, nearest
	// wins) along a camera or crosshair ray. Read-only — never affects the
	// sim. Stable-shape Dictionary; hit=false with blocked =
	// "terrain"/"water"/"proxy" names why the ray stopped without a pickable
	// entity (proxies = wire-decoded geometry on joined visual-only clients).
	Dictionary debug_pick_entity(const Vector3 &p_from_godot,
			const Vector3 &p_dir_godot, float p_max_range_units);
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
	// items.def id of the pool-2 building encoded by blink_hits[0], or 0 when
	// the player is not inside a blink volume. Entity::item_id is the raw BMS
	// type, so this accessor applies mission::kItemIdOffset for database lookup.
	// Lighting keys from hit PRESENCE, independently of the aggregate
	// "indoors" flag bit.
	int local_player_interior_item_id() const;

	// The blink-box owner for a model-light spawn at a world point: retail runs
	// ONE point query at the spawning entity's position before walking its LGHT
	// records, and slot 0's packed hit names the containing building + section
	// every unattached record binds to (retail: Entity_SpawnGlowEffects
	// @0x56c7fc -> Entity_QueryBlinkBoxesAtPoint @0x4af350, decoded @0x56c8c9
	// and @0x56c8db, see docs/render/render-lighting-re.md). Returns
	// [containing bms_id, section], or an empty array when the point sits in no
	// blink volume (or the containing entity carries no bms identity). The
	// caller applies retail's ItemDef-type gate: a BUILDING never runs the
	// query at all (retail: @0x56c7ec).
	PackedInt64Array query_blink_owner_at(const Vector3 &p_world);

	// The per-drawn-entity interior light group: every placed entity currently
	// standing inside a blink volume, as [bms_id, containing bms_id, section]
	// triples. Retail pushes this pair per entity draw so an interior room
	// light reaches exactly the entities in its own section (retail:
	// setup_terrain_effect_for_entity @0x5c74a0 -> Lighting_SetInteriorLightGroup
	// @0x5a90e0, the gate Light_PassesActiveGroups @0x5a9120, see
	// docs/render/render-lighting-re.md). Entities outside every blink volume
	// are absent (their group is (0, 0)).
	PackedInt64Array get_entity_interior_groups() const;

	// The local player's interior light group: [containing bms_id, section], or
	// an empty array outdoors. The local player is a spawned entity with no
	// bms_id, so it is absent from get_entity_interior_groups; its group is what
	// scopes interior lights onto the first-person arms and weapon.
	PackedInt64Array local_player_interior_group() const;

	// Sound-occlusion distance inflation for the audio host [orig:
	// Sound_ApplyOcclusionDistance @0x529970 — two LOS rays through terrain +
	// building solids; occluded sources sound farther]. Positions in Godot
	// world space; distance in/out 16.16.
	int64_t sound_occlusion_distance_q16(const Vector3 &listener_pos,
	                                     const Vector3 &source_pos, int64_t distance_q16,
	                                     int source_bms_id = 0);

	// The marched iris-exposure sampling (D-RLIT-2): three classification codes
	// for WeatherCore.set_exposure_from_iris_samples — the camera ray runs
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
	// the STROVER_USEGUN default). Armory mode rides the zone flag; the nearest-only
	// gate consumes the same complete live fire verdict as body/HUD selection.
	// [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 selection half;
	//  Player_CanFireWeapon @0x5cf780]
	TypedArray<Dictionary> get_attach_labels() const;
	TypedArray<Dictionary> get_friendly_tags() const;

	// Parse weapon.def from the resource root and install the armory table on the sim world
	// (world::World::weapons) — the server-side source for the 0x2F/0x5A loadout service, the
	// extended-uplink equipped-weapon gate, and the player-spawn WPN_M4AUTO default
	// (D-NET-141/143). [orig: Game_StartMission @0x5254bd -> WeaponDefs_LoadFile @0x5450A0,
	// right after AnimDef_InitAll @0x5254b3]. Idempotent; call after load.
	Error load_weapon_table(const Ref<class ResourceRoot> &p_resource_root,
	                        const String &p_name = "weapon.def");

	// Parse ammo.def and install the ballistics/damage table (world::World::ammo), then
	// resolve every armory entry's round_type to its ammo index — the authoritative round
	// sim's data feed (§5.60). Call after load_weapon_table.
	// [orig: Game_StartMission @0x52548a -> AmmoDef_LoadAll @0x40b0b0]
	Error load_ammo_table(const Ref<class ResourceRoot> &p_resource_root,
	                      const String &p_name = "ammo.def");

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Simulation::PresentField);
VARIANT_ENUM_CAST(godot::Simulation::EffectStateField);
VARIANT_ENUM_CAST(godot::Simulation::SeatCode);
VARIANT_ENUM_CAST(godot::Simulation::MountCommand);
VARIANT_ENUM_CAST(godot::Simulation::JoinTerrainTilState);
